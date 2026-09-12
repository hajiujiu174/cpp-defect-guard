#include "rule.hpp"
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const Expr* condition=nullptr;
    if(const auto* s=dyn_cast<IfStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<WhileStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<DoStmt>(statement))condition=s->getCond();
    if(const auto* s=dyn_cast<ForStmt>(statement))condition=s->getCond();
    if(!condition||isa<ParenExpr>(condition->IgnoreImpCasts()))return;
    const auto* assignment=dyn_cast<BinaryOperator>(condition->IgnoreParenImpCasts());
    if(assignment&&assignment->isAssignmentOp())c.emit(condition,"CG003","Assignment directly controls a branch",
        "Use comparison if intended; otherwise parenthesize the deliberate assignment.");
}
}
const Rule& assignment_condition(){static const Rule rule{"CG003",check};return rule;}
}
