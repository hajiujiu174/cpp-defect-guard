#include "codeguard/application.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;
namespace fs=std::filesystem;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f,const std::string& message){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,message);}
struct Fixture {
    fs::path directory,root,database;bool keep=false;
    Fixture(){directory=fs::current_path()/("config-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));root=directory/fs::path(u8"中文 source");fs::create_directories(root);database=directory/"analysis.sqlite3";}
    ~Fixture(){if(!keep){std::error_code error;fs::remove_all(directory,error);}}
    void write(const std::string& file,const std::string& data){const auto p=root/cg::from_utf8(file);fs::create_directories(p.parent_path());std::ofstream out(p);out<<data;check(bool(out),"write fixture");}
    void sql(const char* sql){sqlite3* db=nullptr;check(sqlite3_open(cg::utf8_path(database).c_str(),&db)==SQLITE_OK,"sqlite open");const auto code=sqlite3_exec(db,sql,nullptr,nullptr,nullptr);const std::string error=sqlite3_errmsg(db);sqlite3_close(db);check(code==SQLITE_OK,error);}
    void cmake(){
        write("main.cpp","#include \"generated.h\"\nint main(){return CG_VALUE==42?0:1;}\n");
        write("excluded/broken.cpp","not valid C++\n");
        write("resources/out/required.txt","copy include\n");
        write("CMakeLists.txt",R"cmake(cmake_minimum_required(VERSION 3.24)
project(ConfigFixture LANGUAGES CXX)
if(NOT CG_LABEL STREQUAL "with spaces")
  message(FATAL_ERROR "missing saved definition")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/out/required.txt" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/excluded")
  message(FATAL_ERROR "wrong copy scope")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated.h" "#define CG_VALUE 42\n")
add_executable(demo main.cpp)
target_include_directories(demo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
enable_testing()
add_test(NAME demo COMMAND demo)
)cmake");
    }
    cg::ProjectConfig config(){cg::ProjectConfig c;c.build.output_directory=directory/"workspaces";c.build.cmake_definitions={"CG_LABEL=with spaces"};c.build.copy_includes={"resources/out"};c.build.copy_excludes={"excluded"};c.build.target="demo";c.build.jobs=2;c.threads=2;c.disabled_rules={"CG004"};return c;}
    void commands(bool changed=false){
        std::ofstream out(root/"compile_commands.json");const auto path=cg::utf8_path(root);
        out<<"[{\"directory\":\""<<path<<"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-c\",\"main.cpp\",\"-DMODE="<<(changed?3:1)<<"\"]},"
           <<"{\"directory\":\""<<path<<"\",\"file\":\"main.cpp\",\"arguments\":[\"clang++\",\"-c\",\"main.cpp\",\"-DMODE=2\"]}]";
    }
};
void run(const std::string& name,const std::string& cli){
    Fixture f;
    if(name=="profiles"){
        auto c=f.config();c.build.cmake_definitions.push_back("QUOTED=hello \"world\" \\ path\nnext");
        const auto encoded=cg::encode_config(c);check(cg::encode_config(cg::decode_config(encoded))==encoded,"lossless codec");
        cg::save_project_config(f.root,f.database,c);const auto other=f.directory/"other";fs::create_directory(other);
        auto second=c;second.build.target="other";cg::save_project_config(other,f.database,second);
        check(cg::encode_config(*cg::load_project_config(f.root,f.database))==encoded,"new connection restores full profile");
        check(cg::load_project_config(other,f.database)->build.target=="other","independent project profiles");
        rejects([&]{cg::save_project_config(f.root,f.root/"bad.sqlite3",c);},"database inside source rejected");
        auto bad=c;bad.build.output_directory=f.root/"output";rejects([&]{cg::save_project_config(f.root,f.database,bad);},"source output rejected");
        return;
    }
    if(name=="invalid_profile"){
        const auto c=f.config();cg::save_project_config(f.root,f.database,c);
        for(const auto& payload:std::vector<std::string>{"CodeGuardProjectConfig 99\n",cg::encode_config(c)+"trailing",cg::encode_config(c).substr(0,30)})rejects([&]{cg::decode_config(payload);},"malformed codec rejected");
        for(const auto& path:std::vector<std::string>{"../outside",".git","a/../b","."}){auto bad=c;bad.build.copy_excludes={path};rejects([&]{cg::validate_config(bad);},"unsafe exclusion rejected");}
        auto bad=c;bad.build.cmake_definitions={"CMAKE_EXPORT_COMPILE_COMMANDS=OFF"};rejects([&]{cg::validate_config(bad);},"reserved definitions rejected");
        f.sql("UPDATE project_configuration SET payload='CodeGuardProjectConfig 99'");
        rejects([&]{cg::load_project_config(f.root,f.database);},"future profile read rejected");
        rejects([&]{cg::save_project_config(f.root,f.database,c);},"future profile cannot be overwritten");
        return;
    }
    if(name=="discovery"){
        check(cg::discover_compilation_databases(f.root).empty(),"no DB");f.write("build/debug/compile_commands.json","[]");
        check(cg::discover_compilation_databases(f.root).size()==1,"discover ignored build directory");
        f.write(".git/compile_commands.json","[]");f.write("a/b/c/d/compile_commands.json","[]");check(cg::discover_compilation_databases(f.root).size()==1,"bounded discovery");
        f.write("out/release/compile_commands.json","[]");check(cg::discover_compilation_databases(f.root).size()==2,"retain multiple candidates");
        if(cg::clang_analysis_available())rejects([&]{cg::configured_scan_options(f.root,{});},"no silent multi DB choice");
        return;
    }
    if(name=="config_migration"){
        f.write("main.c","int main(void){return 0;}\n");const auto saved=cg::import_project(f.root,f.database);
        f.sql("DROP TABLE project_configuration; ALTER TABLE analysis DROP COLUMN configuration; ALTER TABLE build_run DROP COLUMN compile_commands; ALTER TABLE build_run DROP COLUMN configuration; PRAGMA user_version=3;");
        {cg::SqliteDatabase db(f.database,true);check(db.latest(saved.root).id==saved.id,"read old scan");check(!cg::load_project_config(f.root,f.database),"old profile defaults");}
        // A read-only load must not migrate: ALTER fails if the field already exists.
        f.sql("ALTER TABLE analysis ADD COLUMN configuration TEXT NOT NULL DEFAULT ''; ALTER TABLE analysis DROP COLUMN configuration;");
        cg::save_project_config(f.root,f.database,f.config());cg::SqliteDatabase db(f.database,true);
        check(db.latest(saved.root).files.front().hash==saved.files.front().hash,"migration preserves historical content");return;
    }
    if(name=="command_selection"){
        f.write("main.cpp","#if MODE == 1\nint selected_one(){return *static_cast<int*>(nullptr);}\n#else\nint selected_two(){return 2;}\n#endif\n");f.commands();
        auto c=f.config();c.compile_commands=cg::utf8_path(f.root/"compile_commands.json");c.disabled_rules.clear();
        const auto ambiguous=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));check(ambiguous.analysis.units.front().status=="ambiguous_command","default ambiguity remains");
        const auto commands=cg::inspect_compile_commands(c.compile_commands);check(commands.size()==2&&commands[0].fingerprint!=commands[1].fingerprint,"unique command identities");
        c.command_choices[commands[0].file]=commands[0].fingerprint;
        const auto selected=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));
        check(selected.analysis.status=="complete"&&!selected.analysis.issues.empty(),"selected macro produces expected finding");
        check(!cg::find_symbols(selected.analysis,"selected_one").empty()&&cg::find_symbols(selected.analysis,"selected_two").empty(),"only explicit macro variant analyzed");
        const auto rule=selected.analysis.issues.front().rule_id;c.disabled_rules={rule};const auto filtered=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));
        check(std::none_of(filtered.analysis.issues.begin(),filtered.analysis.issues.end(),[&](const auto& i){return i.rule_id==rule;}),"rule filtered");
        check(cg::decode_config(filtered.analysis.configuration).disabled_rules==c.disabled_rules,"snapshot records filtering");
        c.command_choices[commands[0].file]=commands[1].fingerprint;c.disabled_rules.clear();const auto alternate=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));
        check(!cg::find_symbols(alternate.analysis,"selected_two").empty(),"alternate command works");
        c.command_choices[commands[0].file]=commands[0].fingerprint;f.commands(true);rejects([&]{cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));},"stale choice rejected");
        cg::SqliteDatabase db(f.database,true);check(db.latest(alternate.root).id==alternate.id,"stale choice does not save snapshot");return;
    }
    if(name=="prepare_config"){
        f.cmake();auto c=f.config();const auto inventory=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));check(inventory.files.size()==1,"excluded files omitted");
        auto b=c.build;b.configure_only=true;auto prepared=cg::build_and_test(f.root,f.database,b);
        check(prepared.status=="configured"&&prepared.source_unchanged,"configure only with generated header");
        check(prepared.steps.size()==2,"prepare does not build or test");c.compile_commands=prepared.compile_commands;
        const auto analyzed=cg::import_project(f.root,f.database,cg::configured_scan_options(f.root,c));check(analyzed.analysis.status=="complete"&&analyzed.analysis.units.size()==1,"rebased original source with generated include");
        const auto built=cg::build_and_test(f.root,f.database,c.build);check(built.status=="passed"&&built.steps.back().tests_total==1,"build/test from saved config");
        check(cg::decode_config(built.configuration).build.copy_excludes==c.build.copy_excludes,"build configuration roundtrip");
        b.cmake="missing-codeguard-cmake-tool";check(cg::build_and_test(f.root,f.database,b).status!="configured","missing tool surfaced");
        b.cmake=c.build.cmake;f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(Bad LANGUAGES CXX)\nfile(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/new.cpp\" \"int generated;\")\nadd_library(bad main.cpp)\n");
        const auto failed=cg::build_and_test(f.root,f.database,b);check(failed.status=="failed"&&failed.compile_commands.empty()&&!fs::exists(f.root/"new.cpp"),"reject copied source mutation without touching original");return;
    }
    if(name=="configuration_cli"){
        f.cmake();auto invoke=[&](std::vector<std::string> args,int expected=0){args.insert(args.begin()+1,cg::utf8_path(f.root));args.insert(args.end(),{"--database",cg::utf8_path(f.database)});args.insert(args.begin(),cli);
            auto result=cg::run_process({args,cg::utf8_path(f.directory),std::chrono::seconds(90)});check(result.exit_code==expected,"CLI failed: "+cg::format_arguments(args)+"\n"+result.stdout_text+result.stderr_text);return result.stdout_text;};
        invoke({"config","--output",cg::utf8_path(f.directory/"cli-work"),"--target","demo","--cmake-define","CG_LABEL=with spaces","--copy-include","resources/out","--copy-exclude","excluded","--disable-rule","CG004","--threads","2"});
        check(invoke({"config"}).find("disabled_rule\tCG004")!=std::string::npos,"new CLI process loads settings");
        invoke({"prepare"});invoke({"scan"});invoke({"build"});
        const auto original=cg::load_project_config(f.root,f.database);check(original&&!original->compile_commands.empty(),"prepare saves generated path");
        invoke({"config","--clear-list","rules"});check(cg::load_project_config(f.root,f.database)->disabled_rules.empty(),"explicit clear list persists");return;
    }
    if(name=="configuration_gui"){
        f.cmake();f.keep=true;
        const auto session=cg::utf8_path(f.directory/"session.ini");
        auto invoke=[&](const std::vector<std::string>& args,const std::string& marker){
            auto result=cg::run_process({args,cg::utf8_path(f.directory),std::chrono::seconds(110)});
            check(result.exit_code==0&&result.stdout_text.find(marker)!=std::string::npos,"GUI failed: "+result.stdout_text+result.stderr_text);
        };
        invoke({cli,"--configuration-smoke","import","--project",cg::utf8_path(f.root),"--database",cg::utf8_path(f.database),"--session-file",session},"GUI_CONFIG_IMPORT_OK");
        std::int64_t id=0;{cg::SqliteDatabase db(f.database,true);id=db.latest(cg::utf8_path(f.root)).id;}
        invoke({cli,"--configuration-smoke","restore","--session-file",session},"GUI_CONFIG_RESTORE_OK");
        cg::SqliteDatabase db(f.database,true);check(db.latest(cg::utf8_path(f.root)).id==id,"restart does not rescan");
        std::cout<<"GUI artifacts: "<<cg::utf8_path(f.directory)<<'\n';return;
    }
    throw std::invalid_argument("unknown test");
}
int main(int argc,char** argv){try{check(argc>=2,"case required");run(argv[1],argc>2?argv[2]:"");std::cout<<"CONFIG_OK "<<argv[1]<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
