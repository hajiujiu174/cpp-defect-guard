#include "codeguard/application.hpp"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cg=codeguard;
namespace fs=std::filesystem;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
std::string trim(std::string value){while(!value.empty()&&(value.back()=='\n'||value.back()=='\r'))value.pop_back();return value;}
// Only the current test process and its children are affected, never registry
// or user/system environment settings.
struct Environment {
    std::string name;
#ifdef _WIN32
    std::optional<std::wstring> previous;
    Environment(std::string key,std::optional<std::string> value):name(std::move(key)) {
        const auto wide=cg::from_utf8(name).native();SetLastError(ERROR_SUCCESS);const auto count=GetEnvironmentVariableW(wide.c_str(),nullptr,0);
        if(count){std::wstring old(count,L'\0');GetEnvironmentVariableW(wide.c_str(),old.data(),count);old.resize(count-1);previous=std::move(old);}
        else if(GetLastError()!=ERROR_ENVVAR_NOT_FOUND)previous=std::wstring{};
        check(SetEnvironmentVariableW(wide.c_str(),value?cg::from_utf8(*value).c_str():nullptr),"set test environment");
    }
    ~Environment(){const auto wide=cg::from_utf8(name).native();SetEnvironmentVariableW(wide.c_str(),previous?previous->c_str():nullptr);}
#else
    std::optional<std::string> previous;
    Environment(std::string key,std::optional<std::string> value):name(std::move(key)){if(const auto old=std::getenv(name.c_str()))previous=old;check((value?setenv(name.c_str(),value->c_str(),1):unsetenv(name.c_str()))==0,"set test environment");}
    ~Environment(){if(previous)setenv(name.c_str(),previous->c_str(),1);else unsetenv(name.c_str());}
#endif
};
struct Fixture {
    fs::path directory,root,empty;
    explicit Fixture(bool outside=false) {
        directory=(outside?fs::temp_directory_path():fs::current_path())/("git-provenance-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(fs::create_directory(directory),"unique test directory");root=directory/fs::path(u8"中文 source");empty=directory/"empty-template";fs::create_directory(root);fs::create_directory(empty);
    }
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"FIXTURE_RETAINED="<<cg::utf8_path(directory)<<'\n';return;}std::error_code error;fs::remove_all(directory,error);}
    void write(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<text;check(bool(out),"write test source");}
    cg::ProcessResult git(const fs::path& where,std::vector<std::string> args) {
        std::vector<std::string> command{"git","--no-pager","--no-optional-locks","-c","core.hooksPath="+cg::utf8_path(empty),"-c","commit.gpgsign=false","-c","core.excludesFile="+cg::utf8_path(empty/"ignore"),"-c","core.attributesFile="+cg::utf8_path(empty/"attributes"),"-C",cg::utf8_path(where)};
        command.insert(command.end(),args.begin(),args.end());auto result=cg::run_process({command,cg::utf8_path(where)});
        check(result.status=="passed","git fixture: "+result.stderr_text);return result;
    }
    void init(const fs::path& where){fs::create_directories(where);git(where,{"init","--template="+cg::utf8_path(empty),"-b","main"});}
    std::string repository(const fs::path& where,const std::string& text="int main(){return 0;}\n") {
        init(where);write(where/"main.cpp",text);git(where,{"add","main.cpp"});
        git(where,{"-c","user.name=CodeGuard Test","-c","user.email=test@example.invalid","commit","-m","fixture"});
        return trim(git(where,{"rev-parse","HEAD"}).stdout_text);
    }
};
int main(int argc,char** argv){try{
    check(argc==4,"CASE CLI PROTOCOL_HELPER required");const std::string mode=argv[1];Fixture f(mode=="missing");
    if(mode=="owned") {
        const auto head=f.repository(f.root);f.write(f.root/"main.cpp","int main(){return 7;}\n");const auto index=f.root/".git/index";const auto before=fs::last_write_time(index);
        std::string revision="stale";const auto log=cg::read_git(f.root,{},&revision);
        check(revision==head&&log.find("source_git_status=owned_root")!=std::string::npos&&log.find("+int main(){return 7;}")!=std::string::npos,"owned revision and diff");
        check(fs::last_write_time(index)==before,"Git read must not refresh index");
    } else if(mode=="parent"||mode=="build"||mode=="cli") {
        const auto head=f.repository(f.root);const auto source=f.root/"extracted";fs::create_directory(source);
        f.write(source/"main.cpp","int main(){return 0;}\n");f.write(f.root/"parent-secret.txt","PARENT_ONLY_SENTINEL\n");
        if(mode=="parent") {
            std::string revision="stale";const auto log=cg::read_git(source,{},&revision);
            check(revision.empty()&&log.find("no_local_metadata")!=std::string::npos,"nested extracted project has unknown revision");
            check(log.find(head)==std::string::npos&&log.find("PARENT_ONLY_SENTINEL")==std::string::npos&&log.find("$ ")==std::string::npos,"parent metadata not collected");
        } else if(mode=="cli") {
            auto r=cg::run_process({{argv[2],"git",cg::utf8_path(source)},cg::utf8_path(f.directory)});
            check(r.exit_code==2&&r.stdout_text.find("no_local_metadata")!=std::string::npos&&r.stdout_text.find(head)==std::string::npos,"CLI rejects parent attribution");
            r=cg::run_process({{argv[2],"git",cg::utf8_path(f.root)},cg::utf8_path(f.directory)});
            check(r.status=="passed"&&r.stdout_text.find(head)!=std::string::npos,"CLI owned root succeeds");
        } else {
            f.write(source/"CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(GitFixture LANGUAGES CXX)\nadd_executable(demo main.cpp)\nenable_testing()\nadd_test(NAME demo COMMAND demo)\n");
            const auto db=f.directory/"scan.sqlite3";const auto scan=cg::import_project(source,db);cg::BuildOptions options;options.output_directory=f.directory/"runs";options.jobs=2;
            const auto run=cg::build_and_test(source,db,options);
            check(run.status=="passed"&&run.git_revision.empty()&&run.git_log.find("no_local_metadata")!=std::string::npos,"build still works without owned Git metadata");
            cg::SqliteDatabase saved(db,true);const auto records=saved.builds(scan.root,true);
            check(records.size()==1&&records[0].git_revision.empty()&&records[0].git_log.find(head)==std::string::npos,"unknown ownership persists without parent revision");
        }
    } else if(mode=="missing") {
        cg::BuildOptions options;options.git="must_not_run_nonexistent_git_824";std::string revision="stale";
        const auto log=cg::read_git(f.root,options,&revision);check(revision.empty()&&log.find("no_local_metadata")!=std::string::npos&&log.find("$ ")==std::string::npos,"no Git metadata needs no Git process");
        revision="stale";check(cg::read_git(f.root/"missing",options,&revision).find("unavailable")!=std::string::npos&&revision.empty(),"missing source is unavailable metadata");
    } else if(mode=="unborn") {
        f.init(f.root);std::string revision="stale";const auto log=cg::read_git(f.root,{},&revision);
        check(revision.empty()&&log.find("owned_root")!=std::string::npos,"unborn repository cannot retain stale revision");
    } else if(mode=="worktree") {
        const auto head=f.repository(f.root);const auto source=f.directory/fs::path(u8"链接工作树");f.git(f.root,{"worktree","add","--detach",cg::utf8_path(source),"HEAD"});
        check(fs::is_regular_file(source/".git"),"linked worktree Git file");std::string revision;const auto log=cg::read_git(source,{},&revision);
        check(revision==head&&log.find("owned_root")!=std::string::npos,"linked worktree retains its revision");
    } else if(mode=="submodule") {
        f.repository(f.root);const auto module=f.directory/"module-repository";const auto head=f.repository(module,"int module;\n");
        f.git(f.root,{"-c","protocol.file.allow=always","submodule","add",cg::utf8_path(module),"module"});
        const auto source=f.root/"module";check(fs::is_regular_file(source/".git"),"submodule Git file");
        std::string revision;cg::read_git(source,{},&revision);check(revision==head,"submodule keeps its own revision");
    } else if(mode=="nested") {
        const auto parent=f.repository(f.root);const auto source=f.root/"child";const auto head=f.repository(source,"int child;\n");
        std::string revision;cg::read_git(source,{},&revision);check(head!=parent&&revision==head,"nested owned repository uses its own HEAD");
    } else if(mode=="environment") {
        const auto head=f.repository(f.root);const auto other=f.directory/"foreign";const auto foreign=f.repository(other,"int foreign;\n");
        Environment dir("GIT_DIR",cg::utf8_path(other/".git")),work("GIT_WORK_TREE",cg::utf8_path(other));
        std::string revision;const auto log=cg::read_git(f.root,{},&revision);check(head!=foreign&&revision==head&&log.find(foreign)==std::string::npos,"explicit source paths override inherited Git directory/worktree");
    } else if(mode=="unavailable"||mode=="cancel") {
        f.repository(f.root);cg::BuildOptions options;
        if(mode=="unavailable")options.git="nonexistent_git_provenance_824";
        else {options.control=std::make_shared<cg::ScanControl>();options.control->request_cancel();}
        std::string revision="stale";const auto log=cg::read_git(f.root,options,&revision);check(revision.empty()&&log.find(mode=="cancel"?"cancelled":"start_failed")!=std::string::npos,"failed/cancelled probe clears stale revision");
    } else if(mode=="invalid") {
        f.init(f.root);cg::BuildOptions options;options.git=argv[3];
        for(const auto& value:{"failed_probe","mismatch","invalid_owner","invalid_head","truncated_head"}) {
            Environment protocol("CODEGUARD_GIT_PROTOCOL_MODE",value);std::string revision="stale";const auto log=cg::read_git(f.root,options,&revision);
            check(revision.empty(),std::string("unverified protocol must not supply a revision: ")+value);
            if(std::string(value)=="truncated_head")check(log.find("[output truncated]")!=std::string::npos,"truncation recorded");
        }
        Environment protocol("CODEGUARD_GIT_PROTOCOL_MODE","valid_sha256");std::string revision;cg::read_git(f.root,options,&revision);check(revision==std::string(64,'a'),"complete SHA-256 revision protocol accepted");
    } else throw std::invalid_argument("unknown case");
    std::cout<<"PASS "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
