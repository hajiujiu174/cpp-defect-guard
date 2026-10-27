#include "comparison_internal.hpp"
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("invalid comparison accepted");}
cg::RepairHint edit(std::string file,std::size_t at,std::string before,std::string after){cg::RepairHint e;e.file=std::move(file);e.offset=at;e.before=std::move(before);e.after=std::move(after);return e;}
cg::CoverageObservation observation(unsigned round,const std::string& version){
    cg::CoverageObservation o;o.round=round;o.version=version;o.status="collected";o.material_verified=true;o.standard_validation_passed=version=="patched";o.all_group_line_union_matches_aggregate=true;o.normalized_catalog="fixture-catalog";o.coverage.status="collected";o.coverage.tool_version="fixture-gcov";o.test_coverage.ctest_version="fixture-ctest";
    cg::CoverageFile file;file.file="helpers.cpp";file.lines={{1,version=="patched"},{2,version=="original"},{3,true}};o.coverage.files={file};cg::BuildStep s;s.name="test";s.result.status=version=="patched"?"passed":"failed";s.tests_total=1;s.tests_failed=version=="patched"?0:1;s.tests_skipped=0;o.build.steps={s};
    cg::RepairTestCoverageEntry e;e.name="test [雪]&";e.selected_tests={e.name,"fixture"};e.coverage=o.coverage;e.tests_total=2;e.tests_failed=s.tests_failed;e.tests_skipped=e.tests_disabled=e.tests_not_run=0;e.status=s.tests_failed?"tests_incomplete":"collected";o.test_coverage.entries={e};return o;
}
cg::RepairCoverageComparison comparison(){cg::RepairCoverageComparison r;r.rounds=2;r.original_cpp_unchanged=r.known_original_inputs_unchanged=true;for(unsigned n=0;n<2;++n){r.observations.push_back(observation(n,"original"));r.observations.push_back(observation(n,"patched"));}return r;}
const cg::CoverageLineChange& row(const cg::RepairCoverageComparison& r,int line){for(const auto& f:r.groups.front().files)if(f.file=="helpers.cpp")for(const auto& v:f.lines)if(v.original_line==line&&v.patched_line==line)return v;throw std::runtime_error("line row missing");}
int main(int argc,char** argv){try{check(argc==2,"mode required");const std::string mode=argv[1];
    if(mode=="mapping"){
        const std::string before="\xef\xbb\xbfhead\r\nrepeat\r\nrepeat\r\ntail";const auto offset=before.find("repeat");const auto e=edit("file.cpp",offset,"repeat","changed\nadded");auto after=before;after.replace(offset,e.before.size(),e.after);const cg::SourceReplacement source{"file.cpp",before,after};const auto map=cg::repair_line_correspondence(source,{e});
        check(map.original_to_patched.at(1)==1&&map.original_to_patched.at(3)==4&&map.original_to_patched.at(4)==5,"unchanged byte coordinates or no-final-newline mapping lost");check(map.modified_original==std::set<int>{2}&&map.modified_patched==std::set<int>({2,3}),"modified ranges guessed by duplicate text");
        auto invalid=e;invalid.before="wrong";rejects([&]{cg::repair_line_correspondence(source,{invalid});});rejects([&]{cg::repair_line_correspondence(source,{e,e});});
        const cg::SourceReplacement insertion{"f","a\nb\n","a\nnew\nb\n"};const auto inserted=cg::repair_line_correspondence(insertion,{edit("f",2,"","new\n")});check(inserted.original_to_patched.at(1)==1&&inserted.modified_original.contains(2),"insertion boundary was falsely paired");
        const auto empty=cg::repair_line_correspondence({"f","","new"},{edit("f",0,"","new")});check(empty.modified_patched==std::set<int>{1}&&empty.original_to_patched.empty(),"empty original mapping wrong");
    }else{
        auto r=comparison();cg::RepairPlan plan;
        if(mode=="variation")r.observations[3].coverage.files.front().lines[1]=false;
        if(mode=="tests")r.observations[2].build.steps.front().tests_failed=0;
        if(mode=="missing")r.observations.back().status="not_run";
        if(mode=="catalog")r.observations.back().normalized_catalog="different";
        if(mode=="tools")r.observations.back().coverage.tool_version="different";
        if(mode=="instrumentation"){for(auto& o:r.observations)if(o.version=="original")o.coverage.files.front().lines.erase(1);}
        if(mode=="materials")r.observations.back().material_verified=false;
        if(mode=="modified"){
            cg::RepairCandidate c;c.edit=edit("helpers.cpp",0,"a","A");plan.files={{"helpers.cpp","a\nb\nc\n","A\nb\nc\n"}};plan.anchors={{&c,0,1,1}};cg::summarize_repair_coverage(r,plan);check(!r.groups.front().files.front().lines.empty(),"edited file comparison missing");bool original=false,patched=false;for(const auto& x:r.groups.front().files.front().lines){if(x.original_line==1){original=x.mapping=="modified_original"&&x.patched_line==0&&x.transition=="not_comparable";}if(x.patched_line==1){patched=x.mapping=="modified_patched"&&x.original_line==0&&x.transition=="not_comparable";}}check(original&&patched,"modified source lines treated as comparable coverage gain");
        }else{
            cg::summarize_repair_coverage(r,plan);
            if(mode=="stable")check(r.ready_for_review&&r.groups.size()==2&&r.groups.front().diagnostic_only&&row(r,1).transition=="newly_recorded"&&row(r,2).transition=="no_longer_recorded","stable diagnostic baseline comparison failed");
            else if(mode=="variation")check(r.comparison_complete&&!r.coverage_stable&&!r.ready_for_review&&row(r,1).transition=="observed_variation","coverage variation hidden");
            else if(mode=="tests")check(r.comparison_complete&&!r.tests_stable&&!r.ready_for_review,"test result variation hidden");
            else if(mode=="instrumentation")check(row(r,1).transition=="instrumentation_added"&&row(r,1).original.state=="not_instrumented","uninstrumented source labeled unexecuted");
            else if(mode=="missing"||mode=="catalog"||mode=="tools"||mode=="materials")check(!r.comparison_complete&&!r.ready_for_review,"incomplete or mismatched comparison passed");
            else if(mode=="invalid"){r.observations.back().round=0;rejects([&]{cg::summarize_repair_coverage(r,plan);});}
            else throw std::runtime_error("unknown mode");
        }
    }std::cout<<"COVERAGE_DELTA_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
