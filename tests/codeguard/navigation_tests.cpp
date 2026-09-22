#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include "codeguard/report.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F operation,const std::string& message){try{operation();}catch(const std::invalid_argument&){return;}throw std::runtime_error(message);}
struct Fixture {
    fs::path dir=fs::current_path()/("navigation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=dir/fs::path(u8"中文 source"),db=dir/"scan.sqlite3";
    cg::ScanOptions options;
    Fixture(){
        fs::create_directories(root);
        write("api.h",R"CPP(#pragma once
struct Box { int value; Box(int n):value(n){} int get() const {return value;} };
enum Kind { One=1 }; using Alias=Box;
int target(int); double target(double);
inline int shared(int n) { return target(n); }
static int local_helper(){return 1;}
)CPP");
        write("a.cpp",R"CPP(#include "api.h"
#define INVOKE(x) target(x)
int target(int n){if(n){for(int i=0;i<n;++i){while(i<1){break;}}}return n;}
double target(double n){return n;}
int shadow(){int target=2;return target;}
int alpha(){Alias box(One);auto fp=static_cast<int(*)(int)>(&target);return box.value+box.get()+shared(1)+local_helper()+INVOKE(1)+fp(2);}
int unevaluated(){return sizeof(target(1))+noexcept(target(1));}
int discarded(){if constexpr(false){return target(1);}return 0;}
int global=target(1);
int straight(){ { {return 1;} } }
int lambda_owner(){auto x=[](){if(true){while(false){}}};return 0;}
template<class T> int dependent(T t){return t.missing();}
)CPP");
        write("b.cpp","#include \"api.h\"\nint beta(){return shared(1)+local_helper();}\n");
        std::ofstream out(dir/"compile_commands.json");out<<'[';
        int n=0;for(const auto* file:{"a.cpp","b.cpp"}){if(n++)out<<',';out<<"{\"directory\":\""<<cg::utf8_path(root)<<"\",\"file\":\""<<file<<"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""<<file<<"\"]}";}out<<']';
        options.compile_commands=cg::utf8_path(dir);options.threads=1;
    }
    ~Fixture(){std::error_code error;fs::remove_all(dir,error);}
    void write(const std::string& name,const std::string& source){std::ofstream out(root/name);out<<source;check(bool(out),"write fixture");}
    cg::ScanResult scan(){return cg::import_project(root,db,options);}
};
const cg::Symbol& symbol(const cg::AnalysisResult& a,const std::string& name){
    const auto it=std::find_if(a.symbols.begin(),a.symbols.end(),[&](const auto& s){return s.name==name;});
    check(it!=a.symbols.end(),"missing symbol "+name);return *it;
}
const cg::FunctionMetric& metric(const cg::AnalysisResult& a,const std::string& name){
    const auto id=symbol(a,name).id;const auto it=std::find_if(a.metrics.begin(),a.metrics.end(),[&](const auto& m){return m.symbol_id==id;});
    check(it!=a.metrics.end(),"missing metric "+name);return *it;
}
void equivalent(const cg::ScanResult& a,const cg::ScanResult& b){
    check(a.analysis.references==b.analysis.references&&a.analysis.reference_index==b.analysis.reference_index,"reference evidence mismatch");
    for(const auto* table:{"symbols","references","functions","edges"})check(cg::execute_query(a,"SELECT * FROM "+std::string(table)).rows==cg::execute_query(b,"SELECT * FROM "+std::string(table)).rows,"query mismatch");
}
void run(const std::string& mode,const std::string& cli){
    Fixture f;auto scan=f.scan();auto& a=scan.analysis;
    check(scan.id>0&&a.status=="complete"&&a.reference_index=="resolved_ast_v1","analysis failed");
    if(mode=="references"){
        auto overloads=cg::find_symbols(a,"target");
        std::erase_if(overloads,[](const auto& s){return s.kind!="function";});check(overloads.size()==2,"overload identities");
        const auto target=std::find_if(overloads.begin(),overloads.end(),[](const auto& s){return s.line==3;});check(target!=overloads.end(),"integer target definition");
        const auto refs=cg::find_references(a,target->id);
        std::set<std::string> shared_tus;bool address=false,unevaluated=false,global=false;
        for(const auto& r:refs){
            if(r.file=="api.h"&&r.line==5)shared_tus.insert(r.translation_unit);
            if(r.file=="a.cpp"&&r.line==7)unevaluated=true;
            if(r.file=="a.cpp"&&r.line==9)global=r.enclosing_symbol.empty();
            check(!(r.file=="a.cpp"&&r.line==5),"shadow variable attributed to function");
            check(!(r.file=="api.h"&&r.line==4),"declaration counted as reference");
            check(r.translation_unit=="a.cpp"||r.translation_unit=="b.cpp","missing TU source");
        }
        // Macro expansion starts at INVOKE, independently calculated from fixture text.
        check(shared_tus==std::set<std::string>({"a.cpp","b.cpp"})&&unevaluated&&global,"reference coverage");
        std::ifstream source(f.root/"a.cpp");std::string line;for(int n=0;n<6;++n)std::getline(source,line);
        const auto macro_column=static_cast<int>(line.find("INVOKE")+1);
        const auto address_column=static_cast<int>(line.find("&target")+2);
        address=std::any_of(refs.begin(),refs.end(),[&](const auto& r){return r.file=="a.cpp"&&r.line==6&&r.column==address_column;});
        check(address,"function address reference missing");
        const bool macro=std::any_of(refs.begin(),refs.end(),[&](const auto& r){return r.file=="a.cpp"&&r.line==6&&r.column==macro_column;});
        check(macro,"macro expansion location");
        for(const auto& other:overloads)if(other.id!=target->id)check(cg::find_references(a,other.id).empty(),"wrong overload references");
        for(const auto* name:{"Box::value","Box::get","Alias","One"})check(!cg::find_references(a,symbol(a,name).id).empty(),"member/type/enum reference missing");
        check(std::any_of(a.references.begin(),a.references.end(),[](const auto& r){return r.kind=="construct";}),"constructor reference missing");
        check(std::none_of(a.symbols.begin(),a.symbols.end(),[](const auto& s){return s.name=="missing";}),"unresolved name fabricated");
        const auto locals=cg::find_symbols(a,"local_helper");check(locals.size()==2&&locals[0].id!=locals[1].id,"header internal symbols merged across TUs");
        for(const auto& local:locals){const auto uses=cg::find_references(a,local.id);check(uses.size()==1&&local.id.ends_with("@tu="+uses[0].translation_unit),"static target crossed TU");}
        rejects([&]{cg::find_references(a,"invalid");},"unknown target accepted");
        cg::ScanContext cancelled;cancelled.control=std::make_shared<cg::ScanControl>();cancelled.control->request_cancel();
        bool stopped=false;try{cg::find_references(a,target->id,cancelled);}catch(const cg::ScanCancelled&){stopped=true;}check(stopped,"reference search ignored cancellation");
    }else if(mode=="metrics"){
        check(metric(a,"target").max_nesting==3,"nested if/for/while depth");
        check(metric(a,"straight").max_nesting==0&&metric(a,"lambda_owner").max_nesting==0,"braces/lambda attributed to outer function");
        check(metric(a,"unevaluated").fan_out==0&&metric(a,"discarded").fan_out==0,"unevaluated or discarded calls counted");
        check(metric(a,"alpha").fan_out==5,"direct endpoint dedup or indirect handling");
        check(metric(a,"shared").fan_in==2&&metric(a,"shared").fan_out==1,"cross TU fan counts");
        check(metric(a,"straight").fan_in==0,"isolated function fan");
        const auto rows=cg::execute_query(scan,"SELECT name FROM functions WHERE max_nesting >= 3 AND fan_in >= 1");check(!rows.rows.empty(),"new metric query fields");
    }else if(mode=="persistence"){
        {cg::SqliteDatabase db(f.db,true);equivalent(scan,db.latest(scan.root));}
        f.options.threads=4;equivalent(scan,f.scan());
        f.options.use_cache=true;equivalent(scan,f.scan());auto warm=f.scan();check(warm.analysis.cache&&warm.analysis.cache->hits==2,"warm cache missing");equivalent(scan,warm);
        const auto json=cg::report_json(scan);const auto html=cg::report_html(scan);
        check(json.find("\"reference_index\":\"resolved_ast_v1\"")!=std::string::npos&&json.find("\"max_nesting\":3")!=std::string::npos&&html.find("引用观测")!=std::string::npos,"report fields missing");
        f.write("b.cpp","#include \"api.h\"\nint broken( {\n");auto partial=f.scan();check(partial.analysis.status=="partial","partial missing");
        check(std::none_of(partial.analysis.references.begin(),partial.analysis.references.end(),[](const auto& r){return r.translation_unit=="b.cpp";}),"failed TU leaked references");
    }else if(mode=="dedup"){
        f.write("implementation.c","static int finding(){int a[1]={};return a[2];}\n");
        f.write("a.cpp","#include \"implementation.c\"\nint alpha(){return finding();}\n");
        f.write("b.cpp","#include \"implementation.c\"\nint beta(){return finding();}\n");
        auto one=f.scan();check(one.analysis.issues.size()==1,"same physical finding duplicated across TUs");
        const auto functions=cg::find_symbols(one.analysis,"finding");check(functions.size()==2,"finding symbols must remain TU-local");
        for(const auto& fn:functions)check(cg::find_references(one.analysis,fn.id).size()==1,"TU-local references merged with issue grouping");
        check(std::any_of(functions.begin(),functions.end(),[&](const auto& fn){return fn.id==one.analysis.issues.front().symbol_id;}),"issue points to a missing representative symbol");
        f.options.threads=4;auto parallel=f.scan();equivalent(one,parallel);check(parallel.analysis.issues.size()==1&&parallel.analysis.issues.front().symbol_id==one.analysis.issues.front().symbol_id,"parallel issue representative changed");
        f.options.use_cache=true;f.scan();auto warm=f.scan();check(warm.analysis.issues.size()==1&&warm.analysis.cache&&warm.analysis.cache->hits==2,"warm cache duplicates finding");
    }else if(mode=="legacy"){
        sqlite3* raw=nullptr;check(sqlite3_open(cg::utf8_path(f.db).c_str(),&raw)==SQLITE_OK,"open migration fixture");
        const int status=sqlite3_exec(raw,"DROP TABLE symbol_reference; ALTER TABLE analysis DROP COLUMN reference_index; ALTER TABLE function_metric DROP COLUMN max_nesting; ALTER TABLE function_metric DROP COLUMN fan_in; ALTER TABLE function_metric DROP COLUMN fan_out; PRAGMA user_version=6;",nullptr,nullptr,nullptr);sqlite3_close(raw);check(status==SQLITE_OK,"schema 6 fixture");
        for(bool readonly:{true,false}){
            cg::SqliteDatabase db(f.db,readonly);const auto old=db.latest(scan.root);
            check(old.analysis.reference_index.empty()&&old.analysis.references.empty(),"legacy references fabricated");
            check(metric(old.analysis,"alpha").max_nesting==-1&&metric(old.analysis,"alpha").fan_in==-1&&metric(old.analysis,"alpha").fan_out==-1,"legacy metrics fabricated");
            rejects([&]{cg::find_references(old.analysis,symbol(a,"alpha").id);},"legacy search treated as empty");
            bool rejected=false;try{cg::execute_query(old,"SELECT * FROM references");}catch(const cg::QueryError&){rejected=true;}check(rejected,"legacy reference query accepted");
        }
        equivalent(scan,f.scan());
    }else if(mode=="cli"){
        const auto id=symbol(a,"shared").id;
        auto call=[&](const std::string& target){return cg::run_process({{cli,"references",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--symbol",target},cg::utf8_path(f.dir),std::chrono::seconds(20)});};
        auto good=call(id);check(good.exit_code==0&&good.stdout_text.find("reference_observations=2")!=std::string::npos&&good.stdout_text.find("translation_unit")!=std::string::npos,"CLI references failed: "+good.stdout_text+good.stderr_text);
        check(call("invalid").exit_code!=0,"CLI unknown target succeeded");
    }else throw std::runtime_error("unknown navigation mode");
}
int main(int argc,char** argv){try{check(argc==3,"mode and CLI required");run(argv[1],argv[2]);std::cout<<"NAVIGATION_OK "<<argv[1]<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
