#include "codeguard/rules.hpp"
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
}
