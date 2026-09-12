#pragma once
#include <string>
#include <vector>
namespace codeguard {
struct RuleInfo { std::string id, severity, title, scope; };
struct Issue {
    std::string rule_id, severity, file;
    int line = 0, column = 0;
    std::string message, evidence, suggestion, symbol_id;
    std::string detector = "clang-ast";
    std::string suppression_reason = {};
};
struct RuleSuppression {
    std::string rule_id, file;
    int line=0, column=0;
    std::string source_hash, reason;
};
const std::vector<RuleInfo>& rule_catalog();
const RuleInfo& rule_info(const std::string& id);
struct ScanResult;
struct ProjectConfig;
RuleSuppression suppress_issue(const ScanResult& snapshot,const Issue& issue,const std::string& reason);
// Apply once to a fresh analyzer result before persistence. Re-scan to change policy;
// previously suppressed issues and policy diagnostics belong to the old snapshot.
void apply_rule_policy(ScanResult& result,const ProjectConfig& config);
} // namespace codeguard
