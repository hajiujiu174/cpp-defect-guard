#include "codeguard/repair.hpp"
#include "coverage_internal.hpp"
#include "repair_internal.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace codeguard {
namespace {
constexpr std::size_t file_limit=2*1024*1024;
std::string fingerprint(const std::string& data){std::uint64_t h=14695981039346656037ULL;for(unsigned char c:data){h^=c;h*=1099511628211ULL;}std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<h;return out.str();}
std::string read_text(const fs::path& path,std::size_t limit=file_limit){
    if(!fs::is_regular_file(path)||fs::file_size(path)>limit)throw std::invalid_argument("file unavailable or exceeds repair size limit");
    std::ifstream input(path,std::ios::binary);std::string result((std::istreambuf_iterator<char>(input)),{});if(!input||input.bad()||result.size()>limit)throw std::runtime_error("cannot read repair input");return result;
}
void write_text(const fs::path& path,const std::string& data){std::ofstream output(path,std::ios::binary);output.write(data.data(),static_cast<std::streamsize>(data.size()));if(!output)throw std::runtime_error("cannot write repair artifact");}
std::string json(const std::string& value){std::ostringstream out;out<<'"';for(unsigned char c:value){if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;else out<<static_cast<char>(c);}out<<'"';return out.str();}
template<class A,class B> bool same_inventory(const A& a,const B& b){if(!a.diagnostics.empty()||!b.diagnostics.empty()||a.files.size()!=b.files.size())return false;for(std::size_t i=0;i<a.files.size();++i)if(a.files[i].path!=b.files[i].path||a.files[i].hash!=b.files[i].hash||a.files[i].size!=b.files[i].size)return false;return true;}
std::string body(const RepairCandidate& c){
    std::ostringstream out;out<<std::quoted(c.root)<<' '<<c.scan_id<<'\n'<<std::quoted(c.configuration)<<'\n'<<std::quoted(c.analyzer_revision)<<'\n';
    const auto& e=c.edit;out<<std::quoted(e.rule_id)<<' '<<std::quoted(e.file)<<' '<<std::quoted(e.recipe)<<'\n'<<std::quoted(e.title)<<'\n'<<std::quoted(e.risk)<<'\n'<<e.line<<' '<<e.column<<' '<<e.offset<<'\n'<<std::quoted(e.before)<<'\n'<<std::quoted(e.after)<<'\n'<<std::quoted(c.original_text)<<'\n'<<std::quoted(c.revised_text)<<'\n';return out.str();
}
void valid(const RepairCandidate& c){
    if(c.scan_id<=0||!from_utf8(c.root).is_absolute()||c.configuration.empty()||c.edit.line<=0||c.edit.column<=0||c.original_text.size()>file_limit||c.revised_text.size()>file_limit||c.edit.before.empty()||c.edit.before==c.edit.after)
        throw std::invalid_argument("invalid repair candidate metadata");
    validate_relative_directory(c.edit.file);
    if(c.original_text.find('\0')!=std::string::npos||c.revised_text.find('\0')!=std::string::npos||c.edit.offset>c.original_text.size()||c.edit.before.size()>c.original_text.size()-c.edit.offset||c.original_text.substr(static_cast<std::size_t>(c.edit.offset),c.edit.before.size())!=c.edit.before)
        throw std::invalid_argument("repair source anchor does not match exact file bytes");
    if(c.edit.recipe!="condition.compare"&&c.edit.recipe!="condition.intent"&&c.edit.recipe!="condition.assignment_result"&&c.edit.recipe!="printf.bound")throw std::invalid_argument("unknown repair recipe");
    auto after=c.original_text;after.replace(static_cast<std::size_t>(c.edit.offset),c.edit.before.size(),c.edit.after);if(after!=c.revised_text)throw std::invalid_argument("repair candidate contains changes outside its source anchor");
    if(c.id!="cgfix-"+fingerprint(body(c)))throw std::invalid_argument("repair candidate content fingerprint mismatch");
}
fs::path unique_directory(const fs::path& output,const fs::path& root,const std::string& prefix){
    if(output.empty()||project_path_inside(output,root))throw std::invalid_argument("repair output must be outside the original source tree");
    fs::create_directories(output);const auto base=fs::canonical(output);
    if(project_path_inside(base,root))throw std::invalid_argument("repair output resolves inside original source");
    for(unsigned i=0;i<100;++i){auto path=base/(prefix+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(i));if(fs::create_directory(path))return path;}throw std::runtime_error("cannot create unique repair workspace");
}
std::vector<std::string> lines(const std::string& text){std::vector<std::string> result;for(std::size_t at=0;at<text.size();){const auto end=text.find('\n',at);const auto count=end==std::string::npos?text.size()-at:end-at+1;result.push_back(text.substr(at,count));at+=count;}return result;}
std::string patch_path(const std::string& path){std::string out="\"";for(unsigned char c:path){if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c<32||c>=127){out+='\\';out+=static_cast<char>('0'+((c>>6)&7));out+=static_cast<char>('0'+((c>>3)&7));out+=static_cast<char>('0'+(c&7));}else out+=static_cast<char>(c);}return out+'"';}
template<class Snapshot> void require_active(const Snapshot& snapshot,const Issue& issue){if(std::none_of(snapshot.analysis.issues.begin(),snapshot.analysis.issues.end(),[&](const auto& i){return i.rule_id==issue.rule_id&&i.file==issue.file&&i.line==issue.line&&i.column==issue.column;}))throw std::invalid_argument("repair requires a selected active issue from this snapshot");}
}

std::vector<RepairCandidate> propose_repairs(const ScanResult& snapshot,const Issue& selected,const ScanContext& context){context.check();return propose_repairs(validation_inputs_of(snapshot),selected,context);}
std::vector<RepairCandidate> propose_repairs(const ScanResult& snapshot,const std::vector<Issue>& selected,const ScanContext& context){context.check();return propose_repairs(validation_inputs_of(snapshot),selected,context);}
std::vector<RepairCandidate> propose_repairs(const ValidationInputs& snapshot,const Issue& selected,const ScanContext& context){return propose_repairs(snapshot,std::vector<Issue>{selected},context);}
std::vector<RepairCandidate> propose_repairs(const ValidationInputs& snapshot,const std::vector<Issue>& selected,const ScanContext& context){
    context.check();if(selected.empty()||selected.size()>16)throw std::invalid_argument("select 1..16 active issues for repair generation");
    std::set<std::tuple<std::string,std::string,int,int>> keys;for(const auto& issue:selected){require_active(snapshot,issue);if(!keys.emplace(issue.rule_id,issue.file,issue.line,issue.column).second)throw std::invalid_argument("duplicate issue selection");}
    if(snapshot.id<=0||snapshot.analysis.configuration.empty()||snapshot.analysis.compile_commands.empty())throw std::invalid_argument("repair needs a saved Clang scan with configuration provenance");
    auto config=decode_config(snapshot.analysis.configuration);config.compile_commands=snapshot.analysis.compile_commands;config.auto_discover=false;
    auto options=configured_scan_options(from_utf8(snapshot.root),config);options.context=context;options.use_cache=false;
    const auto before=scan_project(from_utf8(snapshot.root),options);if(!same_inventory(snapshot,before))throw std::invalid_argument("source differs from snapshot; rescan before proposing repairs");
    context.report("repair_ast");auto fresh=analyze_project(before,options.compile_commands,context,options.threads,options.command_choices,options.disabled_rules,{},true);
    if(fresh.units.size()!=snapshot.analysis.units.size())throw std::invalid_argument("translation unit scope changed; rescan before repair");
    for(const auto& old:snapshot.analysis.units){
        const auto found=std::find_if(fresh.units.begin(),fresh.units.end(),[&](const auto& u){return u.file==old.file;});
        if(found==fresh.units.end()||found->status!=old.status||found->command_fingerprint!=old.command_fingerprint)throw std::invalid_argument("parse coverage or compile command changed; rescan before repair");
    }
    if(!same_inventory(before,scan_project(from_utf8(snapshot.root),options)))throw std::runtime_error("source changed during repair proposal");
    auto current=before;current.analysis.issues=fresh.issues;current.analysis.units=fresh.units;current.analysis.status=fresh.status;apply_rule_policy(current,config);for(const auto& issue:selected)require_active(current,issue);
    std::vector<RepairCandidate> result;
    for(const auto& hint:fresh.repair_hints){context.check();if(!keys.contains({hint.rule_id,hint.file,hint.line,hint.column}))continue;
        const auto path=from_utf8(snapshot.root)/from_utf8(hint.file);if(!project_path_inside(path,from_utf8(snapshot.root))||fs::is_symlink(path))throw std::invalid_argument("repair file escapes project");
        RepairCandidate c;c.root=snapshot.root;c.scan_id=snapshot.id;c.configuration=snapshot.analysis.configuration;c.analyzer_revision=fresh.analyzer_revision;c.edit=hint;c.original_text=read_text(path);c.revised_text=c.original_text;
        const auto record=std::find_if(snapshot.files.begin(),snapshot.files.end(),[&](const auto& f){return f.path==hint.file;});
        if(record==snapshot.files.end()||record->hash!="fnv1a64-v1:"+fingerprint(c.original_text)||record->size!=c.original_text.size())throw std::runtime_error("source changed while materializing repair candidate");
        if(hint.offset>c.original_text.size()||hint.before.size()>c.original_text.size()-hint.offset||c.original_text.substr(static_cast<std::size_t>(hint.offset),hint.before.size())!=hint.before)throw std::runtime_error("AST repair range no longer matches source");
        c.revised_text.replace(static_cast<std::size_t>(hint.offset),hint.before.size(),hint.after);c.id="cgfix-"+fingerprint(body(c));valid(c);result.push_back(std::move(c));
    }
    return result;
}
std::string encode_repair_candidate(const RepairCandidate& c){valid(c);auto payload="CodeGuardRepairCandidate 1\n"+c.id+'\n'+body(c);if(payload.size()>16*1024*1024)throw std::invalid_argument("repair manifest exceeds 16 MiB");return payload;}
RepairCandidate decode_repair_candidate(const std::string& payload){
    if(payload.size()>16*1024*1024)throw std::invalid_argument("repair manifest exceeds 16 MiB");std::istringstream in(payload);std::string magic;int version=0;RepairCandidate c;
    in>>magic>>version>>c.id;if(magic!="CodeGuardRepairCandidate"||version!=1)throw std::invalid_argument("unknown repair manifest version");
    auto& e=c.edit;in>>std::quoted(c.root)>>c.scan_id>>std::quoted(c.configuration)>>std::quoted(c.analyzer_revision)>>std::quoted(e.rule_id)>>std::quoted(e.file)>>std::quoted(e.recipe)>>std::quoted(e.title)>>std::quoted(e.risk)>>e.line>>e.column>>e.offset>>std::quoted(e.before)>>std::quoted(e.after)>>std::quoted(c.original_text)>>std::quoted(c.revised_text);
    if(!in)throw std::invalid_argument("truncated repair manifest");in>>std::ws;if(!in.eof())throw std::invalid_argument("trailing repair manifest data");valid(c);return c;
}
RepairCandidate read_repair_candidate(const fs::path& path){return decode_repair_candidate(read_text(path,16*1024*1024));}
std::string repair_diff(const RepairCandidate& c){
    valid(c);return repair_file_diff(c.edit.file,c.original_text,c.revised_text);
}
std::string repair_file_diff(const std::string& file,const std::string& before,const std::string& after){
    const auto a=lines(before),b=lines(after);std::size_t prefix=0,suffix=0;
    while(prefix<a.size()&&prefix<b.size()&&a[prefix]==b[prefix])++prefix;
    while(suffix<a.size()-prefix&&suffix<b.size()-prefix&&a[a.size()-suffix-1]==b[b.size()-suffix-1])++suffix;
    const auto start=prefix>3?prefix-3:0,tail=std::min<std::size_t>(3,suffix),ae=a.size()-suffix,be=b.size()-suffix;
    std::ostringstream out;out<<"diff --git "<<patch_path("a/"+file)<<' '<<patch_path("b/"+file)<<"\n--- "<<patch_path("a/"+file)<<"\n+++ "<<patch_path("b/"+file)<<"\n@@ -"<<start+1<<','<<ae+tail-start<<" +"<<start+1<<','<<be+tail-start<<" @@\n";
    auto emit=[&](char sign,const std::string& line){out<<sign<<line;if(line.empty()||line.back()!='\n')out<<"\n\\ No newline at end of file\n";};
    for(auto i=start;i<prefix;++i)emit(' ',a[i]);for(auto i=prefix;i<ae;++i)emit('-',a[i]);for(auto i=prefix;i<be;++i)emit('+',b[i]);for(std::size_t i=0;i<tail;++i)emit(' ',a[ae+i]);return out.str();
}
fs::path export_repair_candidate(const RepairCandidate& c,const fs::path& output){
    valid(c);const auto dir=unique_directory(output,from_utf8(c.root),c.id);
    write_text(dir/"candidate.cgfix",encode_repair_candidate(c));write_text(dir/"candidate.diff",repair_diff(c));write_text(dir/"original-source.txt",c.original_text);write_text(dir/"candidate-source.txt",c.revised_text);
    write_text(dir/"README.txt","Candidate only. Review intent, truncation and test coverage. Original project files were not modified.\nRecipe: "+c.edit.recipe+"\nRisk: "+c.edit.risk+"\nContent fingerprints detect accidental changes; they are not digital signatures.\n");
    RepairValidation pending;pending.candidate_id=c.id;pending.workspace=utf8_path(dir);write_text(dir/"validation.json",repair_validation_json(pending));return dir;
}
std::string repair_validation_json(const RepairValidation& r) {
    auto stage=[&](const std::string& name){for(const auto& s:r.build.steps)if(s.name==name)return s.result.status;return std::string("not_run");};
    std::ostringstream out;
    auto field=[&](const std::string& name,const std::string& value){out<<"  "<<json(name)<<": "<<json(value)<<",\n";};
    out<<"{\n";
    field("candidate_id",r.candidate_id);field("status",r.status);field("workspace",r.workspace);field("error",r.error);
    out<<"  \"build_run_id\": "<<r.build.id<<",\n";
    field("build_status",stage("build"));field("test_status",stage("test"));
    const BuildStep* tests=nullptr;for(const auto& s:r.build.steps)if(s.name=="test")tests=&s;
    out<<"  \"tests_total\": "<<(tests?tests->tests_total:-1)<<",\n"
       <<"  \"tests_failed\": "<<(tests?tests->tests_failed:-1)<<",\n"
       <<"  \"tests_skipped\": "<<(tests?tests->tests_skipped:-1)<<",\n";
    out<<"  \"source_unchanged\": "<<(r.build.workspace.empty()?"null":r.build.source_unchanged?"true":"false")<<",\n";
    field("analysis_status",r.analysis_status);field("baseline_status",r.baseline_status);
    out<<"  \"baseline_target_reproduced\": "<<(r.baseline_target_reproduced?"true":"false")<<",\n"
       <<"  \"target_not_detected\": "<<(r.target_not_detected?"true":"false")<<",\n"
       <<"  \"gates_passed\": "<<(r.gates_passed?"true":"false")<<",\n"
       <<"  \"before_issues\": "<<r.before_issues<<",\n"
       <<"  \"after_issues\": "<<r.after_issues<<",\n"
       <<"  \"new_or_unmasked_issues\": "<<r.new_or_unmasked_issues<<",\n"
       <<"  \"coverage\": "<<repair_coverage_json(r.coverage)<<",\n"
       <<"  \"coverage_commands_normalized\": "<<(r.coverage_commands_normalized?"true":"false")<<",\n";
    out<<"  \"proposed_copy_verified\": "<<(r.proposed_copy_verified?"true":"false")<<",\n";
    out<<"  \"test_coverage\": "<<repair_test_coverage_json(r.test_coverage)<<",\n";
    out<<"  \"targets\": [";for(std::size_t n=0;n<r.targets.size();++n){if(n)out<<',';const auto& t=r.targets[n];
        out<<"{\"candidate_id\": "<<json(t.candidate_id)<<", \"rule_id\": "<<json(t.rule_id)<<", \"file\": "<<json(t.file)<<", \"line\": "<<t.line<<", \"column\": "<<t.column
           <<", \"revised_line\": "<<t.revised_line<<", \"revised_column\": "<<t.revised_column<<", \"baseline_reproduced\": "<<(t.baseline_reproduced?"true":"false")
           <<", \"target_not_detected\": "<<(t.target_not_detected?"true":"false")<<", \"coverage_line_status\": "<<json(t.coverage_line_status)<<'}';}
    out<<"],\n"
       <<"  \"correctness\": "<<json(r.correctness)<<"\n}\n";
    return out.str();
}
RepairValidation validate_repair(const fs::path& root,const fs::path& database,const RepairCandidate& candidate,BuildOptions options){
    valid(candidate);const std::vector<RepairCandidate> components{candidate};const auto plan=make_repair_plan(components);
    return validate_repair_plan(root,database,plan,candidate.id,options,[&]{return export_repair_candidate(candidate,options.output_directory);});
}
}
