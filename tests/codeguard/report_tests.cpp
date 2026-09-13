#include "codeguard/application.hpp"
#include "codeguard/report.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;
namespace fs=std::filesystem;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}require(rejected,"expected rejection");}
struct Fixture {
    fs::path dir=fs::current_path()/("report-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool keep=false;
    Fixture(){require(fs::create_directory(dir),"fresh fixture");}
    ~Fixture(){if(!keep){std::error_code error;fs::remove_all(dir,error);}}
};
void history(){
    Fixture fixture;const auto database=fixture.dir/"history.sqlite3";
    const auto root=cg::utf8_path(fixture.dir/"archived-source-中文");
    cg::ScanResult first;first.root=root;first.scanned_at="2026-09-13T01:00:00Z";
    first.files={{"a.cpp","cpp","fnv1a64-v1:0123456789abcdef",10,0,1}};
    first.analysis.status="complete";first.analysis.configuration=cg::encode_config({});
    first.inventory_policy="saved scope";first.analysis.analyzer_revision="saved analyzer";
    first.analysis.units={{"a.cpp","success","",0,"saved command",{"a.cpp"}}};
    first.analysis.issues={{"CG002","error","a.cpp",1,7,"old issue","a[2]","check bounds","function"}};
    cg::ScanResult second,third,foreign;
    {
        cg::SqliteDatabase db(database);db.save(first);
        second=first;second.scanned_at="2026-09-13T02:00:00Z";
        second.analysis.status="failed";second.analysis.issues.clear();second.analysis.units={{"a.cpp","parse_failed","invalid C++",0}};db.save(second);
        third=first;third.scanned_at="2026-09-13T03:00:00Z";
        third.analysis.suppressed_issues=third.analysis.issues;third.analysis.suppressed_issues.front().suppression_reason="reviewed 中文";third.analysis.issues.clear();db.save(third);
        foreign=first;foreign.root=cg::utf8_path(fixture.dir/"different-project");db.save(foreign);
        cg::BuildRun build;build.root=root;build.scan_id=first.id;build.status="passed";build.started_at=first.scanned_at;
        build.git_revision="saved-revision";build.git_log="archived log";build.configuration=first.analysis.configuration;
        cg::BuildStep step;step.name="test";step.result.status="exited";step.result.exit_code=0;step.result.stdout_text="saved test output";step.tests_total=2;step.tests_failed=0;step.tests_skipped=0;build.steps={step};
        for(int n=0;n<51;++n)db.save_build(build);
        build.scan_id=third.id;for(int n=0;n<51;++n)db.save_build(build);
    }
    require(!fs::exists(cg::from_utf8(root)),"historical reads must work after source removal");
    cg::SqliteDatabase db(database,true);
    const auto page=db.scans(root,0,2);require(page.size()==2&&page[0].id==third.id&&page[1].id==second.id,"descending stable history page");
    require(page[0].suppressed_count==1&&page[0].issue_count==0&&page[1].analysis_status=="failed","historical summary distinguishes suppression and failure");
    const auto next=db.scans(root,page.back().id,2);require(next.size()==1&&next.front().id==first.id&&next.front().issue_count==1,"cursor excludes previous page");
    require(db.scans(root,first.id,2).empty(),"history end");
    const auto saved=db.snapshot(root,first.id);
    require(saved.analysis.issues.size()==1&&saved.analysis.issues.front().evidence=="a[2]","exact historic finding restored");
    require(saved.inventory_policy=="saved scope"&&saved.analysis.analyzer_revision=="saved analyzer"&&saved.analysis.units.front().command_fingerprint=="saved command"&&saved.analysis.units.front().covered_files==std::vector<std::string>{"a.cpp"},"analysis provenance belongs to its historical snapshot");
    require(saved.build_runs.size()==51&&saved.build_runs.front().scan_id==first.id,"exact snapshot includes all associated builds beyond latest 50 project builds");
    require(saved.build_runs.front().git_log.empty()&&saved.build_runs.front().steps.front().result.stdout_text.empty(),"history summaries do not load large logs");
    require(saved.build_runs.front().steps.front().tests_total==2,"test counts available without logs");
    require(db.snapshot(root,first.id,true).build_runs.front().steps.front().result.stdout_text=="saved test output","report can restore complete archived logs");
    require(db.snapshot(root,second.id).analysis.status=="failed","failed snapshot remains failed");
    require(db.snapshot(root,third.id).analysis.suppressed_issues.front().suppression_reason=="reviewed 中文","suppression reason restored");
    require(db.latest(root).id==third.id&&db.builds(root).size()==50,"existing latest history contract retained");
    rejects([&]{db.snapshot(root,foreign.id);});rejects([&]{db.snapshot(root,0);});rejects([&]{db.snapshot(root,99999);});
    rejects([&]{db.scans(root,-1);});rejects([&]{db.scans(root,0,0);});rejects([&]{db.scans(root,0,501);});
}
void legacy(){
    Fixture fixture;auto path=fixture.dir/"legacy.sqlite3";sqlite3* raw=nullptr;
    require(sqlite3_open(cg::utf8_path(path).c_str(),&raw)==SQLITE_OK,"open legacy database");
    const auto code=sqlite3_exec(raw,R"SQL(
CREATE TABLE project(id INTEGER PRIMARY KEY,root TEXT UNIQUE);
CREATE TABLE scan(id INTEGER PRIMARY KEY,project_id INTEGER,scanned_at TEXT,added INTEGER,changed INTEGER,unchanged INTEGER,removed INTEGER);
CREATE TABLE file(scan_id INTEGER,path TEXT,language TEXT,hash TEXT,size INTEGER,mtime INTEGER,lines INTEGER);
INSERT INTO project VALUES(1,'archived'); INSERT INTO scan VALUES(1,1,'time',1,0,0,0);
INSERT INTO file VALUES(1,'old.c','C','old-hash',1,1,1);
PRAGMA application_id=1128744018; PRAGMA user_version=1;
)SQL",nullptr,nullptr,nullptr);sqlite3_close(raw);require(code==SQLITE_OK,"schema 1 fixture");
    {cg::SqliteDatabase db(path,true);const auto pages=db.scans("archived");require(pages.size()==1&&pages.front().analysis_status=="not_requested"&&pages.front().file_count==1,"schema 1 history");require(db.snapshot("archived",1).analysis.analyzer_revision.empty(),"old version has unknown provenance");}
    {cg::SqliteDatabase db(path);const auto old=db.snapshot("archived",1);require(old.files.front().hash=="old-hash"&&old.inventory_policy.empty()&&old.analysis.analyzer_revision.empty(),"migration preserves unknown historical provenance");}
    require(sqlite3_open(cg::utf8_path(path).c_str(),&raw)==SQLITE_OK,"open schema 5 fixture");
    const auto downgrade=sqlite3_exec(raw,"DROP TABLE unit_coverage; ALTER TABLE scan DROP COLUMN inventory_policy; ALTER TABLE analysis DROP COLUMN analyzer_revision; ALTER TABLE translation_unit DROP COLUMN command_fingerprint; PRAGMA user_version=5;",nullptr,nullptr,nullptr);sqlite3_close(raw);require(downgrade==SQLITE_OK,"schema 5 fixture");
    {cg::SqliteDatabase db(path,true);require(db.snapshot("archived",1).id==1&&db.scans("archived").front().suppressed_count==0,"read schema 5 without migration");}
    {cg::SqliteDatabase db(path);require(db.snapshot("archived",1).inventory_policy.empty(),"schema 5 to 6 does not invent provenance");}
}
void provenance(){
    Fixture fixture;const auto source=fixture.dir/"source";fs::create_directory(source);
    auto write=[&](const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;require(bool(out),"write fixture");};
    write(source/"api.h","inline int probe(){int a[2]={};return a[2];}\n");write(source/"main.cpp","#include \"api.h\"\nint main(){return probe();}\n");
    const auto commands=fixture.dir/"compile_commands.json";
    auto command=[&](const std::string& flag){write(commands,"[{\"directory\":\""+cg::utf8_path(source)+"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-std=c++20\",\""+flag+"\",\"-c\",\"main.cpp\"]}]");};
    command("-DFLAG=1");cg::ScanOptions options;options.compile_commands=cg::utf8_path(commands);options.threads=1;
    const auto database=fixture.dir/"analysis.sqlite3";auto first=cg::import_project(source,database,options);
    require(first.analysis.status=="complete"&&!first.analysis.analyzer_revision.empty(),"versioned analyzer");
    require(first.analysis.units.front().command_fingerprint.size()==64&&first.analysis.units.front().covered_files==std::vector<std::string>({"api.h","main.cpp"}),"actual command and contributing header coverage recorded");
    options.threads=4;auto parallel=cg::import_project(source,database,options);
    require(parallel.analysis.analyzer_revision==first.analysis.analyzer_revision&&parallel.analysis.units.front().command_fingerprint==first.analysis.units.front().command_fingerprint,"threads do not change provenance");
    command("-DFLAG=2");auto changed=cg::import_project(source,database,options);
    require(changed.analysis.units.front().command_fingerprint!=first.analysis.units.front().command_fingerprint,"changed command contents detected at unchanged database path");
    write(source/"main.cpp","#include \"api.h\"\ninvalid C++ !!!\n");auto failed=cg::import_project(source,database,options);
    require(failed.analysis.status=="failed"&&failed.analysis.units.front().command_fingerprint==changed.analysis.units.front().command_fingerprint&&failed.analysis.units.front().covered_files.empty(),"failed TU does not claim contributed header coverage");
    require(cg::compare_scans(changed,failed).changes.front().status=="parse_failed","real header finding cannot disappear as a repair when its TU fails");
    write(source/"main.cpp","#include \"api.h\"\nint main(){return probe();}\n");
    write(source/"api.h","inline int probe(){int a[2]={};return a[1];}\n");auto fixed=cg::import_project(source,database,options);
    require(fixed.analysis.issues.empty()&&cg::compare_scans(changed,fixed).changes.front().status=="not_detected","real source repair becomes reviewable disappearance with matching command provenance");
    command("-DFLAG=3");auto different=cg::import_project(source,database,options);
    require(cg::compare_scans(changed,different).changes.front().status=="configuration_changed","real command change must not be counted as verified repair");
    cg::SqliteDatabase db(database,true);require(db.snapshot(first.root,first.id).analysis.units.front().command_fingerprint==first.analysis.units.front().command_fingerprint,"later scan cannot overwrite earlier command provenance");
    auto scope=cg::scan_project(source);options.ignored_directories.push_back("custom");auto excluded=cg::scan_project(source,options);
    require(scope.inventory_policy!=excluded.inventory_policy,"actual custom ignore policy recorded");
}
void comparison(){
    cg::ScanResult before;before.id=1;before.root="archived";before.inventory_policy="scope";
    before.analysis.status="complete";before.analysis.configuration=cg::encode_config({});before.analysis.analyzer_revision="version";
    before.files={{"api.h","header","old",10,0,1},{"main.cpp","cpp","same",10,0,1}};
    before.analysis.units={{"main.cpp","success","",0,"command",{"api.h","main.cpp"}}};
    before.analysis.issues={{"CG002","error","api.h",2,20,"overflow","a[2]","repair","function"}};
    auto after=before;after.id=2;
    auto state=[&](const cg::ScanResult& scan,const std::string& expected){auto result=cg::compare_scans(before,scan);require(result.changes.size()==1&&result.changes.front().status==expected,"wrong disappearance classification");};
    state(after,"persistent");after.analysis.issues.front().line=20;state(after,"persistent");
    after=before;after.id=2;after.analysis.suppressed_issues=after.analysis.issues;after.analysis.issues.clear();after.analysis.suppressed_issues.front().suppression_reason="reviewed";state(after,"suppressed");
    auto suppressed=after;suppressed.id=1;after=before;after.id=2;require(cg::compare_scans(suppressed,after).changes.front().status=="reactivated","reactivated suppression");
    after.analysis.issues.clear();after.files.front().hash="changed";state(after,"not_detected");
    auto absent=after;
    after.analysis.units.front().status="parse_failed";after.analysis.units.front().covered_files.clear();after.analysis.status="failed";state(after,"parse_failed");
    after=absent;after.analysis.units.front().status="missing_command";state(after,"coverage_lost");
    after=absent;after.analysis.units.front().covered_files={"main.cpp"};state(after,"coverage_lost");
    after=absent;after.analysis.units.front().command_fingerprint="other flags";state(after,"configuration_changed");
    after=absent;after.analysis.analyzer_revision="other version";state(after,"analyzer_changed");
    after=absent;after.analysis.analyzer_revision.clear();state(after,"provenance_unknown");
    after=absent;after.analysis.configuration.clear();state(after,"configuration_unknown");
    after=absent;after.analysis.status="not_requested";after.analysis.units.clear();state(after,"analysis_disabled");
    after=absent;cg::ProjectConfig config;config.disabled_rules={"CG002"};after.analysis.configuration=cg::encode_config(config);state(after,"rule_disabled");
    after=absent;after.files.erase(after.files.begin());state(after,"file_removed");
    after.inventory_policy="new scope";state(after,"inventory_scope_changed");
    after.inventory_policy.clear();state(after,"inventory_unknown");
    after=absent;after.analysis.issues=before.analysis.issues;after.analysis.issues.front().evidence="different[2]";
    auto changed=cg::compare_scans(before,after);require(changed.changes.size()==2&&changed.changes.front().status=="not_detected"&&changed.changes.back().status=="added","edited evidence is not arbitrarily identified as the old problem");
    auto repeated=before;repeated.analysis.issues.push_back(repeated.analysis.issues.front());repeated.analysis.issues.back().line=4;
    after=repeated;after.id=2;after.analysis.issues.front().line=3;after.analysis.issues.back().line=5;
    require(cg::compare_scans(repeated,after).changes.size()==4,"ambiguous repeated expressions must not be paired arbitrarily");
    rejects([&]{cg::compare_scans(before,before);});after.root="different";rejects([&]{cg::compare_scans(before,after);});
}
std::string json_value(const std::string& payload,const std::string& path){
    sqlite3* db=nullptr;require(sqlite3_open(":memory:",&db)==SQLITE_OK,"JSON validation database");sqlite3_stmt* statement=nullptr;
    require(sqlite3_prepare_v2(db,"SELECT json_valid(?1), json_extract(?1,?2)",-1,&statement,nullptr)==SQLITE_OK,"JSON reader available in locked SQLite");
    sqlite3_bind_text(statement,1,payload.data(),static_cast<int>(payload.size()),SQLITE_TRANSIENT);sqlite3_bind_text(statement,2,path.c_str(),-1,SQLITE_TRANSIENT);
    require(sqlite3_step(statement)==SQLITE_ROW&&sqlite3_column_int(statement,0)==1,"export must be parseable JSON");
    const auto* text=sqlite3_column_text(statement,1);std::string result=text?std::string(reinterpret_cast<const char*>(text),sqlite3_column_bytes(statement,1)):"";
    sqlite3_finalize(statement);sqlite3_close(db);return result;
}
void exporting(){
    require(cg::report_time("0")=="1970-01-01 00:00:00 UTC"&&cg::report_time("2026-09-13T00:00:00Z")=="2026-09-13T00:00:00Z","saved timestamp display");
    Fixture fixture;fixture.keep=true;
    cg::ScanResult before;before.id=1;before.root=cg::utf8_path(fixture.dir/"archived-source");before.scanned_at="2026-09-13T01:00:00Z";
    before.files={{"main.cpp","cpp","old hash",40,0,2}};before.inventory_policy="saved scope";
    before.analysis.status="complete";before.analysis.configuration=cg::encode_config({});before.analysis.analyzer_revision="version";
    before.analysis.units={{"main.cpp","success","",0,"command",{"main.cpp"}}};before.analysis.covered_files={"main.cpp"};
    const std::string dangerous="</script><img src=x onerror=alert(1)> & \"中文\"\n";
    before.analysis.issues={{"CG002","error","main.cpp",1,30,dangerous,dangerous+std::string("\0",1)+"tail\xe4\xb8","检查边界","function"}};
    before.analysis.symbols={{"function","function","probe","main.cpp",1,1,true,false}};before.analysis.metrics={{"function",2,0,-1}};
    auto current=before;current.id=2;current.scanned_at="2026-09-13T02:00:00Z";current.analysis.units.front().status="parse_failed";current.analysis.units.front().diagnostics="error: expected ';'";current.analysis.status="failed";current.analysis.issues.clear();current.analysis.units.front().covered_files.clear();current.analysis.covered_files.clear();
    cg::BuildRun run;run.id=1;run.scan_id=current.id;run.root=current.root;run.status="failed";run.configuration=current.analysis.configuration;
    cg::BuildStep step;step.name="test";step.result.status="exited";step.result.exit_code=1;step.result.stderr_text=dangerous;run.steps={step};current.build_runs={run};current.build_logs_loaded=true;
    const auto payload=cg::report_json(current,&before);
    require(json_value(payload,"$.comparison.changes[0].status")=="parse_failed","report retains failed analysis cause");
    require(json_value(payload,"$.snapshot.build_runs[0].steps[0].tests_total")=="-1","unknown tests remain unknown");
    require(json_value(payload,"$.baseline.analysis.issues[0].evidence")==dangerous+std::string("\0",1)+"tail\xef\xbf\xbd\xef\xbf\xbd","JSON roundtrip preserves control characters and replaces incomplete UTF-8");
    const auto html=cg::report_html(current,&before);
    require(html.find("<img")==std::string::npos&&html.find("<script")==std::string::npos&&html.find("&lt;/script&gt;")!=std::string::npos,"evidence cannot inject HTML or script");
    require(html.find("解析失败")!=std::string::npos&&html.find("未知")!=std::string::npos&&html.find("CodeGuardReport")==std::string::npos,"standalone human-readable report");
    const auto output=fixture.dir/"output";const auto files=cg::export_report(current,output,&before);
    require(fs::file_size(files.html)>1000&&fs::file_size(files.json)>1000,"both report formats written");
    const auto original_size=fs::file_size(files.html);rejects([&]{cg::export_report(current,output,&before);});require(fs::file_size(files.html)==original_size,"existing report not replaced");
    rejects([&]{cg::export_report(current,cg::from_utf8(current.root)/"report",&before);});require(!fs::exists(cg::from_utf8(current.root)),"report cannot create source directories");
    auto wrong=current;wrong.build_runs.front().scan_id=1;rejects([&]{cg::report_json(wrong,&before);});
    std::cout<<"REPORT_ARTIFACTS "<<cg::utf8_path(output)<<'\n';
}
void cli(const std::string& executable){
    Fixture fixture;const auto database=fixture.dir/"scan.sqlite3";cg::ScanResult first;first.root=cg::utf8_path(fixture.dir/"missing-source");first.analysis.status="complete";first.scanned_at="2026-09-13";
    auto second=first;second.analysis.status="failed";
    {cg::SqliteDatabase db(database);db.save(first);db.save(second);}
    auto call=[&](std::vector<std::string> args,int expected){args.insert(args.begin()+1,first.root);args.insert(args.end(),{"--database",cg::utf8_path(database)});args.insert(args.begin(),executable);const auto result=cg::run_process({args,cg::utf8_path(fixture.dir)});require(result.exit_code==expected,("report CLI: "+result.stdout_text+result.stderr_text).c_str());return result.stdout_text;};
    require(call({"scans","--limit","1"},0).find("failed")!=std::string::npos,"history CLI shows most recent failed scan");
    require(call({"scans","--before",std::to_string(second.id)},0).find("complete")!=std::string::npos,"history CLI pagination");
    const auto output=cg::utf8_path(fixture.dir/"export");
    require(call({"report","--scan",std::to_string(second.id),"--baseline",std::to_string(first.id),"--output",output},0).find("analysis=failed")!=std::string::npos,"export succeeds while describing failed scan honestly");
    call({"report","--output",output},1);call({"report","--scan","0","--output",output+"-zero"},1);
    call({"report","--scan",std::to_string(first.id),"--baseline",std::to_string(second.id),"--output",output+"-reverse"},1);
    call({"scans","--limit","0"},1);call({"report","--output",output+"-invalid","--threads","4"},1);
}
void gui(const std::string& executable){
    Fixture fixture;fixture.keep=true;const auto source=fixture.dir/"source";fs::create_directory(source);
    {std::ofstream out(source/"main.cpp");out<<"int probe(){int a[2]={};return a[2];}\n";}
    const auto commands=fixture.dir/"compile_commands.json";
    {std::ofstream out(commands);out<<"[{\"directory\":\""<<cg::utf8_path(source)<<"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\"main.cpp\"]}]";}
    const auto result=cg::run_process({{executable,"--configuration-smoke","reports","--project",cg::utf8_path(source),"--database",cg::utf8_path(fixture.dir/"scan.sqlite3"),"--compile-commands",cg::utf8_path(commands),"--session-file",cg::utf8_path(fixture.dir/"session.ini")},cg::utf8_path(fixture.dir),std::chrono::seconds(50)});
    require(result.exit_code==0&&result.stdout_text.find("GUI_REPORT_OK")!=std::string::npos,("report GUI: "+result.status+" exit="+std::to_string(result.exit_code)+" "+result.stdout_text+result.stderr_text).c_str());
    std::ifstream in(fixture.dir/"gui-report/report.json");const std::string payload((std::istreambuf_iterator<char>(in)),{});
    require(json_value(payload,"$.comparison.changes[0].status")=="persistent","GUI export connects selected history and baseline");
    std::cout<<"REPORT_GUI_ARTIFACTS "<<cg::utf8_path(fixture.dir)<<'\n';
}
int main(int argc,char** argv){try{require(argc>=2,"mode required");const std::string mode=argv[1];if(mode=="history")history();else if(mode=="legacy")legacy();else if(mode=="provenance")provenance();else if(mode=="comparison")comparison();else if(mode=="export")exporting();else if(mode=="cli"){require(argc==3,"CLI path required");cli(argv[2]);}else if(mode=="gui"){require(argc==3,"GUI path required");gui(argv[2]);}else throw std::runtime_error("unknown report test mode");std::cout<<"REPORT_OK "<<mode<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
