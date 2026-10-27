#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <exception>
namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
struct Case {std::string name,source,rule;int expected;};
const std::vector<Case> cases={
    {"template_paren_initializer","template<class T> int f(){T value(1);return 0;}","CG005",0},
    {"template_paren_effect","template<class T> int f(){int*p=nullptr;T value(p=nullptr);return *p;}","CG005",0},
    {"template_list_initializer","template<class T> int f(){T value{1};return 0;}","CG005",0},
    // The primary template and its explicit specialization have distinct USRs.
    {"template_instantiated_initializer","template<class T> int f(){T value(1);int*p=nullptr;return *p;} template int f<int>();","CG005",2},
    {"null_assignment","int f(){int* p=nullptr;return *p;}","CG005",1},
    {"null_copy","int f(){int*p=nullptr;int*q=p;return *q;}","CG005",1},
    {"null_reassignment","int f(){int x;int*p=&x;p=nullptr;return *p;}","CG005",1},
    {"null_branch","int f(int*p){if(!p)return *p;return 0;}","CG005",1},
    {"null_else","int f(int*p){if(p)return *p;else return *p;}","CG005",1},
    {"null_member","struct X{int n;};int f(){X*p=nullptr;return p->n;}","CG005",1},
    {"null_join","int f(bool c){int*p;if(c)p=nullptr;else p=nullptr;return *p;}","CG005",1},
    {"null_partial_join","int f(bool c,int*p){if(c)p=nullptr;return *p;}","CG005",0},
    {"null_guard","int f(int*p){if(!p)return 0;return *p;}","CG005",0},
    {"null_nonnull","int f(){int x=1;int*p=&x;return *p;}","CG005",0},
    {"null_address_only","int*f(){int*p=nullptr;return &*p;}","CG005",0},
    {"null_short_safe","int f(int*p){return p&&*p;}","CG005",0},
    {"null_short_bad","int f(int*p){return !p&&*p;}","CG005",1},
    {"null_or_bad","int f(int*p){return p||*p;}","CG005",1},
    {"null_conditional","int f(int*p){return p?*p:*p;}","CG005",1},
    {"null_unreachable","int f(){int*p=nullptr;if(p)return *p;return 0;}","CG005",0},
    {"null_throw","int f(){int*p=nullptr;throw 1;p=nullptr;return *p;}","CG005",0},
    {"null_alias","void set(int*&);int f(){int*p=nullptr;int*&q=p;p=nullptr;set(q);return *p;}","CG005",0},
    {"null_alias_assignment","int f(){int x;int*p=nullptr;int*&q=p;q=&x;return *p;}","CG005",0},
    {"null_address_escape","void set(int**);int f(){int*p=nullptr;set(&p);return *p;}","CG005",0},
    {"null_capture","int f(){int x;int*p=nullptr;auto set=[&p,&x](){p=&x;};set();return *p;}","CG005",0},
    {"null_aggregate_destructor","struct R{int*&p;int*x;~R(){p=x;}};int f(){int x;int*p=nullptr;{R r{p,&x};p=nullptr;}return *p;}","CG005",0},
    {"null_temporary_destructor","struct R{int*&p;int*x;~R(){p=x;}};int f(){int x;int*p=nullptr;(R{p,&x},p=nullptr);return *p;}","CG005",0},
    {"null_side_effect_order","int use(int,int*);int f(){int x;int*p=nullptr;return use(*p,p=&x);}","CG005",0},
    {"null_volatile","int f(){int*volatile p=nullptr;return *p;}","CG005",0},
    {"null_static","int f(){static int*p=nullptr;return *p;}","CG005",0},
    {"null_static_init","int f(){int x;int*p=&x;static int v=(p=nullptr,1);return *p;}","CG005",0},
    {"null_unknown_call","void other();int f(){int*p=nullptr;other();return *p;}","CG005",0},
    {"null_loop_mutation","int f(bool c){int x;int*p=nullptr;while(c){p=&x;c=false;}return *p;}","CG005",0},
    {"null_loop_guard","int f(int*p){while(!p){return *p;}return 0;}","CG005",1},
    {"null_goto_unknown","int f(){int*p=nullptr;goto label;label:return *p;}","CG005",0},
    {"null_sizeof","int f(){int*p=nullptr;return sizeof(*p);}","CG005",0},
    {"null_constexpr","int f(){int*p=nullptr;if constexpr(false)return *p;return 0;}","CG005",0},
    {"array_assignment","int f(){int a[3];int i=3;return a[i];}","CG002",1},
    {"array_negative","int f(){int a[3];int i=-1;return a[i];}","CG002",1},
    {"array_arithmetic","int f(){int a[3];int i=1;i+=2;return a[i];}","CG002",1},
    {"array_join","int f(bool c){int a[3];int i;if(c)i=3;else i=5;return a[i];}","CG002",1},
    {"array_mixed_join","int f(bool c){int a[3];int i;if(c)i=2;else i=5;return a[i];}","CG002",0},
    {"array_guard_bad","int f(int i){int a[3];if(i>=3)return a[i];return 0;}","CG002",1},
    {"array_guard_safe","int f(int i){int a[3];if(i>=0&&i<3)return a[i];return 0;}","CG002",0},
    {"array_guard_else","int f(int i){int a[3];if(i<3)return 0;return a[i];}","CG002",1},
    {"array_conditional","int f(bool c){int a[3];int i=c?3:4;return a[i];}","CG002",1},
    {"array_increment","int f(){int a[3];int i=2;++i;return a[i];}","CG002",1},
    {"array_multiply","int f(){int a[3];int i=2;return a[i*2];}","CG002",1},
    {"array_end_address","void f(){int a[3];int i=3;int*p=&a[i];}","CG002",0},
    {"array_past_end_address","void f(){int a[3];int i=4;int*p=&a[i];}","CG002",1},
    {"array_unsigned_wrap","int f(){int a[3];unsigned i=0;--i;return a[i];}","CG002",0},
    {"array_signed_overflow","int f(){int a[3];int i=2147483647;++i;return a[i];}","CG002",0},
    {"array_cast_wrap","int f(){int a[3];int i=256;return a[(unsigned char)i];}","CG002",0},
    {"array_mixed_sign_guard","int f(int i){int a[3];if(i>=3u)return a[i];return 0;}","CG002",0},
    {"array_alias","int f(){int a[3];int i=3;int&r=i;r=0;return a[i];}","CG002",0},
    {"array_unknown","int f(int i){int a[3];return a[i];}","CG002",0},
    {"array_loop_guard","int f(int i){int a[3];while(i>=3){return a[i];}return 0;}","CG002",1},
    {"array_loop_written","int f(){int a[3];int i=3;while(i>0){--i;}return a[i];}","CG002",0},
    {"array_loop_unreachable","int f(){int a[3];int i=0;while(i>=3){int x=a[i];++i;}return 0;}","CG002",0},
    {"array_for_unreachable","int f(){int a[3];for(int i=0;i>=3;++i){int x=a[i];}return 0;}","CG002",0},
    {"array_for_safe","int f(){int a[3]={};for(int i=0;i<3;++i)a[i]=1;return 0;}","CG002",0},
    {"array_switch_unknown","int f(int c){int a[3];int i=3;switch(c){case 1:i=0;break;default:i=1;}return a[i];}","CG002",0},
    {"array_unsequenced","int f(){int a[3];int i=3;return (i=0)+a[i];}","CG002",0},
    {"array_sizeof","int f(){int a[3];int i=4;return sizeof(a[i]);}","CG002",0},
};
struct Fixture {
    fs::path dir=fs::current_path()/("local-flow-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),root=dir/"source",db=dir/"scan.sqlite3";
    Fixture(){fs::create_directories(root);std::ofstream commands(dir/"compile_commands.json");commands<<'[';bool first=true;
        for(const auto& c:cases){std::ofstream(root/(c.name+".cpp"))<<c.source<<'\n';if(!first)commands<<',';first=false;
            commands<<"{\"directory\":\""<<cg::utf8_path(root)<<"\",\"file\":\""<<c.name<<".cpp\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""<<c.name<<".cpp\"]}";
        }commands<<']';
    }
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"LOCAL_FLOW_FIXTURE_RETAINED="<<cg::utf8_path(dir)<<std::endl;return;}std::error_code error;fs::remove_all(dir,error);}
};
int main(int argc,char** argv){try{
    check(argc==2,"mode required");const std::string mode=argv[1];Fixture f;
    const auto began=std::chrono::steady_clock::now();auto phase=[&](const char* name){std::cerr<<"LOCAL_FLOW_PHASE "<<name<<" elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count()<<std::endl;};
    if(mode=="cache"){std::cerr<<"LOCAL_FLOW_FIXTURE="<<cg::utf8_path(f.dir)<<std::endl;phase("serial-baseline-start");}
    cg::ScanOptions options;options.compile_commands=cg::utf8_path(f.dir);options.threads=4;
    if(mode=="cache")options.threads=1;
    if(mode=="disabled")options.disabled_rules={"CG002","CG005"};
    auto scan=cg::import_project(f.root,f.db,options);check(scan.analysis.status=="complete","all fixtures must parse");
    if(mode=="cache")phase("serial-baseline-complete");
    if(mode=="disabled"){check(scan.analysis.issues.empty(),"disabled flow rules emitted findings");return 0;}
    int positive=0,negative=0;
    for(const auto& c:cases){int count=0;for(const auto& i:scan.analysis.issues)if(i.file==c.name+".cpp"&&i.rule_id==c.rule){++count;check(i.message.find("Local")!=std::string::npos,"unexpected syntactic finding: "+c.name);}
        check(count==c.expected,c.name+" expected="+std::to_string(c.expected)+" actual="+std::to_string(count));
        if(c.expected)++positive;else ++negative;
    }
    cg::SqliteDatabase database(f.db,true);auto stored=database.latest(scan.root);
    check(cg::execute_query(stored,"SELECT * FROM issues").rows==cg::execute_query(scan,"SELECT * FROM issues").rows,"flow findings roundtrip");
    if(mode=="policy"){
        check(!scan.analysis.issues.empty(),"policy fixture missing findings");
        options.rule_severities={{"CG002","info"},{"CG005","warning"}};
        options.suppressions={cg::suppress_issue(scan,scan.analysis.issues.front(),"reviewed local-flow fixture")};
        const auto revised=cg::import_project(f.root,f.db,options);
        check(revised.analysis.suppressed_issues.size()==1&&revised.analysis.issues.size()+1==scan.analysis.issues.size(),"flow suppression lost or multiplied");
        for(const auto& i:revised.analysis.issues)check(i.severity==(i.rule_id=="CG002"?"info":"warning"),"flow severity ignored");
    }
    if(mode=="cache"){
        options.use_cache=true;options.threads=4;phase("cold-start");const auto cold=cg::import_project(f.root,f.db,options);phase("cold-complete");phase("warm-start");auto warm=cg::import_project(f.root,f.db,options);phase("warm-complete");
        check(cg::execute_query(cold,"SELECT * FROM issues").rows==cg::execute_query(scan,"SELECT * FROM issues").rows,"parallel flow changed findings");
        check(warm.analysis.cache&&warm.analysis.cache->hits==cases.size(),"flow cache misses");
        check(cg::execute_query(warm,"SELECT * FROM issues").rows==cg::execute_query(scan,"SELECT * FROM issues").rows,"flow cache changed findings");
    }
    std::cout<<"LOCAL_FLOW_OK cases="<<cases.size()<<" positive="<<positive<<" negative_or_unknown="<<negative<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
