#include "codeguard/repair.hpp"
#include <algorithm>
#include <fstream>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void write(const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;check(bool(out),"fixture write failed");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
struct Fixture{
    fs::path base=fs::temp_directory_path()/("ct-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),root=base/"s",db=base/"scan.sqlite3";
    std::map<std::string,std::string> originals;cg::ScanResult snapshot;cg::RepairSet set;
    explicit Fixture(const std::string& mode){fs::create_directories(root);
        originals={{"zero.cpp","int zero(int n){\nif(n=0)return 1;\nreturn 0;\n}\n"},{"one.cpp","int one(int n){\nif(n=1)return 1;\nreturn 0;\n}\n"},{"main.cpp","int zero(int);int one(int);\nint main(int n,char**v){if(n<2)return 1;\nif(v[1][0]=='z')return zero(0)==1&&zero(1)==0?0:1;\nif(v[1][0]=='o')return one(1)==1&&one(0)==0?0:1;\nreturn 0;}\n"}};
        if(mode=="mismatch"){originals["extra.cpp"]="int extra(){return 0;}\n";auto& main=originals["main.cpp"];main="int extra();\n"+main;const auto at=main.rfind("return 0;}");main.replace(at,10,"if(v[1][0]=='x')return extra();return 0;}");}
        for(const auto& [name,value]:originals)write(root/name,value);
        std::string cmake="cmake_minimum_required(VERSION 3.29)\nproject(TestCoverage LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp zero.cpp one.cpp)\nadd_test(NAME [=[zero [a]&雪]=] COMMAND demo z)\nadd_test(NAME one.+ COMMAND demo o)\nadd_test(NAME idle COMMAND demo i)\n";
        if(mode=="fixtures")cmake+="add_test(NAME setup COMMAND demo z)\nadd_test(NAME cleanup COMMAND demo i)\nset_tests_properties(setup PROPERTIES FIXTURES_SETUP seed DEPENDS idle)\nset_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP seed)\nset_tests_properties(one.+ PROPERTIES FIXTURES_REQUIRED seed DEPENDS [=[zero [a]&雪]=])\n";
        if(mode=="failures"||mode=="mutation"||mode=="metadata"){
            std::string action=mode=="failures"?"message(FATAL_ERROR \"intentional replay failure\")":mode=="mutation"?"file(WRITE \"${CMAKE_CURRENT_LIST_DIR}/main.cpp\" \"int main(){return 0;}\\n\")":"file(APPEND \"${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake\" \"\\nadd_test(added \\\"${CMAKE_COMMAND}\\\" -E true)\\n\")";
            write(root/"extra.cmake","if(EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/replay.state\")\n"+action+"\nendif()\nfile(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/replay.state\" \"1\")\n");
            cmake+="add_test(NAME extra COMMAND \"${CMAKE_COMMAND}\" -P \"${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake\")\n";
        }
        if(mode=="skip")cmake+="add_test(NAME disabled COMMAND demo i)\nset_tests_properties(disabled PROPERTIES DISABLED TRUE)\n";
        if(mode=="longname")cmake+="add_test(NAME [=["+std::string(2048,'&')+"雪]=] COMMAND demo i)\n";
        if(mode=="mismatch"){
            cmake+="target_sources(demo PRIVATE extra.cpp)\nadd_test(NAME extra COMMAND \"${CMAKE_COMMAND}\" \"-DDEMO=$<TARGET_FILE:demo>\" -P \"${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake\")\n";
            write(root/"extra.cmake","if(EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/replay.state\")\nexecute_process(COMMAND \"${DEMO}\" i RESULT_VARIABLE result)\nelse()\nexecute_process(COMMAND \"${DEMO}\" x RESULT_VARIABLE result)\nendif()\nif(NOT result EQUAL 0)\nmessage(FATAL_ERROR \"helper failed\")\nendif()\nfile(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/replay.state\" \"1\")\n");
        }
        write(root/"CMakeLists.txt",cmake);const auto process=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"cfg"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_GNU_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)});check(process.status=="passed","fixture configure failed: "+process.stderr_text);
        cg::ScanOptions scan;scan.compile_commands=cg::utf8_path(base/"cfg");snapshot=cg::import_project(root,db,scan);check(snapshot.analysis.issues.size()==2,"fixture issue count changed");
        auto all=cg::propose_repairs(snapshot,snapshot.analysis.issues);std::erase_if(all,[](const auto& c){return c.edit.recipe!="condition.compare";});set=cg::compose_repairs(std::move(all));
    }
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"FIXTURE_RETAINED="<<cg::utf8_path(base)<<'\n';return;}std::error_code ec;fs::remove_all(base,ec);}
    cg::BuildOptions options(){cg::BuildOptions o;o.output_directory=base/"v";o.cxx_compiler=CODEGUARD_TEST_GNU_CXX;o.gcov=CODEGUARD_TEST_GCOV;o.collect_coverage=true;o.collect_test_coverage=true;o.timeout=std::chrono::seconds(30);o.jobs=2;return o;}
    void unchanged(){for(const auto& [name,value]:originals)check(read(root/name)==value,"original file changed during replay");}
};
const cg::RepairTestCoverageEntry& entry(const cg::RepairValidation& r,const std::string& name){const auto it=std::find_if(r.test_coverage.entries.begin(),r.test_coverage.entries.end(),[&](const auto& e){return e.name==name;});check(it!=r.test_coverage.entries.end(),"missing requested test entry");return *it;}
std::string target(const cg::RepairTestCoverageEntry& e,const std::string& file){for(const auto& t:e.targets)if(t.file==file)return t.status;throw std::runtime_error("missing target line");}
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];Fixture f(mode);auto options=f.options();
    if(mode=="limit")options.coverage_test_limit=2;
    if(mode=="cancel"){options.control=std::make_shared<cg::ScanControl>();const auto control=options.control;options.progress=[control](const std::string& phase){if(phase=="repair_test_coverage_collected")control->request_cancel();};}
    if(mode=="cli"){
        check(argc==3,"CLI path required");const auto exported=cg::export_repair_set(f.set,f.base/"exports");
        const auto p=cg::run_process({{argv[2],"repair","validate-set",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.base/"cli"),"--set",cg::utf8_path(exported/"repair.cgset"),"--cxx-compiler",CODEGUARD_TEST_GNU_CXX,"--coverage","gcov-tests","--gcov-tool",CODEGUARD_TEST_GCOV,"--coverage-test-limit","3"},cg::utf8_path(f.base),std::chrono::seconds(90)});
        check(p.status=="passed"&&p.stdout_text.find("\"line_union_matches_aggregate\": true")!=std::string::npos,"CLI per-test validation failed: "+p.stdout_text+p.stderr_text);
    }else{
        const auto result=cg::validate_repair_set(f.root,f.db,f.set,options);const auto dump=cg::repair_validation_json(result);
        if(mode=="isolation"||mode=="fixtures"){
            check(result.gates_passed&&result.test_coverage.status=="collected"&&result.test_coverage.line_union_matches_aggregate,"replay failed: "+dump);
            const auto& zero=entry(result,"zero [a]&雪");const auto& one=entry(result,"one.+");const auto& idle=entry(result,"idle");
            check(target(zero,"zero.cpp")=="recorded"&&target(zero,"one.cpp")=="no_recorded_execution","zero group polluted by aggregate counters");
            check(target(idle,"zero.cpp")=="no_recorded_execution"&&target(idle,"one.cpp")=="no_recorded_execution","idle group reused previous counters");
            if(mode=="fixtures")check(one.selected_tests.size()==5&&target(one,"zero.cpp")=="recorded"&&target(one,"one.cpp")=="recorded","fixture/dependency group was mislabeled");
            else check(one.selected_tests.size()==1&&target(one,"zero.cpp")=="no_recorded_execution"&&target(one,"one.cpp")=="recorded","exact test name selection failed");
        }else if(mode=="limit")check(!result.gates_passed&&result.test_coverage.status=="limit_exceeded"&&result.test_coverage.entries.size()==3&&std::all_of(result.test_coverage.entries.begin(),result.test_coverage.entries.end(),[](const auto& e){return e.status=="not_run";}),"limit replay preconditions or ledger failed: "+dump);
        else if(mode=="cancel")check(result.status=="cancelled"&&result.test_coverage.status=="cancelled"&&result.test_coverage.entries.size()==3&&result.test_coverage.entries.front().status=="collected"&&result.test_coverage.entries.back().status=="not_run","cancellation lost completed/pending replay evidence: "+dump);
        else if(mode=="mutation")check(!result.gates_passed&&!result.proposed_copy_verified&&result.error.find("proposed repair")!=std::string::npos,"source mutation during replay passed: "+dump);
        else if(mode=="failures")check(!result.gates_passed&&entry(result,"extra").status=="tests_incomplete"&&result.test_coverage.status=="incomplete","failed replay counted as passing attribution: "+dump);
        else if(mode=="metadata")check(!result.gates_passed&&entry(result,"extra").status=="failed"&&entry(result,"extra").error.find("catalog changed")!=std::string::npos,"changed CTest catalog accepted: "+dump);
        else if(mode=="skip")check(!result.gates_passed&&entry(result,"disabled").status=="tests_incomplete"&&entry(result,"disabled").tests_disabled==1&&entry(result,"disabled").tests_not_run==1&&entry(result,"disabled").tests_skipped==0,"disabled test reported as executed: "+dump);
        else if(mode=="mismatch")check(!result.gates_passed&&result.test_coverage.status=="incomplete"&&!result.test_coverage.line_union_matches_aggregate&&result.test_coverage.error.find("line union differs")!=std::string::npos&&std::all_of(result.test_coverage.entries.begin(),result.test_coverage.entries.end(),[](const auto& e){return e.status=="collected";}),"replay union drift passed: "+dump);
        else if(mode=="longname")check(result.gates_passed&&result.test_coverage.entries.size()==4&&entry(result,std::string(2048,'&')+"雪").status=="collected","long escaped JUnit test name failed: "+dump);
        else throw std::runtime_error("unknown test mode");
        check(read(cg::from_utf8(result.workspace)/"validation.json").find("\"test_coverage\"")!=std::string::npos,"validation ledger omitted replay evidence");
    }
    f.unchanged();std::cout<<"TEST_COVERAGE_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
