#pragma once
#include "codeguard/process.hpp"
#include <filesystem>
#include <functional>
namespace codeguard {
struct SourceReplacement { std::string file,before,after; };
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
    std::string compile_commands, configuration;
};
struct BuildOptions {
    std::filesystem::path output_directory;
    std::string cmake = "cmake", ctest = "ctest", git = "git", generator = "Ninja";
    std::string c_compiler, cxx_compiler, target;
    std::string build_type = "Debug";
    bool configure_only = false;
    std::vector<std::string> cmake_definitions; // KEY=VALUE; forwarded as individual -D arguments
    std::vector<std::string> copy_includes; // exact project-relative directories overriding default ignores
    std::vector<std::string> copy_excludes;
    unsigned jobs = 0;
    std::chrono::milliseconds timeout{120000}; // per stage
    std::shared_ptr<ScanControl> control;
    std::function<void(const std::string&)> progress;
    // Transient, exact-byte replacements applied only after the ordinary
    // source-copy/snapshot checks. Never encoded as persistent build settings.
    std::vector<SourceReplacement> copy_replacements;
    bool defer_commit_fence=false; // used by a larger validation operation owning the control
    bool keep_unpatched_copy=false; // transient validation baseline, before any CMake/test execution
    // Optional repair validation instrumentation; not persistent project settings.
    bool collect_coverage=false;
    bool collect_test_coverage=false;
    unsigned coverage_test_limit=64; // explicit bounded replay; 1..256
    std::string gcov="gcov";
    std::function<void(const std::filesystem::path&)> before_test;
    // Transient observer/guard after copy + replacements, before any CMake runs.
    std::function<void(const std::filesystem::path&)> before_configure;
};
// Configures, builds, and tests an isolated source copy; retains logs/artifacts.
// Runs are attached to an existing, source-matching scan, including failed/cancelled runs.
BuildRun build_and_test(const std::filesystem::path& root, const std::filesystem::path& database, const BuildOptions& options);
// Git belongs to this source root only when it has its own .git directory/file.
// Missing/unverified metadata clears revision; ancestor checkouts are not used.
std::string read_git(const std::filesystem::path& root, const BuildOptions& options, std::string* revision = nullptr);
} // namespace codeguard
