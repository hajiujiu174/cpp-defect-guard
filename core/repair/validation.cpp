#include "repair_internal.hpp"
#include "coverage_internal.hpp"
#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>
#include <iomanip>
#include <sstream>

namespace codeguard {
namespace {
std::string read(const fs::path& path){if(!fs::is_regular_file(path)||fs::file_size(path)>2*1024*1024)throw std::invalid_argument("repair source file unavailable or exceeds 2 MiB");std::ifstream in(path,std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});if(!in||text.size()>2*1024*1024)throw std::runtime_error("cannot read repair source");return text;}
void save(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary);out.write(text.data(),static_cast<std::streamsize>(text.size()));if(!out)throw std::runtime_error("cannot write repair validation record");}
template<class A,class B> bool same_inventory(const A& a,const B& b){if(!a.diagnostics.empty()||!b.diagnostics.empty()||a.files.size()!=b.files.size())return false;for(std::size_t n=0;n<a.files.size();++n)if(a.files[n].path!=b.files[n].path||a.files[n].hash!=b.files[n].hash||a.files[n].size!=b.files[n].size)return false;return true;}
std::string source_hash(const std::string& text){std::uint64_t h=14695981039346656037ULL;for(unsigned char c:text){h^=c;h*=1099511628211ULL;}std::ostringstream out;out<<"fnv1a64-v1:"<<std::hex<<std::setw(16)<<std::setfill('0')<<h;return out.str();}
std::size_t position(const std::string& text,int line,int column){if(line<1||column<1)return text.size()+1;std::size_t at=0;for(int n=1;n<line;++n){at=text.find('\n',at);if(at==std::string::npos)return text.size()+1;++at;}return at+static_cast<std::size_t>(column-1);}
std::pair<int,int> location(const std::string& text,std::size_t at){int line=1,column=1;for(std::size_t n=0;n<at;++n){if(text[n]=='\n'){++line;column=1;}else ++column;}return {line,column};}
std::optional<std::size_t> one_inserted_parenthesis(const std::string& before,const std::string& after,std::size_t at,char inserted){
    if(after.size()!=before.size()+1||at>=after.size())return {};
    std::size_t prefix=0,suffix=0;while(prefix<before.size()&&before[prefix]==after[prefix])++prefix;
    while(suffix<before.size()&&before[before.size()-1-suffix]==after[after.size()-1-suffix])++suffix;
    const auto begin=before.size()-suffix;std::optional<std::size_t> mapped;
    for(auto p=begin;p<=prefix;++p)if(after[p]==inserted){if(at==p)return {};const auto value=at-(at>p?1:0);if(mapped&&*mapped!=value)return {};mapped=value;}
    return mapped;
}
std::optional<std::size_t> within_edit(const RepairHint& edit,std::size_t at){
    const auto& before=edit.before;const auto& after=edit.after;
    if(edit.recipe=="condition.intent"&&after=="("+before+")")return at>0&&at<=before.size()?std::optional<std::size_t>(at-1):std::nullopt;
    if(edit.recipe=="condition.assignment_result"&&after.size()==before.size()+2&&after.front()=='(')
        return at>0?one_inserted_parenthesis(before,after.substr(1),at-1,')'):std::nullopt;
    if(edit.recipe=="condition.intent"&&after.size()==before.size()+2&&after.back()==')')
        return at+1<after.size()?one_inserted_parenthesis(before,after.substr(0,after.size()-1),at,'('):std::nullopt;
    std::size_t prefix=0,suffix=0;while(prefix<before.size()&&prefix<after.size()&&before[prefix]==after[prefix])++prefix;
    while(suffix<before.size()-prefix&&suffix<after.size()-prefix&&before[before.size()-1-suffix]==after[after.size()-1-suffix])++suffix;
    if(edit.recipe=="printf.bound"&&after.size()>before.size()+1&&prefix<after.size()&&after[prefix]=='n'){
        auto temporary=after;temporary.erase(prefix,1);const auto added=temporary.size()-before.size();std::vector<std::size_t> matches;
        for(auto p=temporary.find(", sizeof(");p!=std::string::npos;p=temporary.find(", sizeof(",p+1))if(p<=before.size()&&p+added<=temporary.size()&&temporary[p+added-1]==')'&&temporary.substr(0,p)==before.substr(0,p)&&temporary.substr(p+added)==before.substr(p))matches.push_back(p);
        if(matches.size()==1&&prefix<=matches[0]){
            const auto second=matches[0]+1;if(at==prefix||(at>=second&&at<second+added))return {};
            return at-(at>prefix?1:0)-(at>=second+added?added:0);
        }
    }
    if(at<prefix)return at;
    if(at>=after.size()-suffix)return at-after.size()+before.size();
    return {}; // edited/ambiguous source bytes do not prove an old issue location
}
std::optional<std::size_t> original_position(const RepairPlan& plan,const std::string& file,std::size_t at){
    std::int64_t delta=0;
    for(const auto& anchor:plan.anchors)if(anchor.component->edit.file==file){const auto& e=anchor.component->edit;
        if(at<anchor.revised_offset)return static_cast<std::size_t>(static_cast<std::int64_t>(at)-delta);
        if(at<anchor.revised_offset+e.after.size()){const auto relative=within_edit(e,at-static_cast<std::size_t>(anchor.revised_offset));if(!relative)return {};return static_cast<std::size_t>(e.offset)+*relative;}
        delta+=static_cast<std::int64_t>(e.after.size())-static_cast<std::int64_t>(e.before.size());
    }
    return static_cast<std::size_t>(static_cast<std::int64_t>(at)-delta);
}
void coverage_targets(RepairValidation& result){
    bool all=true,unknown=false;
    for(auto& target:result.targets){target.coverage_line_status="not_instrumented";
        for(const auto& file:result.coverage.files)if(file.file==target.file)if(const auto line=file.lines.find(target.revised_line);line!=file.lines.end())target.coverage_line_status=line->second?"recorded":"no_recorded_execution";
        all=all&&target.coverage_line_status=="recorded";unknown=unknown||target.coverage_line_status=="not_instrumented";
    }
    result.coverage.target_line_status=all?"recorded":unknown?"not_instrumented":"no_recorded_execution";
}
}
RepairValidation validate_repair_plan(const fs::path& input,const fs::path& database,const RepairPlan& plan,
    const std::string& id,BuildOptions options,const std::function<fs::path()>& export_artifact){
    if(plan.anchors.empty())throw std::invalid_argument("empty repair validation plan");const auto& first=*plan.anchors.front().component;
    const auto root=fs::canonical(input);if(!fs::equivalent(root,from_utf8(first.root)))throw std::invalid_argument("repair belongs to a different project");
    SqliteDatabase db(database,true);const auto snapshot=db.validation_inputs(utf8_path(root));
    if(snapshot.id!=first.scan_id)throw std::invalid_argument("repair scan is stale; regenerate from the current scan");
    if(first.configuration!=snapshot.analysis.configuration)throw std::invalid_argument("repair configuration differs from saved scan");
    for(const auto& anchor:plan.anchors){const auto& c=*anchor.component;
        if(std::none_of(snapshot.analysis.issues.begin(),snapshot.analysis.issues.end(),[&](const auto& i){return i.rule_id==c.edit.rule_id&&i.file==c.edit.file&&i.line==c.edit.line&&i.column==c.edit.column;}))throw std::invalid_argument("repair requires every selected issue to be active in this snapshot");
    }
    for(const auto& file:plan.files)if(read(root/from_utf8(file.file))!=file.before)throw std::invalid_argument("repair original file no longer matches exact source bytes: "+file.file);
    auto expected=snapshot;for(const auto& change:plan.files){const auto file=std::find_if(expected.files.begin(),expected.files.end(),[&](const auto& f){return f.path==change.file;});if(file==expected.files.end())throw std::invalid_argument("repair file is outside saved inventory");file->hash=source_hash(change.after);file->size=change.after.size();}
    RepairValidation r;r.candidate_id=id;r.targets=repair_plan_targets(plan);r.workspace=utf8_path(export_artifact());
    const auto dir=from_utf8(r.workspace);ScanContext context;context.control=options.control;
    auto inventory_options=[&](const fs::path& path){auto result=configured_scan_options(path,decode_config(first.configuration));result.ignored_paths=options.copy_excludes;result.context=context;return result;};
    if(options.collect_coverage){r.coverage.status="not_run";r.coverage.tool=options.gcov;r.coverage.workspace=utf8_path(dir/"coverage");}
    if(options.collect_test_coverage){r.test_coverage.status="not_run";r.test_coverage.test_limit=options.coverage_test_limit;r.test_coverage.workspace=utf8_path(dir/"test-coverage");}
    r.status="running";save(dir/"validation.json",repair_validation_json(r));
    try {
        if(options.collect_test_coverage&&!options.collect_coverage)throw std::invalid_argument("per-test coverage requires gcov instrumentation");
        context.check();options.output_directory=dir/"build-runs";options.copy_replacements=plan.files;options.defer_commit_fence=true;options.keep_unpatched_copy=true;options.configure_only=false;
        if(options.collect_coverage)prepare_repair_coverage(options,dir/"coverage");
        r.build=build_and_test(root,database,options);context.check();
        if(r.build.scan_id!=first.scan_id)throw std::runtime_error("scan changed while starting repair validation; regenerate repair");
        auto passed=[&](const std::string& name){for(const auto& step:r.build.steps)if(step.name==name)return step.result.status=="passed";return false;};
        if(!r.build.source_unchanged)throw std::runtime_error("original source changed during repair validation");
        if(passed("configure")&&passed("build")){
            for(const auto& file:plan.files)if(read(from_utf8(r.build.workspace)/"source"/from_utf8(file.file))!=file.after)throw std::runtime_error("candidate file changed during configure/build/test: "+file.file);
            if(!same_inventory(expected,scan_project(from_utf8(r.build.workspace)/"source",inventory_options(from_utf8(r.build.workspace)/"source"))))throw std::runtime_error("copied source differs from the proposed repair; an unselected C/C++ file may have changed");
            r.proposed_copy_verified=true;
            if(options.collect_coverage){if(options.progress)options.progress("repair_coverage");auto target=first;target.edit.line=plan.anchors.front().revised_line;
                r.coverage=collect_repair_coverage(r.build,target,snapshot,options,dir/"coverage");context.check();if(r.coverage.status=="collected")coverage_targets(r);
                if(options.collect_test_coverage){r.test_coverage=collect_repair_test_coverage(r.build,r.targets,snapshot,r.coverage,options,dir/"test-coverage");context.check();}
            }
            if(clang_analysis_available()){
                if(options.progress)options.progress("repair_analysis");auto config=decode_config(first.configuration);config.compile_commands=utf8_path(from_utf8(r.build.workspace)/"build");config.auto_discover=false;config.command_choices.clear();
                const auto baseline_root=from_utf8(r.build.workspace)/"baseline-source",copy_root=from_utf8(r.build.workspace)/"source";
                if(options.collect_coverage){config.compile_commands=relocate_compile_commands(config.compile_commands,utf8_path(copy_root),utf8_path(copy_root),utf8_path(dir/"analysis-commands/compile_commands.json"),true);r.coverage_commands_normalized=true;}
                auto baseline_config=config;baseline_config.compile_commands=relocate_compile_commands(config.compile_commands,utf8_path(copy_root),utf8_path(baseline_root),utf8_path(dir/"baseline-commands/compile_commands.json"));
                auto baseline_options=configured_scan_options(baseline_root,baseline_config);baseline_options.ignored_paths=options.copy_excludes;baseline_options.context=context;baseline_options.use_cache=false;
                auto baseline=scan_project(baseline_root,baseline_options);if(!same_inventory(snapshot,baseline))throw std::runtime_error("unpatched baseline changed before validation analysis");
                baseline.analysis=analyze_project(baseline,baseline_options.compile_commands,context,baseline_options.threads,{},baseline_options.disabled_rules);apply_rule_policy(baseline,baseline_config);baseline.analysis.configuration=encode_config(baseline_config);
                r.baseline_status=baseline.analysis.status;r.before_issues=baseline.analysis.issues.size();
                for(auto& target:r.targets)target.baseline_reproduced=std::any_of(baseline.analysis.issues.begin(),baseline.analysis.issues.end(),[&](const auto& i){return i.rule_id==target.rule_id&&i.file==target.file&&i.line==target.line&&i.column==target.column;});
                r.baseline_target_reproduced=std::all_of(r.targets.begin(),r.targets.end(),[](const auto& t){return t.baseline_reproduced;});
                if(!same_inventory(baseline,scan_project(baseline_root,baseline_options)))throw std::runtime_error("unpatched baseline changed during analysis");
                {SqliteDatabase saved(dir/"baseline.sqlite3");saved.save(baseline);}
                auto scan_options=configured_scan_options(copy_root,config);scan_options.ignored_paths=options.copy_excludes;scan_options.context=context;scan_options.use_cache=false;
                auto after=scan_project(copy_root,scan_options);if(!same_inventory(expected,after)){r.proposed_copy_verified=false;throw std::runtime_error("copied source differs from the proposed repair before analysis");}after.analysis=analyze_project(after,scan_options.compile_commands,context,scan_options.threads,{},scan_options.disabled_rules);
                if(after.analysis.analyzer_revision!=first.analyzer_revision)throw std::runtime_error("analyzer changed since candidate generation; regenerate repair");
                if(!same_inventory(expected,scan_project(copy_root,scan_options))){r.proposed_copy_verified=false;throw std::runtime_error("copied source differs from the proposed repair during analysis");}
                apply_rule_policy(after,config);after.analysis.configuration=encode_config(config);r.analysis_status=after.analysis.status;r.after_issues=after.analysis.issues.size();
                for(std::size_t n=0;n<plan.anchors.size();++n){const auto& a=plan.anchors[n];const auto& c=*a.component;
                    const auto file=std::find_if(plan.files.begin(),plan.files.end(),[&](const auto& f){return f.file==c.edit.file;});
                    r.targets[n].target_not_detected=after.analysis.status=="complete"&&std::none_of(after.analysis.issues.begin(),after.analysis.issues.end(),[&](const auto& i){if(i.rule_id!=c.edit.rule_id||i.file!=c.edit.file)return false;const auto at=position(file->after,i.line,i.column);return at>=a.revised_offset&&at<a.revised_offset+c.edit.after.size();});
                }
                r.target_not_detected=std::all_of(r.targets.begin(),r.targets.end(),[](const auto& t){return t.target_not_detected;});
                if(baseline.analysis.status=="complete"&&after.analysis.status=="complete"){
                    using Key=std::tuple<std::string,std::string,int,int>;std::set<Key> old;
                    for(const auto& i:baseline.analysis.issues)old.emplace(i.rule_id,i.file,i.line,i.column);r.new_or_unmasked_issues=0;
                    for(const auto& issue:after.analysis.issues){auto line=issue.line,column=issue.column;const auto file=std::find_if(plan.files.begin(),plan.files.end(),[&](const auto& f){return f.file==issue.file;});
                        if(file!=plan.files.end()){const auto mapped=original_position(plan,issue.file,position(file->after,line,column));if(!mapped||*mapped>file->before.size()){++r.new_or_unmasked_issues;continue;}const auto original=location(file->before,*mapped);line=original.first;column=original.second;}
                        if(!old.contains({issue.rule_id,issue.file,line,column}))++r.new_or_unmasked_issues;
                    }
                }
                {SqliteDatabase saved(dir/"after.sqlite3");saved.save(after);}
            }else{r.analysis_status="unavailable";r.baseline_status="unavailable";}
        }
        bool tests_complete=false;for(const auto& step:r.build.steps)if(step.name=="test")tests_complete=step.result.status=="passed"&&step.tests_total>0&&step.tests_failed==0&&step.tests_skipped==0;
        context.check();r.gates_passed=r.build.status=="passed"&&r.proposed_copy_verified&&tests_complete&&r.baseline_status=="complete"&&r.baseline_target_reproduced&&r.analysis_status=="complete"&&r.target_not_detected&&r.new_or_unmasked_issues==0;
        r.status=r.gates_passed?"gates_passed":!passed("build")?"build_failed":!passed("test")?"tests_failed":!tests_complete?"tests_incomplete":r.analysis_status=="unavailable"?"analysis_unavailable":r.baseline_status!="complete"||!r.baseline_target_reproduced?"baseline_not_reproduced":r.analysis_status!="complete"?"analysis_incomplete":!r.target_not_detected?"target_still_detected":"new_or_unmasked_issues";
        if(options.collect_coverage&&r.gates_passed&&r.coverage.status!="collected"){r.gates_passed=false;r.status="coverage_failed";}
        if(options.collect_coverage&&r.gates_passed&&r.coverage.target_line_status!="recorded"){r.gates_passed=false;r.status="coverage_target_uncovered";}
        if(options.collect_test_coverage&&r.gates_passed&&r.test_coverage.status!="collected"){r.gates_passed=false;r.status="test_coverage_incomplete";}
    }catch(const std::exception& e){r.error=e.what();r.status=options.control&&options.control->state()==ScanControl::State::cancel_requested?"cancelled":"failed";r.gates_passed=false;}
    if(options.collect_coverage&&r.coverage.status=="not_run"){r.coverage.status=r.status=="cancelled"?"cancelled":"failed";r.coverage.error=r.error.empty()?"Coverage collection was not reached; inspect build/test records.":r.error;}
    if(options.collect_test_coverage&&r.test_coverage.status=="not_run"){r.test_coverage.status=r.status=="cancelled"?"cancelled":"failed";r.test_coverage.error=r.error.empty()?"Per-test collection was not reached; inspect aggregate build/test records.":r.error;}
    if(!r.build.workspace.empty())try{auto verification=inventory_options(root);verification.context={};r.build.source_unchanged=same_inventory(snapshot,scan_project(root,verification));if(!r.build.source_unchanged){r.gates_passed=false;if(r.status!="cancelled")r.status="source_changed";}}catch(const std::exception& e){r.build.source_unchanged=false;r.gates_passed=false;if(r.status!="cancelled")r.status="source_changed";r.error+=std::string("\nSource verification: ")+e.what();}
    if(r.proposed_copy_verified)try{const auto copy=from_utf8(r.build.workspace)/"source";auto verification=inventory_options(copy);verification.context={};if(!same_inventory(expected,scan_project(copy,verification)))throw std::runtime_error("copied source differs from the proposed repair at final verification");}catch(const std::exception& e){r.proposed_copy_verified=false;r.gates_passed=false;if(r.status!="cancelled")r.status="copy_changed";r.error+=std::string("\nCopy verification: ")+e.what();}
    if(options.control)try{options.control->begin_commit();}catch(const ScanCancelled&){r.status="cancelled";r.gates_passed=false;}
    save(dir/"validation.json",repair_validation_json(r));save(dir/"validation.txt","status="+r.status+"\ncorrectness=not_assessed\nEvery target is evaluated against the same original baseline and combined source copy. Passing gates does not prove correctness.\n"+r.error+'\n');return r;
}
}
