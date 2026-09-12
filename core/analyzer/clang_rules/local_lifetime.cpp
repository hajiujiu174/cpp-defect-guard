#include "rule.hpp"
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const auto* ret=dyn_cast<ReturnStmt>(statement);if(!ret)return;
    const Expr* expression=ret->getRetValue();if(!expression||!expression->getType()->isPointerType())return;
    expression=expression->IgnoreParenImpCasts();bool address_of=false;
    if(const auto* unary=dyn_cast<UnaryOperator>(expression);unary&&unary->getOpcode()==UO_AddrOf){expression=unary->getSubExpr()->IgnoreParenImpCasts();address_of=true;}
    if(const auto* ref=dyn_cast<DeclRefExpr>(expression)) {
        const auto* var=dyn_cast<VarDecl>(ref->getDecl());
        if(var&&var->hasLocalStorage()&&!var->getType()->isReferenceType()&&(address_of||var->getType()->isArrayType()))
            c.emit(statement,"CG004","Returning storage owned by automatic local "+var->getNameAsString(),
                "Return a value or use storage whose lifetime outlives the call.");
    }
}
}
const Rule& local_lifetime(){static const Rule rule{"CG004",check};return rule;}
}
