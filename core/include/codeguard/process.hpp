#pragma once
#include "codeguard/scan_control.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
namespace codeguard {
struct ProcessOptions {
    std::vector<std::string> arguments; // executable + argv, never a shell command
    std::string working_directory;
    std::chrono::milliseconds timeout{120000};
    std::size_t capture_limit = 4 * 1024 * 1024; // per stream; excess is drained
    std::shared_ptr<ScanControl> control;
};
struct ProcessResult {
    std::string status = "start_failed"; // passed/failed/start_failed/timed_out/cancelled
    int exit_code = -1;
    std::int64_t duration_ms = 0;
    std::string stdout_text, stderr_text;
    bool output_truncated = false;
};
ProcessResult run_process(const ProcessOptions& options);
std::string executable_directory();
std::string format_arguments(const std::vector<std::string>& args); // display only
} // namespace codeguard
