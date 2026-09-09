#include "codeguard/application.hpp"
#include <sqlite3.h>
#include <barrier>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace cg = codeguard;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    cg::fs::path dir, root, db;
    Fixture() {
        dir = cg::fs::current_path() / ("cancel-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(cg::fs::create_directory(dir), "unique fixture"); root = dir / "source";
        cg::fs::create_directory(root); db = dir / "inventory.sqlite3";
        std::ofstream(root / "a.cpp") << "int a() { return 1; }\n";
        std::ofstream(root / "b.cpp") << "int b() { return 2; }\n";
        std::ofstream commands(dir / "compile_commands.json");
        commands << "[{\"directory\":\"" << cg::utf8_path(root) << "\",\"file\":\"a.cpp\",\"arguments\":[\"clang++\",\"-c\",\"a.cpp\"]},"
                 << "{\"directory\":\"" << cg::utf8_path(root) << "\",\"file\":\"b.cpp\",\"arguments\":[\"clang++\",\"-c\",\"b.cpp\"]}]";
    }
    ~Fixture() { std::error_code error; cg::fs::remove_all(dir, error); }
};
std::int64_t scans(const cg::fs::path& file) {
    sqlite3* db = nullptr; sqlite3_stmt* query = nullptr;
    require(sqlite3_open_v2(cg::utf8_path(file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "read database");
    sqlite3_prepare_v2(db, "SELECT count(*) FROM scan", -1, &query, nullptr); sqlite3_step(query);
    const auto result = sqlite3_column_int64(query, 0); sqlite3_finalize(query); sqlite3_close(db); return result;
}
void run(const std::string& mode) {
    if (mode == "cancel_fence") {
        for (int i = 0; i < 200; ++i) {
            cg::ScanControl control; std::barrier ready(3);
            bool accepted = false, committed = false;
            std::thread cancel([&]() { ready.arrive_and_wait(); accepted = control.request_cancel(); });
            std::thread commit([&]() { ready.arrive_and_wait(); try { control.begin_commit(); committed = true; } catch (const cg::ScanCancelled&) {} });
            ready.arrive_and_wait(); cancel.join(); commit.join();
            require(accepted != committed, "exactly one of cancellation and commit must win");
        }
        return;
    }
    Fixture fixture;
    const auto cwd = cg::fs::current_path();
    const auto baseline = cg::import_project(fixture.root, fixture.db);
    cg::ScanOptions options; options.context.control = std::make_shared<cg::ScanControl>();
    bool reached = false;
    auto target = fixture.db;
    if (mode == "cancel_new_transaction") target = fixture.dir / "new.sqlite3";
    if (mode == "cancel_new") { target = fixture.dir / "new.sqlite3"; options.context.control->request_cancel(); }
    else options.context.progress = [&](const cg::ScanProgress& progress) {
        require(cg::fs::current_path() == cwd, "background parser changed process working directory");
        const bool cancel = (mode == "cancel_scan" && progress.phase == "scanning" && progress.completed == 1)
            || ((mode == "cancel_transaction" || mode == "cancel_new_transaction") && progress.phase == "before_commit")
            || (mode == "cancel_analysis" && progress.phase == "analyzing" && progress.completed == 1);
        if (cancel) { reached = true; require(options.context.control->request_cancel(), "cancel must be accepted"); }
    };
    if (mode == "cancel_analysis") options.compile_commands = cg::utf8_path(fixture.dir);
    if (mode == "cancel_after_commit") {
        const auto result = cg::import_project(fixture.root, target, options);
        require(result.id > baseline.id && !options.context.control->request_cancel(), "committed result cannot be cancelled");
        require(scans(target) == 2, "one complete new scan"); return;
    }
    bool cancelled = false;
    try { cg::import_project(fixture.root, target, options); } catch (const cg::ScanCancelled&) { cancelled = true; }
    require(cancelled, "expected cancellation exception");
    if (mode == "cancel_new") require(!cg::fs::exists(target), "early cancellation must not create database");
    else require(reached, "cancellation boundary exercised");
    if (mode == "cancel_new_transaction") {
        cg::SqliteDatabase empty(target, true);
        require(empty.projects().empty() && scans(target) == 0, "cancelled first transaction left partial project/scan");
    }
    cg::SqliteDatabase db(fixture.db, true); const auto saved = db.latest(baseline.root);
    require(saved.id == baseline.id && saved.files.size() == baseline.files.size() && scans(fixture.db) == 1,
        "cancelled scan changed persisted snapshot/history");
    require(cg::fs::current_path() == cwd, "working directory not preserved");
}
int main(int argc, char** argv) {
    try { require(argc == 2, "mode required"); run(argv[1]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
