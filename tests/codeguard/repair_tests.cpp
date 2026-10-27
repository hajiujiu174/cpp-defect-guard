#include "codeguard/repair.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F work){try{work();}catch(const std::exception&){return;}throw std::runtime_error("invalid repair request accepted");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
const std::string default_source="int is_zero(int n){if(n=0)return 1;return 0;}\nint main(){return is_zero(0)==1&&is_zero(1)==0?0:1;}\n";
struct Fixture {
    fs::path dir=fs::current_path()/("repairs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=dir/"source",db=dir/"scan.sqlite3",configure=dir/"configure";std::string file="main.cpp";cg::ScanOptions scan_options;
    explicit Fixture(std::string source=default_source,std::string name="main.cpp"):file(std::move(name)){
        const bool c=file.ends_with(".c");fs::create_directories(root);write(file,source);write("CMakeLists.txt",std::string("cmake_minimum_required(VERSION 3.24)\nproject(RepairFixture LANGUAGES ")+(c?"C":"CXX")+")\nenable_testing()\nadd_executable(demo \""+file+"\")\ntarget_compile_features(demo PRIVATE "+(c?"c_std_11":"cxx_std_20")+")\nadd_test(NAME behavior COMMAND demo)\n");
        const auto compiler=std::string(c?"-DCMAKE_C_COMPILER=":"-DCMAKE_CXX_COMPILER=")+(c?CODEGUARD_TEST_CC:CODEGUARD_TEST_CXX);
        auto run=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(configure),"-G","Ninja",compiler,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(dir),std::chrono::seconds(30)});
        check(run.status=="passed","fixture configure failed: "+run.stderr_text);if(cg::clang_analysis_available())scan_options.compile_commands=cg::utf8_path(configure);
    }
    ~Fixture(){std::error_code ec;fs::remove_all(dir,ec);}
    void write(const std::string& path,const std::string& text){std::ofstream out(root/cg::from_utf8(path),std::ios::binary);out<<text;check(bool(out),"fixture write failed");}
    cg::ScanResult scan(){return cg::import_project(root,db,scan_options);}
    cg::BuildOptions build(){cg::BuildOptions o;o.output_directory=dir/"runs";o.c_compiler=CODEGUARD_TEST_CC;o.cxx_compiler=CODEGUARD_TEST_CXX;o.jobs=2;o.timeout=std::chrono::seconds(30);return o;}
};
const cg::Issue& issue(const cg::ScanResult& s,const std::string& rule){for(const auto& i:s.analysis.issues)if(i.rule_id==rule)return i;throw std::runtime_error("fixture issue missing: "+rule);}
const cg::RepairCandidate& recipe(const std::vector<cg::RepairCandidate>& items,const std::string& name){for(const auto& c:items)if(c.edit.recipe==name)return c;throw std::runtime_error("recipe missing "+name);}
std::string stage(const cg::RepairValidation& v,const std::string& name){for(const auto& step:v.build.steps)if(step.name==name)return step.result.status;return "not_run";}
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];
    if(mode=="copy"){
        Fixture f;auto scan=f.scan();const auto original=read(f.root/f.file);auto options=f.build();auto after=original;after.replace(after.find("n=0"),3,"n==0");options.copy_replacements={{f.file,original,after}};
        const auto run=cg::build_and_test(f.root,f.db,options);check(run.status=="passed"&&run.source_unchanged,"copy patch did not pass build/tests");
        check(read(f.root/f.file)==original&&read(cg::from_utf8(run.workspace)/"source"/f.file)==after,"copy patch wrote original source or missed copy");
        options.copy_replacements={{"CMakeLists.txt",read(f.root/"CMakeLists.txt"),"bad"}};const auto bad=cg::build_and_test(f.root,f.db,options);check(bad.status=="failed"&&bad.source_unchanged,"unscanned replacement accepted");
        options.copy_replacements={{f.file,"stale",after}};check(cg::build_and_test(f.root,f.db,options).status=="failed","inexact source replacement accepted");
    }else if(mode=="propose"){
        Fixture f;f.scan_options.use_cache=true;f.scan();auto s=f.scan();check(s.analysis.repair_hints.empty(),"historical scan should not retain ephemeral repair ranges");
        const auto candidates=cg::propose_repairs(s,issue(s,"CG003"));check(candidates.size()==2,"assignment should have comparison and intent alternatives");
        check(recipe(candidates,"condition.compare").revised_text.find("n==0")!=std::string::npos&&recipe(candidates,"condition.intent").revised_text.find("(n=0)")!=std::string::npos,"assignment source edit wrong");
        Fixture macro("#define ASSIGN(n) n=0\nint f(int n){if(ASSIGN(n))return 1;return 0;}\nint main(){return 0;}\n");auto ms=macro.scan();check(cg::propose_repairs(ms,issue(ms,"CG003")).empty(),"macro expansion received a guessed patch range");
        Fixture pointer("#include <cstdio>\nint f(char*p){return std::sprintf(p,\"%s\",\"abc\");}\nint main(){return 0;}\n");auto ps=pointer.scan();check(cg::propose_repairs(ps,issue(ps,"CG001")).empty(),"pointer capacity was fabricated");
        Fixture compound("int f(int n){if(n+=1)return 1;return 0;}\nint main(){return 0;}\n");auto cs=compound.scan();const auto one=cg::propose_repairs(cs,issue(cs,"CG003"));check(one.size()==1&&one[0].edit.recipe=="condition.intent","compound assignment became equality");
    }else if(mode=="roundtrip"){
        const std::string source="\xef\xbb\xbf// UTF-8 中文\r\nint f(int n){if(n=0)return 1;return 0;}\r\nint main(){return f(0)==1&&f(1)==0?0:1;}";
        Fixture f(source,"中文 空格.cpp");auto s=f.scan();const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");
        const auto data=cg::encode_repair_candidate(c);const auto loaded=cg::decode_repair_candidate(data);check(loaded.original_text==source&&loaded.revised_text==c.revised_text,"codec lost BOM/CRLF/unicode or final newline state");
        const auto out=cg::export_repair_candidate(c,f.dir/"exports");check(cg::read_repair_candidate(out/"candidate.cgfix").id==c.id,"manifest read mismatch");
        check(read(out/"validation.json").find("\"status\": \"not_run\"")!=std::string::npos,"exported candidate lacks explicit unverified status");
        const auto check_root=f.dir/"diff-copy";fs::create_directory(check_root);fs::copy_file(f.root/cg::from_utf8(f.file),check_root/cg::from_utf8(f.file));
        check(cg::run_process({{"git","init",cg::utf8_path(check_root)},cg::utf8_path(f.dir),std::chrono::seconds(10)}).status=="passed","diff test Git init failed");
        auto diff=cg::run_process({{"git","-c","core.autocrlf=false","apply","--check",cg::utf8_path(out/"candidate.diff")},cg::utf8_path(check_root),std::chrono::seconds(10)});check(diff.status=="passed","diff is not applicable: "+diff.stderr_text);
        check(cg::run_process({{"git","-c","core.autocrlf=false","apply",cg::utf8_path(out/"candidate.diff")},cg::utf8_path(check_root),std::chrono::seconds(10)}).status=="passed"&&read(check_root/cg::from_utf8(f.file))==c.revised_text,"diff application did not reproduce exact candidate bytes");
        check(read(f.root/cg::from_utf8(f.file))==source,"diff test changed its original project");
        rejects([&]{cg::decode_repair_candidate(data+"extra");});auto tampered=data;tampered.back()='x';rejects([&]{cg::decode_repair_candidate(tampered);});
        rejects([&]{cg::export_repair_candidate(c,f.root/"exports");});check(cg::export_repair_candidate(c,f.dir/"exports")!=out,"candidate export overwrote a previous directory");
        auto bad=c;bad.edit.file="../escape.cpp";rejects([&]{cg::encode_repair_candidate(bad);});
    }else if(mode=="validate"){
        Fixture f;auto s=f.scan();const auto original=read(f.root/f.file);const auto candidates=cg::propose_repairs(s,issue(s,"CG003"));
        const auto good=cg::validate_repair(f.root,f.db,recipe(candidates,"condition.compare"),f.build());
        check(good.gates_passed&&good.status=="gates_passed"&&good.target_not_detected&&good.new_or_unmasked_issues==0&&good.correctness=="not_assessed","comparison validation gates wrong: "+cg::repair_validation_json(good));
        const auto intent=cg::validate_repair(f.root,f.db,recipe(candidates,"condition.intent"),f.build());
        check(!intent.gates_passed&&intent.status=="tests_failed"&&stage(intent,"build")=="passed"&&stage(intent,"test")=="failed"&&intent.target_not_detected,"intent warning disappearance hid a failing behavioral test");
        check(read(f.root/f.file)==original&&good.build.source_unchanged&&intent.build.source_unchanged,"candidate validation modified original source");
        check(fs::is_regular_file(cg::from_utf8(good.workspace)/"validation.json")&&fs::is_regular_file(cg::from_utf8(intent.workspace)/"validation.json"),"passing/failing candidate evidence missing");
    }else if(mode=="failures"){
        Fixture f;auto s=f.scan();const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");f.scan();rejects([&]{cg::validate_repair(f.root,f.db,c,f.build());});
        Fixture no_header("extern \"C\" int sprintf(char*,const char*,...);\nint f(){char b[4];return sprintf(b,\"%s\",\"abc\");}\nint main(){return 0;}\n");auto n=no_header.scan();const auto nc=cg::propose_repairs(n,issue(n,"CG001"));check(nc.size()==1,"bounded sprintf fixture not proposed");
        const auto failed=cg::validate_repair(no_header.root,no_header.db,nc[0],no_header.build());check(failed.status=="build_failed"&&failed.analysis_status=="not_run"&&failed.after_issues==-1&&failed.build.source_unchanged,"build failure was counted as verified repair");
        no_header.write(no_header.file,read(no_header.root/no_header.file)+"\n");rejects([&]{cg::propose_repairs(n,issue(n,"CG001"));});
        Fixture mutation;mutation.write("CMakeLists.txt",read(mutation.root/"CMakeLists.txt")+"\nfile(APPEND \"${CMAKE_CURRENT_SOURCE_DIR}/main.cpp\" \"\\n// generated by fixture\\n\")\n");auto ms=mutation.scan();const auto mc=recipe(cg::propose_repairs(ms,issue(ms,"CG003")),"condition.compare");
        const auto changed=cg::validate_repair(mutation.root,mutation.db,mc,mutation.build());check(!changed.gates_passed&&changed.error.find("candidate file changed")!=std::string::npos&&changed.build.source_unchanged,"rewritten copied source was treated as the proposed bytes");
        Fixture skipped;skipped.write("CMakeLists.txt",read(skipped.root/"CMakeLists.txt")+"\nset_tests_properties(behavior PROPERTIES DISABLED TRUE)\n");auto ss=skipped.scan();const auto sc=recipe(cg::propose_repairs(ss,issue(ss,"CG003")),"condition.compare");const auto incomplete=cg::validate_repair(skipped.root,skipped.db,sc,skipped.build());
        check(!incomplete.gates_passed&&(incomplete.status=="tests_incomplete"||incomplete.status=="tests_failed")&&stage(incomplete,"test")!="not_run","all-skipped test gate result: "+cg::repair_validation_json(incomplete));
        Fixture skip_return;skip_return.write("CMakeLists.txt",read(skip_return.root/"CMakeLists.txt")+"\nset_tests_properties(behavior PROPERTIES SKIP_RETURN_CODE 0)\n");auto sr=skip_return.scan();const auto rc=recipe(cg::propose_repairs(sr,issue(sr,"CG003")),"condition.compare");const auto skipped_success=cg::validate_repair(skip_return.root,skip_return.db,rc,skip_return.build());
        check(!skipped_success.gates_passed&&skipped_success.status=="tests_incomplete"&&stage(skipped_success,"test")=="passed","CTest success with skipped tests passed the repair gate: "+cg::repair_validation_json(skipped_success));
        bool counted_skip=false;for(const auto& step:skipped_success.build.steps)if(step.name=="test")counted_skip=step.tests_total==1&&step.tests_skipped==1;check(counted_skip,"CTest skipped-test count was lost");
    }else if(mode=="baseline"){
        const std::string source="int f(int n){\n#ifdef OMIT_ASSIGNMENT\nif(n==0)return 1;\n#else\nif(n=0)return 1;\n#endif\nreturn 0;}\nint main(){return f(0)==1&&f(1)==0?0:1;}\n";
        Fixture f(source);f.write("CMakeLists.txt",read(f.root/"CMakeLists.txt")+"\noption(OMIT_ASSIGNMENT \"Use another branch\" OFF)\nif(OMIT_ASSIGNMENT)\ntarget_compile_definitions(demo PRIVATE OMIT_ASSIGNMENT)\nendif()\n");
        auto s=f.scan();const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");auto options=f.build();options.cmake_definitions={"OMIT_ASSIGNMENT=ON"};
        const auto result=cg::validate_repair(f.root,f.db,c,options);check(stage(result,"test")=="passed"&&result.target_not_detected&&!result.baseline_target_reproduced&&!result.gates_passed&&result.status=="baseline_not_reproduced","configuration change was mistaken for a successful repair");
    }else if(mode=="c"){
        Fixture f(default_source,"main.c");auto s=f.scan();const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");
        const auto result=cg::validate_repair(f.root,f.db,c,f.build());check(result.gates_passed&&result.baseline_target_reproduced,"C comparison recipe did not validate");
        Fixture formatting("#include <stdio.h>\n#include <string.h>\nint main(){char b[4];int n=sprintf(b,\"%s\",\"abcdef\");return n==6&&strcmp(b,\"abc\")==0?0:1;}\n","main.c");auto fsnapshot=formatting.scan();const auto edits=cg::propose_repairs(fsnapshot,issue(fsnapshot,"CG001"));check(edits.size()==1,"C sprintf recipe missing");
        check(cg::validate_repair(formatting.root,formatting.db,edits[0],formatting.build()).gates_passed,"C bounded output recipe did not validate");
    }else if(mode=="printf"){
        Fixture f("#include <cstdio>\n#include <cstring>\nint f(){char b[4];int n=std::sprintf((b),\"%s\",\"abcdef\");return n==6&&std::strcmp(b,\"abc\")==0?0:1;}\nint main(){return f();}\n");auto s=f.scan();const auto candidates=cg::propose_repairs(s,issue(s,"CG001"));check(candidates.size()==1&&candidates[0].revised_text.find("std::snprintf((b), sizeof(b),")!=std::string::npos,"bounded output edit wrong");
        const auto result=cg::validate_repair(f.root,f.db,candidates[0],f.build());check(result.gates_passed&&result.correctness=="not_assessed","bounded formatting validation failed: "+cg::repair_validation_json(result));
    }else if(mode=="policy"){
        Fixture f(default_source+"int other(){int a[2];return a[2];}\n");auto s=f.scan();f.scan_options.suppressions={cg::suppress_issue(s,issue(s,"CG002"),"fixture baseline suppression")};s=f.scan();
        const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");const auto result=cg::validate_repair(f.root,f.db,c,f.build());
        check(result.target_not_detected&&result.new_or_unmasked_issues==1&&!result.gates_passed&&result.status=="new_or_unmasked_issues","source-bound suppression changes were hidden");
    }else if(mode=="cli"){
        check(argc==3,"CLI path required");Fixture f;auto s=f.scan();const auto& selected=issue(s,"CG003");const std::string cli=argv[2];
        auto proposal=cg::run_process({{cli,"repair","propose",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.dir/"cli-exports"),"--rule","CG003","--file",f.file,"--line",std::to_string(selected.line),"--column",std::to_string(selected.column)},cg::utf8_path(f.dir),std::chrono::seconds(30)});
        check(proposal.exit_code==0&&proposal.stdout_text.find("candidates=2")!=std::string::npos,"CLI proposal failed: "+proposal.stderr_text);
        std::istringstream lines(proposal.stdout_text);std::string line;std::map<std::string,std::string> manifests;
        while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();const auto a=line.find('\t'),b=line.rfind('\t');if(a!=std::string::npos&&b>a)manifests[line.substr(a+1,b-a-1)]=line.substr(b+1);}
        for(const auto* name:{"condition.compare","condition.intent"}){check(manifests.contains(name),"CLI candidate manifest missing");const auto validated=cg::run_process({{cli,"repair","validate",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.dir/"cli-validation"),"--candidate",manifests.at(name),"--cxx-compiler",CODEGUARD_TEST_CXX},cg::utf8_path(f.dir),std::chrono::seconds(60)});
            check(validated.exit_code==(std::string(name)=="condition.compare"?0:2)&&validated.stdout_text.find("\"correctness\": \"not_assessed\"")!=std::string::npos,"CLI candidate gates conflated with correctness: "+validated.stdout_text+validated.stderr_text);}
        check(read(f.root/f.file)==default_source,"CLI validation modified original source");
    }else if(mode=="cancel"){
        Fixture f;auto s=f.scan();const auto c=recipe(cg::propose_repairs(s,issue(s,"CG003")),"condition.compare");auto options=f.build();options.control=std::make_shared<cg::ScanControl>();
        const auto control=options.control;options.progress=[control](const std::string& phase){if(phase=="repair_analysis")control->request_cancel();};
        const auto result=cg::validate_repair(f.root,f.db,c,options);check(result.status=="cancelled"&&!result.gates_passed&&stage(result,"test")=="passed"&&fs::is_regular_file(cg::from_utf8(result.workspace)/"validation.json"),"post-build cancellation was sealed too early or lost logs");
        cg::ScanContext cancelled;cancelled.control=std::make_shared<cg::ScanControl>();cancelled.control->request_cancel();bool caught=false;try{cg::propose_repairs(s,issue(s,"CG003"),cancelled);}catch(const cg::ScanCancelled&){caught=true;}check(caught,"cancelled proposal accepted");
    }else throw std::runtime_error("unknown repair test mode");
    std::cout<<"REPAIR_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
