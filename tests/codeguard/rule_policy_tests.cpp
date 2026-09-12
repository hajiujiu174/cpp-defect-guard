#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <tuple>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f,const std::string& why){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,why);}
struct Fixture {
    fs::path dir,root,db;bool keep=false;
    Fixture(){dir=fs::current_path()/("rule-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));root=dir/"source";db=dir/"scan.sqlite3";fs::create_directories(root);}
    ~Fixture(){if(!keep){std::error_code e;fs::remove_all(dir,e);}}
    void write(const std::string& file,const std::string& contents){std::ofstream out(root/file);out<<contents;check(bool(out),"write fixture");}
    std::string commands(){std::ofstream out(dir/"compile_commands.json");out<<'[';bool first=true;for(const auto& item:fs::directory_iterator(root))if(item.path().extension()==".cpp"){
        if(!first)out<<',';first=false;out<<"{\"directory\":\""<<cg::utf8_path(root)<<"\",\"file\":\""<<cg::utf8_path(item.path().filename())<<"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""<<cg::utf8_path(item.path().filename())<<"\"]}";
    }out<<']';return cg::utf8_path(dir/"compile_commands.json");}
};
void policy(){
    Fixture f;f.write("main.cpp","int value;\n");auto original=cg::import_project(f.root,f.db);
    original.analysis.status="complete";original.analysis.issues={{"CG001","warning","main.cpp",1,1,"message","evidence","suggestion","function"}};
    cg::ProjectConfig config;config.rule_severities["CG001"]="info";config.suppressions={cg::suppress_issue(original,original.analysis.issues.front(),"reviewed \"capacity\"\nsecond line 中文")};
    rejects([&]{cg::suppress_issue(original,original.analysis.issues.front()," \t");},"empty reason rejected");
    auto encoded=cg::encode_config(config);check(cg::encode_config(cg::decode_config(encoded))==encoded,"policy codec");
    auto old=cg::encode_config({});old.replace(old.find("Config 2"),8,"Config 1");old.resize(old.size()-4);
    check(cg::decode_config(old).suppressions.empty(),"version 1 config migration");
    auto bad=config;bad.rule_severities["CG001"]="urgent";rejects([&]{cg::validate_config(bad);},"invalid severity rejected");
    bad=config;bad.suppressions.push_back(bad.suppressions.front());rejects([&]{cg::validate_config(bad);},"duplicate suppression rejected");
    cg::save_project_config(f.root,f.db,config);config=*cg::load_project_config(f.root,f.db);
    auto suppressed=original;cg::apply_rule_policy(suppressed,config);
    check(suppressed.analysis.issues.empty()&&suppressed.analysis.suppressed_issues.size()==1&&suppressed.analysis.suppressed_issues[0].severity=="info","reasoned suppression and severity applied");
    {cg::SqliteDatabase db(f.db);db.save(suppressed);}
    cg::SqliteDatabase db(f.db,true);auto restored=db.latest(original.root);
    check(restored.analysis.suppressed_issues[0].suppression_reason==config.suppressions[0].reason,"persist suppressed evidence/reason");
    check(cg::execute_query(restored,"SELECT reason FROM suppressed_issues").rows.size()==1,"suppression query");
    auto changed=original;changed.files.front().hash="fnv1a64-v1:0000000000000000";cg::apply_rule_policy(changed,config);
    check(changed.analysis.issues.size()==1&&changed.analysis.suppressed_issues.empty()&&changed.analysis.rule_diagnostics.front().find("source changed")!=std::string::npos,"source changes invalidate suppression");
    auto failed=original;failed.analysis.issues.clear();failed.analysis.status="failed";cg::apply_rule_policy(failed,config);
    check(failed.analysis.status=="failed"&&!failed.analysis.rule_diagnostics.empty(),"parse failure remains failed with unapplied policy");
    {cg::SqliteDatabase write(f.db);write.save(failed);}
    check(!db.latest(original.root).analysis.rule_diagnostics.empty(),"policy diagnostics persist");
    check(cg::execute_query(db.latest(original.root),"SELECT message FROM rule_diagnostics").rows.size()==1,"diagnostics query");
    for(const auto& rule:cg::rule_catalog())for(const auto& level:{"info","warning","error"}){
        auto sample=original;sample.analysis.issues.front().rule_id=rule.id;sample.analysis.issues.front().severity=rule.severity;
        cg::ProjectConfig settings;settings.rule_severities[rule.id]=level;
        settings.suppressions={cg::suppress_issue(sample,sample.analysis.issues.front(),"reviewed "+rule.id)};
        cg::save_project_config(f.root,f.db,settings);
        cg::apply_rule_policy(sample,*cg::load_project_config(f.root,f.db));
        check(sample.analysis.issues.empty()&&sample.analysis.suppressed_issues.size()==1&&sample.analysis.suppressed_issues.front().severity==level,"all rule severity/suppression settings survive reload");
    }
}
void migration(){
    Fixture f;f.write("main.cpp","int value;\n");const auto saved=cg::import_project(f.root,f.db);
    sqlite3* raw=nullptr;check(sqlite3_open(cg::utf8_path(f.db).c_str(),&raw)==SQLITE_OK,"open migration fixture");
    check(sqlite3_exec(raw,"DROP TABLE suppressed_issue;DROP TABLE rule_diagnostic;PRAGMA user_version=4;",nullptr,nullptr,nullptr)==SQLITE_OK,"make schema 4");sqlite3_close(raw);
    {cg::SqliteDatabase db(f.db,true);check(db.latest(saved.root).files.size()==1,"schema 4 remains readable");}
    {cg::SqliteDatabase db(f.db);check(db.latest(saved.root).id==saved.id,"schema 5 preserves snapshot");}
    check(sqlite3_open(cg::utf8_path(f.db).c_str(),&raw)==SQLITE_OK,"reopen schema");sqlite3_stmt* stmt=nullptr;sqlite3_prepare_v2(raw,"PRAGMA user_version",-1,&stmt,nullptr);sqlite3_step(stmt);check(sqlite3_column_int(stmt,0)==5,"migration version");sqlite3_finalize(stmt);sqlite3_close(raw);
}
void evaluation(const std::string& rule,const fs::path& corpus){
    Fixture f;std::ifstream manifest(corpus/"manifest.tsv");check(bool(manifest),"read labeled corpus");std::string line;std::getline(manifest,line);
    struct Case{std::string name,label;int expected;};std::vector<Case> cases;
    while(std::getline(manifest,line)){std::istringstream row(line);std::string id,name,label;int expected=-1;row>>id>>name>>label>>expected;
        if(id!=rule)continue;check(expected>=0&&(label=="positive"||label=="negative"),"invalid corpus row");const auto filename=rule+"_"+name+".cpp";fs::copy_file(corpus/filename,f.root/filename);cases.push_back({filename,label,expected});}
    check(cases.size()>=8,"insufficient labeled cases");cg::ProjectConfig config;config.compile_commands=f.commands();config.threads=4;
    for(const auto& info:cg::rule_catalog())if(info.id!=rule)config.disabled_rules.push_back(info.id);
    const auto result=cg::import_project(f.root,f.db,cg::configured_scan_options(f.root,config));
    for(const auto& unit:result.analysis.units)check(unit.status=="success","unparsed evaluation file: "+unit.file+"\n"+unit.diagnostics);
    check(result.analysis.status=="complete","evaluation coverage is complete");int tp=0,fp=0,tn=0,fn=0;bool baseline=true;
    for(const auto& test:cases){std::set<std::pair<int,int>> sites;for(const auto& issue:result.analysis.issues){check(issue.rule_id==rule,"disabled rule executed");check(!issue.evidence.empty(),"macro evidence missing");if(issue.file==test.name)sites.emplace(issue.line,issue.column);}
        const bool detected=!sites.empty();if(test.label=="positive"){if(detected)++tp;else ++fn;}else{if(detected)++fp;else ++tn;}
        if(sites.size()!=static_cast<std::size_t>(test.expected)){baseline=false;std::cerr<<test.name<<" expected="<<test.expected<<" actual="<<sites.size()<<'\n';}
        std::cout<<"CASE "<<test.name<<" label="<<test.label<<" sites="<<sites.size()<<'\n';
    }
    std::cout<<"RULE_EVALUATION {\"rule\":\""<<rule<<"\",\"scope\":\"labeled synthetic cases only; location detection\",\"cases\":"<<cases.size()<<",\"TP\":"<<tp<<",\"FP\":"<<fp<<",\"TN\":"<<tn<<",\"FN\":"<<fn<<",\"parse_failed\":0}\n";
    check(baseline,"evaluation changed; inspect case labels and detector before updating the baseline");
}
void cli(const std::string& program){
    Fixture f;f.write("main.cpp","int probe(){int a[2]={};return a[2];}\n");auto commands=f.commands();
    auto call=[&](std::vector<std::string> args,int expected=0){args.insert(args.begin()+1,cg::utf8_path(f.root));args.insert(args.end(),{"--database",cg::utf8_path(f.db)});args.insert(args.begin(),program);auto result=cg::run_process({args,cg::utf8_path(f.dir)});check(result.exit_code==expected,"CLI: "+result.stdout_text+result.stderr_text);return result.stdout_text;};
    call({"config","--compile-commands",commands,"--rule-severity","CG002=info"});call({"scan"});cg::ScanResult saved;
    {cg::SqliteDatabase db(f.db,true);saved=db.latest(cg::utf8_path(f.root));}check(saved.analysis.issues.size()==1&&saved.analysis.issues.front().severity=="info","CLI severity honored");
    const auto& issue=saved.analysis.issues.front();const auto args=std::vector<std::string>{"suppress","--rule",issue.rule_id,"--file",issue.file,"--line",std::to_string(issue.line),"--column",std::to_string(issue.column),"--reason","test reviewed finding"};
    auto unrelated=args;unrelated.insert(unrelated.end(),{"--threads","2"});call(unrelated,1);
    auto empty=args;empty.back()=" ";call(empty,1);call(args);call({"scan"});check(call({"suppressed"}).find("test reviewed finding")!=std::string::npos,"CLI displays suppression reason");
    call({"config","--clear-list","suppressions","--clear-list","severities"});call({"scan"});check(call({"issues","--severity","error"}).find("CG002")!=std::string::npos,"clear policy restores default finding");
}
void coverage(){
    Fixture f;f.write("main.cpp","int probe(){int a[2]={};return a[2];}\n");cg::ProjectConfig config;config.compile_commands=f.commands();
    const auto good=cg::import_project(f.root,f.db,cg::configured_scan_options(f.root,config));check(good.analysis.issues.size()==1,"positive fixture");
    config.suppressions={cg::suppress_issue(good,good.analysis.issues.front(),"test reason")};
    for(const auto& rule:cg::rule_catalog())config.disabled_rules.push_back(rule.id);
    auto disabled=cg::import_project(f.root,f.db,cg::configured_scan_options(f.root,config));
    check(disabled.analysis.status=="complete"&&disabled.analysis.issues.empty()&&disabled.analysis.rule_diagnostics.size()==1,"all disabled still parses and reports unapplied suppression");
    f.write("main.cpp","int probe(){int a[2]={};return a[2];}\ninvalid C++ !!!\n");config.disabled_rules.clear();
    auto failed=cg::import_project(f.root,f.db,cg::configured_scan_options(f.root,config));
    check(failed.analysis.status=="failed"&&failed.analysis.units.front().status=="parse_failed"&&failed.analysis.issues.empty()&&failed.analysis.suppressed_issues.empty(),"partial AST cannot claim defect-free scan");
    check(!failed.analysis.rule_diagnostics.empty(),"unapplied suppression remains visible on parse failure");
}
void gui(const std::string& program){
    Fixture f;f.keep=true;f.write("main.cpp","int probe(){int a[2]={};return a[2];}\n");const auto commands=f.commands();
    auto result=cg::run_process({{program,"--configuration-smoke","rules","--project",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--compile-commands",commands,"--session-file",cg::utf8_path(f.dir/"session.ini")},cg::utf8_path(f.dir),std::chrono::seconds(50)});
    check(result.exit_code==0&&result.stdout_text.find("GUI_RULE_POLICY_OK")!=std::string::npos,"GUI rule workflow: "+result.stdout_text+result.stderr_text);
    std::cout<<"GUI rule artifacts: "<<cg::utf8_path(f.dir)<<'\n';
}
int main(int argc,char** argv){try{check(argc>=2,"mode required");const std::string mode=argv[1];if(mode=="policy")policy();else if(mode=="migration")migration();else if(mode=="coverage")coverage();else if(mode=="cli"){check(argc==3,"CLI path required");cli(argv[2]);}else if(mode=="gui"){check(argc==3,"GUI path required");gui(argv[2]);}else{check(argc==3,"corpus path required");evaluation(mode,cg::from_utf8(argv[2]));}std::cout<<"RULE_TEST_OK "<<mode<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
