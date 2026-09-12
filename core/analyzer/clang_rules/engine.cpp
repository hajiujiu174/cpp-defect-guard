#include "rule.hpp"
#include "codeguard/rules.hpp"
#include <algorithm>
#include <stdexcept>

namespace codeguard::clang_rules {
using namespace clang;
Engine::Engine(const std::vector<std::string>& disabled) {
    const Rule* registry[]{&unbounded_call(),&array_bounds(),&assignment_condition(),&local_lifetime(),&null_dereference()};
    for(const auto& id:disabled)(void)rule_info(id);
    for(const auto* rule:registry)if(std::find(disabled.begin(),disabled.end(),rule->id)==disabled.end())rules_.push_back(rule);
}
void Engine::inspect(const Stmt* statement,ASTContext& ast,const Emit& emit,bool address) const {
    if(rules_.empty()||!statement||isa<UnaryExprOrTypeTraitExpr>(statement)||isa<CXXNoexceptExpr>(statement)||isa<CXXTypeidExpr>(statement))return;
    if(const auto* lambda=dyn_cast<LambdaExpr>(statement)) {
        for(const auto* capture:lambda->capture_inits())inspect(capture,ast,emit);
        return; // call operator gets its own function callback
    }
    for(const auto* rule:rules_)rule->check(statement,{ast,emit,address});
    if(const auto* declaration=dyn_cast<DeclStmt>(statement)) {
        for(const auto* decl:declaration->decls())if(const auto* var=dyn_cast<VarDecl>(decl))inspect(var->getInit(),ast,emit);
        return; // do not attribute local class methods to the outer function
    }
    if(const auto* branch=dyn_cast<IfStmt>(statement);branch&&branch->isConstexpr()) {
        bool condition=false;
        if(!branch->getCond()->isValueDependent()&&branch->getCond()->EvaluateAsBooleanCondition(condition,ast)) {
            inspect(branch->getInit(),ast,emit);inspect(branch->getConditionVariableDeclStmt(),ast,emit);
            inspect(branch->getCond(),ast,emit);inspect(condition?branch->getThen():branch->getElse(),ast,emit);return;
        }
    }
    bool child_address=address&&(isa<ParenExpr>(statement)||isa<ImplicitCastExpr>(statement));
    if(const auto* unary=dyn_cast<UnaryOperator>(statement))child_address=unary->getOpcode()==UO_AddrOf;
    for(const auto* child:statement->children())inspect(child,ast,emit,child_address);
}
}
