#include "rule.hpp"
#include "condition_assignment.hpp"
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const Expr* condition=nullptr;
    if(const auto* s=dyn_cast<IfStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<WhileStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<DoStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<ForStmt>(statement))condition=s->getCond();
    if(!condition)return;
    std::vector<const Expr*> pending{condition};
    while(!pending.empty()){
        const auto* expression=pending.back();pending.pop_back();
        const auto* binary=dyn_cast<BinaryOperator>(expression->IgnoreParenImpCasts());if(!binary)continue;
        if(binary->isLogicalOp()){pending.push_back(binary->getRHS());pending.push_back(binary->getLHS());continue;}
        if(!binary->isAssignmentOp())continue;
        if(assigned_comparison(binary))c.emit(binary,"CG003","Comparison result is assigned inside a branch condition",
            "Review precedence: compare the assigned value, or parenthesize the RHS if assigning a boolean result is deliberate.");
        else if(!isa<ParenExpr>(expression->IgnoreImpCasts()))c.emit(binary,"CG003","Assignment directly controls a branch",
            "Use comparison if intended; otherwise parenthesize the deliberate assignment.");
    }
}
}
const Rule& assignment_condition(){static const Rule rule{"CG003",check};return rule;}
}
