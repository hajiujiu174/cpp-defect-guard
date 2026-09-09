#include "codeguard/application.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace cg = codeguard;
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path directory, root, database;
    Fixture() {
        directory = fs::current_path() / ("codeguard-analysis-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(directory), "unique fixture creation");
        root = directory / fs::path(u8"中文 source"); fs::create_directory(root);
        database = directory / "inventory.sqlite3";
    }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
    void write(const std::string& name, const std::string& content) {
        fs::create_directories((root / cg::from_utf8(name)).parent_path());
        std::ofstream output(root / cg::from_utf8(name)); output << content;
        require(bool(output), "fixture write");
    }
    void commands(const std::vector<std::string>& files, const std::string& extra = "") {
        // generic UTF-8 paths contain no JSON-special characters in this fixture.
        std::ofstream output(directory / "compile_commands.json"); output << '[';
        bool first = true;
        for (const auto& file : files) {
            if (!first) output << ','; first = false;
            output << "{\"directory\":\"" << cg::utf8_path(root) << "\",\"file\":\"" << file
                   << "\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-DFLAG=1\",\"-c\",\"" << file << "\"" << extra << "]}";
        }
        output << ']';
    }
    cg::ScanResult scan() {
        cg::ScanOptions options; options.compile_commands = cg::utf8_path(directory / "compile_commands.json");
        return cg::import_project(root, database, options);
    }
};
void run(const std::string& test) {
    Fixture fixture;
    if (test == "graphs") {
        std::vector<cg::GraphEdge> edges{{"call","a","b","",1}, {"call","b","a","",1}, {"call","c","c","",1}, {"call","a","b","",2}};
        const auto graph = cg::summarize_graph({"isolated"}, edges);
        require(graph.nodes.size() == 4 && graph.edge_count == 3 && graph.cycles.size() == 2 && graph.topological_order.empty(), "cycles/self-loop/dedup/isolated");
        const auto reached = cg::reachable("a", edges);
        require(reached == std::vector<std::string>({"a", "b"}), "BFS");
        require(cg::summarize_graph({}, {}).nodes.empty(), "empty graph");
        require(cg::summarize_graph({}, {{"include", "a", "b", "", 0}}).topological_order == std::vector<std::string>({"a", "b"}), "DAG order");
        edges.clear();
        for (int i = 0; i < 12000; ++i) edges.push_back({"call",std::to_string(i),std::to_string(i+1),"",0});
        const auto deep = cg::summarize_graph({}, edges);
        require(deep.cycles.empty() && deep.topological_order.size() == 12001, "deep iterative DFS");
        return;
    }
    if (test == "migration") {
        sqlite3* raw = nullptr;
        require(sqlite3_open(cg::utf8_path(fixture.database).c_str(), &raw) == SQLITE_OK, "open v1");
        const int code = sqlite3_exec(raw, R"SQL(
CREATE TABLE project(id INTEGER PRIMARY KEY,root TEXT UNIQUE);
CREATE TABLE scan(id INTEGER PRIMARY KEY,project_id INTEGER,scanned_at TEXT,added INTEGER,changed INTEGER,unchanged INTEGER,removed INTEGER);
CREATE TABLE file(scan_id INTEGER,path TEXT,language TEXT,hash TEXT,size INTEGER,mtime INTEGER,lines INTEGER);
INSERT INTO project VALUES(1,'old'); INSERT INTO scan VALUES(1,1,'time',1,0,0,0);
INSERT INTO file VALUES(1,'old.c','C','old-hash',1,1,1);
PRAGMA application_id=1128744018; PRAGMA user_version=1;
)SQL", nullptr, nullptr, nullptr); sqlite3_close(raw);
        require(code == SQLITE_OK, "create v1");
        { cg::SqliteDatabase readonly(fixture.database, true); require(readonly.latest("old").analysis.status == "not_requested", "read-only v1"); }
        { cg::SqliteDatabase migrated(fixture.database); require(migrated.latest("old").files[0].hash == "old-hash", "preserved v1 snapshot"); }
        cg::SqliteDatabase reopened(fixture.database, true); require(reopened.latest("old").id == 1, "reopen v2");
        return;
    }
    fixture.write("api.h", "#pragma once\nint alpha(int); int beta(int); int overload(int); double overload(double);\nstruct Object { int value; int get() { return value; } };\n");
    fixture.write("a.h", "#ifndef A_H\n#define A_H\n#include \"b.h\"\n#endif\n");
    fixture.write("b.h", "#ifndef B_H\n#define B_H\n#include \"a.h\"\n#endif\n");
    fixture.write("orphan.h", "int orphan;\n");
    fixture.write("a.cpp", "#include \"api.h\"\n#include \"a.h\"\n#ifndef FLAG\n#error missing flag\n#endif\nint alpha(int n) { if (n > 0 && n < 4) return beta(n-1); return 0; }\nint call(int (*fn)(int)) { return fn(1); }\n");
    fixture.write("b.cpp", "#include \"api.h\"\nint beta(int n) { if (n>0) return alpha(n-1); return 0; }\nint overload(int n) { return n; }\ndouble overload(double n) { return n; }\nint variable=0;\n");
    fixture.commands({"a.cpp", "b.cpp"});
    if (test == "partial") fixture.write("b.cpp", "int broken( {\n");
    if (test == "missing_command") fixture.commands({"a.cpp"});
    if (test == "ambiguous") fixture.commands({"a.cpp", "a.cpp", "b.cpp"});
    if (test == "unsafe") fixture.commands({"a.cpp", "b.cpp"}, ",\"-Xclang\",\"-load\",\"evil.dll\"");
    if (test == "identity") {
        fixture.write("left/same.cpp", "static int helper(){return 1;} int left(){return helper();}\n");
        fixture.write("right/same.cpp", "static int helper(){return 2;} int right(){return helper();}\n");
        fixture.commands({"a.cpp", "b.cpp", "left/same.cpp", "right/same.cpp"});
    }
    const auto scan = fixture.scan();
    const auto& data = scan.analysis;
    require(scan.id != 0 && scan.diagnostics.empty(), "snapshot persisted");
    cg::SqliteDatabase db(fixture.database, true);
    const auto loaded = db.latest(scan.root);
    require(loaded.analysis.status == data.status && loaded.analysis.symbols.size() == data.symbols.size()
        && loaded.analysis.edges.size() == data.edges.size() && loaded.analysis.metrics.size() == data.metrics.size()
        && loaded.analysis.covered_files == data.covered_files, "analysis roundtrip");
    if (test == "partial" || test == "missing_command" || test == "ambiguous") {
        require(data.status == "partial", "partial status");
        require(std::count_if(data.units.begin(), data.units.end(), [](const auto& unit) { return unit.status == "success"; }) == 1, "one successful TU retained");
        if (test == "partial") require(!data.units[1].diagnostics.empty(), "compiler error captured");
        return;
    }
    if (test == "unsafe") { require(data.status == "failed" && data.symbols.empty(), "reject unsafe compiler flags"); return; }
    require(data.status == "complete", "Clang success including unicode root");
    if (test == "identity") {
        const auto helpers = cg::find_symbols(data, "helper");
        require(helpers.size() == 2 && helpers[0].id != helpers[1].id, "same basename static USRs isolated");
        return;
    }
    require(cg::find_symbols(data, "overload").size() == 2, "overload symbols");
    const auto alpha = cg::find_symbols(data, "alpha");
    require(alpha.size() == 1 && alpha[0].definition && alpha[0].file == "a.cpp", "prefer cross-file definition");
    const auto metric = std::find_if(data.metrics.begin(), data.metrics.end(), [&](const auto& value) { return value.symbol_id == alpha[0].id; });
    require(metric != data.metrics.end() && metric->complexity == 3 && metric->parameters == 1, "CFG if and logical-and complexity");
    require(cg::project_graph(scan, "call").cycles.size() == 1, "mutual recursion");
    const auto includes = cg::project_graph(scan, "include");
    require(includes.cycles == std::vector<std::vector<std::string>>({{"a.h", "b.h"}}), "guarded include cycle");
    require(std::find(data.covered_files.begin(), data.covered_files.end(), "orphan.h") == data.covered_files.end(), "uncovered header not fabricated");
    require(data.units[0].indirect_calls == 1, "indirect call explicitly counted");
    require(cg::find_symbols(data, "Object").size() >= 2 && cg::find_symbols(data, "variable").size() == 1, "record/method/variable symbols");
}
int main(int argc, char** argv) {
    try { require(argc == 2, "test name"); run(argv[1]); }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
