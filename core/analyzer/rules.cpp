#include "codeguard/rules.hpp"
#include "codeguard/application.hpp"
#include <algorithm>
#include <stdexcept>
namespace codeguard {
const std::vector<RuleInfo>& rule_catalog() {
    static const std::vector<RuleInfo> rules = {
        {"CG001", "warning", "Unbounded C library call", "Resolved library gets/strcpy/strcat/sprintf calls; review capacity."},
        {"CG002", "error", "Constant array index out of bounds", "Fixed arrays and compile-time integer indices."},
        {"CG003", "warning", "Assignment used as a condition", "Unparenthesized assignment in if/while/do/for."},
        {"CG004", "error", "Return address of automatic local", "Direct address-of/array decay of an automatic local; excludes references and static storage."},
        {"CG005", "error", "Constant null dereference", "Dereference/arrow on an expression provably a null pointer constant; no alias/path inference."}
    };
    return rules;
}
const RuleInfo& rule_info(const std::string& id) {
    const auto& rules=rule_catalog();const auto found=std::find_if(rules.begin(),rules.end(),[&](const auto& rule){return rule.id==id;});
    if(found==rules.end())throw std::invalid_argument("unknown rule: "+id);return *found;
}
RuleSuppression suppress_issue(const ScanResult& snapshot,const Issue& issue,const std::string& reason) {
    const auto file=std::find_if(snapshot.files.begin(),snapshot.files.end(),[&](const auto& f){return f.path==issue.file;});
    if(file==snapshot.files.end())throw std::invalid_argument("issue is outside saved inventory");
    const auto found=std::find_if(snapshot.analysis.issues.begin(),snapshot.analysis.issues.end(),[&](const auto& i){return i.rule_id==issue.rule_id&&i.file==issue.file&&i.line==issue.line&&i.column==issue.column;});
    if(found==snapshot.analysis.issues.end())throw std::invalid_argument("select an active issue from the saved snapshot");
    RuleSuppression result{issue.rule_id,issue.file,issue.line,issue.column,file->hash,reason};
    ProjectConfig config;config.suppressions={result};validate_config(config);return result;
}
void apply_rule_policy(ScanResult& result,const ProjectConfig& config) {
    validate_config(config);
    std::vector<bool> used(config.suppressions.size());
    std::vector<Issue> active;
    for(auto issue:result.analysis.issues) {
        if(const auto it=config.rule_severities.find(issue.rule_id);it!=config.rule_severities.end())issue.severity=it->second;
        for(std::size_t n=0;n<config.suppressions.size();++n) {
            const auto& s=config.suppressions[n];
            if(s.rule_id!=issue.rule_id||s.file!=issue.file||s.line!=issue.line||s.column!=issue.column)continue;
            const auto file=std::find_if(result.files.begin(),result.files.end(),[&](const auto& f){return f.path==s.file&&f.hash==s.source_hash;});
            if(file!=result.files.end()){issue.suppression_reason=s.reason;used[n]=true;break;}
        }
        if(issue.suppression_reason.empty())active.push_back(std::move(issue));else result.analysis.suppressed_issues.push_back(std::move(issue));
    }
    result.analysis.issues=std::move(active);
    for(std::size_t n=0;n<used.size();++n)if(!used[n]){
        const auto& s=config.suppressions[n];
        const auto file=std::find_if(result.files.begin(),result.files.end(),[&](const auto& f){return f.path==s.file;});
        std::string reason;
        if(file==result.files.end())reason="file is missing or excluded";
        else if(file->hash!=s.source_hash)reason="source changed; review the issue again";
        else if(std::find(config.disabled_rules.begin(),config.disabled_rules.end(),s.rule_id)!=config.disabled_rules.end())reason="rule disabled";
        else reason="no matching issue; check parse coverage, active configuration or whether the issue was fixed";
        result.analysis.rule_diagnostics.push_back("Suppression not applied: "+s.rule_id+" "+s.file+":"+std::to_string(s.line)+":"+std::to_string(s.column)+"; "+reason);
    }
}
}
