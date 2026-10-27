#include "codeguard/repair.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

namespace codeguard {
namespace {
std::string quote(const std::string& text) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:text) {
        if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);
        else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;
        else out<<static_cast<char>(c);
    }
    return out.str()+'"';
}
void save(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary);out.write(text.data(),static_cast<std::streamsize>(text.size()));
    if(!out)throw std::runtime_error("cannot write repair batch record: "+utf8_path(path));
}
fs::path directory(const fs::path& output,const fs::path& root) {
    if(output.empty()||project_path_inside(output,root))throw std::invalid_argument("batch output must be outside original source");
    fs::create_directories(output);const auto base=fs::canonical(output);
    if(project_path_inside(base,root))throw std::invalid_argument("batch output resolves inside original source");
    for(unsigned n=0;n<100;++n) {
        auto path=base/("batch-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(n));
        if(fs::create_directory(path))return path;
    }
    throw std::runtime_error("cannot create repair batch workspace");
}
bool passed(const RepairValidation& v,const std::string& name) {
    for(const auto& step:v.build.steps)if(step.name==name)return step.result.status=="passed";
    return false;
}
auto ranking_key(const RepairBatchEntry& entry,bool coverage) {
    const auto& v=entry.validation;bool tests_passed=false,tests_executed=false;
    for(const auto& step:v.build.steps)if(step.name=="test") {
        tests_executed=step.tests_total>0&&step.tests_failed>=0&&step.tests_skipped==0;
        tests_passed=tests_executed&&step.tests_failed==0&&step.result.status=="passed";
    }
    const int tier=v.gates_passed?0:v.status=="not_run"?3:v.status=="cancelled"?2:1;
    const auto issues=v.new_or_unmasked_issues<0?std::numeric_limits<std::int64_t>::max():v.new_or_unmasked_issues;
    return std::tuple{tier,!tests_passed,!tests_executed,!passed(v,"build"),
        !(v.baseline_status=="complete"&&v.baseline_target_reproduced&&v.analysis_status=="complete"),
        !v.target_not_detected,issues,coverage&&v.coverage.target_line_status!="recorded",
        coverage?-static_cast<std::int64_t>(v.coverage.lines_hit):0,entry.changed_bytes,entry.candidate_id,entry.input_index};
}
std::uint64_t changed_bytes(const RepairHint& edit) {
    auto subsequence=[](const std::string& a,const std::string& b){std::size_t n=0;for(char c:b)if(n<a.size()&&a[n]==c)++n;return n==a.size();};
    if(subsequence(edit.before,edit.after))return edit.after.size()-edit.before.size();
    if(subsequence(edit.after,edit.before))return edit.before.size()-edit.after.size();
    std::size_t prefix=0,suffix=0;
    while(prefix<edit.before.size()&&prefix<edit.after.size()&&edit.before[prefix]==edit.after[prefix])++prefix;
    while(suffix<edit.before.size()-prefix&&suffix<edit.after.size()-prefix&&
        edit.before[edit.before.size()-1-suffix]==edit.after[edit.after.size()-1-suffix])++suffix;
    return edit.before.size()+edit.after.size()-2*(prefix+suffix);
}
}
std::vector<std::size_t> rank_repairs(const std::vector<RepairBatchEntry>& entries) {
    std::vector<std::size_t> order(entries.size());std::iota(order.begin(),order.end(),0);
    const bool coverage=comparable_repair_coverage(entries);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return ranking_key(entries[a],coverage)<ranking_key(entries[b],coverage);});
    return order;
}
bool comparable_repair_coverage(const std::vector<RepairBatchEntry>& entries) {
    if(entries.empty())return false;const auto& first=entries.front().validation.coverage;
    if(first.status!="collected"||first.scope_signature.empty()||first.lines_total==0)return false;
    return std::all_of(entries.begin(),entries.end(),[&](const auto& e){const auto& c=e.validation.coverage;return c.status=="collected"&&c.scope_signature==first.scope_signature&&c.tool_version==first.tool_version&&c.lines_total==first.lines_total;});
}
std::string repair_batch_json(const RepairBatch& batch) {
    const auto order=rank_repairs(batch.entries);std::size_t gates=0,measured=0,requested=0;
    for(const auto& entry:batch.entries)if(entry.validation.gates_passed)++gates;
    for(const auto& entry:batch.entries){if(entry.validation.coverage.status=="collected")++measured;if(entry.validation.coverage.status!="not_measured")++requested;}
    std::ostringstream out;
    out<<"{\n  \"format\": \"CodeGuardRepairBatch 1\",\n  \"status\": "<<quote(batch.status)
       <<",\n  \"workspace\": "<<quote(batch.workspace)<<",\n  \"attempted\": "<<batch.attempted
       <<",\n  \"total\": "<<batch.entries.size()<<",\n  \"gates_passed_count\": "<<gates
       <<",\n  \"any_gates_passed\": "<<(gates?"true":"false")
       <<",\n  \"all_gates_passed\": "<<(!batch.entries.empty()&&gates==batch.entries.size()?"true":"false")
       <<",\n  \"correctness\": \"not_assessed\",\n  \"test_coverage\": "<<quote(!requested?"not_measured":measured==batch.entries.size()?"collected":"incomplete")
       <<",\n  \"coverage_comparable\": "<<(comparable_repair_coverage(batch.entries)?"true":"false")<<",\n"
       <<"  \"ranking_policy\": \"evidence-lexicographic-v2: gates; complete passing tests; executed tests; build; same-config static evidence; target absent; fewer new issues; only if all coverage footprints and gcov versions match: target line recorded, more project lines hit; fewer changed bytes; content id; input order\",\n"
       <<"  \"entries\": [\n";
    for(std::size_t rank=0;rank<order.size();++rank) {
        const auto& entry=batch.entries[order[rank]];
        out<<"    {\"rank\": "<<rank+1<<", \"input_index\": "<<entry.input_index
           <<", \"candidate_id\": "<<quote(entry.candidate_id)<<", \"recipe\": "<<quote(entry.recipe)
           <<", \"file\": "<<quote(entry.file)<<", \"line\": "<<entry.line<<", \"column\": "<<entry.column
           <<", \"changed_bytes\": "<<entry.changed_bytes<<", \"manifest\": "<<quote(entry.manifest)
           <<", \"validation\": "<<repair_validation_json(entry.validation)<<"}"<<(rank+1<order.size()?",":"")<<'\n';
    }
    out<<"  ]\n}\n";return out.str();
}
RepairBatch evaluate_repairs(const fs::path& input,const fs::path& database,
    const std::vector<RepairCandidate>& candidates,BuildOptions options) {
    if(candidates.empty()||candidates.size()>64)throw std::invalid_argument("batch requires 1..64 candidates");
    const auto root=fs::canonical(input);SqliteDatabase db(database,true);const auto snapshot=db.validation_inputs(utf8_path(root));
    std::set<std::string> ids;std::size_t bytes=0;
    for(const auto& c:candidates) {
        encode_repair_candidate(c); // validate all inputs before exporting or executing any
        if(!ids.insert(c.id).second)throw std::invalid_argument("duplicate candidate in batch");
        if(fs::canonical(from_utf8(c.root))!=root||c.scan_id!=snapshot.id||c.configuration!=snapshot.analysis.configuration)
            throw std::invalid_argument("batch candidates must share the current project, scan and configuration");
        bytes+=c.original_text.size()+c.revised_text.size();
        if(bytes>64*1024*1024)throw std::invalid_argument("batch source payload exceeds 64 MiB");
    }
    const auto dir=directory(options.output_directory,root);RepairBatch batch;batch.workspace=utf8_path(dir);
    for(std::size_t n=0;n<candidates.size();++n) {
        const auto& c=candidates[n];RepairBatchEntry entry;
        entry.input_index=n;entry.candidate_id=c.id;entry.recipe=c.edit.recipe;entry.file=c.edit.file;
        entry.line=c.edit.line;entry.column=c.edit.column;entry.changed_bytes=changed_bytes(c.edit);
        const auto exported=export_repair_candidate(c,dir/"candidates");entry.manifest=utf8_path(exported/"candidate.cgfix");
        entry.validation.candidate_id=c.id;entry.validation.workspace=utf8_path(exported);if(options.collect_coverage){entry.validation.coverage.status="not_run";entry.validation.coverage.tool=options.gcov;}batch.entries.push_back(std::move(entry));
    }
    auto checkpoint=[&]{std::ostringstream name;name<<"progress-"<<std::setw(4)<<std::setfill('0')<<batch.attempted<<".json";save(dir/name.str(),repair_batch_json(batch));};
    checkpoint();const auto parent=options.control;bool source_changed=false;
    for(std::size_t n=0;n<candidates.size();++n) {
        if(parent&&parent->state()==ScanControl::State::cancel_requested)break;
        auto child=std::make_shared<ScanControl>();auto item_options=options;item_options.control=child;item_options.output_directory=dir;
        const auto progress=options.progress;
        item_options.progress=[parent,child,progress,n,total=candidates.size()](const std::string& phase){
            if(parent&&parent->state()==ScanControl::State::cancel_requested)child->request_cancel();
            if(progress)progress("candidate "+std::to_string(n+1)+"/"+std::to_string(total)+": "+phase);
            if(parent&&parent->state()==ScanControl::State::cancel_requested)child->request_cancel();
        };
        // A single-candidate commit fence must not consume the batch cancel control.
        std::jthread bridge([parent,child](std::stop_token token){
            while(!token.stop_requested()) {
                if(parent&&parent->state()==ScanControl::State::cancel_requested)child->request_cancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        auto& entry=batch.entries[n];
        try {entry.validation=validate_repair(root,database,candidates[n],item_options);}
        catch(const std::exception& e) {entry.validation.status="failed";entry.validation.error=e.what();}
        bridge.request_stop();++batch.attempted;
        if(!entry.validation.build.workspace.empty()&&!entry.validation.build.source_unchanged)source_changed=true;
        // Even pre-build exceptions get a persistent per-candidate terminal record.
        save(from_utf8(entry.manifest).parent_path()/"batch-validation.json",repair_validation_json(entry.validation));
        checkpoint();if(source_changed)break;
    }
    batch.status=source_changed?"source_changed":batch.attempted==batch.entries.size()?"completed":"cancelled";
    if(parent)try{parent->begin_commit();}catch(const ScanCancelled&){batch.status="cancelled";}
    for(auto& entry:batch.entries)if(entry.validation.status=="not_run") {
        entry.validation.error=batch.status=="source_changed"?"Not run because original source changed during this batch.":"Not run because the batch was cancelled.";
        save(from_utf8(entry.manifest).parent_path()/"batch-validation.json",repair_validation_json(entry.validation));
    }
    save(dir/"batch.json",repair_batch_json(batch));return batch;
}
}
