#pragma once
#include "codeguard/application.hpp"
#include <string>

namespace validation_test {
struct Wire {
    std::string value="CodeGuardValidationMetadata 1\n";
    void text(const std::string& s){value+=std::to_string(s.size())+":"+s;}
    template<class Number> void number(Number n){text(std::to_string(n));}
    void strings(const std::vector<std::string>& items){number(items.size());for(const auto& s:items)text(s);}
    void issue(const codeguard::Issue& i){text(i.rule_id);text(i.severity);text(i.file);number(i.line);number(i.column);text(i.message);text(i.evidence);text(i.suggestion);text(i.symbol_id);text(i.detector);text(i.suppression_reason);}
};
// This independent field-by-field wire format compares kept metadata, not the
// full ScanResult against a second call to the projection factory.
template<class Snapshot> std::string metadata(const Snapshot& s) {
    Wire w;w.number(s.id);w.text(s.root);w.text(s.scanned_at);w.text(s.inventory_policy);
    w.number(s.added);w.number(s.changed);w.number(s.unchanged);w.number(s.removed);
    w.number(s.files.size());for(const auto& f:s.files){w.text(f.path);w.text(f.language);w.text(f.hash);w.number(f.size);w.number(f.mtime);w.number(f.lines);}
    w.number(s.diagnostics.size());for(const auto& d:s.diagnostics){w.text(d.path);w.text(d.message);}
    const auto& a=s.analysis;w.text(a.status);w.text(a.compile_commands);w.text(a.configuration);w.text(a.analyzer_revision);w.number(a.workers);w.number(a.elapsed_ms);
    w.number(a.units.size());for(const auto& u:a.units){w.text(u.file);w.text(u.status);w.text(u.diagnostics);w.number(u.indirect_calls);w.text(u.command_fingerprint);w.strings(u.covered_files);}
    w.strings(a.covered_files);w.number(a.issues.size());for(const auto& i:a.issues)w.issue(i);
    w.number(a.suppressed_issues.size());for(const auto& i:a.suppressed_issues)w.issue(i);w.strings(a.rule_diagnostics);return w.value;
}
template<class Snapshot> std::uint64_t lines(const Snapshot& s){std::uint64_t n=0;for(const auto& f:s.files)n+=f.lines;return n;}
}
