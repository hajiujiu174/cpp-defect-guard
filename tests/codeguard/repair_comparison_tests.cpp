#include "codeguard/repair_comparison.hpp"
#include "../../core/repair/comparison_internal.hpp"
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void write(const fs::path& p,const std::string& s){std::ofstream o(p,std::ios::binary);o<<s;check(bool(o),"fixture write failed");}
std::string read(const fs::path& p){std::ifstream i(p,std::ios::binary);return {(std::istreambuf_iterator<char>(i)),{}};}
struct Fixture {
    fs::path base=fs::temp_directory_path()/("cp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),root=base/"s",db=base/"scan.db";
    std::map<std::string,std::string> originals;cg::ScanResult snapshot;std::vector<cg::RepairCandidate> candidates;
    explicit Fixture(const std::string& mode){fs::create_directories(root);
        originals={{"zero.cpp","int chosen();int fallback();\nint zero(int n){\nif(n=0)return chosen();\nreturn fallback();\n}\n"},
            {"helpers.cpp","int chosen(){return 7;}\nint fallback(){return 0;}\n"},
            {"main.cpp","int zero(int);\nint main(){return zero(0)==7?0:1;}\n"},{"resource.txt","unchanged input\n"}};
        std::string cmake="cmake_minimum_required(VERSION 3.29)\nproject(Comparison LANGUAGES CXX)\nenable_testing()\nadd_executable(demo main.cpp zero.cpp helpers.cpp)\nadd_test(NAME [=[zero [a]&雪]=] COMMAND demo)\n";
        if(mode=="set"){
            originals["one.cpp"]="int one(int n){\nif(n=1)return 1;\nreturn 0;\n}\n";
            originals["main.cpp"]="int zero(int);int one(int);\nint main(){return zero(0)==7&&one(0)==0&&one(1)==1?0:1;}\n";
            cmake+="target_sources(demo PRIVATE one.cpp)\n";
        }
        if(mode=="catalog")cmake+="file(READ \"${CMAKE_CURRENT_SOURCE_DIR}/zero.cpp\" body)\nif(body MATCHES \"n==0\")\nadd_test(NAME patched_only COMMAND demo)\nendif()\n";
        if(mode=="abnormal")originals["helpers.cpp"]="#include <cstdlib>\nint chosen(){return 7;}\nint fallback(){std::abort();}\n";
        if(mode=="variation"){
            write(base/"counter","0");
            cmake+="file(READ \""+cg::utf8_path(base/"counter")+"\" n)\nmath(EXPR n \"${n}+1\")\nfile(WRITE \""+cg::utf8_path(base/"counter")+"\" \"${n}\")\nif(n GREATER 3)\ntarget_compile_definitions(demo PRIVATE LATER=1)\nendif()\n";
            originals["helpers.cpp"]="int later(){return 7;}\nint early(){return 7;}\nint chosen(){\n#ifdef LATER\nreturn later();\n#else\nreturn early();\n#endif\n}\nint fallback(){return 0;}\n";
        }
        originals["CMakeLists.txt"]=cmake;for(const auto& [n,s]:originals)write(root/n,s);
        auto p=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"cfg"),"-G","Ninja","-DCMAKE_CXX_COMPILER=" CODEGUARD_TEST_GNU_CXX,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)});check(p.status=="passed","fixture configure failed: "+p.stderr_text);
        cg::ScanOptions scan;scan.compile_commands=cg::utf8_path(base/"cfg");snapshot=cg::import_project(root,db,scan);
        candidates=cg::propose_repairs(snapshot,snapshot.analysis.issues);std::erase_if(candidates,[](const auto& c){return c.edit.recipe!="condition.compare";});check(candidates.size()==(mode=="set"?2:1),"fixture proposals missing");
    }
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"FIXTURE_RETAINED="<<cg::utf8_path(base)<<'\n';return;}std::error_code ec;fs::remove_all(base,ec);}
    cg::BuildOptions options(){cg::BuildOptions o;o.output_directory=base/"v";o.cxx_compiler=CODEGUARD_TEST_GNU_CXX;o.gcov=CODEGUARD_TEST_GCOV;o.timeout=std::chrono::seconds(30);o.jobs=2;return o;}
};
int main(int argc,char** argv){try{
    check(argc>=2,"mode required");const std::string mode=argv[1];Fixture f(mode);auto options=f.options();unsigned completed=0;
    if(mode=="materials")options.before_configure=[](const fs::path& work){write(work/"source"/"resource.txt","mutated\n");};
    if(mode=="cancel"||mode=="original_inputs"){
        options.control=std::make_shared<cg::ScanControl>();const auto control=options.control;
        options.progress=[&](const std::string& phase){if(phase=="repair_compare_observation_complete"){++completed;if(mode=="cancel"&&completed==2)control->request_cancel();if(mode=="original_inputs"&&completed==1)write(f.root/"resource.txt","changed original\n");}};
    }
    if(mode=="invalid"){
        for(unsigned rounds:{0u,1u,6u}){bool rejected=false;try{cg::compare_repair_coverage(f.root,f.db,f.candidates,options,rounds);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"invalid rounds accepted");}
        options.coverage_test_limit=0;bool rejected=false;try{cg::compare_repair_coverage(f.root,f.db,f.candidates,options);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"invalid test limit accepted");
    }else if(mode=="cli"){
        check(argc==3,"CLI required");auto out=cg::export_repair_candidate(f.candidates.front(),f.base/"exports");
        auto p=cg::run_process({{argv[2],"repair","compare",cg::utf8_path(f.root),"--database",cg::utf8_path(f.db),"--output",cg::utf8_path(f.base/"cli"),"--candidate",cg::utf8_path(out/"candidate.cgfix"),"--runs","2","--cxx-compiler",CODEGUARD_TEST_GNU_CXX,"--gcov-tool",CODEGUARD_TEST_GCOV},cg::utf8_path(f.base),std::chrono::seconds(150)});
        check(p.status=="passed"&&p.stdout_text.find("\"ready_for_review\": true")!=std::string::npos&&p.stdout_text.find("\"correctness\": \"not_assessed\"")!=std::string::npos,"CLI comparison failed: "+p.stdout_text+p.stderr_text);
    }else{
        const auto r=cg::compare_repair_coverage(f.root,f.db,f.candidates,options);const auto dump=cg::repair_coverage_comparison_json(r);
        check(fs::is_regular_file(cg::from_utf8(r.workspace)/"comparison.json"),"comparison ledger missing");
        if(mode=="stable"||mode=="set"){
            check(r.ready_for_review&&r.comparison_complete&&r.coverage_stable&&r.tests_stable&&r.original_cpp_unchanged&&r.known_original_inputs_unchanged,"comparison failed: "+dump);
            check(r.observations.size()==4&&r.observations[0].version=="original"&&r.observations[1].version=="patched"&&r.observations[2].version=="patched"&&r.observations[3].version=="original","fresh paired order wrong");
            bool gain=false,loss=false,modified=false;for(const auto& g:r.groups){check(g.diagnostic_only,"original failures lost diagnostic label");for(const auto& file:g.files)for(const auto& l:file.lines){if(file.file=="helpers.cpp"){gain|=l.transition=="newly_recorded";loss|=l.transition=="no_longer_recorded";}if(l.mapping=="modified_original"||l.mapping=="modified_patched"){modified=true;check(l.transition=="not_comparable","modified line compared as unchanged");}}}
            check(gain&&loss&&modified,"unchanged helper coverage or modified line mapping missing: "+dump);
            for(const auto& o:r.observations){check(o.build.scan_id==f.snapshot.id&&o.material_verified,"observation scan/material mismatch");if(o.version=="original")check(!o.standard_validation_passed&&o.build.status=="failed"&&o.test_coverage.entries.front().tests_not_run==0,"failed original treated as passed or not executed");}
        }else if(mode=="cancel")check(r.status=="cancelled"&&!r.ready_for_review&&r.observations[0].status=="collected"&&r.observations[1].status=="collected"&&r.observations[2].status=="not_run","comparison cancellation lost ledger or sealed early: "+dump);
        else if(mode=="original_inputs")check(!r.known_original_inputs_unchanged&&!r.ready_for_review&&r.observations[1].status=="not_run","original resource change did not stop observations: "+dump);
        else if(mode=="materials")check(!r.ready_for_review&&!r.all_materials_verified&&r.observations[0].status=="failed"&&r.observations[0].error.find("paired")!=std::string::npos,"copied material rewrite accepted: "+dump);
        else if(mode=="catalog")check(!r.ready_for_review&&!r.catalogs_match&&std::all_of(r.observations.begin(),r.observations.end(),[](const auto& o){return o.status=="collected";}),"different test catalogs compared as equal: "+dump);
        else if(mode=="abnormal")check(!r.ready_for_review&&!r.comparison_complete&&r.observations.front().status=="failed","crashed test counters treated as complete diagnostic observations: "+dump);
        else if(mode=="variation")check(!r.ready_for_review&&!r.coverage_stable&&r.status=="observed_variation","round variation accepted as stable: "+dump);
        else throw std::runtime_error("unknown mode");
    }
    for(const auto& [n,s]:f.originals)if(mode!="original_inputs"||n!="resource.txt")check(read(f.root/n)==s,"original input changed: "+n);
    std::cout<<"COMPARISON_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
