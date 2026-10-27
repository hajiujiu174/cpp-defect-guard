#pragma once
#include "codeguard/rules.hpp"
#include "../analyzer/clang_rules/condition_assignment.hpp"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/Lex/Lexer.h>
#include <optional>

namespace codeguard {
inline std::vector<RepairHint> ast_repair_hints(const clang::Stmt* statement,clang::ASTContext& ast,const Issue& issue) {
    using namespace clang;auto& sources=ast.getSourceManager();
    struct Span { FileID file;unsigned begin,end;std::string text; };
    auto span=[&](const Stmt* s)->std::optional<Span>{
        auto begin=s->getBeginLoc(),end=s->getEndLoc();
        if(begin.isInvalid()||end.isInvalid()||begin.isMacroID()||end.isMacroID()||sources.getFileID(begin)!=sources.getFileID(end))return {};
        end=Lexer::getLocForEndOfToken(end,0,sources,ast.getLangOpts());if(end.isInvalid())return {};
        const auto a=sources.getFileOffset(begin),b=sources.getFileOffset(end);
        bool bad=false;const auto data=sources.getBufferData(sources.getFileID(begin),&bad);
        if(bad||b<a||b>data.size()||b-a>32768)return {};
        return Span{sources.getFileID(begin),a,b,data.substr(a,b-a).str()};
    };
    std::vector<RepairHint> result;const auto* expression=dyn_cast<Expr>(statement);if(!expression)return result;
    if(issue.rule_id=="CG003") {
        const auto* assignment=dyn_cast<BinaryOperator>(expression->IgnoreParenImpCasts());if(!assignment)return result;
        const auto original=span(assignment);if(!original)return result;
        auto add=[&](const std::string& recipe,const std::string& title,const std::string& risk,std::string after){
            result.push_back({issue.rule_id,issue.file,recipe,title,risk,issue.line,issue.column,original->begin,original->text,std::move(after)});
        };
        const auto op=assignment->getOperatorLoc();
        if(assignment->getOpcode()==BO_Assign&&!op.isMacroID()&&sources.getFileID(op)==original->file) {
            const auto offset=sources.getFileOffset(op)-original->begin;
            if(offset<original->text.size()&&original->text.substr(offset,1)=="="){
                auto after=original->text;after.replace(offset,1,"==");
                add("condition.compare","Replace assignment with equality comparison","Changes branch semantics and stops the assignment; review intended comparison and test coverage.",std::move(after));
            }
        }
        if(const auto* comparison=clang_rules::assigned_comparison(assignment)){
            const auto value=span(comparison->getLHS()),rhs=span(comparison);
            if(value&&rhs&&value->file==original->file&&rhs->file==original->file&&value->end>original->begin&&value->end<=original->end&&rhs->begin>=original->begin&&rhs->end==original->end){
                auto after=original->text;after.insert(value->end-original->begin,")");after.insert(0,"(");
                add("condition.assignment_result","Compare the assigned return value","Moves comparison after assignment; changes stored value and may change conversions or evaluation order. Review the intended error handling.",std::move(after));
                auto intent=original->text;intent.insert(rhs->end-original->begin,")");intent.insert(rhs->begin-original->begin,"(");
                add("condition.intent","Parenthesize deliberate boolean assignment","Preserves assignment of the comparison result; does not repair lost error codes.",std::move(intent));
            }
        }else add("condition.intent","Parenthesize deliberate assignment","Preserves assignment semantics and explicitly marks intent; removes this style warning without proving a behavioral defect was fixed.","("+original->text+")");
    } else if(issue.rule_id=="CG001") {
        const auto* call=dyn_cast<CallExpr>(expression);if(!call||call->getNumArgs()<2||!call->getDirectCallee()||call->getDirectCallee()->getNameAsString()!="sprintf")return result;
        const auto* function=dyn_cast<DeclRefExpr>(call->getCallee()->IgnoreParenImpCasts());
        const auto* destination=dyn_cast<DeclRefExpr>(call->getArg(0)->IgnoreParenImpCasts());
        const auto* var=destination?dyn_cast<VarDecl>(destination->getDecl()):nullptr;
        const auto* array=var?ast.getAsConstantArrayType(var->getType()):nullptr;
        if(!function||!var||!var->hasLocalStorage()||!array||array->getSize().isZero()||array->getElementType().isVolatileQualified()
            ||!ast.hasSameType(array->getElementType().getUnqualifiedType(),ast.CharTy))return result;
        const auto original=span(call),name=span(function),dest=span(destination),argument=span(call->getArg(0));
        if(!original||!name||!dest||!argument||name->file!=original->file||dest->file!=original->file||argument->file!=original->file)return result;
        const auto at=function->getLocation();if(at.isMacroID()||sources.getFileID(at)!=original->file)return result;
        const auto position=sources.getFileOffset(at);
        if(position<original->begin||position+7>original->end||argument->end>original->end||argument->end<=position+7)return result;
        auto after=original->text;const auto relative=position-original->begin;
        if(after.substr(relative,7)!="sprintf")return result;
        after.insert(argument->end-original->begin,", sizeof("+dest->text+")");after.replace(relative,7,"snprintf");
        result.push_back({issue.rule_id,issue.file,"printf.bound","Bound formatted output by the local character array capacity",
            "May truncate output and changes return/error handling; requires snprintf to be declared. A passing build/test is not proof of correct behavior.",
            issue.line,issue.column,original->begin,original->text,std::move(after)});
    }
    return result;
}
}
