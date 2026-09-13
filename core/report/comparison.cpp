#include "codeguard/report.hpp"
#include <algorithm>
#include <map>
#include <tuple>
#include <stdexcept>

namespace codeguard {
namespace {
struct Item { const Issue* issue; bool suppressed; };
std::vector<Item> items(const ScanResult& scan){
    std::vector<Item> result;
    for(const auto& issue:scan.analysis.issues)result.push_back({&issue,false});
    for(const auto& issue:scan.analysis.suppressed_issues)result.push_back({&issue,true});
    return result;
}
const FileRecord* file(const ScanResult& scan,const std::string& path){
    const auto found=std::find_if(scan.files.begin(),scan.files.end(),[&](const auto& f){return f.path==path;});
    return found==scan.files.end()?nullptr:&*found;
}
std::optional<ProjectConfig> configuration(const ScanResult& scan){
    if(scan.analysis.configuration.empty())return {};
    try{return decode_config(scan.analysis.configuration);}catch(const std::exception&){return {};}
}
std::pair<std::string,std::string> absent(const ScanResult& before,const ScanResult& after,const Issue& issue){
    const auto* current=file(after,issue.file);
    if(!current){
        if(before.inventory_policy.empty()||after.inventory_policy.empty())return {"inventory_unknown","文件未出现在新清单中；旧扫描范围未知，不能判为修复或确定删除"};
        if(before.inventory_policy!=after.inventory_policy)return {"inventory_scope_changed","扫描范围发生变化且文件不再纳入，不能计为修复"};
        return {"file_removed","文件已移出相同范围的完整清单（删除、改名或路径类型变化），不能计为修复"};
    }
    if(after.analysis.status=="not_requested")return {"analysis_disabled","本次仅导入清单，未执行规则分析"};
    const auto config=configuration(after);
    if(config&&std::find(config->disabled_rules.begin(),config->disabled_rules.end(),issue.rule_id)!=config->disabled_rules.end())
        return {"rule_disabled","规则已停用，未执行检查"};
    const auto direct=std::find_if(after.analysis.units.begin(),after.analysis.units.end(),[&](const auto& unit){return unit.file==issue.file;});
    if(direct!=after.analysis.units.end()&&direct->status!="success")
        return {direct->status=="parse_failed"?"parse_failed":"coverage_lost","该翻译单元未成功解析："+direct->status};
    if(!config||!configuration(before))return {"configuration_unknown","缺少可解码的历史规则配置，无法判定相同规则是否得到执行"};
    if(before.analysis.analyzer_revision.empty()||after.analysis.analyzer_revision.empty())return {"provenance_unknown","历史快照未记录分析器版本，不能确定检查条件相同"};
    if(before.analysis.analyzer_revision!=after.analysis.analyzer_revision)return {"analyzer_changed","分析器版本已变化；未再检出需独立复核"};
    std::size_t contributors=0;
    for(const auto& unit:before.analysis.units){
        if(std::find(unit.covered_files.begin(),unit.covered_files.end(),issue.file)==unit.covered_files.end())continue;
        ++contributors;
        const auto now=std::find_if(after.analysis.units.begin(),after.analysis.units.end(),[&](const auto& u){return u.file==unit.file;});
        if(now==after.analysis.units.end())return {"coverage_lost","原先覆盖该位置的翻译单元已不再纳入："+unit.file};
        if(now->status!="success")return {now->status=="parse_failed"?"parse_failed":"coverage_lost","原先覆盖该位置的翻译单元未成功解析："+unit.file+" ("+now->status+")"};
        if(unit.command_fingerprint.empty()||now->command_fingerprint.empty())return {"provenance_unknown","历史编译命令内容未记录，不能只按数据库路径判断条件相同"};
        if(unit.command_fingerprint!=now->command_fingerprint)return {"configuration_changed","覆盖该位置的实际编译命令发生变化："+unit.file};
        if(std::find(now->covered_files.begin(),now->covered_files.end(),issue.file)==now->covered_files.end())return {"coverage_lost","原先的翻译单元不再覆盖该文件："+unit.file};
    }
    if(contributors==0)return {"coverage_unknown","缺少该位置的翻译单元覆盖来源，不能将未检出判为修复"};
    const auto* previous=file(before,issue.file);
    return {"not_detected",previous&&previous->hash!=current->hash?
        "源码变化后，在相同已记录分析条件下未再检出（修复候选，仍需复核）":
        "源文件未变化但未再检出；需检查未记录的依赖或运行环境变化，不能直接计为修复"};
}
using MatchKey=std::tuple<std::string,std::string,std::string,std::string,std::string>;
MatchKey key(const Issue& i){return {i.rule_id,i.file,i.symbol_id,i.detector,i.evidence};}
}
ScanComparison compare_scans(const ScanResult& before,const ScanResult& after){
    if(before.root!=after.root||before.root.empty())throw std::invalid_argument("comparison requires two scans of the same saved project");
    if(before.id<=0||after.id<=before.id)throw std::invalid_argument("comparison needs an older baseline and a newer saved scan");
    if(!before.diagnostics.empty()||!after.diagnostics.empty())throw std::invalid_argument("cannot compare incomplete file inventories");
    ScanComparison result;result.before_id=before.id;result.after_id=after.id;
    result.notes={
        "新增表示本次新观测到的问题，不等于证明缺陷在两个版本之间引入。",
        "未再检出只给出修复候选；未记录外部头文件内容和完整运行环境，不能证明修复正确。",
        "优先匹配规则、文件、函数标识、证据及行列；只有证据唯一且非占位时，才在同一函数内忽略行列变化。改名或证据改变可能表现为旧项未检出与新项出现。"
    };
    if(before.inventory_policy.empty()||after.inventory_policy.empty())result.notes.push_back("至少一份快照缺少扫描范围溯源。");
    else if(before.inventory_policy!=after.inventory_policy)result.notes.push_back("两次扫描的文件忽略范围不同。");
    if(before.analysis.status!="complete"||after.analysis.status!="complete")result.notes.push_back("至少一份扫描并非完整分析；逐项结论以相关 TU 覆盖为准。");
    const auto old=items(before),now=items(after);
    std::vector<int> matched(old.size(),-1);std::vector<bool> used(now.size());
    std::map<MatchKey,std::vector<std::size_t>> old_groups,new_groups;
    for(std::size_t i=0;i<old.size();++i)old_groups[key(*old[i].issue)].push_back(i);
    for(std::size_t i=0;i<now.size();++i)new_groups[key(*now[i].issue)].push_back(i);
    for(const auto& [group,left]:old_groups){
        const auto right=new_groups.find(group);if(right==new_groups.end())continue;
        // Match exact sites first. Repeated expressions must not be paired by
        // arbitrary order after an edit shifts or deletes one of them.
        for(const auto i:left)for(const auto j:right->second)if(!used[j]&&old[i].issue->line==now[j].issue->line&&old[i].issue->column==now[j].issue->column){matched[i]=static_cast<int>(j);used[j]=true;break;}
        const auto& issue=*old[left.front()].issue;
        if(left.size()==1&&right->second.size()==1&&matched[left.front()]<0&&!issue.symbol_id.empty()&&!issue.evidence.empty()&&!issue.evidence.starts_with("[AST node:")){
            matched[left.front()]=static_cast<int>(right->second.front());used[right->second.front()]=true;
        }
    }
    for(std::size_t i=0;i<old.size();++i){
        IssueChange change;change.before=*old[i].issue;
        if(matched[i]>=0){
            const auto& item=now[static_cast<std::size_t>(matched[i])];change.after=*item.issue;
            if(!old[i].suppressed&&item.suppressed){change.status="suppressed";change.reason="问题仍被检测到，已按理由抑制；不计为修复";}
            else if(old[i].suppressed&&!item.suppressed){change.status="reactivated";change.reason="原抑制问题恢复为活动问题";}
            else {change.status=item.suppressed?"persistent_suppressed":"persistent";change.reason="相同规则证据持续存在";}
        }else{auto [status,reason]=absent(before,after,*old[i].issue);change.status=std::move(status);change.reason=std::move(reason);}
        result.changes.push_back(std::move(change));
    }
    for(std::size_t i=0;i<now.size();++i)if(!used[i])result.changes.push_back({now[i].suppressed?"new_suppressed":"added","基线中未找到可可靠匹配的问题；本次新观测到",{},*now[i].issue});
    return result;
}
} // namespace codeguard
