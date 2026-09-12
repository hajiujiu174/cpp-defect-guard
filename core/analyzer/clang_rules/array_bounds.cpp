#include "rule.hpp"
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const auto* access=dyn_cast<ArraySubscriptExpr>(statement);if(!access)return;
    const auto* array=c.ast.getAsConstantArrayType(access->getBase()->IgnoreParenImpCasts()->getType());Expr::EvalResult value;
    if(!array||access->getIdx()->isValueDependent()||!access->getIdx()->EvaluateAsInt(value,c.ast))return;
    const auto& index=value.Val.getInt();const auto size=array->getSize().getLimitedValue();
    if(index.isNegative()||index.getLimitedValue()>size||(!c.address_only&&index.getLimitedValue()==size))
        c.emit(statement,"CG002","Constant index is outside the array extent "+std::to_string(size),
            "Keep evaluated indices in [0, size); one-past address formation is permitted.");
}
}
const Rule& array_bounds(){static const Rule rule{"CG002",check};return rule;}
}
