#include "codeguard/repair.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
template<class F>void rejects(F work){try{work();}catch(const std::exception&){return;}throw std::runtime_error("invalid batch was accepted");}
struct Fixture {
    fs::path base=fs::temp_directory_path()/("cgb-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=base/"source",db=base/"scan.sqlite3";cg::ScanResult snapshot;
    const std::string original="int f(int n){if(n=0)return 1;return 0;}\nint extra(int n){if(n=1)return 1;return 0;}\nint main(){return f(0)==1&&f(1)==0?0:1;}\n";
    Fixture(){
        fs::create_directories(root);write(original);
        {std::ofstream out(root/"CMakeLists.txt");out<<"cmake_minimum_required(VERSION 3.24)\nproject(RepairBatch LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp)\nadd_test(NAME behavior COMMAND demo)\n";}
        const auto configured=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"configure"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)});
        check(configured.status=="passed","batch fixture configure failed");cg::ScanOptions o;o.compile_commands=cg::utf8_path(base/"configure");snapshot=cg::import_project(root,db,o);
    }
    ~Fixture(){std::error_code error;fs::remove_all(base,error);}
    void write(const std::string& text){std::ofstream out(root/"main.cpp",std::ios::binary);out<<text;}
    std::vector<cg::RepairCandidate> candidates(){
        std::vector<cg::RepairCandidate> out;
        for(const auto& issue:snapshot.analysis.issues)if(issue.rule_id=="CG003"){
            auto items=cg::propose_repairs(snapshot,issue);out.insert(out.end(),items.begin(),items.end());
        }
        check(out.size()==4,"expected two alternatives for two assignments");return out;
    }
    cg::BuildOptions options(){cg::BuildOptions o;o.output_directory=base/"runs";o.cxx_compiler=CODEGUARD_TEST_CXX;o.jobs=2;o.timeout=std::chrono::seconds(30);return o;}
};
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];
    if(mode=="ranking"){
        cg::RepairBatchEntry unrun;unrun.candidate_id="pending";unrun.changed_bytes=1;
        cg::RepairBatchEntry failed;failed.candidate_id="failed";failed.validation.status="tests_failed";
        cg::BuildStep build;build.name="build";build.result.status="passed";failed.validation.build.steps.push_back(build);
        cg::RepairBatchEntry success=failed;success.candidate_id="success";success.changed_bytes=5;success.validation.status="gates_passed";success.validation.gates_passed=true;
        cg::RepairBatchEntry cancelled;cancelled.candidate_id="cancelled";cancelled.validation.status="cancelled";
        auto smaller=success;smaller.candidate_id="smaller";smaller.changed_bytes=1;
        std::vector<cg::RepairBatchEntry> entries{unrun,failed,success,cancelled,smaller};
        check(cg::rank_repairs(entries)==std::vector<std::size_t>({4,2,1,3,0}),"evidence tier/size ranking is wrong");
        const auto copy=entries;cg::rank_repairs(entries);check(entries[0].candidate_id==copy[0].candidate_id,"ranking mutated input records");
        cg::RepairBatch batch;batch.entries=entries;const auto json=cg::repair_batch_json(batch);
        check(json.find("\"test_coverage\": \"not_measured\"")!=std::string::npos&&json.find("\"correctness\": \"not_assessed\"")!=std::string::npos&&json.find("\"all_gates_passed\": false")!=std::string::npos,"ranking fabricates correctness/coverage");
        cg::RepairBatchEntry equal=smaller;equal.input_index=3;smaller.input_index=1;
        check(cg::rank_repairs({equal,smaller})==std::vector<std::size_t>({1,0}),"equal evidence is not deterministic");
    }else{
        Fixture f;auto items=f.candidates(); // line 1 compare/intent, line 2 compare/intent
        check(items[0].edit.recipe=="condition.compare"&&items[0].edit.line==1&&items[1].edit.recipe=="condition.intent","fixture order changed");
        if(mode=="workflow"){
            const auto result=cg::evaluate_repairs(f.root,f.db,{items[1],items[0]},f.options());
            check(result.status=="completed"&&result.attempted==2&&result.entries[0].validation.status=="tests_failed"&&result.entries[1].validation.gates_passed,"batch discarded failure or stopped before valid candidate");
            check(cg::rank_repairs(result.entries)==std::vector<std::size_t>({1,0}),"batch did not prioritize verified candidate");
            check(result.entries[1].changed_bytes==1&&result.entries[0].changed_bytes==2,"changed-byte count includes unchanged AST context");
            const auto dir=cg::from_utf8(result.workspace);check(fs::exists(dir/"progress-0000.json")&&fs::exists(dir/"progress-0002.json")&&fs::exists(dir/"batch.json"),"batch checkpoints missing");
            for(const auto& entry:result.entries)check(fs::exists(cg::from_utf8(entry.manifest).parent_path()/"batch-validation.json"),"per-candidate terminal record missing");
            rejects([&]{cg::evaluate_repairs(f.root,f.db,{items[0],items[0]},f.options());});
            rejects([&]{cg::evaluate_repairs(f.root,f.db,{},f.options());});
            rejects([&]{cg::evaluate_repairs(f.root,f.db,std::vector<cg::RepairCandidate>(65,items[0]),f.options());});
            auto inside=f.options();inside.output_directory=f.root/"bad";rejects([&]{cg::evaluate_repairs(f.root,f.db,{items[0]},inside);});
            f.write(f.original+"// external change\n");const auto stale=cg::evaluate_repairs(f.root,f.db,{items[0]},f.options());
            check(stale.entries[0].validation.status=="failed"&&stale.entries[0].validation.build.workspace.empty()&&fs::exists(cg::from_utf8(stale.entries[0].manifest).parent_path()/"batch-validation.json"),"pre-build exception was dropped");f.write(f.original);
        }else if(mode=="cancel"){
            auto options=f.options();options.control=std::make_shared<cg::ScanControl>();const auto parent=options.control;
            options.progress=[parent](const std::string& phase){if(phase=="candidate 2/3: configure")parent->request_cancel();};
            const auto batch=cg::evaluate_repairs(f.root,f.db,{items[0],items[1],items[2]},options);
            check(batch.status=="cancelled"&&batch.attempted==2&&batch.entries[0].validation.gates_passed&&batch.entries[1].validation.status=="cancelled"&&batch.entries[2].validation.status=="not_run","first candidate's commit fence blocked later cancellation");
            check(!batch.entries[2].validation.error.empty()&&fs::exists(cg::from_utf8(batch.workspace)/"batch.json"),"cancelled batch lost pending entry");
            auto early=f.options();early.control=std::make_shared<cg::ScanControl>();early.control->request_cancel();const auto pending=cg::evaluate_repairs(f.root,f.db,{items[0],items[1]},early);
            check(pending.status=="cancelled"&&pending.attempted==0&&pending.entries.size()==2,"pre-cancelled batch executed or lost entries");
            for(const auto& e:pending.entries)check(e.validation.status=="not_run"&&fs::exists(cg::from_utf8(e.manifest)),"pre-cancelled candidate was not retained");
        }else if(mode=="source_changed"){
            auto options=f.options();options.progress=[&](const std::string& phase){if(phase=="candidate 2/3: build")f.write(f.original+"// simulated external edit\n");};
            const auto batch=cg::evaluate_repairs(f.root,f.db,{items[0],items[1],items[2]},options);
            check(batch.status=="source_changed"&&batch.attempted==2&&!batch.entries[1].validation.build.source_unchanged&&batch.entries[2].validation.status=="not_run","source change did not stop remaining candidates");
            check(batch.entries[2].validation.error.find("source changed")!=std::string::npos,"pending source-change reason lost");f.write(f.original);
        }else if(mode=="cli"){
            check(argc==3,"CLI required");const auto a=cg::export_repair_candidate(items[1],f.base/"exports")/"candidate.cgfix",b=cg::export_repair_candidate(items[0],f.base/"exports")/"candidate.cgfix";
            auto args=std::vector<std::string>{argv[2],"repair","evaluate",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.base/"cli-runs"),"--candidate",cg::utf8_path(a),"--candidate",cg::utf8_path(b),"--cxx-compiler",CODEGUARD_TEST_CXX};
            const auto run=cg::run_process({args,cg::utf8_path(f.base),std::chrono::seconds(90)});
            check(run.exit_code==0&&run.stdout_text.find("\"gates_passed_count\": 1")!=std::string::npos&&run.stdout_text.find("\"input_index\": 1")<run.stdout_text.find("\"input_index\": 0"),"CLI batch repeated options/ranking failed: "+run.stdout_text+run.stderr_text);
            args[args.size()-3]=cg::utf8_path(a);check(cg::run_process({args,cg::utf8_path(f.base),std::chrono::seconds(20)}).exit_code!=0,"CLI accepted duplicate candidate IDs");
        }else throw std::runtime_error("unknown batch mode");
        check(read(f.root/"main.cpp")==f.original,"batch modified original source");
    }
    std::cout<<"REPAIR_BATCH_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
