#include "codeguard/query.hpp"
#include "codeguard/application.hpp"
#include <iostream>
#include <functional>
#include <limits>

using namespace codeguard;
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
ScanResult fixture() {
    ScanResult s; s.id = 42; s.analysis.status = "complete";
    s.files = {{"a.cpp", "C++", "ha", 20, 0, 2}, {"b.cpp", "C++", "hb", 30, 0, 10}, {"中文/O'Brien.h", "C++", "hc", 40, 0, 10}};
    s.analysis.symbols = {{"a","function","alpha","a.cpp",1,1,true,false}, {"b","function","beta","b.cpp",2,1,true,false}, {"c","function","gamma","b.cpp",8,1,true,false}};
    s.analysis.metrics = {{"a",20,1,3},{"b",80,3,12},{"c",100,0,-1}};
    s.analysis.edges = {{"call","a","b","a.cpp",10,5}};
    return s;
}
std::string cell(const QueryResult& r, std::size_t row, std::size_t col = 0) { return query_value_text(r.rows.at(row).at(col)); }
void reject(const ScanResult& s, const std::string& q, const std::string& message) {
    try { execute_query(s,q); } catch (const QueryError& e) { check(std::string(e.what()).find(message) != std::string::npos, e.what()); return; }
    throw std::runtime_error("query accepted: " + q);
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2,"case required"); const std::string test = argv[1]; auto s = fixture();
        if (test == "pipeline") {
            auto ast = parse_query("SELECT name FROM functions WHERE complexity > 10 AND lines > 50 ORDER BY complexity DESC LIMIT 20;");
            check(ast.expressions.size() == 3 && ast.expressions[ast.predicate].kind == QueryExpression::Kind::conjunction,"AST structure");
            const auto r = execute_query(s,"SELECT name, file, complexity FROM functions WHERE complexity > 10 AND lines > 50 ORDER BY complexity DESC LIMIT 20;");
            check(r.rows.size() == 1 && cell(r,0) == "beta" && cell(r,0,2) == "12", "proposal query");
            check(r.scanned_rows == 3 && r.matched_rows == 1 && r.scan_id == 42 && r.plan.find("StableSort(complexity DESC)") != std::string::npos,"plan provenance");
            check(execute_query(s,"SELECT * FROM symbols WHERE definition = 1").rows.size() == 3,"symbols schema");
            check(cell(execute_query(s,"SELECT target FROM edges WHERE kind = 'call'"),0) == "b","edges schema");
        } else if (test == "precedence") {
            check(execute_query(s,"SELECT name FROM functions WHERE name = 'alpha' OR complexity > 10 AND lines > 90").rows.size() == 1,"AND priority");
            check(execute_query(s,"SELECT name FROM functions WHERE (name = 'alpha' OR complexity > 10) AND lines > 90").rows.empty(),"parentheses");
            check(execute_query(s,"SELECT name FROM functions WHERE complexity != -1 AND (lines <= 20 OR parameters >= 3)").rows.size() == 2,"operators");
            check(execute_query(s,"SELECT name FROM functions WHERE complexity <> 3 AND complexity < 12").rows.size() == 1,"not equal and less");
        } else if (test == "ordering") {
            auto r = execute_query(s,"select file from FILES order by lines DESC, size asc limit 2");
            check(r.rows.size() == 2 && cell(r,0) == "b.cpp" && cell(r,1) == "中文/O'Brien.h", "numeric multi-column order before projection");
            r = execute_query(s,"SELECT file FROM files ORDER BY lines DESC LIMIT 9223372036854775807");
            check(cell(r,0) == "b.cpp" && cell(r,1) == "中文/O'Brien.h" && cell(r,2) == "a.cpp", "stable ties, numeric not lexical order");
            check(execute_query(s,"SELECT * FROM files LIMIT 0").rows.empty(),"zero limit");
            check(cell(execute_query(s,"SELECT file FROM files WHERE file = '中文/O''Brien.h'"),0) == "中文/O'Brien.h","UTF-8 and quote escaping");
            check(execute_query(s,"SELECT * FROM files WHERE file = 'B.cpp'").rows.empty(),"case-sensitive data");
        } else if (test == "errors") {
            reject(s,"SELECT missing FROM files","unknown field");
            reject(s,"SELECT * FROM secrets","unknown table");
            reject(s,"SELECT * FROM files WHERE lines = '2'","type mismatch");
            reject(s,"SELECT * FROM files ORDER BY missing","unknown field");
            reject(s,"SELECT * FROM files LIMIT -1","non-negative");
            reject(s,"SELECT * FROM files LIMIT 9223372036854775808","64-bit");
            reject(s,"SELECT * FROM files WHERE lines > 1.5","unsupported character");
            reject(s,"SELECT * FROM files WHERE file = 'oops","unterminated");
            reject(s,"SELECT * FROM files WHERE (lines > 1","expected )");
            reject(s,"SELECT * FROM files WHERE lines == 1","expected integer");
            reject(s,"SELECT * FROM files; DELETE FROM files","only one SELECT");
            reject(s,"DELETE FROM files","expected select");
            reject(s,"SELECT * FROM files JOIN symbols","only one SELECT");
            reject(s,"SELECT * FROM files --comment","unsupported character");
            reject(s,"SELECT file, FROM files","expected from");
            reject(s,"SELECT * FROM files WHERE lines = 1 OR unknown = 2","unknown field");
            try { execute_query(s,"SELECT file\nFROM files\nWHERE lines = 'x'"); throw std::runtime_error("missing error"); }
            catch (const QueryError& e) { check(std::string(e.what()).find("line 3, byte column 7") != std::string::npos,"error location"); }
        } else if (test == "bounds") {
            reject(s,std::string(65537,' '),"64 KiB");
            reject(s,"SELECT * FROM files WHERE " + std::string(66,'(') + "lines = 1" + std::string(66,')'),"depth 64");
            std::string q = "SELECT * FROM files WHERE lines = 1";
            for (int i = 0; i < 130; ++i) q += " OR lines = 1";
            reject(s,q,"256 nodes");
            s.files[0].size = std::numeric_limits<std::uintmax_t>::max();
            bool failed = false;
            try { execute_query(s,"SELECT * FROM files"); } catch (const std::runtime_error&) { failed = true; }
            check(failed,"unsigned data range checked");
        } else if (test == "coverage") {
            s.analysis.status = "not_requested";
            check(execute_query(s,"SELECT * FROM files").rows.size() == 3,"inventory query needs no Clang");
            reject(s,"SELECT * FROM functions","requires a Clang");
            s.analysis.status = "partial";
            check(execute_query(s,"SELECT * FROM functions").analysis_status == "partial","partial preserved");
            s.files.clear(); reject(s,"SELECT * FROM files WHERE missing = 1","unknown field");
            reject(s,"SELECT * FROM files WHERE lines = '1'","type mismatch");
            auto r = execute_query(s,"SELECT file FROM files"); check(r.rows.empty() && r.columns.size() == 1,"empty schema");
        } else throw std::runtime_error("unknown case");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
