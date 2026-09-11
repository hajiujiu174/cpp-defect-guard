#pragma once
#include "codeguard/process.hpp"
#include <filesystem>
#include <functional>
namespace codeguard {
struct BuildStep {
    std::string name, command, working_directory;
    ProcessResult result;
    int tests_total = -1, tests_failed = -1, tests_skipped = -1;
};
struct BuildRun {
    std::int64_t id = 0, scan_id = 0;
    std::string root, started_at, status, workspace, target, git_revision, git_log;
    bool source_unchanged = false;
    std::vector<BuildStep> steps;
};
struct BuildOptions {
    std::filesystem::path output_directory;
    std::string cmake = "cmake", ctest = "ctest", git = "git", generator = "Ninja";
    std::string c_compiler, cxx_compiler, target;
    std::vector<std::string> cmake_definitions; // KEY=VALUE; forwarded as individual -D arguments
    std::vector<std::string> copy_includes; // exact project-relative directories overriding default ignores
    unsigned jobs = 0;
    std::chrono::milliseconds timeout{120000}; // per stage
    std::shared_ptr<ScanControl> control;
    std::function<void(const std::string&)> progress;
};
// Configures, builds, and tests an isolated source copy; retains logs/artifacts.
// Runs are attached to an existing, source-matching scan, including failed/cancelled runs.
BuildRun build_and_test(const std::filesystem::path& root, const std::filesystem::path& database, const BuildOptions& options);
std::string read_git(const std::filesystem::path& root, const BuildOptions& options, std::string* revision = nullptr);
} // namespace codeguard
