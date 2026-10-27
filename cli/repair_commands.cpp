#include "repair_commands.hpp"
#include "codeguard/repair.hpp"
#include "codeguard/repair_comparison.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace cg=codeguard;
namespace {
volatile std::sig_atomic_t interrupted=0;
void on_interrupt(int){interrupted=1;}
int positive(const std::string& value){int number=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),number);if(error!=std::errc{}||end!=value.data()+value.size()||number<=0)throw std::invalid_argument("repair numeric options must be positive integers");return number;}
}
int repair_commands(const std::vector<std::string>& args){
    if(args.size()<3||(args[1]!="propose"&&args[1]!="validate"&&args[1]!="evaluate"&&args[1]!="compose"&&args[1]!="validate-set"&&args[1]!="compare"))throw std::invalid_argument("repair propose|validate|evaluate|compose|validate-set|compare PROJECT --database DB --output DIR");
    const auto operation=args[1];const auto root=cg::fs::canonical(cg::from_utf8(args[2]));std::map<std::string,std::string> values;std::vector<std::string> definitions,manifests;
    const std::set<std::string> allowed{"--database","--output","--rule","--file","--line","--column","--candidate","--set","--c-compiler","--cxx-compiler","--jobs","--timeout","--target","--cmake-define","--coverage","--gcov-tool","--coverage-test-limit","--runs"};
    for(std::size_t i=3;i<args.size();i+=2){if(i+1==args.size()||!allowed.contains(args[i])||args[i+1].empty())throw std::invalid_argument("unknown or empty repair option");if(args[i]=="--cmake-define")definitions.push_back(args[i+1]);else if(args[i]=="--candidate"&&(operation=="evaluate"||operation=="compose"))manifests.push_back(args[i+1]);else if(!values.emplace(args[i],args[i+1]).second)throw std::invalid_argument("duplicate repair option");}
    if(!values.contains("--database")||!values.contains("--output"))throw std::invalid_argument("repair requires --database and --output");
    if(values.contains("--runs")&&operation!="compare")throw std::invalid_argument("--runs only applies to repair compare");
    auto control=std::make_shared<cg::ScanControl>();interrupted=0;const auto previous=std::signal(SIGINT,on_interrupt);
    struct Scope{decltype(previous) handler;std::jthread monitor;~Scope(){monitor.request_stop();std::signal(SIGINT,handler);}} scope{previous,std::jthread([control](std::stop_token token){while(!token.stop_requested()){if(interrupted)control->request_cancel();std::this_thread::sleep_for(std::chrono::milliseconds(10));}})};
    const auto database=cg::from_utf8(values.at("--database"));cg::SqliteDatabase db(database,true);const auto snapshot=db.validation_inputs(cg::utf8_path(root));if(!snapshot.id)throw std::invalid_argument("scan project before repair");
    cg::ScanContext context;context.control=control;
    if(operation=="propose"){
        for(const auto& [key,value]:values)if(key!="--database"&&key!="--output"&&key!="--rule"&&key!="--file"&&key!="--line"&&key!="--column")throw std::invalid_argument("build/validation options do not apply to repair propose");
        if(!definitions.empty())throw std::invalid_argument("CMake definitions do not apply to repair propose");
        if(!values.contains("--rule")||!values.contains("--file")||!values.contains("--line")||values.contains("--candidate"))throw std::invalid_argument("repair propose requires --rule, --file and --line");
        const int line=positive(values.at("--line")),column=values.contains("--column")?positive(values.at("--column")):0;const cg::Issue* selected=nullptr;
        for(const auto& issue:snapshot.analysis.issues)if(issue.rule_id==values.at("--rule")&&issue.file==values.at("--file")&&issue.line==line&&(!column||issue.column==column)){
            if(selected&&selected->column!=issue.column)throw std::invalid_argument("multiple issue positions; specify --column");selected=&issue;
        }
        if(!selected)throw std::invalid_argument("no matching active issue");const auto candidates=cg::propose_repairs(snapshot,*selected,context);std::cout<<"candidates="<<candidates.size()<<"\ncorrectness=not_assessed\n";
        for(const auto& candidate:candidates){context.check();const auto out=cg::export_repair_candidate(candidate,cg::from_utf8(values.at("--output")));std::cout<<candidate.id<<'\t'<<candidate.edit.recipe<<'\t'<<cg::utf8_path(out/"candidate.cgfix")<<'\n';}
        if(candidates.empty())std::cout<<"No eligible deterministic recipe; macros, pointers and unsupported semantics are not guessed.\n";return candidates.empty()?2:0;
    }
    if(operation=="compose"){
        if(manifests.size()<2||manifests.size()>16||!definitions.empty())throw std::invalid_argument("compose requires 2..16 candidates, without build options");
        for(const auto& [key,value]:values)if(key!="--database"&&key!="--output")throw std::invalid_argument("compose accepts only database/output/candidate options");
        std::vector<cg::RepairCandidate> parts;std::size_t bytes=0;
        for(const auto& path:manifests){context.check();auto c=cg::read_repair_candidate(cg::from_utf8(path));bytes+=cg::encode_repair_candidate(c).size();if(bytes>32*1024*1024)throw std::invalid_argument("set payload exceeds 32 MiB");parts.push_back(std::move(c));}
        const auto set=cg::compose_repairs(std::move(parts));const auto& first=set.components.front();
        if(cg::fs::canonical(cg::from_utf8(first.root))!=root||first.scan_id!=snapshot.id||first.configuration!=snapshot.analysis.configuration)throw std::invalid_argument("set components do not belong to the current saved scan/configuration");
        for(const auto& c:set.components)if(std::none_of(snapshot.analysis.issues.begin(),snapshot.analysis.issues.end(),[&](const auto& i){return i.rule_id==c.edit.rule_id&&i.file==c.edit.file&&i.line==c.edit.line&&i.column==c.edit.column;}))throw std::invalid_argument("set component issue is not active in saved scan");
        context.check();const auto out=cg::export_repair_set(set,cg::from_utf8(values.at("--output")));
        std::cout<<"set_id="<<set.id<<"\ncomponents="<<set.components.size()<<"\nmanifest="<<cg::utf8_path(out/"repair.cgset")<<"\ndiff="<<cg::utf8_path(out/"candidate.diff")<<"\ncorrectness=not_assessed\n";return 0;
    }
    const bool selected=operation=="evaluate"?(!manifests.empty()&&manifests.size()<=64&&!values.contains("--set")):
        operation=="compare"?(values.contains("--set")!=values.contains("--candidate")):
        operation=="validate-set"?(values.contains("--set")&&!values.contains("--candidate")):(values.contains("--candidate")&&!values.contains("--set"));
    if(!selected||values.contains("--rule")||values.contains("--file")||values.contains("--line")||values.contains("--column"))throw std::invalid_argument("validate requires one candidate; evaluate 1..64 candidates; validate-set one --set; issue selectors do not apply");
    auto config=snapshot.analysis.configuration.empty()?cg::ProjectConfig{}:cg::decode_config(snapshot.analysis.configuration);auto options=config.build;options.output_directory=cg::from_utf8(values.at("--output"));options.control=control;
    if(values.contains("--c-compiler"))options.c_compiler=values.at("--c-compiler");if(values.contains("--cxx-compiler"))options.cxx_compiler=values.at("--cxx-compiler");if(values.contains("--target"))options.target=values.at("--target");
    if(values.contains("--jobs")){const auto n=positive(values.at("--jobs"));if(n>64)throw std::invalid_argument("jobs must be 1..64");options.jobs=n;}
    if(values.contains("--timeout")){const auto n=positive(values.at("--timeout"));if(n>86400)throw std::invalid_argument("timeout must be 1..86400 seconds");options.timeout=std::chrono::seconds(n);}
    if(!definitions.empty())options.cmake_definitions=std::move(definitions);
    if(values.contains("--coverage")){const auto& mode=values.at("--coverage");if(mode!="off"&&mode!="gcov"&&mode!="gcov-tests")throw std::invalid_argument("coverage must be off, gcov or gcov-tests");options.collect_coverage=mode!="off";options.collect_test_coverage=mode=="gcov-tests";}
    if(operation=="compare"){
        if(values.contains("--coverage")&&values.at("--coverage")!="gcov-tests")throw std::invalid_argument("compare requires gcov-tests coverage");
        options.collect_coverage=true;options.collect_test_coverage=true;
    }
    if(values.contains("--coverage-test-limit")){if(!options.collect_test_coverage)throw std::invalid_argument("--coverage-test-limit requires --coverage gcov-tests");const auto limit=positive(values.at("--coverage-test-limit"));if(limit>256)throw std::invalid_argument("coverage test limit must be 1..256");options.coverage_test_limit=static_cast<unsigned>(limit);}
    if(values.contains("--gcov-tool")){if(!options.collect_coverage)throw std::invalid_argument("--gcov-tool requires --coverage gcov");options.gcov=values.at("--gcov-tool");}
    if(operation=="compare"){
        const auto rounds=values.contains("--runs")?positive(values.at("--runs")):2;
        if(rounds<2||rounds>5)throw std::invalid_argument("comparison runs must be 2..5 per version");
        auto parts=values.contains("--set")?cg::read_repair_set(cg::from_utf8(values.at("--set"))).components:std::vector<cg::RepairCandidate>{cg::read_repair_candidate(cg::from_utf8(values.at("--candidate")))};
        const auto result=cg::compare_repair_coverage(root,database,std::move(parts),options,static_cast<unsigned>(rounds));std::cout<<cg::repair_coverage_comparison_json(result);return result.status=="cancelled"?130:result.ready_for_review?0:2;
    }
    if(operation=="validate-set"){
        const auto result=cg::validate_repair_set(root,database,cg::read_repair_set(cg::from_utf8(values.at("--set"))),options);std::cout<<cg::repair_validation_json(result);return result.status=="cancelled"?130:result.gates_passed?0:2;
    }
    if(operation=="evaluate"){
        std::vector<cg::RepairCandidate> candidates;std::size_t bytes=0;
        for(const auto& path:manifests){auto candidate=cg::read_repair_candidate(cg::from_utf8(path));bytes+=candidate.original_text.size()+candidate.revised_text.size();if(bytes>64*1024*1024)throw std::invalid_argument("batch source payload exceeds 64 MiB");candidates.push_back(std::move(candidate));}
        const auto batch=cg::evaluate_repairs(root,database,candidates,options);std::cout<<cg::repair_batch_json(batch);
        const bool any=std::any_of(batch.entries.begin(),batch.entries.end(),[](const auto& e){return e.validation.gates_passed;});
        return batch.status=="cancelled"?130:batch.status=="completed"&&any?0:2;
    }
    const auto result=cg::validate_repair(root,database,cg::read_repair_candidate(cg::from_utf8(values.at("--candidate"))),options);std::cout<<cg::repair_validation_json(result);return result.status=="cancelled"?130:result.gates_passed?0:2;
}
