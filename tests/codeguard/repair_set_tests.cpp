#include "codeguard/repair.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F work){try{work();}catch(const std::exception&){return;}throw std::runtime_error("invalid repair set accepted");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
void write(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<text;check(bool(out),"fixture write failed");}
struct Temp {fs::path path=fs::temp_directory_path()/("cgs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Temp(){fs::create_directory(path);}~Temp(){std::error_code ec;fs::remove_all(path,ec);}};
struct Fixture:Temp {
    fs::path root=path/"source",db=path/"scan.sqlite3";cg::ScanResult snapshot;std::map<std::string,std::string> originals;
    explicit Fixture(bool multi=false,std::string source={}){
        const std::string f="int f(int n){if(n=0)return 1;return 0;}\n",g="int g(int n){if(n=1)return 1;return 0;}\n";
        const std::string main="int f(int);int g(int);int main(){return f(0)==1&&f(1)==0&&g(1)==1&&g(0)==0?0:1;}\n";
        if(!source.empty())originals["main.cpp"]=std::move(source);else if(multi)originals={{"f.cpp",f},{"g.cpp",g},{"main.cpp",main}};else originals["main.cpp"]=f+g+main;
        std::string cmake="cmake_minimum_required(VERSION 3.24)\nproject(RepairSet LANGUAGES CXX)\nenable_testing()\nadd_executable(demo";
        for(const auto& [file,text]:originals){write(root/file,text);cmake+=" \""+file+"\"";}cmake+=")\nadd_test(NAME behavior COMMAND demo)\n";write(root/"CMakeLists.txt",cmake);
        const auto configured=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(path/"configure"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(path),std::chrono::seconds(30)});check(configured.status=="passed","set fixture configure failed");
        cg::ScanOptions o;o.compile_commands=cg::utf8_path(path/"configure");snapshot=cg::import_project(root,db,o);
    }
    std::vector<cg::RepairCandidate> candidates(){std::vector<cg::Issue> selected;for(const auto& i:snapshot.analysis.issues)if(i.rule_id=="CG003")selected.push_back(i);return cg::propose_repairs(snapshot,selected);}
    cg::BuildOptions options(bool coverage=false){cg::BuildOptions o;o.output_directory=path/"runs";o.cxx_compiler=coverage?CODEGUARD_TEST_GNU_CXX:CODEGUARD_TEST_CXX;o.collect_coverage=coverage;o.gcov=CODEGUARD_TEST_GCOV;o.jobs=2;o.timeout=std::chrono::seconds(30);return o;}
    void unchanged(){for(const auto& [file,text]:originals)check(read(root/file)==text,"set validation modified an original file");}
};
std::vector<cg::RepairCandidate> comparisons(const std::vector<cg::RepairCandidate>& all){std::vector<cg::RepairCandidate> out;for(const auto& c:all)if(c.edit.recipe=="condition.compare")out.push_back(c);return out;}
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];
    if(mode=="codec"){
        const auto folder=cg::from_utf8(CODEGUARD_SET_FIXTURES);const auto a=cg::read_repair_candidate(folder/"first.cgfix"),b=cg::read_repair_candidate(folder/"second.cgfix");
        const auto set=cg::compose_repairs({a,b});check(set.id==cg::compose_repairs({b,a}).id,"set ID depends on input order");const auto payload=cg::encode_repair_set(set);const auto decoded=cg::decode_repair_set(payload);check(decoded.id==set.id&&decoded.components.size()==2,"set codec lost components");
        const auto files=cg::repair_set_changes(set);check(files.size()==1&&files[0].before==a.original_text&&files[0].after.find("n==0")!=std::string::npos&&files[0].after.find("m==1")!=std::string::npos,"original-coordinate edits were applied sequentially");
        Temp temp;const auto exported=cg::export_repair_set(set,temp.path/"exports");check(cg::read_repair_set(exported/"repair.cgset").id==set.id&&read(exported/"validation.json").find(b.id)!=std::string::npos,"export lost manifest/target provenance");
        const auto copy=temp.path/"diff-copy";fs::create_directory(copy);write(copy/cg::from_utf8(files[0].file),files[0].before);check(cg::run_process({{"git","init",cg::utf8_path(copy)},cg::utf8_path(temp.path),std::chrono::seconds(10)}).status=="passed","diff fixture git init failed");
        check(cg::run_process({{"git","-c","core.autocrlf=false","apply",cg::utf8_path(exported/"candidate.diff")},cg::utf8_path(copy),std::chrono::seconds(10)}).status=="passed"&&read(copy/cg::from_utf8(files[0].file))==files[0].after,"set diff lost BOM/CRLF/Unicode/no-final-newline bytes");
        rejects([&]{cg::compose_repairs({a});});rejects([&]{cg::compose_repairs({a,a});});rejects([&]{cg::compose_repairs(std::vector<cg::RepairCandidate>(17,a));});
        rejects([&]{cg::compose_repairs({a,cg::read_repair_candidate(folder/"inconsistent-original.cgfix")});});rejects([&]{cg::compose_repairs({a,cg::read_repair_candidate(folder/"different-config.cgfix")});});
        rejects([&]{cg::decode_repair_set(payload+"tail");});rejects([&]{cg::decode_repair_set(payload.substr(0,payload.size()-50));});auto wrong=set;wrong.id+="x";rejects([&]{cg::encode_repair_set(wrong);});rejects([&]{cg::export_repair_set(set,cg::from_utf8(a.root)/"bad");});
    }else{
        Fixture f(mode=="multifile"||mode=="rewrite"||mode=="policy");const auto all=f.candidates();const auto selected=comparisons(all);check(all.size()==4&&selected.size()==2,"multi-issue AST proposal lost alternatives");
        if(mode=="samefile"||mode=="multifile"){
            for(const auto& c:selected){const auto single=cg::validate_repair(f.root,f.db,c,f.options());check(single.status=="tests_failed"&&!single.gates_passed,"a partial fix unexpectedly passed fixture behavior");}
            const auto set=cg::compose_repairs(selected);const auto result=cg::validate_repair_set(f.root,f.db,set,f.options());
            check(result.gates_passed&&result.targets.size()==2&&result.target_not_detected&&result.baseline_target_reproduced&&result.new_or_unmasked_issues==0,"combined repair failed: "+cg::repair_validation_json(result));for(const auto& t:result.targets)check(t.baseline_reproduced&&t.target_not_detected,"combined target evidence was aggregated incorrectly");
            check(cg::repair_set_changes(set).size()==(mode=="multifile"?2:1),"file combination count wrong");
        }else if(mode=="conflicts"){
            rejects([&]{cg::compose_repairs({all[0],all[1]});});
            Fixture nested(false,"#include <cstdio>\nint f(){char b[4];int n=0;if(n=std::sprintf(b,\"%s\",\"x\"))return 1;return 0;}\nint main(){return 0;}\n");
            auto parts=cg::propose_repairs(nested.snapshot,nested.snapshot.analysis.issues);std::vector<cg::RepairCandidate> overlap;for(const auto& c:parts)if(c.edit.recipe!="condition.intent")overlap.push_back(c);check(overlap.size()==2,"nested AST conflict fixture did not produce two recipes");rejects([&]{cg::compose_repairs(overlap);});
            cg::ScanOptions options;options.compile_commands=cg::utf8_path(f.path/"configure");f.snapshot=cg::import_project(f.root,f.db,options);const auto fresh=comparisons(f.candidates());rejects([&]{cg::compose_repairs({selected[0],fresh[1]});});
            const auto old=cg::compose_repairs(selected);rejects([&]{cg::validate_repair_set(f.root,f.db,old,f.options());});
        }else if(mode=="mapping"){
            Fixture same_line(false,"int f(int n){if(n=0)return 1;return 0;} int other(){int a[2]={};return a[2];} int g(int n){if(n=1)return 1;return 0;}\nint main(){return f(0)==1&&f(1)==0&&g(1)==1&&g(0)==0?0:1;}\n");const auto set=cg::compose_repairs(comparisons(same_line.candidates()));const auto result=cg::validate_repair_set(same_line.root,same_line.db,set,same_line.options());check(result.gates_passed&&result.after_issues==1&&result.new_or_unmasked_issues==0&&result.targets[1].revised_column>result.targets[1].column,"multi-edit column shifts were treated as new issues: "+cg::repair_validation_json(result));
        }else if(mode=="rewrite"){
            write(f.root/"CMakeLists.txt",read(f.root/"CMakeLists.txt")+"\nfile(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/main.cpp\" \"int main(){return 0;}\\n\")\n");
            const auto result=cg::validate_repair_set(f.root,f.db,cg::compose_repairs(selected),f.options());check(!result.gates_passed&&result.error.find("proposed repair")!=std::string::npos&&result.build.source_unchanged,"build script rewrote an unselected C++ file and still passed proposed repair gates: "+cg::repair_validation_json(result));
        }else if(mode=="baseline"){
            Fixture flags(false,"int f(int n){if(n=0)return 1;return 0;}\nint g(int n){\n#ifdef OMIT_SECOND\nif(n==1)return 1;\n#else\nif(n=1)return 1;\n#endif\nreturn 0;}\nint main(){return f(0)==1&&f(1)==0&&g(1)==1&&g(0)==0?0:1;}\n");
            write(flags.root/"CMakeLists.txt",read(flags.root/"CMakeLists.txt")+"\noption(OMIT_SECOND \"Use another branch\" OFF)\nif(OMIT_SECOND)\ntarget_compile_definitions(demo PRIVATE OMIT_SECOND)\nendif()\n");auto options=flags.options();options.cmake_definitions={"OMIT_SECOND=ON"};const auto result=cg::validate_repair_set(flags.root,flags.db,cg::compose_repairs(comparisons(flags.candidates())),options);
            check(!result.gates_passed&&result.status=="baseline_not_reproduced"&&result.targets[0].baseline_reproduced&&!result.targets[1].baseline_reproduced,"set baseline checked only the first target: "+cg::repair_validation_json(result));
        }else if(mode=="policy"){
            f.originals["g.cpp"]+="int other(){int a[2]={};return a[2];}\n";write(f.root/"g.cpp",f.originals["g.cpp"]);cg::ScanOptions options;options.compile_commands=cg::utf8_path(f.path/"configure");f.snapshot=cg::import_project(f.root,f.db,options);
            const auto issue=std::find_if(f.snapshot.analysis.issues.begin(),f.snapshot.analysis.issues.end(),[](const auto& i){return i.rule_id=="CG002";});check(issue!=f.snapshot.analysis.issues.end(),"policy fixture array issue missing");options.suppressions={cg::suppress_issue(f.snapshot,*issue,"set policy fixture")};f.snapshot=cg::import_project(f.root,f.db,options);
            const auto result=cg::validate_repair_set(f.root,f.db,cg::compose_repairs(comparisons(f.candidates())),f.options());check(!result.gates_passed&&result.status=="new_or_unmasked_issues"&&result.new_or_unmasked_issues==1&&result.target_not_detected,"second-file suppression invalidation was hidden");
        }else if(mode=="coverage"){
            auto result=cg::validate_repair_set(f.root,f.db,cg::compose_repairs(selected),f.options(true));check(result.gates_passed&&result.coverage.status=="collected"&&result.targets.size()==2,"covered repair set failed: "+cg::repair_validation_json(result));for(const auto& t:result.targets)check(t.coverage_line_status=="recorded","coverage checked only the first target");
            Fixture unused(false,"int f(int n){if(n=0)return 1;return 0;}\nint g(int n){if(n=1)return 1;return 0;}\nint main(){return f(0)==1&&f(1)==0?0:1;}\n");const auto set=cg::compose_repairs(comparisons(unused.candidates()));const auto blocked=cg::validate_repair_set(unused.root,unused.db,set,unused.options(true));check(blocked.status=="coverage_target_uncovered"&&!blocked.gates_passed&&blocked.targets[0].coverage_line_status=="recorded"&&blocked.targets[1].coverage_line_status=="no_recorded_execution","unexecuted second repair target passed coverage gate");
        }else if(mode=="cancel"){
            auto options=f.options();options.control=std::make_shared<cg::ScanControl>();options.progress=[control=options.control](const std::string& phase){if(phase=="repair_analysis")control->request_cancel();};const auto set=cg::compose_repairs(selected);const auto result=cg::validate_repair_set(f.root,f.db,set,options);check(result.status=="cancelled"&&!result.gates_passed&&result.targets.size()==2&&fs::exists(cg::from_utf8(result.workspace)/"validation.json"),"cancelled set lost component records");
            auto early=f.options();early.control=std::make_shared<cg::ScanControl>();early.control->request_cancel();const auto pending=cg::validate_repair_set(f.root,f.db,set,early);check(pending.status=="cancelled"&&pending.targets.size()==2,"pre-cancelled set targets disappeared");
        }else if(mode=="cli"){
            check(argc==3,"CLI required");std::vector<std::string> args{argv[2],"repair","compose",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.path/"cli-exports")};for(const auto& c:selected){args.push_back("--candidate");args.push_back(cg::utf8_path(cg::export_repair_candidate(c,f.path/"parts")/"candidate.cgfix"));}
            const auto composed=cg::run_process({args,cg::utf8_path(f.path),std::chrono::seconds(30)});check(composed.exit_code==0,"CLI compose failed: "+composed.stderr_text);std::string manifest;std::istringstream lines(composed.stdout_text);std::string line;while(std::getline(lines,line))if(line.starts_with("manifest=")){if(line.back()=='\r')line.pop_back();manifest=line.substr(9);}check(!manifest.empty(),"CLI set manifest path missing");
            auto validated=cg::run_process({{argv[2],"repair","validate-set",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.path/"cli-runs"),"--set",manifest,"--cxx-compiler",CODEGUARD_TEST_CXX},cg::utf8_path(f.path),std::chrono::seconds(60)});check(validated.exit_code==0&&validated.stdout_text.find("\"targets\": [")!=std::string::npos&&validated.stdout_text.find("\"correctness\": \"not_assessed\"")!=std::string::npos,"CLI set validation failed: "+validated.stdout_text+validated.stderr_text);
        }else throw std::runtime_error("unknown set test");f.unchanged();
    }
    std::cout<<"REPAIR_SET_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
