#include "comparison_internal.hpp"
#include <algorithm>
#include <sstream>
#include <tuple>

namespace codeguard {
namespace {
using Span=std::pair<std::size_t,std::size_t>;
std::vector<Span> spans(const std::string& text){std::vector<Span> out;for(std::size_t at=0;at<text.size();){const auto newline=text.find('\n',at),end=newline==std::string::npos?text.size():newline+1;out.emplace_back(at,end);at=end;}return out;}
using Lines=std::map<std::pair<std::string,int>,bool>;
struct Sample{const CoverageObservation* observation;Lines records;};
Lines lines(const RepairCoverage& coverage){Lines out;for(const auto& file:coverage.files)for(const auto& [line,hit]:file.lines)out[{file.file,line}]=hit;return out;}
std::vector<std::string> sorted(std::vector<std::string> names){std::sort(names.begin(),names.end());return names;}
const RepairTestCoverageEntry* group(const CoverageObservation& o,const std::string& name){for(const auto& e:o.test_coverage.entries)if(e.name==name)return &e;return nullptr;}
CoverageLineHistory history(const std::vector<Sample>& samples,unsigned rounds,const std::string& version,const std::string& file,int line){
    CoverageLineHistory out;if(line<=0)return out;
    for(const auto& sample:samples)if(sample.observation->version==version){
        ++out.observations;if(const auto at=sample.records.find({file,line});at!=sample.records.end()){++out.instrumented;out.recorded+=at->second;}
    }
    out.state=out.observations!=rounds?"unknown":out.instrumented==0?"not_instrumented":out.instrumented!=rounds?"instrumentation_varies":out.recorded==0?"never_recorded":out.recorded==rounds?"always_recorded":"varies";return out;
}
std::string transition(const CoverageLineChange& line){
    if(line.mapping!="unchanged")return "not_comparable";const auto& a=line.original.state;const auto& b=line.patched.state;
    if(a=="unknown"||b=="unknown")return "unknown";
    if(a=="varies"||b=="varies"||a=="instrumentation_varies"||b=="instrumentation_varies")return "observed_variation";
    if(a=="not_instrumented")return b==a?"not_instrumented":"instrumentation_added";
    if(b=="not_instrumented")return "instrumentation_removed";
    if(a==b)return "unchanged";return a=="never_recorded"?"newly_recorded":"no_longer_recorded";
}
std::string test_signature(const CoverageObservation& o,const std::string& name){
    std::ostringstream out;if(name.empty()){for(const auto& s:o.build.steps)if(s.name=="test")out<<s.result.status<<':'<<s.tests_total<<':'<<s.tests_failed<<':'<<s.tests_skipped;}
    else if(const auto* e=group(o,name))out<<e->status<<':'<<e->tests_total<<':'<<e->tests_failed<<':'<<e->tests_skipped<<':'<<e->tests_disabled<<':'<<e->tests_not_run;
    return out.str();
}
}
RepairLineCorrespondence repair_line_correspondence(const SourceReplacement& change,const std::vector<RepairHint>& input){
    if(change.before.size()>2*1024*1024||change.after.size()>2*1024*1024)throw std::invalid_argument("line correspondence supports files up to 2 MiB");
    std::vector<RepairHint> edits;for(const auto& e:input)if(e.file==change.file)edits.push_back(e);std::sort(edits.begin(),edits.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    std::size_t previous=0;for(const auto& e:edits){if(e.offset>change.before.size()||e.before.size()>change.before.size()-e.offset||e.offset<previous||change.before.compare(static_cast<std::size_t>(e.offset),e.before.size(),e.before)!=0)throw std::invalid_argument("invalid original byte ranges for coverage mapping");previous=static_cast<std::size_t>(e.offset)+e.before.size();}
    auto after=change.before;for(auto it=edits.rbegin();it!=edits.rend();++it)after.replace(static_cast<std::size_t>(it->offset),it->before.size(),it->after);if(after!=change.after)throw std::invalid_argument("coverage mapping does not reproduce proposed file");
    const auto original=spans(change.before),patched=spans(change.after);RepairLineCorrespondence out;std::map<std::size_t,int> starts;for(std::size_t n=0;n<patched.size();++n){starts[patched[n].first]=static_cast<int>(n+1);out.modified_patched.insert(static_cast<int>(n+1));}
    for(std::size_t n=0;n<original.size();++n){const auto [begin,end]=original[n];bool touched=false;std::int64_t delta=0;
        for(const auto& e:edits){if((!e.before.empty()&&e.offset<end&&e.offset+e.before.size()>begin)||(e.before.empty()&&e.offset>=begin&&e.offset<end))touched=true;if(e.offset+e.before.size()<=begin)delta+=static_cast<std::int64_t>(e.after.size())-static_cast<std::int64_t>(e.before.size());}
        const auto at=static_cast<std::int64_t>(begin)+delta;int line=0;if(!touched&&at>=0){const auto found=starts.find(static_cast<std::size_t>(at));if(found!=starts.end()){const auto& span=patched[static_cast<std::size_t>(found->second-1)];if(span.second-span.first==end-begin&&change.after.compare(span.first,end-begin,change.before,begin,end-begin)==0)line=found->second;}}
        if(line){if(!out.modified_patched.erase(line))throw std::runtime_error("ambiguous coverage line correspondence");out.original_to_patched.emplace(static_cast<int>(n+1),line);}else out.modified_original.insert(static_cast<int>(n+1));
    }return out;
}
void summarize_repair_coverage(RepairCoverageComparison& result,const RepairPlan& plan,const ScanContext& context){
    result.groups.clear();result.comparison_complete=false;result.ready_for_review=false;result.coverage_stable=false;result.tests_stable=false;result.catalogs_match=false;result.tools_match=false;result.all_materials_verified=false;result.all_patched_validations_passed=false;
    if(result.rounds<2||result.rounds>5)throw std::invalid_argument("coverage comparison requires 2..5 rounds");
    std::set<std::pair<unsigned,std::string>> identities;std::set<std::string> names{std::string{}};const CoverageObservation* first=nullptr;bool complete=result.observations.size()==2*result.rounds,catalogs=true,tools=true,materials=true,patched_valid=true,stable=true,tests_stable=true;
    for(const auto& o:result.observations){context.check();if(o.round>=result.rounds||(o.version!="original"&&o.version!="patched")||!identities.emplace(o.round,o.version).second)throw std::invalid_argument("duplicate or invalid coverage observation identity");
        const bool valid=o.status=="collected"&&o.coverage.status=="collected"&&o.material_verified;complete=complete&&valid;materials=materials&&o.material_verified;
        if(o.version=="patched")patched_valid=patched_valid&&o.standard_validation_passed&&valid;
        if(valid){tools=tools&&!o.coverage.tool_version.empty()&&!o.test_coverage.ctest_version.empty();if(!first)first=&o;else{catalogs=catalogs&&!o.normalized_catalog.empty()&&o.normalized_catalog==first->normalized_catalog;tools=tools&&o.coverage.tool_version==first->coverage.tool_version&&o.test_coverage.ctest_version==first->test_coverage.ctest_version;}}
        for(const auto& e:o.test_coverage.entries)names.insert(e.name);stable=stable&&o.all_group_line_union_matches_aggregate;
    }
    std::map<std::string,RepairLineCorrespondence> maps;std::vector<RepairHint> edits;for(const auto& a:plan.anchors)edits.push_back(a.component->edit);for(const auto& file:plan.files)maps.emplace(file.file,repair_line_correspondence(file,edits));
    std::size_t rows=0;
    for(const auto& name:names){context.check();CoverageGroupComparison output;output.name=name;std::map<std::string,std::set<int>> old_lines,new_lines;std::map<std::string,std::pair<Lines,std::string>> previous;
        bool selected_known=false;std::vector<Sample> samples;
        for(const auto& o:result.observations)if(o.status=="collected"&&o.material_verified){
            const RepairCoverage* data=&o.coverage;
            if(!name.empty()){const auto* e=group(o,name);if(!e){complete=false;continue;}data=&e->coverage;auto selected=sorted(e->selected_tests);if(!selected_known){output.selected_tests=selected;selected_known=true;}else if(selected!=output.selected_tests)catalogs=false;if(o.version=="original"&&e->tests_failed>0)output.diagnostic_only=true;}
            else if(o.version=="original")for(const auto& step:o.build.steps)if(step.name=="test"&&step.tests_failed>0)output.diagnostic_only=true;
            const auto current=lines(*data);const auto signature=test_signature(o,name);if(const auto p=previous.find(o.version);p!=previous.end()){stable=stable&&p->second.first==current;tests_stable=tests_stable&&p->second.second==signature;}else previous[o.version]={current,signature};
            samples.push_back({&o,current});
            auto& files=o.version=="original"?old_lines:new_lines;for(const auto& f:data->files)for(const auto& [line,hit]:f.lines){(void)hit;files[f.file].insert(line);}
        }
        std::set<std::string> files;for(const auto& [file,values]:old_lines){(void)values;files.insert(file);}for(const auto& [file,values]:new_lines){(void)values;files.insert(file);}
        for(const auto& file:files){CoverageFileComparison f;f.file=file;std::set<int> consumed;
            auto add=[&](int old_line,int new_line,const std::string& mapping){if(++rows>200000)throw std::runtime_error("coverage comparison exceeds 200000 line rows");if(rows%512==0)context.check();CoverageLineChange c;c.original_line=old_line;c.patched_line=new_line;c.mapping=mapping;c.original=history(samples,result.rounds,"original",file,old_line);c.patched=history(samples,result.rounds,"patched",file,new_line);c.transition=transition(c);f.lines.push_back(std::move(c));};
            if(const auto m=maps.find(file);m!=maps.end()){
                std::map<int,int> inverse;for(const auto& [a,b]:m->second.original_to_patched)inverse.emplace(b,a);
                for(int line:old_lines[file]){const auto at=m->second.original_to_patched.find(line);if(at!=m->second.original_to_patched.end()){add(line,at->second,"unchanged");consumed.insert(at->second);}else add(line,0,m->second.modified_original.contains(line)?"modified_original":"unmapped_original");}
                for(int line:new_lines[file])if(!consumed.contains(line)){if(const auto at=inverse.find(line);at!=inverse.end())add(at->second,line,"unchanged");else add(0,line,m->second.modified_patched.contains(line)?"modified_patched":"unmapped_patched");}
            }else{auto values=old_lines[file];values.insert(new_lines[file].begin(),new_lines[file].end());for(int line:values)add(line,line,"unchanged");}
            output.files.push_back(std::move(f));
        }result.groups.push_back(std::move(output));
    }
    result.catalogs_match=first&&catalogs;result.tools_match=first&&tools;result.all_materials_verified=complete&&materials;
    result.comparison_complete=complete&&result.catalogs_match&&result.tools_match;result.coverage_stable=result.comparison_complete&&stable;result.tests_stable=result.comparison_complete&&tests_stable;result.all_patched_validations_passed=complete&&patched_valid;
    result.ready_for_review=result.comparison_complete&&result.coverage_stable&&result.tests_stable&&result.all_patched_validations_passed&&result.original_cpp_unchanged&&result.known_original_inputs_unchanged;
    result.status=!result.comparison_complete?"incomplete":!result.coverage_stable||!result.tests_stable?"observed_variation":result.ready_for_review?"ready_for_review":"compared";
}
}
