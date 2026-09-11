#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include "codeguard/thread_pool.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <atomic>
#include <barrier>

namespace cg=codeguard;
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
struct Fixture {
    cg::fs::path dir,root,db;
    Fixture(){dir=cg::fs::current_path()/("level3-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));cg::fs::create_directory(dir);root=dir/cg::from_utf8("中文 source");cg::fs::create_directory(root);db=dir/"scan.sqlite3";}
    ~Fixture(){std::error_code e;cg::fs::remove_all(dir,e);}
    void write(const std::string& file,const std::string& value){std::ofstream(root/cg::from_utf8(file))<<value;}
    void commands(unsigned count){std::ofstream out(dir/"compile_commands.json");out<<'[';for(unsigned i=0;i<count;++i){if(i)out<<',';const auto file="f"+std::to_string(i)+".cpp";out<<"{\"directory\":\""<<cg::utf8_path(root)<<"\",\"file\":\""<<file<<"\",\"arguments\":[\"clang++\",\"-std=c++20\",\"-c\",\""<<file<<"\"]}";}out<<']';}
};
int main(int argc,char**argv){try{
    require(argc>=2,"mode required");const std::string mode=argv[1];Fixture f;
    if(mode=="pool"){
        cg::ThreadPool pool(4,2);std::barrier gate(4);std::atomic<int> active=0,peak=0;
        std::vector<std::future<int>> jobs;
        for(int i=0;i<4;++i)jobs.push_back(pool.submit([&,i]{const int n=++active;int p=peak;while(n>p&&!peak.compare_exchange_weak(p,n)){}gate.arrive_and_wait();--active;return i*i;}));
        for(int i=0;i<4;++i)require(jobs[i].get()==i*i,"pool values");require(peak==4,"actual concurrency");
        auto bad=pool.submit([]()->int{throw std::runtime_error("worker error");});bool caught=false;try{bad.get();}catch(...){caught=true;}require(caught,"exception propagation");
        require(pool.submit([]{return 19;}).get()==19,"pool survives task failure");pool.close();
        caught=false;try{pool.submit([]{});}catch(...){caught=true;}require(caught,"closed pool rejects");
    }else if(mode.starts_with("process_")){
        require(argc==3,"helper path required");cg::ProcessOptions options;options.arguments={argv[2]};options.working_directory=cg::utf8_path(f.root);options.timeout=std::chrono::seconds(5);
        if(mode=="process_args"){
            const std::vector<std::string> values{"","a b","quoted\"value","trailing\\","中文","$() & ;"};options.arguments.push_back("echo");options.arguments.insert(options.arguments.end(),values.begin(),values.end());
            const auto r=cg::run_process(options);std::string expected;for(const auto&v:values)expected+=std::to_string(v.size())+":"+v+"\n";
            auto normalized=r.stdout_text;normalized.erase(std::remove(normalized.begin(),normalized.end(),'\r'),normalized.end());
            require(r.status=="passed"&&normalized==expected&&r.stderr_text.find("error-stream")!=std::string::npos,"argv/stdout/stderr: "+r.stderr_text);
        }else if(mode=="process_flood"){options.arguments.push_back("flood");options.capture_limit=1024;const auto r=cg::run_process(options);require(r.status=="passed"&&r.output_truncated&&r.stdout_text.size()==1024&&r.stderr_text.size()==1024,"pipe drain / cap");
        }else if(mode=="process_failure"){options.arguments.push_back("fail");auto r=cg::run_process(options);require(r.status=="failed"&&r.exit_code==7,"exit code");options.arguments={"codeguard_nonexistent_program_123"};r=cg::run_process(options);require(r.status=="start_failed","missing executable");
        }else if(mode=="process_timeout"||mode=="process_cancel"){
            options.arguments.insert(options.arguments.end(),{"tree",cg::utf8_path(f.dir/"orphan.txt")});options.control=std::make_shared<cg::ScanControl>();
            std::jthread cancel;
            if(mode=="process_cancel")cancel=std::jthread([&]{std::this_thread::sleep_for(std::chrono::milliseconds(200));options.control->request_cancel();});else options.timeout=std::chrono::milliseconds(200);
            const auto r=cg::run_process(options);require(r.status==(mode=="process_cancel"?"cancelled":"timed_out")&&r.duration_ms<2000,"bounded termination");
            std::this_thread::sleep_for(std::chrono::milliseconds(1600));require(!cg::fs::exists(f.dir/"orphan.txt"),"orphan descendant survived");
        }
    }else if(mode=="rules"||mode=="parallel"){
        f.write("f0.cpp",R"CPP(
extern "C" char* strcpy(char*, const char*);
int bad_index(){ int a[2]={}; return a[2]; }
int negative(){ int a[2]={}; return a[-1]; }
int* bad_lifetime(){ int x; return &x; }
int null_value(){ return *((int*)0); }
int branch(int x){ if(x=2) return x; return 0; }
void risk(char* p){ strcpy(p,"text"); }
int safe(int x){int a[2]={}; if((x=2)) return a[1]; return sizeof(a[9]);}
int* static_lifetime(){static int x;return &x;}
int* reference_lifetime(int& x){return &x;}
void end_address(){int a[2];int* e=&a[2];(void)e;}
)CPP");
        const unsigned count=mode=="parallel"?16:1;
        for(unsigned i=1;i<count;++i)f.write("f"+std::to_string(i)+".cpp","int unit"+std::to_string(i)+"(){int a[2]={};return a[2];}\n");f.commands(count);
        cg::ScanOptions options;options.compile_commands=cg::utf8_path(f.dir);options.threads=1;
        const auto serial=cg::import_project(f.root,f.db,options);require(serial.analysis.status=="complete","Clang parse");
        std::set<std::string> rules;for(const auto&i:serial.analysis.issues){rules.insert(i.rule_id);require(!i.evidence.empty()&&!i.suggestion.empty(),"issue evidence");}
        require(rules==std::set<std::string>({"CG001","CG002","CG003","CG004","CG005"}),"five AST rules");
        require(serial.analysis.issues.size()==6+count-1,"positive/negative rules boundary");
        cg::SqliteDatabase db(f.db,true);const auto saved=db.latest(serial.root);require(saved.analysis.issues.size()==serial.analysis.issues.size(),"issue persistence");
        require(cg::execute_query(saved,"SELECT rule_id FROM issues WHERE severity = 'error'").rows.size()==4+count-1,"issue query");
        if(mode=="parallel"){
            options.threads=4;const auto parallel=cg::import_project(f.root,f.db,options);require(parallel.analysis.workers==4,"worker count");
            require(serial.analysis.symbols.size()==parallel.analysis.symbols.size()&&serial.analysis.issues.size()==parallel.analysis.issues.size(),"deterministic parallel totals");
            for(std::size_t i=0;i<serial.analysis.issues.size();++i)require(serial.analysis.issues[i].file==parallel.analysis.issues[i].file&&serial.analysis.issues[i].rule_id==parallel.analysis.issues[i].rule_id,"deterministic issue order");
            options.context.control=std::make_shared<cg::ScanControl>();options.context.progress=[&](const auto&p){if(p.phase=="analyzing"&&p.completed>=1)options.context.control->request_cancel();};
            bool cancelled=false;try{cg::import_project(f.root,f.db,options);}catch(const cg::ScanCancelled&){cancelled=true;}
            require(cancelled&&db.latest(serial.root).id==parallel.id,"cancel parallel without commit");
        }
    }else if(mode=="git"){
        f.write("main.cpp","int main(){return 0;}\n");
        for(const auto& args:std::vector<std::vector<std::string>>{{"git","init","-b","main"},{"git","add","main.cpp"},
            {"git","-c","user.name=CodeGuard Test","-c","user.email=test@example.invalid","commit","-m","fixture"}}){
            auto result=cg::run_process({args,cg::utf8_path(f.root)});require(result.status=="passed","git fixture: "+result.stderr_text);
        }
        f.write("main.cpp","int main(){return 1;}\n");const auto before=cg::fs::last_write_time(f.root/".git"/"index");
        std::string revision;auto log=cg::read_git(f.root,{},&revision);
        require(revision.size()>=40&&log.find("main")!=std::string::npos&&log.find("+int main(){return 1;}")!=std::string::npos,"git branch/revision/diff");
        require(before==cg::fs::last_write_time(f.root/".git"/"index"),"read-only git index");
    }else if(mode.starts_with("build_")){
        f.write("main.cpp","int main(){return 0;}\n");
        f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(Fixture LANGUAGES CXX)\nadd_executable(demo main.cpp)\nenable_testing()\nadd_test(NAME demo COMMAND demo)\n");
        if(mode=="build_failure")f.write("main.cpp","int main(){not_valid;}\n");
        if(mode=="build_testfail")f.write("main.cpp","int main(){return 7;}\n");
        if(mode=="build_notests")f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(Fixture LANGUAGES CXX)\nadd_executable(demo main.cpp)\n");
        if(mode=="build_timeout"||mode=="build_cancel")f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nexecute_process(COMMAND ${CMAKE_COMMAND} -E sleep 10)\nproject(Fixture LANGUAGES CXX)\n");
        const auto scan=cg::import_project(f.root,f.db);
        cg::BuildOptions options;options.output_directory=f.dir/"runs";options.jobs=2;options.timeout=std::chrono::seconds(30);
        if(mode=="build_configuration") {
            cg::fs::create_directories(f.root/"resources"/"out");
            f.write("resources/out/required.txt","test resource");
            f.write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(Fixture LANGUAGES CXX)\nif(NOT CG_LABEL STREQUAL \"value with spaces\" OR NOT CG_SECOND STREQUAL \"second\")\nmessage(FATAL_ERROR \"missing definitions\")\nendif()\nif(NOT EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/resources/out/required.txt\")\nmessage(FATAL_ERROR \"missing ignored resource\")\nendif()\nadd_executable(demo main.cpp)\nenable_testing()\nadd_test(NAME demo COMMAND demo)\n");
            options.cmake_definitions={"CG_LABEL=value with spaces","CG_SECOND=second"};
            options.copy_includes={"resources/out"};
            for(const auto& invalid:std::vector<std::string>{"../outside",".",".git","missing"}) {
                auto bad=options;bad.copy_includes={invalid};bool rejected=false;
                try{cg::build_and_test(f.root,f.db,bad);}catch(const std::invalid_argument&){rejected=true;}
                require(rejected,"unsafe copy include rejected");
            }
            auto bad=options;bad.cmake_definitions={"-S=elsewhere"};bool rejected=false;
            try{cg::build_and_test(f.root,f.db,bad);}catch(const std::invalid_argument&){rejected=true;}
            require(rejected,"non-definition argument rejected");
        }
        if(mode=="build_timeout")options.timeout=std::chrono::milliseconds(300);
        if(mode=="build_cancel"){options.control=std::make_shared<cg::ScanControl>();options.progress=[&](const auto& stage){if(stage=="configure")options.control->request_cancel();};}
        if(mode=="build_stale")f.write("main.cpp","int main(){return 1;}\n");
        const auto run=cg::build_and_test(f.root,f.db,options);
        if(mode=="build_pass"||mode=="build_configuration")require(run.status=="passed"&&run.steps.back().tests_total==1&&run.steps.back().tests_failed==0,"build/test pass: "+run.steps.back().result.stderr_text);
        else if(mode=="build_cancel")require(run.status=="cancelled","build cancelled logs");
        else if(mode=="build_timeout")require(run.status=="timed_out","configure timeout");
        else require(run.status!="passed","failure cannot pass");
        require(run.source_unchanged==(mode!="build_stale"),"original source status");
        require(run.id>0&&cg::fs::exists(cg::from_utf8(run.workspace)/"summary.txt"),"retained artifacts");
        cg::SqliteDatabase db(f.db,true);auto history=db.builds(scan.root);require(history.size()==1&&history[0].status==run.status&&history[0].steps.size()==run.steps.size(),"build persistence");
        require(cg::execute_query(db.latest(scan.root),"SELECT stage FROM builds ORDER BY run_id").rows.size()==run.steps.size(),"build query");
        options.output_directory=f.root/"build";bool rejected=false;try{cg::build_and_test(f.root,f.db,options);}catch(...){rejected=true;}require(rejected,"source output guard");
    }else throw std::runtime_error("unknown mode");
    std::cout<<"LEVEL3_OK "<<mode<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
