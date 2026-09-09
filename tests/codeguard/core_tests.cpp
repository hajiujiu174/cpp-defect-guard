#include "codeguard/application.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg = codeguard;
namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void throws(Function action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "expected rejection");
}
struct Fixture {
    fs::path directory, root, database;
    Fixture() {
        directory = fs::current_path() / ("codeguard-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(directory), "unique fixture creation failed");
        root = directory / "source";
        fs::create_directory(root);
        database = directory / "inventory.sqlite3";
    }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
    void write(const fs::path& name, const std::string& content) {
        fs::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary); out << content;
        require(bool(out), "fixture write failed");
    }
};
void run(const std::string& test) {
    Fixture fixture;
    auto import = [&]() { return cg::import_project(fixture.root, fixture.database); };
    if (test == "scan") {
        fixture.write("main.cpp", "int main() {\nreturn 0;\n}");
        fixture.write("include/a.HPP", "#pragma once\r\n");
        fixture.write("notes.txt", "ignored");
        const auto result = cg::scan_project(fixture.root);
        require(result.files.size() == 2 && result.total_lines() == 4, "scan counts");
        require(result.files[0].path == "include/a.HPP" && result.files[0].language == "header", "deterministic order and language");
    } else if (test == "empty") {
        fixture.write("empty.c", "");
        auto result = import();
        require(result.files[0].lines == 0 && result.files[0].hash == "fnv1a64-v1:cbf29ce484222325", "empty hash/line count");
    } else if (test == "ignore") {
        fixture.write("build/a.cpp", "x"); fixture.write("vendor/a.c", "x");
        fixture.write("cmake-build-debug/b.cpp", "x"); fixture.write("skip/c.c", "x");
        cg::ScanOptions options; options.ignored_directories.push_back("skip");
        require(cg::scan_project(fixture.root, options).files.empty(), "ignore directories");
    } else if (test == "missing") {
        throws([&]() { cg::scan_project(fixture.root / "absent"); });
        require(!fs::exists(fixture.database), "read-only scanner");
    } else if (test == "persistence") {
        fixture.write("a.c", "x\n");
        const auto first = import(), second = import();
        require(first.added == 1 && second.unchanged == 1 && second.id > first.id, "rescan");
        cg::SqliteDatabase db(fixture.database, true);
        const auto saved = db.latest(first.root);
        require(saved.id == second.id && saved.files[0].hash == first.files[0].hash && db.projects().size() == 1, "persist/reopen");
    } else if (test == "changes") {
        fixture.write("a.c", "a"); fixture.write("remove.c", "r"); import();
        const auto stamp = fs::last_write_time(fixture.root / "a.c");
        fixture.write("a.c", "b"); fs::last_write_time(fixture.root / "a.c", stamp);
        fs::remove(fixture.root / "remove.c"); fixture.write("new.c", "n");
        const auto result = import();
        require(result.added == 1 && result.changed == 1 && result.removed == 1, "hash catches same size and mtime");
    } else if (test == "rollback") {
        fixture.write("a.c", "a"); auto result = import();
        const auto previous = result.id;
        cg::SqliteDatabase db(fixture.database);
        result.files.push_back(result.files[0]);
        throws([&]() { db.save(result); });
        require(db.latest(result.root).id == previous, "transaction rollback");
        result.files.pop_back(); result.diagnostics.push_back({"a", "failed"});
        throws([&]() { db.save(result); });
        require(db.latest(result.root).id == previous, "partial scan rejected");
    } else if (test == "readonly") {
        throws([&]() { cg::SqliteDatabase db(fixture.database, true); });
        require(!fs::exists(fixture.database), "read-only open created file");
        auto result = import(); cg::SqliteDatabase db(fixture.database, true);
        throws([&]() { db.save(result); });
    } else if (test == "foreign_db") {
        sqlite3* db = nullptr;
        require(sqlite3_open(cg::utf8_path(fixture.database).c_str(), &db) == SQLITE_OK, "fixture sqlite open");
        sqlite3_exec(db, "CREATE TABLE legacy(value TEXT)", nullptr, nullptr, nullptr); sqlite3_close(db);
        throws([&]() { cg::SqliteDatabase other(fixture.database); });
    } else if (test == "source_protection") {
        fixture.write("a.c", "a");
        throws([&]() { cg::import_project(fixture.root, fixture.root / "result.sqlite3"); });
        require(!fs::exists(fixture.root / "result.sqlite3"), "database created inside source");
        const auto before = cg::scan_project(fixture.root); import();
        require(cg::scan_project(fixture.root).files[0].hash == before.files[0].hash, "source changed");
    } else if (test == "unicode") {
        const auto name = fs::path(u8"中文 空格'文件.cpp");
        fixture.write(name, "int x;\n"); const auto result = import();
        cg::SqliteDatabase db(fixture.database, true);
        require(db.latest(result.root).files[0].path == cg::utf8_path(name), "unicode and bound SQL string roundtrip");
    } else if (test == "symlink") {
        fixture.write("real.c", "a");
        std::error_code error;
        fs::create_directory_symlink(fixture.root, fixture.root / "loop", error);
        // Some Windows hosts deny symlink creation. Report this explicitly.
        if (error) { std::cout << "SKIP: symlink privilege unavailable\n"; return; }
        require(cg::scan_project(fixture.root).files.size() == 1, "symlink recursion");
    } else throw std::invalid_argument("unknown test");
}
int main(int argc, char** argv) {
    try { require(argc == 2, "test name required"); run(argv[1]); }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
