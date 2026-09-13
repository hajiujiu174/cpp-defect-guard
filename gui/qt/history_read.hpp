#pragma once
#include "codeguard/application.hpp"

struct HistoryRead {
    enum class Kind { project, builds, log } kind = Kind::project;
    std::string database, configuration_error;
    std::shared_ptr<codeguard::ScanResult> snapshot;
    std::optional<codeguard::ProjectConfig> configuration;
    std::vector<codeguard::BuildRun> runs;
    int row = 0, step = 0;
};

inline HistoryRead read_project(const std::string& database, std::string root, const codeguard::ScanContext& context) {
    codeguard::SqliteDatabase db(codeguard::from_utf8(database), true, context);
    if (root.empty()) {
        const auto projects = db.projects();
        if (projects.empty()) throw std::runtime_error("No saved projects");
        root = projects.front().root;
    }
    HistoryRead result; result.database = database;
    result.snapshot = std::make_shared<codeguard::ScanResult>(db.latest(root));
    if (!result.snapshot->id) throw std::runtime_error("Saved project snapshot not found");
    try {
        result.configuration = db.configuration(root);
        if (!result.configuration) {
            const auto& analysis = result.snapshot->analysis;
            if (!analysis.configuration.empty()) result.configuration = codeguard::decode_config(analysis.configuration);
            else {
                result.configuration = codeguard::ProjectConfig{};
                if (!analysis.compile_commands.empty()) {
                    result.configuration->compile_commands = analysis.compile_commands;
                    result.configuration->auto_discover = false;
                }
            }
        }
    } catch (const codeguard::ScanCancelled&) { throw; }
    catch (const std::exception& e) { result.configuration_error = e.what(); }
    context.check(); return result;
}
