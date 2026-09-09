#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "codeguard/analysis.hpp"
#include "codeguard/engineering.hpp"

namespace codeguard {
namespace fs = std::filesystem;

// DTOs and public APIs use only C++ standard library types, never Qt types.
struct FileRecord {
    std::string path;
    std::string language;
    std::string hash; // fnv1a64-v1: change detection, NOT security verification
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    std::uint64_t lines = 0; // physical lines, not AST-derived code lines
};
struct Diagnostic {
    std::string path;
    std::string message;
};
struct ScanResult {
    std::int64_t id = 0;
    std::string root;
    std::string scanned_at;
    std::vector<FileRecord> files;
    std::vector<Diagnostic> diagnostics;
    AnalysisResult analysis;
    std::vector<BuildRun> build_runs; // saved runs for this scan; populated on read
    std::uint64_t added = 0, changed = 0, unchanged = 0, removed = 0;
    std::uint64_t total_lines() const;
};
struct ProjectSummary {
    std::string root;
    std::string last_scan;
};
struct ScanOptions {
    ScanContext context;
    std::string scan_phase = "scanning";
    std::string compile_commands; // opt-in; empty means inventory only
    unsigned threads = 0; // 0=hardware concurrency (capped at 8); explicit 1..64
    // Exact directory basenames, case insensitive; no glob interpretation.
    std::vector<std::string> ignored_directories = {
        "build", "out", "dist", "artifacts", ".git", ".venv", ".venv-ml",
        "third_party", "third-party", "vendor", "external", "node_modules"};
};

std::string utf8_path(const fs::path& path);
fs::path from_utf8(const std::string& path);
ScanResult scan_project(const fs::path& root, const ScanOptions& options = {});

class IDatabase {
public:
    virtual ~IDatabase() = default;
    virtual ScanResult latest(const std::string& root) = 0;
    virtual std::vector<ProjectSummary> projects() = 0;
    virtual void save(ScanResult& result, const ScanContext& context = {}) = 0;
    virtual void save_build(BuildRun& run) = 0;
    virtual std::vector<BuildRun> builds(const std::string& root, bool include_logs = false, std::int64_t only_id = 0) = 0;
};

class SqliteDatabase final : public IDatabase {
public:
    explicit SqliteDatabase(const fs::path& path, bool read_only = false, const ScanContext& context = {});
    ~SqliteDatabase() override;
    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    ScanResult latest(const std::string& root) override;
    std::vector<ProjectSummary> projects() override;
    void save(ScanResult& result, const ScanContext& context = {}) override;
    void save_build(BuildRun& run) override;
    std::vector<BuildRun> builds(const std::string& root, bool include_logs = false, std::int64_t only_id = 0) override;
private:
    struct Impl;
    Impl* impl_;
};

// Scan source read-only; all database writes are outside the source tree.
ScanResult import_project(const fs::path& root, const fs::path& database,
                          const ScanOptions& options = {});
} // namespace codeguard
