#include "repair_internal.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <tuple>

namespace codeguard {
namespace {
constexpr std::size_t payload_limit=64*1024*1024;
std::string fingerprint(const std::string& text){std::uint64_t h=14695981039346656037ULL;for(unsigned char c:text){h^=c;h*=1099511628211ULL;}std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<h;return out.str();}
void save(const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary);out.write(text.data(),static_cast<std::streamsize>(text.size()));if(!out)throw std::runtime_error("cannot write repair set artifact");}
std::string content(const std::vector<RepairCandidate>& parts){
    std::ostringstream out;out<<parts.size()<<'\n';std::size_t total=0;
    for(const auto& c:parts){auto payload=encode_repair_candidate(c);total+=payload.size();if(total>32*1024*1024)throw std::invalid_argument("repair set component payload exceeds 32 MiB");out<<std::quoted(payload)<<'\n';}
    return out.str();
}
fs::path directory(const fs::path& output,const std::string& root,const std::string& id){
    if(output.empty()||project_path_inside(output,from_utf8(root)))throw std::invalid_argument("repair set output must be outside original source");
    fs::create_directories(output);const auto base=fs::canonical(output);if(project_path_inside(base,from_utf8(root)))throw std::invalid_argument("repair set output resolves inside original source");
    for(unsigned n=0;n<100;++n){auto path=base/(id+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(n));if(fs::create_directory(path))return path;}
    throw std::runtime_error("cannot create unique repair set directory");
}
std::pair<int,int> location(const std::string& text,std::size_t offset){
    if(offset>text.size())throw std::invalid_argument("repair target offset is outside file");int line=1,column=1;
    for(std::size_t n=0;n<offset;++n){if(text[n]=='\n'){++line;column=1;}else ++column;}return {line,column};
}
std::size_t position(const std::string& text,int line,int column){std::size_t at=0;for(int n=1;n<line;++n){at=text.find('\n',at);if(at==std::string::npos)throw std::invalid_argument("repair issue line is outside file");++at;}at+=static_cast<std::size_t>(column-1);if(at>text.size())throw std::invalid_argument("repair issue column is outside file");return at;}
}
RepairPlan make_repair_plan(const std::vector<RepairCandidate>& components){
    if(components.empty()||components.size()>16)throw std::invalid_argument("repair plan requires 1..16 components");
    std::map<std::string,std::vector<const RepairCandidate*>> grouped;std::set<std::string> ids,folded;
    std::set<std::tuple<std::string,std::string,int,int>> issues;std::size_t total=0;
    const auto& first=components.front();
    for(const auto& c:components){
        const auto encoded=encode_repair_candidate(c);total+=encoded.size();if(total>32*1024*1024)throw std::invalid_argument("repair plan component payload exceeds 32 MiB");
        if(c.root!=first.root||c.scan_id!=first.scan_id||c.configuration!=first.configuration||c.analyzer_revision!=first.analyzer_revision)throw std::invalid_argument("repair components must share project, scan, configuration and analyzer");
        if(!ids.insert(c.id).second)throw std::invalid_argument("duplicate repair component: "+c.id);
        if(!issues.emplace(c.edit.rule_id,c.edit.file,c.edit.line,c.edit.column).second)throw std::invalid_argument("choose only one alternative for each issue: "+c.edit.file);
        if(utf8_path(from_utf8(c.edit.file).lexically_normal())!=c.edit.file)throw std::invalid_argument("repair set file paths must use normalized project-relative names");
        if(!grouped.contains(c.edit.file)){
            auto key=c.edit.file;
#ifdef _WIN32
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
#endif
            if(!folded.insert(key).second)throw std::invalid_argument("ambiguous file name aliases in repair set");
        }
        grouped[c.edit.file].push_back(&c);
    }
    RepairPlan plan;
    for(auto& [file,parts]:grouped){
        std::sort(parts.begin(),parts.end(),[](auto* a,auto* b){return std::tie(a->edit.offset,a->id)<std::tie(b->edit.offset,b->id);});
        const auto& original=parts.front()->original_text;std::uint64_t end=0;std::int64_t delta=0;
        SourceReplacement change{file,original,original};
        for(const auto* c:parts){
            if(c->original_text!=original)throw std::invalid_argument("repair components disagree on original bytes: "+file);
            if(c->edit.offset<end)throw std::invalid_argument("overlapping repair source ranges: "+file);
            end=c->edit.offset+c->edit.before.size();
            plan.anchors.push_back({c,static_cast<std::uint64_t>(static_cast<std::int64_t>(c->edit.offset)+delta),0,0});
            delta+=static_cast<std::int64_t>(c->edit.after.size())-static_cast<std::int64_t>(c->edit.before.size());
        }
        for(auto it=parts.rbegin();it!=parts.rend();++it){const auto& c=**it;change.after.replace(static_cast<std::size_t>(c.edit.offset),c.edit.before.size(),c.edit.after);}
        if(change.after.size()>2*1024*1024)throw std::invalid_argument("combined repair file exceeds 2 MiB");
        for(auto& anchor:plan.anchors)if(anchor.component->edit.file==file){
            const auto& c=*anchor.component;auto relative=position(original,c.edit.line,c.edit.column);
            if(relative<c.edit.offset||relative>=c.edit.offset+c.edit.before.size())throw std::invalid_argument("repair issue location lies outside its edit anchor");
            relative-=static_cast<std::size_t>(c.edit.offset);if(c.edit.after=="("+c.edit.before+")"||c.edit.recipe=="condition.assignment_result")++relative;
            const auto [line,column]=location(change.after,static_cast<std::size_t>(anchor.revised_offset)+relative);anchor.revised_line=line;anchor.revised_column=column;
        }
        plan.files.push_back(std::move(change));
    }
    return plan;
}
std::vector<RepairTargetValidation> repair_plan_targets(const RepairPlan& plan){
    std::vector<RepairTargetValidation> out;for(const auto& a:plan.anchors){const auto& c=*a.component;out.push_back({c.id,c.edit.rule_id,c.edit.file,c.edit.line,c.edit.column,a.revised_line,a.revised_column,false,false,"unknown"});}return out;
}
RepairSet compose_repairs(std::vector<RepairCandidate> components){
    if(components.size()<2||components.size()>16)throw std::invalid_argument("repair set requires 2..16 components");
    std::sort(components.begin(),components.end(),[](const auto& a,const auto& b){return std::tie(a.edit.file,a.edit.offset,a.id)<std::tie(b.edit.file,b.edit.offset,b.id);});
    make_repair_plan(components);auto body=content(components);if(body.size()>payload_limit-64)throw std::invalid_argument("repair set manifest exceeds 64 MiB");RepairSet set;set.id="cgset-"+fingerprint(body);set.components=std::move(components);return set;
}
std::string encode_repair_set(const RepairSet& set){
    const auto normalized=compose_repairs(set.components);if(normalized.id!=set.id)throw std::invalid_argument("repair set content fingerprint mismatch");
    auto result="CodeGuardRepairSet 1\n"+set.id+'\n'+content(normalized.components);if(result.size()>payload_limit)throw std::invalid_argument("repair set manifest exceeds 64 MiB");return result;
}
RepairSet decode_repair_set(const std::string& payload){
    if(payload.size()>payload_limit)throw std::invalid_argument("repair set manifest exceeds 64 MiB");std::istringstream in(payload);std::string magic,id;int version=0;std::size_t count=0;
    in>>magic>>version>>id>>count;if(!in||magic!="CodeGuardRepairSet"||version!=1||count<2||count>16)throw std::invalid_argument("invalid repair set header");
    std::vector<RepairCandidate> parts;for(std::size_t n=0;n<count;++n){std::string data;in>>std::quoted(data);if(!in)throw std::invalid_argument("truncated repair set");parts.push_back(decode_repair_candidate(data));}
    in>>std::ws;if(!in.eof())throw std::invalid_argument("trailing repair set data");auto set=compose_repairs(std::move(parts));if(set.id!=id)throw std::invalid_argument("repair set content fingerprint mismatch");return set;
}
RepairSet read_repair_set(const fs::path& path){if(!fs::is_regular_file(path)||fs::file_size(path)>payload_limit)throw std::invalid_argument("repair set file unavailable or exceeds 64 MiB");std::ifstream in(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(in)),{});if(!in||data.size()>payload_limit)throw std::runtime_error("cannot read repair set");return decode_repair_set(data);}
std::vector<SourceReplacement> repair_set_changes(const RepairSet& set){encode_repair_set(set);return make_repair_plan(set.components).files;}
std::string repair_set_diff(const RepairSet& set){const auto files=repair_set_changes(set);std::string diff;for(const auto& f:files)diff+=repair_file_diff(f.file,f.before,f.after);return diff;}
fs::path export_repair_set(const RepairSet& set,const fs::path& output){
    const auto payload=encode_repair_set(set);const auto plan=make_repair_plan(set.components);const auto dir=directory(output,set.components.front().root,set.id);
    save(dir/"repair.cgset",payload);std::string diff;for(std::size_t n=0;n<plan.files.size();++n){const auto& f=plan.files[n];diff+=repair_file_diff(f.file,f.before,f.after);save(dir/("file-"+std::to_string(n)+"-original.txt"),f.before);save(dir/("file-"+std::to_string(n)+"-candidate.txt"),f.after);}save(dir/"candidate.diff",diff);
    RepairValidation pending;pending.candidate_id=set.id;pending.workspace=utf8_path(dir);pending.targets=repair_plan_targets(plan);save(dir/"validation.json",repair_validation_json(pending));
    save(dir/"README.txt","Explicit repair set; all components share one original scan. Review candidate.diff and each target result. Original project files are not modified.\nContent fingerprints are not digital signatures. Correctness remains not_assessed.\n");return dir;
}
RepairValidation validate_repair_set(const fs::path& root,const fs::path& database,const RepairSet& set,BuildOptions options){
    encode_repair_set(set);const auto plan=make_repair_plan(set.components);
    return validate_repair_plan(root,database,plan,set.id,options,[&]{return export_repair_set(set,options.output_directory);});
}
}
