#include "codeguard/repair.hpp"
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
struct Fixture{
    fs::path base=fs::temp_directory_path()/("cp7-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())),root=base/"s",db=base/"scan.db";
    std::string original;
    Fixture(std::string text,bool c=false):original(std::move(text)){fs::create_directories(root);std::ofstream(root/(c?"main.c":"main.cpp"),std::ios::binary)<<original;
        std::ofstream(root/"CMakeLists.txt")<<"cmake_minimum_required(VERSION 3.24)\nproject(Precedence LANGUAGES "<<(c?"C":"CXX")<<")\nenable_testing()\nadd_executable(demo "<<(c?"main.c":"main.cpp")<<")\nadd_test(NAME behavior COMMAND demo)\n";
        const auto p=cg::run_process({{"cmake","-S",cg::utf8_path(root),"-B",cg::utf8_path(base/"cfg"),"-G","Ninja",std::string(c?"-DCMAKE_C_COMPILER=":"-DCMAKE_CXX_COMPILER=")+(c?CODEGUARD_TEST_CC:CODEGUARD_TEST_CXX),"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},cg::utf8_path(base),std::chrono::seconds(30)});check(p.status=="passed","configure failed: "+p.stderr_text);
    }
    ~Fixture(){if(std::uncaught_exceptions()){std::cerr<<"FIXTURE_RETAINED="<<cg::utf8_path(base)<<'\n';return;}std::error_code ec;fs::remove_all(base,ec);}
    cg::ScanResult scan(){cg::ScanOptions o;o.compile_commands=cg::utf8_path(base/"cfg");return cg::import_project(root,db,o);}
    cg::BuildOptions build(){cg::BuildOptions o;o.output_directory=base/"v";o.c_compiler=CODEGUARD_TEST_CC;o.cxx_compiler=CODEGUARD_TEST_CXX;o.jobs=2;return o;}
};
int main(int argc,char** argv){try{check(argc==2,"mode required");const std::string mode=argv[1];
    if(mode=="detect"||mode=="c"){
        const std::string source="int ret(void){return -7;}\n"
            "int a(int n){int e=0;if((e=ret()<0)||n)return e;return 0;}\n"
            "int b(int n){int e=0;while(n&&(e=ret()<0))return e;return 0;}\n"
            "int c(int n){int e=0;do{--n;}while(n&&(e=ret()<0));return e;}\n"
            "int d(int n){int e=0;for(;n&&(e=ret()<0);--n)return e;return 0;}\n"
            "int good(int n){int e=0;if((e=ret())<0||n)return e;if((e=(ret()<0)))return e;return 0;}\n"
            "int main(void){return 0;}\n";
        Fixture f(source,mode=="c");const auto s=f.scan();std::vector<cg::Issue> issues;for(const auto& i:s.analysis.issues)if(i.rule_id=="CG003")issues.push_back(i);check(issues.size()==4,"nested branch precedence detection or explicit-intent negatives changed");
        auto all=cg::propose_repairs(s,issues);check(all.size()==12,"precedence recipes missing");check(std::count_if(all.begin(),all.end(),[](const auto& c){return c.edit.recipe=="condition.assignment_result";})==4,"new recipe missing across loop conditions");
        for(const auto& c:all)check(cg::decode_repair_candidate(cg::encode_repair_candidate(c)).id==c.id,"new recipe manifest roundtrip failed");
    }else if(mode=="validate"||mode=="set"){
        std::string source="#include <cstdio>\nint ret(){return -7;}\nint f(int n){int error=0;if((error=ret()<0)||n)return error;return 0;}\n";
        if(mode=="set")source+="int g(int n){int error=0;if(n&&(error=ret()<0))return error;return 0;}\n";
        source+=mode=="set"?"int main(){return f(0)==-7&&g(1)==-7?0:1;}\n":"int main(){return f(0)==-7?0:1;}\n";
        Fixture f(source);const auto s=f.scan();auto candidates=cg::propose_repairs(s,s.analysis.issues);std::vector<cg::RepairCandidate> fixes;
        for(const auto& c:candidates)if(c.edit.recipe=="condition.assignment_result")fixes.push_back(c);
        check(fixes.size()==(mode=="set"?2:1),"eligible precedence target count changed");
        cg::RepairValidation r=mode=="set"?cg::validate_repair_set(f.root,f.db,cg::compose_repairs(fixes),f.build()):cg::validate_repair(f.root,f.db,fixes.front(),f.build());
        check(r.gates_passed&&r.new_or_unmasked_issues==0&&r.correctness=="not_assessed","precedence repair rejected: "+cg::repair_validation_json(r));
        std::vector<cg::RepairCandidate> intents;for(const auto& c:candidates)if(c.edit.recipe=="condition.intent")intents.push_back(c);
        r=mode=="set"?cg::validate_repair_set(f.root,f.db,cg::compose_repairs(intents),f.build()):cg::validate_repair(f.root,f.db,intents.front(),f.build());
        check(!r.gates_passed&&r.target_not_detected&&r.status=="tests_failed","intent confused with return-code repair: "+cg::repair_validation_json(r));
    }else if(mode=="mapping"){
        Fixture f("#include <cstdio>\nint f(){char b[8];int error=0;if((error=std::sprintf(b,\"x\")<0)||error)return error;return 0;}\nint main(){return f()==1?0:1;}\n");const auto s=f.scan();std::vector<cg::Issue> selected;for(const auto& i:s.analysis.issues)if(i.rule_id=="CG003")selected.push_back(i);check(selected.size()==1,"mapping target missing");
        const auto all=cg::propose_repairs(s,selected);for(const auto& c:all)if(c.edit.recipe=="condition.assignment_result"){
            const auto r=cg::validate_repair(f.root,f.db,c,f.build());check(r.gates_passed&&r.new_or_unmasked_issues==0,"unchanged inner sprintf was counted as new after inserting parentheses: "+cg::repair_validation_json(r));
        }
    }else throw std::runtime_error("unknown mode");
    std::cout<<"CONDITION_PRECEDENCE_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
