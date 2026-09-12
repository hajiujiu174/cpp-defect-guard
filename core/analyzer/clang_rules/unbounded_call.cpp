#include "rule.hpp"
#include <clang/Basic/SourceManager.h>
namespace codeguard::clang_rules {
using namespace clang;
namespace {
void check(const Stmt* statement,const Context& c) {
    const auto* call=dyn_cast<CallExpr>(statement);if(!call)return;
    const auto* callee=call->getDirectCallee();
    if(!callee||!callee->getIdentifier()||callee->isCXXClassMember()||
        !(callee->getBuiltinID()||c.ast.getSourceManager().isInSystemHeader(callee->getLocation())))return;
    const auto name=callee->getNameAsString();
    if(name=="gets"||name=="strcpy"||name=="strcat"||name=="sprintf")
        c.emit(statement,"CG001","Unbounded library call: "+name,
            "Use a capacity-aware operation and check destination size; this is an API-risk warning, not proof of overflow.");
}
}
const Rule& unbounded_call(){static const Rule rule{"CG001",check};return rule;}
}
