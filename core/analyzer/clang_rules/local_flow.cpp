#include "rule.hpp"
#include <clang/AST/RecursiveASTVisitor.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace codeguard::clang_rules {
namespace {
using namespace clang;
struct Range { std::int64_t low,high; bool operator==(const Range&) const = default; };
enum class Pointer { unknown, null, nonnull };
struct Value { std::optional<Range> range; Pointer pointer=Pointer::unknown; };
struct State { std::map<const VarDecl*,Value> values; bool alive=true; };
const VarDecl* variable(const Expr* e) {
    if(!e)return nullptr;
    const auto* use=dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts());
    return use?dyn_cast<VarDecl>(use->getDecl()):nullptr;
}
class Variables final : public RecursiveASTVisitor<Variables> {
public:
    std::set<const VarDecl*> found;
    bool VisitDeclRefExpr(DeclRefExpr* e){if(const auto* v=dyn_cast<VarDecl>(e->getDecl()))found.insert(v);return true;}
};
// Exclude variables with storage reachable through aliases for the whole
// function, even if a later direct assignment would otherwise restore a fact.
class Escapes final : public RecursiveASTVisitor<Escapes> {
    void exclude(const Expr* e){Variables vars;vars.TraverseStmt(const_cast<Expr*>(e));excluded.insert(vars.found.begin(),vars.found.end());}
public:
    std::set<const VarDecl*> excluded;
    bool unsupported=false;
    bool TraverseFunctionDecl(FunctionDecl*){return true;}
    bool VisitUnaryOperator(UnaryOperator* e){
        if(e->getOpcode()==UO_AddrOf){
            const auto* operand=e->getSubExpr()->IgnoreParenImpCasts();
            // &a[i] and &p->field expose the selected object's storage, not the
            // storage of the scalar index or the pointer used to reach it.
            if(!isa<ArraySubscriptExpr,MemberExpr>(operand))exclude(operand);
        }return true;
    }
    bool VisitVarDecl(VarDecl* v){
        // Aggregates can bind reference fields without a CXXConstructExpr.
        // Their destructors may later write through those fields at scope exit.
        if(v->getType()->isReferenceType()||v->getType()->isRecordType())exclude(v->getInit());return true;
    }
    bool VisitInitListExpr(InitListExpr* e){if(!e->getType().isNull()&&e->getType()->isRecordType())exclude(e);return true;}
    bool VisitLambdaExpr(LambdaExpr* e){for(const auto& c:e->captures())if(c.capturesVariable()&&c.getCaptureKind()==LCK_ByRef)if(const auto* v=dyn_cast<VarDecl>(c.getCapturedVar()))excluded.insert(v);return true;}
    bool VisitCallExpr(CallExpr* e){
        const auto* f=e->getDirectCallee();
        for(unsigned i=0;i<e->getNumArgs();++i)if(!f||i>=f->getNumParams()||f->getParamDecl(i)->getType()->isReferenceType())exclude(e->getArg(i));
        return true;
    }
    bool VisitCXXConstructExpr(CXXConstructExpr* e){
        const auto* f=e->getConstructor();
        for(unsigned i=0;i<e->getNumArgs();++i)if(!f||i>=f->getNumParams()||f->getParamDecl(i)->getType()->isReferenceType())exclude(e->getArg(i));
        return true;
    }
    bool VisitStmt(Stmt* s){if(isa<GotoStmt,IndirectGotoStmt,LabelStmt,AsmStmt,CoreturnStmt,CoroutineBodyStmt>(s))unsupported=true;return true;}
};
class Writes final : public RecursiveASTVisitor<Writes> {
public:
    std::set<const VarDecl*> vars;
    bool unknown=false;
    bool TraverseLambdaExpr(LambdaExpr*){return true;}
    bool TraverseFunctionDecl(FunctionDecl*){return true;}
    bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr*){return true;}
    bool TraverseCXXNoexceptExpr(CXXNoexceptExpr*){return true;}
    bool TraverseCXXTypeidExpr(CXXTypeidExpr*){return true;}
    bool VisitBinaryOperator(BinaryOperator* e){if(e->isAssignmentOp()){if(auto* v=variable(e->getLHS()))vars.insert(v);else unknown=true;}return true;}
    bool VisitUnaryOperator(UnaryOperator* e){if(e->isIncrementDecrementOp()){if(auto* v=variable(e->getSubExpr()))vars.insert(v);else unknown=true;}return true;}
    bool VisitCallExpr(CallExpr*){unknown=true;return true;}
    bool VisitCXXConstructExpr(CXXConstructExpr*){unknown=true;return true;}
};
class Flow {
    ASTContext& ast;
    const Emit& emit;
    bool arrays,nulls;
    Escapes escapes;
    unsigned steps=0;
    // Bound pathological AST traversal without manufacturing proofs. The
    // syntactic rules still run independently when this budget is exhausted.
    static constexpr unsigned limit=100000;
    std::optional<Range> type_range(QualType type) const {
        if(type.isNull()||!type->isIntegerType()||type->isEnumeralType())return {};
        const auto bits=ast.getIntWidth(type);if(bits>32)return {};
        if(type->isBooleanType())return Range{0,1};
        if(type->isUnsignedIntegerType())return Range{0,(std::int64_t{1}<<bits)-1};
        return Range{-(std::int64_t{1}<<(bits-1)),(std::int64_t{1}<<(bits-1))-1};
    }
    bool tracked(const VarDecl* v) const {
        return v&&v->hasLocalStorage()&&!v->getType().isVolatileQualified()&&!v->getType()->isReferenceType()&&!escapes.excluded.contains(v)
            &&(v->getType()->isPointerType()||type_range(v->getType()).has_value());
    }
    Value unknown(QualType type)const{return {type_range(type),Pointer::unknown};}
    Value get(const VarDecl* v,const State& s)const {
        if(!v)return {};
        const auto found=s.values.find(v);return tracked(v)&&found!=s.values.end()?found->second:unknown(v->getType());
    }
    Value convert(Value value,QualType type)const {
        if(type->isPointerType())return {{},value.pointer};
        const auto bounds=type_range(type);if(!bounds)return {};
        if(!value.range||value.range->low<bounds->low||value.range->high>bounds->high)return {*bounds,Pointer::unknown};
        return {value.range,Pointer::unknown};
    }
    void assign(const VarDecl* v,Value value,State& s) {
        if(tracked(v))s.values[v]=convert(value,v->getType());
    }
    State join(const State& a,const State& b)const {
        if(!a.alive)return b;if(!b.alive)return a;State result;
        std::set<const VarDecl*> vars;for(const auto& [v,x]:a.values)vars.insert(v);for(const auto& [v,x]:b.values)vars.insert(v);
        for(const auto* v:vars){const auto x=get(v,a),y=get(v,b);Value value;
            if(x.range&&y.range)value.range=Range{std::min(x.range->low,y.range->low),std::max(x.range->high,y.range->high)};
            if(x.pointer==y.pointer)value.pointer=x.pointer;
            result.values[v]=value;
        }return result;
    }
    void havoc(const Stmt* stmt,State& s) {
        Writes writes;writes.TraverseStmt(const_cast<Stmt*>(stmt));
        if(writes.unknown)s.values.clear();else for(const auto* v:writes.vars)s.values.erase(v);
    }
    std::optional<std::int64_t> constant(const Expr* e)const {
        if(!e||e->getType().isNull()||e->isValueDependent()||!e->getType()->isIntegerType()||e->HasSideEffects(ast))return {};
        Expr::EvalResult result;if(!e->EvaluateAsInt(result,ast))return {};
        const auto& n=result.Val.getInt();if(n.isSigned()?!n.isSignedIntN(63):!n.isIntN(62))return {};
        return n.isSigned()?n.getSExtValue():static_cast<std::int64_t>(n.getZExtValue());
    }
    bool null_constant(const Expr* e)const {
        return e&&!e->getType().isNull()&&!e->isValueDependent()&&e->isNullPointerConstant(ast,Expr::NPC_ValueDependentIsNotNull)!=Expr::NPCK_NotNull;
    }
    std::optional<bool> truth(const Expr* e,const State& s)const {
        if(const auto c=constant(e))return *c!=0;
        const auto* v=variable(e);if(!tracked(v))return {};
        const auto value=get(v,s);
        if(v->getType()->isPointerType()){
            if(value.pointer==Pointer::null)return false;if(value.pointer==Pointer::nonnull)return true;
        }
        if(value.range){if(value.range->low==0&&value.range->high==0)return false;if(value.range->high<0||value.range->low>0)return true;}
        return {};
    }
    void narrow(const Expr* input,bool yes,State& s) {
        if(!input||!s.alive)return;
        const auto* e=input->IgnoreParenImpCasts();
        if(e->HasSideEffects(ast))return;
        if(const auto known=truth(e,s);known&&*known!=yes){s.alive=false;return;}
        if(const auto* unary=dyn_cast<UnaryOperator>(e);unary&&unary->getOpcode()==UO_LNot){narrow(unary->getSubExpr(),!yes,s);return;}
        if(const auto* binary=dyn_cast<BinaryOperator>(e)){
            const auto op=binary->getOpcode();
            if((op==BO_LAnd&&yes)||(op==BO_LOr&&!yes)){narrow(binary->getLHS(),yes,s);narrow(binary->getRHS(),yes,s);return;}
            if(op==BO_LAnd||op==BO_LOr){auto a=s,b=s;narrow(binary->getLHS(),yes,a);narrow(binary->getLHS(),!yes,b);narrow(binary->getRHS(),yes,b);s=join(a,b);return;}
            const VarDecl* v=variable(binary->getLHS());const Expr* rhs=binary->getRHS();auto comparison=op;
            if(!tracked(v)){v=variable(binary->getRHS());rhs=binary->getLHS();if(op==BO_LT)comparison=BO_GT;else if(op==BO_LE)comparison=BO_GE;else if(op==BO_GT)comparison=BO_LT;else if(op==BO_GE)comparison=BO_LE;}
            if(!tracked(v))return;
            if(v->getType()->isPointerType()&&null_constant(rhs)&&(comparison==BO_EQ||comparison==BO_NE)){
                const auto p=(yes==(comparison==BO_EQ))?Pointer::null:Pointer::nonnull;
                const auto old=get(v,s).pointer;if(old!=Pointer::unknown&&old!=p)s.alive=false;else s.values[v]={{},p};return;
            }
            // Only compare in the variable's original integer domain. Mixed
            // signedness or narrowing promotions may not preserve ordering.
            const Expr* operand=variable(binary->getLHS())==v?binary->getLHS():binary->getRHS();
            if(!ast.hasSameType(operand->getType().getUnqualifiedType(),v->getType().getUnqualifiedType()))return;
            const auto c=constant(rhs);auto value=get(v,s);if(!c||!value.range||*c<-4294967296LL||*c>4294967296LL)return;
            if(!yes){switch(comparison){case BO_EQ:comparison=BO_NE;break;case BO_NE:comparison=BO_EQ;break;case BO_LT:comparison=BO_GE;break;case BO_LE:comparison=BO_GT;break;case BO_GT:comparison=BO_LE;break;case BO_GE:comparison=BO_LT;break;default:return;}}
            auto& r=*value.range;
            switch(comparison){case BO_EQ:r.low=std::max(r.low,*c);r.high=std::min(r.high,*c);break;
                case BO_NE:if(r.low==*c)++r.low;else if(r.high==*c)--r.high;break;
                case BO_LT:r.high=std::min(r.high,*c-1);break;case BO_LE:r.high=std::min(r.high,*c);break;
                case BO_GT:r.low=std::max(r.low,*c+1);break;case BO_GE:r.low=std::max(r.low,*c);break;default:return;}
            if(r.low>r.high)s.alive=false;else s.values[v]=value;return;
        }
        if(const auto* v=variable(e);tracked(v)){
            auto value=get(v,s);
            if(v->getType()->isPointerType())value.pointer=yes?Pointer::nonnull:Pointer::null;
            else if(value.range){auto& r=*value.range;if(!yes)r={0,0};else if(r.low==0)++r.low;else if(r.high==0)--r.high;}
            s.values[v]=value;
        }
    }
    void dereference(const Stmt* at,const Expr* pointer,Value value) {
        if(nulls&&value.pointer==Pointer::null&&!null_constant(pointer->IgnoreParenCasts()))
            emit(at,"CG005","Local propagation proves this pointer null on the analyzed path","Check initialization and branch conditions before dereferencing; unknown aliases are not modeled.");
    }
    Value arithmetic(BinaryOperatorKind op,Value a,Value b,QualType type)const {
        if(!a.range||!b.range)return unknown(type);
        const auto x=*a.range,y=*b.range;
        // Operands are at most 32 bits; addition/subtraction fit int64. Products
        // need an explicit magnitude guard before host arithmetic.
        Range result;
        if(op==BO_Add)result={x.low+y.low,x.high+y.high};
        else if(op==BO_Sub)result={x.low-y.high,x.high-y.low};
        else if(op==BO_Mul){
            constexpr std::int64_t safe=2147483647;
            if(x.low<-safe||x.high>safe||y.low<-safe||y.high>safe)return unknown(type);
            const std::int64_t candidates[]={x.low*y.low,x.low*y.high,x.high*y.low,x.high*y.high};
            result={*std::min_element(std::begin(candidates),std::end(candidates)),*std::max_element(std::begin(candidates),std::end(candidates))};
        }else return unknown(type);
        return convert({result,Pointer::unknown},type);
    }
    Value eval(const Expr* e,State& s,bool address=false) {
        if(!e||!s.alive||++steps>limit){if(steps>limit)s.values.clear();return {};}
        // Uninstantiated template initializers can contain an untyped
        // ParenListExpr. It is syntax, not a value suitable for evaluation;
        // its arguments may still have unknown effects on tracked locals.
        if(e->getType().isNull()){s.values.clear();return {};}
        if(isa<UnaryExprOrTypeTraitExpr,CXXNoexceptExpr,CXXTypeidExpr>(e))return unknown(e->getType());
        if(const auto* p=dyn_cast<ParenExpr>(e))return eval(p->getSubExpr(),s,address);
        if(const auto* cast=dyn_cast<CastExpr>(e)){
            auto value=eval(cast->getSubExpr(),s,address&&cast->getCastKind()==CK_NoOp);
            if(cast->getCastKind()==CK_NullToPointer)return {{},Pointer::null};
            if(cast->getCastKind()==CK_LValueToRValue||cast->getCastKind()==CK_NoOp||cast->getCastKind()==CK_IntegralCast)return convert(value,cast->getType());
            return unknown(cast->getType());
        }
        if(null_constant(e)&&e->getType()->isNullPtrType())return {{},Pointer::null};
        if(const auto c=constant(e);c&&*c>=-2147483648LL&&*c<=4294967295LL)return {Range{*c,*c},Pointer::unknown};
        if(const auto* use=dyn_cast<DeclRefExpr>(e))return get(dyn_cast<VarDecl>(use->getDecl()),s);
        if(const auto* conditional=dyn_cast<ConditionalOperator>(e)){
            eval(conditional->getCond(),s);auto left=s,right=s;narrow(conditional->getCond(),true,left);narrow(conditional->getCond(),false,right);
            const auto a=eval(conditional->getTrueExpr(),left),b=eval(conditional->getFalseExpr(),right);s=join(left,right);
            if(!left.alive)return b;if(!right.alive)return a;Value value;
            if(a.range&&b.range)value.range=Range{std::min(a.range->low,b.range->low),std::max(a.range->high,b.range->high)};
            if(a.pointer==b.pointer)value.pointer=a.pointer;return value;
        }
        if(const auto* binary=dyn_cast<BinaryOperator>(e)){
            const auto op=binary->getOpcode();
            if(op==BO_Comma){eval(binary->getLHS(),s);return eval(binary->getRHS(),s);}
            if(op==BO_LAnd||op==BO_LOr){eval(binary->getLHS(),s);auto skip=s,take=s;narrow(binary->getLHS(),op==BO_LOr,skip);narrow(binary->getLHS(),op==BO_LAnd,take);eval(binary->getRHS(),take);s=join(skip,take);return unknown(e->getType());}
            if(binary->isAssignmentOp()){
                const auto* v=variable(binary->getLHS());
                if(!tracked(v)){s.values.clear();eval(binary->getRHS(),s);eval(binary->getLHS(),s);s.values.clear();return unknown(e->getType());}
                auto value=eval(binary->getRHS(),s);
                if(op!=BO_Assign)value=arithmetic(op==BO_AddAssign?BO_Add:op==BO_SubAssign?BO_Sub:op==BO_MulAssign?BO_Mul:BO_Assign,get(v,s),value,e->getType());
                assign(v,value,s);return get(v,s);
            }
            havoc(e,s);auto left=s,right=s;
            const auto a=eval(binary->getLHS(),left),b=eval(binary->getRHS(),right);s=join(left,right);
            return arithmetic(op,a,b,e->getType());
        }
        if(const auto* unary=dyn_cast<UnaryOperator>(e)){
            const auto op=unary->getOpcode();
            if(op==UO_AddrOf){eval(unary->getSubExpr(),s,true);const auto* v=variable(unary->getSubExpr());return {{},v&&!v->getType()->isReferenceType()?Pointer::nonnull:Pointer::unknown};}
            const auto value=eval(unary->getSubExpr(),s);
            if(op==UO_Deref&&!address)dereference(e,unary->getSubExpr(),value);
            if(unary->isIncrementDecrementOp()){
                const auto next=arithmetic(unary->isIncrementOp()?BO_Add:BO_Sub,value,{Range{1,1},Pointer::unknown},e->getType());assign(variable(unary->getSubExpr()),next,s);return unary->isPostfix()?value:next;
            }
            if(op==UO_Plus)return value;
            if(op==UO_Minus)return arithmetic(BO_Sub,{Range{0,0},Pointer::unknown},value,e->getType());
            return unknown(e->getType());
        }
        if(const auto* access=dyn_cast<ArraySubscriptExpr>(e)){
            havoc(e,s);auto base=s,index=s;eval(access->getBase(),base);const auto value=eval(access->getIdx(),index);s=join(base,index);
            const auto* array=ast.getAsConstantArrayType(access->getBase()->IgnoreParenImpCasts()->getType());
            if(arrays&&array&&value.range&&!constant(access->getIdx())){
                const auto size=array->getSize().getLimitedValue();const auto r=*value.range;
                if(size<=4294967295ULL&&(r.high<0||r.low>static_cast<std::int64_t>(size)||(!address&&r.low==static_cast<std::int64_t>(size))))
                    emit(e,"CG002","Local index interval ["+std::to_string(r.low)+", "+std::to_string(r.high)+"] is outside array extent "+std::to_string(size),"Keep the entire evaluated index range inside [0, size); one-past address formation is permitted.");
            }return unknown(e->getType());
        }
        if(const auto* member=dyn_cast<MemberExpr>(e)){auto value=eval(member->getBase(),s);if(member->isArrow())dereference(e,member->getBase(),value);return unknown(e->getType());}
        if(const auto* lambda=dyn_cast<LambdaExpr>(e)){for(const auto* capture:lambda->capture_inits())eval(capture,s);return {};}
        if(const auto* throwing=dyn_cast<CXXThrowExpr>(e)){eval(throwing->getSubExpr(),s);s.alive=false;return {};}
        // Calls, user-defined conversions, new/delete and unknown expression
        // forms can execute arbitrary code. Discard facts before each child so
        // argument evaluation order cannot create an invalid proof.
        s.values.clear();
        for(const auto* child:e->children())if(const auto* expression=dyn_cast_or_null<Expr>(child)){auto separate=s;eval(expression,separate);}
        return unknown(e->getType());
    }
    void statement(const Stmt* stmt,State& s) {
        if(!stmt||!s.alive||++steps>limit){if(steps>limit)s.values.clear();return;}
        if(const auto* expr=dyn_cast<Expr>(stmt)){eval(expr,s);return;}
        if(const auto* compound=dyn_cast<CompoundStmt>(stmt)){for(const auto* child:compound->body())statement(child,s);return;}
        if(const auto* declarations=dyn_cast<DeclStmt>(stmt)){
            for(const auto* d:declarations->decls())if(const auto* v=dyn_cast<VarDecl>(d)){
                if(!v->hasLocalStorage()){s.values.clear();continue;} // static initialization is conditional across invocations
                const auto value=eval(v->getInit(),s);assign(v,value,s);
            }
            return;
        }
        if(const auto* branch=dyn_cast<IfStmt>(stmt)){
            statement(branch->getInit(),s);statement(branch->getConditionVariableDeclStmt(),s);eval(branch->getCond(),s);
            auto yes=s,no=s;narrow(branch->getCond(),true,yes);narrow(branch->getCond(),false,no);
            statement(branch->getThen(),yes);statement(branch->getElse(),no);s=join(yes,no);return;
        }
        if(const auto* result=dyn_cast<ReturnStmt>(stmt)){eval(result->getRetValue(),s);s.alive=false;return;}
        if(isa<BreakStmt,ContinueStmt>(stmt)){s.alive=false;return;}
        const Expr* condition=nullptr;const Stmt* body=nullptr;bool loop=false,post=false;
        if(const auto* f=dyn_cast<ForStmt>(stmt)){statement(f->getInit(),s);condition=f->getCond();body=f->getBody();loop=true;}
        else if(const auto* w=dyn_cast<WhileStmt>(stmt)){condition=w->getCond();body=w->getBody();loop=true;}
        else if(const auto* d=dyn_cast<DoStmt>(stmt)){condition=d->getCond();body=d->getBody();loop=true;post=true;}
        if(loop){
            if(!post&&condition){
                eval(condition,s);auto first=s;narrow(condition,true,first);
                if(!first.alive)return; // no iteration is possible from this entry state
            }
            // Widen every variable written by any iteration before inspecting
            // the body, then use only the loop's explicit entry condition.
            havoc(stmt,s);auto iteration=s;
            if(!post&&condition){eval(condition,iteration);narrow(condition,true,iteration);}
            statement(body,iteration);if(post)eval(condition,iteration);
            if(const auto* f=dyn_cast<ForStmt>(stmt);f&&iteration.alive)eval(f->getInc(),iteration);
            havoc(stmt,s);return;
        }
        if(isa<NullStmt>(stmt))return;
        // Switch fallthrough, exception handling and other unsupported control
        // paths are unknown to this pass; the independent AST rules still run.
        s.values.clear();
    }
public:
    Flow(ASTContext& context,const Emit& sink,bool bounds,bool pointers):ast(context),emit(sink),arrays(bounds),nulls(pointers){}
    void run(const Stmt* body){escapes.TraverseStmt(const_cast<Stmt*>(body));if(escapes.unsupported)return;State state;statement(body,state);}
};
}
void inspect_local_flow(const clang::Stmt* body,clang::ASTContext& ast,const Emit& emit,bool arrays,bool nulls){Flow(ast,emit,arrays,nulls).run(body);}
}
