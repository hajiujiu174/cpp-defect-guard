#include "codeguard/application.hpp"
#include "codeguard/thread_pool.hpp"

#include <map>
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <cwctype>
#endif

namespace codeguard {
ScanResult import_project(const fs::path& input, const fs::path& database, const ScanOptions& options) try {
    options.context.report("preparing");
    auto recorded=options.configuration.empty()?ProjectConfig{}:decode_config(options.configuration);
    recorded.analysis_enabled=!options.compile_commands.empty();recorded.auto_discover=false;
    recorded.compile_commands=options.compile_commands;recorded.threads=options.threads;recorded.command_choices=options.command_choices;
    recorded.disabled_rules=options.disabled_rules;recorded.build.copy_excludes=options.ignored_paths;
    recorded.rule_severities=options.rule_severities;recorded.suppressions=options.suppressions;validate_config(recorded);
    const auto root = fs::canonical(input);
    if (!options.compile_commands.empty() && !clang_analysis_available())
        throw std::invalid_argument("Clang analysis is disabled; rebuild with CODEGUARD_ENABLE_ANALYSIS=ON");
    const auto target = fs::weakly_canonical(database);
    auto root_part = root.begin();
    auto target_part = target.begin();
    for (; root_part != root.end() && target_part != target.end(); ++root_part, ++target_part) {
#ifdef _WIN32
        auto a = root_part->wstring(), b = target_part->wstring();
        for (auto& c : a) c = static_cast<wchar_t>(std::towlower(c));
        for (auto& c : b) c = static_cast<wchar_t>(std::towlower(c));
        if (a != b) break;
#else
        if (*root_part != *target_part) break;
#endif
    }
    if (root_part == root.end())
        throw std::invalid_argument("database must be outside the analyzed source directory");
    auto result = scan_project(root, options);
    ScanResult previous;
    if (fs::exists(target)) {
        SqliteDatabase db(target, true, options.context);
        previous = db.latest(result.root);
    }
    options.context.check();
    std::map<std::string, FileRecord> old;
    for (const auto& file : previous.files) old.emplace(file.path, file);
    for (const auto& file : result.files) {
        options.context.check();
        const auto found = old.find(file.path);
        if (found == old.end()) ++result.added;
        else {
            const auto& before = found->second;
            if (before.hash == file.hash && before.size == file.size && before.mtime == file.mtime)
                ++result.unchanged;
            else ++result.changed;
            old.erase(found);
        }
    }
    // Incomplete scans cannot distinguish deleted files from unreadable files.
    // Keep the last complete inventory and do not publish misleading removal counts.
    if (!result.diagnostics.empty()) return result;
    result.removed = old.size();
    if (!options.compile_commands.empty()) {
        result.analysis = analyze_project(result, options.compile_commands, options.context, options.threads, options.command_choices,options.disabled_rules);
        // Do not attach analysis to a different source snapshot if files changed meanwhile.
        auto verification = options; verification.scan_phase = "verifying";
        const auto after = scan_project(root, verification);
        bool stable = after.diagnostics.empty() && after.files.size() == result.files.size();
        for (std::size_t i = 0; stable && i < after.files.size(); ++i)
            stable = after.files[i].path == result.files[i].path && after.files[i].hash == result.files[i].hash
                && after.files[i].mtime == result.files[i].mtime && after.files[i].size == result.files[i].size;
        if (!stable) {
            result.diagnostics.push_back({result.root, "source changed during analysis; snapshot not saved"});
            return result;
        }
    }
    options.context.report("saving");
    apply_rule_policy(result,recorded);
    result.analysis.configuration=encode_config(recorded);
    // Only this dedicated writer owns a writable SQLite connection. Worker
    // results are merged first, then queued as one atomic snapshot transaction.
    ThreadPool writer(1, 1);
    writer.submit([&] {
        SqliteDatabase db(target, false, options.context);
        db.save(result, options.context);
    }).get();
    return result;
} catch (...) {
    // SQLite returns SQLITE_BUSY when cooperative lock waiting is cancelled.
    // Keep the public cancellation contract independent of the backend error.
    options.context.check();
    throw;
}
} // namespace codeguard
