#pragma once
// Internal Clang adapter; no Clang types enter the public Core DTO API.
#include <clang/AST/ASTContext.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <functional>
#include <string>
#include <vector>

namespace codeguard::clang_rules {
using Emit = std::function<void(const clang::Stmt*,const std::string&,const std::string&,const std::string&)>;
struct Context {
    clang::ASTContext& ast;
    const Emit& emit;
    bool address_only;
};
struct Rule {
    const char* id;
    void (*check)(const clang::Stmt*,const Context&);
};
const Rule& unbounded_call();
const Rule& array_bounds();
const Rule& assignment_condition();
const Rule& local_lifetime();
const Rule& null_dereference();
// One dispatcher per TU; disabled checks are never invoked.
class Engine {
public:
    explicit Engine(const std::vector<std::string>& disabled);
    void inspect(const clang::Stmt* statement,clang::ASTContext& ast,const Emit& emit,bool address_only=false) const;
private:
    std::vector<const Rule*> rules_;
};
}
