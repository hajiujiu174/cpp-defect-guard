#include "validation_inputs_wire.hpp"
#include "codeguard/repair.hpp"
#include <sqlite3.h>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <set>
#include <type_traits>

namespace cg=codeguard;namespace fs=std::filesystem;
static_assert(!std::is_convertible_v<cg::ValidationInputs,cg::ScanResult>);
static_assert(!std::is_convertible_v<cg::ValidationAnalysis,cg::AnalysisResult>);
static_assert(!std::is_invocable_v<decltype(&cg::SqliteDatabase::save),cg::SqliteDatabase&,cg::ValidationInputs&,const cg::ScanContext&>);
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"expected rejection");}
struct Probe {std::set<std::string> denied;int attempts=0;std::shared_ptr<cg::ScanControl> cancel;};
Probe* probe=nullptr;
int authorize(void*,int action,const char* table,const char*,const char*,const char*) {
    if(probe&&action==SQLITE_READ&&table&&probe->denied.contains(table)){++probe->attempts;return SQLITE_DENY;}return SQLITE_OK;
}
int trace(unsigned,void*,void*,void* sql) {
    if(probe&&probe->cancel&&sql&&std::string(static_cast<const char*>(sql)).find("FROM translation_unit")!=std::string::npos)probe->cancel->request_cancel();return 0;
}
int extension(sqlite3* db,char**,const sqlite3_api_routines*) {sqlite3_trace_v2(db,SQLITE_TRACE_STMT,trace,nullptr);return sqlite3_set_authorizer(db,authorize,nullptr);}
struct Observe {
    Probe state;
    Observe(){check(!probe,"one observer per test");probe=&state;check(sqlite3_auto_extension(reinterpret_cast<void(*)()>(extension))==SQLITE_OK,"register SQLite observer");}
    ~Observe(){sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(extension));probe=nullptr;}
};
struct Fixture {
    fs::path directory=fs::current_path()/("validation-inputs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=directory/fs::path(u8"中文 source ' quote"),database=directory/"scan.sqlite3";
    Fixture(){check(fs::create_directory(directory),"fresh fixture");fs::create_directory(root);write("main.cpp","int main(){return 0;}\n");write("a.h","#pragma once\n");}
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"FIXTURE_RETAINED="<<cg::utf8_path(directory)<<'\n';return;}std::error_code error;fs::remove_all(directory,error);}
    void write(const std::string& name,const std::string& text){std::ofstream out(root/cg::from_utf8(name),std::ios::binary);out<<text;check(bool(out),"fixture source");}
    cg::ScanResult saved() {
        auto s=cg::scan_project(root);s.changed=2;s.unchanged=3;s.removed=4;
        auto& a=s.analysis;a.status="complete";a.compile_commands="saved compile commands";a.configuration=cg::encode_config({});a.analyzer_revision="saved revision";a.workers=3;a.elapsed_ms=1234;
        a.units={{"main.cpp","success","unit diagnostic\nnext",2,"command-fingerprint",{"a.h","main.cpp"}}};a.covered_files={"a.h","main.cpp"};
        a.symbols={{"f","function","main","main.cpp",1,1,true,false}};a.metrics={{"f",1,0,1,0,0,0}};a.edges={{"call","f","f","main.cpp",1,1}};a.references={{"f","f","read","main.cpp",1,2,"main.cpp"}};a.reference_index="resolved_ast_v1";
        a.issues={{"CG003","warning","main.cpp",1,5,"message","evidence","suggestion","f","clang-ast"}};
        a.suppressed_issues={{"CG001","warning","main.cpp",2,5,"hidden","evidence","suggestion","f","clang-ast","approved reason"}};a.rule_diagnostics={"rule diagnostic"};
        cg::SqliteDatabase db(database);db.save(s);return s;
    }
    void sql(const std::string& text) {sqlite3* db=nullptr;check(sqlite3_open(cg::utf8_path(database).c_str(),&db)==SQLITE_OK,"fixture SQL open");const auto code=sqlite3_exec(db,text.c_str(),nullptr,nullptr,nullptr);const std::string message=sqlite3_errmsg(db);sqlite3_close(db);check(code==SQLITE_OK,message);}
};
const std::set<std::string> graph_tables{"symbol","symbol_reference","function_metric","graph_edge"};
int main(int argc,char** argv){try{
    check(argc==2,"case required");const std::string mode=argv[1];Fixture f;
    if(mode=="legacy") {
        f.sql("PRAGMA application_id=1128744018;PRAGMA user_version=1;CREATE TABLE project(id INTEGER PRIMARY KEY,root TEXT);CREATE TABLE scan(id INTEGER PRIMARY KEY,project_id INTEGER,scanned_at TEXT,added INTEGER,changed INTEGER,unchanged INTEGER,removed INTEGER);CREATE TABLE file(scan_id INTEGER,path TEXT,language TEXT,hash TEXT,size INTEGER,mtime INTEGER,lines INTEGER);INSERT INTO project VALUES(1,'legacy');INSERT INTO scan VALUES(1,1,'old',1,0,0,0);INSERT INTO file VALUES(1,'a.c','C','hash',3,4,5);");
        cg::SqliteDatabase db(f.database,true);const auto full=db.latest("legacy");const auto input=db.validation_inputs("legacy");
        check(validation_test::metadata(full)==validation_test::metadata(input)&&input.analysis.status=="not_requested"&&db.latest_scan_id("legacy")==1,"legacy schema metadata");
    } else if(mode=="missing") {
        cg::SqliteDatabase create(f.database);cg::SqliteDatabase db(f.database,true);const auto input=db.validation_inputs("unknown");
        check(!input.id&&input.root=="unknown"&&input.files.empty()&&db.latest_scan_id("unknown")==0,"unknown root has no scan");
    } else if(mode=="proposal") {
        check(cg::clang_analysis_available(),"analysis-enabled test");f.write("main.cpp","int main(){int x=1;if(x=0)return 0;return 1;}\n");
        std::ofstream commands(f.directory/"compile_commands.json");commands<<"[{\"directory\":\""<<cg::utf8_path(f.root)<<"\",\"file\":\"main.cpp\",\"arguments\":[\""<<cg::utf8_path(cg::from_utf8(CODEGUARD_TEST_CXX))<<"\",\"-std=c++20\",\"-c\",\"main.cpp\"]}]";commands.close();
        cg::ProjectConfig config;config.compile_commands=cg::utf8_path(f.directory);config.auto_discover=false;const auto options=cg::configured_scan_options(f.root,config);const auto full=cg::import_project(f.root,f.database,options);check(full.analysis.issues.size()==1,"one selected condition");
        const auto before=cg::propose_repairs(full,full.analysis.issues.front());Observe observe;observe.state.denied=graph_tables;
        cg::SqliteDatabase db(f.database,true);const auto inputs=db.validation_inputs(full.root);const auto after=cg::propose_repairs(inputs,inputs.analysis.issues.front());
        check(before.size()==2&&after.size()==before.size()&&!observe.state.attempts,"typed proposal needs no saved graph");
        for(std::size_t n=0;n<before.size();++n)check(cg::encode_repair_candidate(before[n])==cg::encode_repair_candidate(after[n]),"candidate bytes identical");
    } else {
        auto saved=f.saved();
        if(mode=="parity") {
            cg::SqliteDatabase db(f.database,true);const auto full=db.latest(saved.root);const auto inputs=db.validation_inputs(saved.root);
            check(validation_test::metadata(full)==validation_test::metadata(inputs),"every kept metadata field matches");
            check(full.analysis.symbols.size()==1&&full.analysis.references.size()==1&&full.analysis.metrics.size()==1&&full.analysis.edges.size()==1,"ordinary latest is still complete");
            auto diagnostic=full;diagnostic.diagnostics.push_back({"a.c","incomplete"});check(validation_test::metadata(diagnostic)==validation_test::metadata(cg::validation_inputs_of(diagnostic)),"explicit factory retains inventory diagnostic");
        } else if(mode=="scope") {
            Observe observe;observe.state.denied=graph_tables;observe.state.denied.insert("build_run");observe.state.denied.insert("build_step");cg::SqliteDatabase db(f.database,true);
            const auto inputs=db.validation_inputs(saved.root);check(inputs.id==saved.id&&db.latest_scan_id(saved.root)==saved.id&&observe.state.attempts==0,"projection must not read graph or history tables");
            rejects([&]{db.latest(saved.root);});check(observe.state.attempts>0,"ordinary full read is independently checked");observe.state.denied.clear();check(db.latest(saved.root).analysis.references.size()==1,"full read remains available");
        } else if(mode=="latest") {
            const auto old=saved.id;saved.scanned_at="second";saved.analysis.issues.push_back({"CG004","error","main.cpp",3,1,"changed","e","s","f","clang-ast"});cg::SqliteDatabase db(f.database);db.save(saved);
            check(saved.id>old&&db.latest_scan_id(saved.root)==saved.id,"latest ID changes");check(validation_test::metadata(db.latest(saved.root))==validation_test::metadata(db.validation_inputs(saved.root)),"newest metadata, not a cached projection");check(db.snapshot(saved.root,old).analysis.issues.size()==1,"historical full read intact");
        } else if(mode=="archive") {
            fs::rename(f.root,f.directory/"moved");cg::SqliteDatabase db(f.database,true);check(db.validation_inputs(saved.root).id==saved.id,"metadata does not require source existence");
        } else if(mode=="cancel"||mode=="during") {
            auto control=std::make_shared<cg::ScanControl>();cg::ScanContext context;context.control=control;Observe observe;cg::SqliteDatabase db(f.database,true,context);
            if(mode=="cancel")control->request_cancel();else observe.state.cancel=control;
            bool cancelled=false;try{db.validation_inputs(saved.root);}catch(const cg::ScanCancelled&){cancelled=true;}check(cancelled,"cancelled projection must not return partial success");
            cg::SqliteDatabase verify(f.database,true);check(verify.latest_scan_id(saved.root)==saved.id,"read cancellation writes no scan");
        } else if(mode=="build") {
            f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(InputsFixture LANGUAGES CXX)\nadd_executable(demo main.cpp)\nenable_testing()\nadd_test(NAME demo COMMAND demo)\n");
            Observe observe;observe.state.denied=graph_tables;cg::BuildOptions options;options.output_directory=f.directory/"runs";options.jobs=2;
            const auto result=cg::build_and_test(f.root,f.database,options);check(result.status=="passed"&&result.source_unchanged&&observe.state.attempts==0,"native build must not load stored graph");
        } else throw std::invalid_argument("unknown case");
    }
    std::cout<<"PASS "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
