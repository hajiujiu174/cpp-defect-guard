#pragma once
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>

namespace codeguard::clang_rules {
// An unparenthesized comparison converted to a non-bool local integer before
// assignment. Parenthesizing the RHS explicitly marks a deliberate boolean value.
inline const clang::BinaryOperator* assigned_comparison(const clang::BinaryOperator* assignment){
    using namespace clang;
    if(!assignment||assignment->getOpcode()!=BO_Assign)return nullptr;
    const auto* name=dyn_cast<DeclRefExpr>(assignment->getLHS()->IgnoreParenImpCasts());
    const auto* variable=name?dyn_cast<VarDecl>(name->getDecl()):nullptr;
    if(!variable||!variable->hasLocalStorage()||!variable->getType()->isIntegerType()||variable->getType()->isBooleanType()||variable->getType().isVolatileQualified())return nullptr;
    const auto* comparison=dyn_cast<BinaryOperator>(assignment->getRHS()->IgnoreImpCasts());
    return comparison&&comparison->isComparisonOp()&&comparison->getLHS()->getType()->isIntegerType()?comparison:nullptr;
}
}
