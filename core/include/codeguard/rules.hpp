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
};
const std::vector<RuleInfo>& rule_catalog();
} // namespace codeguard
