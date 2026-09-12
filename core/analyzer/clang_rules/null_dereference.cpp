#include "rule.hpp"
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const Expr* pointer=nullptr;
    if(const auto* unary=dyn_cast<UnaryOperator>(statement);unary&&unary->getOpcode()==UO_Deref&&!c.address_only)pointer=unary->getSubExpr();
    if(const auto* member=dyn_cast<MemberExpr>(statement);member&&member->isArrow())pointer=member->getBase();
    if(pointer&&pointer->getType()->isPointerType()&&!pointer->isValueDependent()&&
        pointer->IgnoreParenCasts()->isNullPointerConstant(c.ast,Expr::NPC_ValueDependentIsNotNull)!=Expr::NPCK_NotNull)
        c.emit(statement,"CG005","Dereference of a constant null pointer","Provide a valid object before dereferencing this pointer.");
}
}
const Rule& null_dereference(){static const Rule rule{"CG005",check};return rule;}
}
