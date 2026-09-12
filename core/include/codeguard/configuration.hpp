#pragma once
#include "codeguard/engineering.hpp"
#include "codeguard/rules.hpp"
#include <map>
#include <optional>

namespace codeguard {
struct ProjectConfig {
    bool analysis_enabled = true;
    bool auto_discover = true;
    std::string compile_commands;
    unsigned threads = 0;
    BuildOptions build;
    std::vector<std::string> disabled_rules;
    std::map<std::string,std::string> rule_severities;
    std::vector<RuleSuppression> suppressions;
    // Canonical file path -> full command fingerprint. Stale selections never fall back.
    std::map<std::string, std::string> command_choices;
};
struct ScanOptions;
ScanOptions configured_scan_options(const std::filesystem::path& root, const ProjectConfig& config);
void validate_config(const ProjectConfig& config);
std::string encode_config(const ProjectConfig& config);
ProjectConfig decode_config(const std::string& payload);
std::optional<ProjectConfig> load_project_config(const std::filesystem::path& root, const std::filesystem::path& database);
void save_project_config(const std::filesystem::path& root, const std::filesystem::path& database, const ProjectConfig& config);
// Bounded search: root plus directories at depth <= 3, never follows symlinks.
std::vector<std::string> discover_compilation_databases(const std::filesystem::path& root);
bool project_path_inside(const std::filesystem::path& path, const std::filesystem::path& root);
void validate_relative_directory(const std::string& value);
} // namespace codeguard
