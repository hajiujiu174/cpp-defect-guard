#include "codeguard/repair.hpp"
#include "../../core/repair/coverage_internal.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <regex>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
struct Fixture {
    fs::path base=fs::temp_directory_path()/("cgc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=base/"source",db=base/"scan.sqlite3";cg::ScanResult snapshot;std::string original;
    Fixture(std::string source,bool run_on_build=false):original(std::move(source)){
        fs::create_directories(root);write("main.cpp",original);write("unused.hpp","int never_compiled_header();\n");
        write("CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nproject(CoverageFixture LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp)\nadd_test(NAME behavior COMMAND demo)\n"+std::string(run_on_build?"add_custom_command(TARGET demo POST_BUILD COMMAND $<TARGET_FILE:demo>)\n":""));
        const auto run=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"configure"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_GNU_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)});
        check(run.status=="passed","coverage configure failed");cg::ScanOptions o;if(cg::clang_analysis_available())o.compile_commands=cg::utf8_path(base/"configure");snapshot=cg::import_project(root,db,o);
    }
    ~Fixture(){std::error_code ec;fs::remove_all(base,ec);}
    void write(const std::string& name,const std::string& text){std::ofstream out(root/name,std::ios::binary);out<<text;}
    cg::BuildOptions options(){cg::BuildOptions o;o.output_directory=base/"runs";o.cxx_compiler=CODEGUARD_TEST_GNU_CXX;o.gcov=CODEGUARD_TEST_GCOV;o.collect_coverage=true;o.timeout=std::chrono::seconds(30);o.jobs=2;return o;}
    std::vector<cg::RepairCandidate> candidates(){for(const auto& i:snapshot.analysis.issues)if(i.rule_id=="CG003")return cg::propose_repairs(snapshot,i);throw std::runtime_error("assignment issue missing");}
};
const std::string bug="int f(int n){if(n=0)return 1;return 0;}\nint unused(){return 9;}\nint main(){return f(0)==1&&f(1)==0?0:1;}\n";
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];
    if(mode=="ranking"){
        cg::RepairBatchEntry a,b;a.candidate_id="a";b.candidate_id="b";a.changed_bytes=1;b.changed_bytes=9;
        for(auto* e:{&a,&b}){auto& c=e->validation.coverage;c.status="collected";c.scope_signature="same";c.tool_version="gcov 16";c.lines_total=10;c.target_line_status="recorded";e->validation.status="tests_failed";}
        a.validation.coverage.lines_hit=5;b.validation.coverage.lines_hit=8;
        check(cg::comparable_repair_coverage({a,b})&&cg::rank_repairs({a,b})==std::vector<std::size_t>({1,0}),"comparable coverage did not precede edit size");
        b.validation.coverage.scope_signature="different";check(!cg::comparable_repair_coverage({a,b})&&cg::rank_repairs({a,b})==std::vector<std::size_t>({0,1}),"different denominators influenced ranking");
        b.validation.coverage.scope_signature="same";b.validation.coverage.tool_version="gcov 15";check(!cg::comparable_repair_coverage({a,b}),"different gcov versions compared");
        b.validation.coverage.status="failed";cg::RepairBatch batch;batch.entries={a,b};check(cg::repair_batch_json(batch).find("\"test_coverage\": \"incomplete\"")!=std::string::npos,"partial coverage was called complete");
    }else if(mode=="reset"){
        Fixture f("int f(int n) {\n if(n==0) return 1;\n return 0;\n}\nint unused(){return 9;}\nint main(){return f(0)==1?0:1;}\n",true);
        auto options=f.options();cg::prepare_repair_coverage(options,f.base/"coverage");auto run=cg::build_and_test(f.root,f.db,options);check(run.status=="passed","instrumented copy build/test failed");
        cg::RepairCandidate c;c.edit.file="main.cpp";c.edit.line=2;const auto coverage=cg::collect_repair_coverage(run,c,f.snapshot,options,f.base/"coverage");
        check(coverage.status=="collected"&&coverage.lines_hit<coverage.lines_total&&coverage.target_line_status=="recorded"&&coverage.branch_records_taken<coverage.branch_records,"line/branch coverage is not bounded by actual execution: "+cg::repair_coverage_json(coverage));
        check(coverage.unmeasured_files==std::vector<std::string>{"unused.hpp"},"uninstrumented inventory was hidden");
        bool saved_old=false;for(const auto& e:fs::recursive_directory_iterator(cg::from_utf8(run.workspace)/"pre-test-profile"))if(e.path().extension()==".gcda")saved_old=true;
        const auto json=read(f.base/"coverage/object-0.json");std::smatch main;const std::regex main_name(R"("name"\s*:\s*"main")");check(std::regex_search(json,main,main_name),"gcov omitted main function counts");
        const auto begin=static_cast<std::size_t>(main.position()),end=json.find('}',begin);const auto function=json.substr(begin,end-begin);
        check(saved_old&&std::regex_search(function,std::regex(R"("execution_count"\s*:\s*1(?:\s*,|\s*$))")),"pre-CTest execution leaked into coverage counters");
        check(read(f.root/"main.cpp")==f.original&&run.source_unchanged,"coverage changed original source");
    }else if(mode=="validate"){
        Fixture f(bug);const auto items=f.candidates();auto result=cg::validate_repair(f.root,f.db,items[0],f.options());
        check(result.gates_passed&&result.coverage.status=="collected"&&result.coverage.target_line_status=="recorded"&&result.coverage.lines_hit<result.coverage.lines_total&&result.coverage_commands_normalized,"covered comparison was not validated: "+cg::repair_validation_json(result));
        const auto intent=cg::validate_repair(f.root,f.db,items[1],f.options());check(intent.status=="tests_failed"&&intent.coverage.status=="collected"&&!intent.gates_passed,"coverage hid a failing behavioral test");
        check(read(f.root/"main.cpp")==f.original,"covered validation touched original");
    }else if(mode=="target"){
        Fixture f("int f(int n){if(n=0)return 1;return 0;}\nint main(){return 0;}\n");auto result=cg::validate_repair(f.root,f.db,f.candidates()[0],f.options());
        check(result.status=="coverage_target_uncovered"&&!result.gates_passed&&result.coverage.status=="collected"&&result.coverage.target_line_status=="no_recorded_execution","unexecuted repair line passed coverage gate: "+cg::repair_validation_json(result));
    }else if(mode=="failures"){
        Fixture f(bug);const auto c=f.candidates()[0];auto missing=f.options();missing.gcov=cg::utf8_path(f.base/"missing-gcov.exe");const auto m=cg::validate_repair(f.root,f.db,c,missing);
        check(m.status=="coverage_failed"&&m.coverage.status=="failed"&&!m.gates_passed&&m.build.source_unchanged,"missing coverage tool was ignored");
        auto hooked=f.options();hooked.cmake_definitions={"CMAKE_PROJECT_INCLUDE=existing.cmake"};const auto h=cg::validate_repair(f.root,f.db,c,hooked);check(!h.gates_passed&&h.coverage.status=="failed"&&h.error.find("existing CMAKE_PROJECT_INCLUDE")!=std::string::npos,"existing injection hook was replaced");
        auto cancelled=f.options();cancelled.control=std::make_shared<cg::ScanControl>();cancelled.progress=[control=cancelled.control](const std::string& phase){if(phase=="repair_coverage")control->request_cancel();};const auto x=cg::validate_repair(f.root,f.db,c,cancelled);
        check(x.status=="cancelled"&&x.coverage.status=="cancelled"&&!x.gates_passed,"coverage-stage cancellation was lost");
        auto clang=f.options();clang.cxx_compiler=CODEGUARD_TEST_CXX;const auto unsupported=cg::validate_repair(f.root,f.db,c,clang);
        if(std::string(CODEGUARD_TEST_CXX)!=CODEGUARD_TEST_GNU_CXX)check(!unsupported.gates_passed&&unsupported.coverage.status=="failed","non-GNU compiler was accepted for gcov");
        auto unsafe=f.options();unsafe.collect_coverage=false;unsafe.cmake_definitions={"CMAKE_CXX_FLAGS=-fprofile-use="+cg::utf8_path(f.base/"missing-profile")};const auto rejected=cg::validate_repair(f.root,f.db,c,unsafe);
        check(rejected.analysis_status=="failed"&&!rejected.target_not_detected&&rejected.new_or_unmasked_issues==-1&&!rejected.gates_passed,"failed analysis was displayed as absent target/zero new findings: "+cg::repair_validation_json(rejected));
#ifdef _WIN32
        auto unicode_output=f.options();unicode_output.output_directory=f.base/cg::from_utf8("覆盖率输出");const auto path_error=cg::validate_repair(f.root,f.db,c,unicode_output);
        check(!path_error.gates_passed&&path_error.error.find("ASCII output directory")!=std::string::npos,"unsafe Windows runtime profile directory was accepted");
#endif
    }else if(mode=="safety"){
        Fixture f(bug);
        // Use argument vectors to verify that only the three exact generated flags disappear.
        const auto file=cg::utf8_path(f.root/"main.cpp"),dir=cg::utf8_path(f.root);
        auto json_quote=[](const std::string& s){std::string out="\"";for(char c:s){if(c=='"'||c=='\\')out+='\\';out+=c;}return out+'"';};
        {std::ofstream out(f.base/"unsafe.json");out<<"[{\"directory\":"<<json_quote(dir)<<",\"file\":"<<json_quote(file)<<",\"arguments\":[\"clang++\",\"--coverage\",\"-fprofile-abs-path\",\"-fprofile-update=atomic\",\"-fprofile-use=external\",\"-c\","<<json_quote(file)<<"]}]";}
        const auto normalized=cg::relocate_compile_commands(cg::utf8_path(f.base/"unsafe.json"),dir,dir,cg::utf8_path(f.base/"normalized/compile_commands.json"),true);
        const auto text=read(cg::from_utf8(normalized));check(text.find("-fprofile-use=external")!=std::string::npos&&text.find("--coverage")==std::string::npos,"normalization removed unapproved flags");
        cg::ScanOptions o;o.compile_commands=normalized;const auto blocked=cg::import_project(f.root,f.base/"blocked.sqlite3",o);check(blocked.analysis.units[0].status=="rejected_command","read-only command guard was weakened");
    }else if(mode=="cli"){
        check(argc==3,"CLI path required");Fixture f(bug);const auto items=f.candidates();std::vector<std::string> args{argv[2],"repair","evaluate",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.base/"cli"),"--cxx-compiler",CODEGUARD_TEST_GNU_CXX,"--coverage","gcov","--gcov-tool",CODEGUARD_TEST_GCOV};
        for(const auto& c:items){args.push_back("--candidate");args.push_back(cg::utf8_path(cg::export_repair_candidate(c,f.base/"exports")/"candidate.cgfix"));}
        const auto run=cg::run_process({args,cg::utf8_path(f.base),std::chrono::seconds(90)});check(run.exit_code==0&&run.stdout_text.find("\"test_coverage\": \"collected\"")!=std::string::npos&&run.stdout_text.find("\"gates_passed_count\": 1")!=std::string::npos,"CLI coverage batch failed: "+run.stdout_text+run.stderr_text);
        args[11]="off";check(cg::run_process({args,cg::utf8_path(f.base),std::chrono::seconds(20)}).exit_code!=0,"gcov tool was allowed while coverage disabled");
    }else throw std::runtime_error("unknown coverage mode");
    std::cout<<"REPAIR_COVERAGE_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
