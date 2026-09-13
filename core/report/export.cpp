#include "codeguard/report.hpp"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace codeguard {
namespace {
struct Renderer {
const ScanContext& context;
// Evidence can end in a truncated UTF-8 sequence; keep report text valid without
// treating source/diagnostic bytes as markup or executable code.
std::string utf8(const std::string& input){
    context.check();
    std::string out;
    for(std::size_t i=0;i<input.size();){
        if(i%4096==0)context.check();
        const auto c=static_cast<unsigned char>(input[i]);
        if(c<0x80){out+=input[i++];continue;}
        unsigned length=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        bool valid=length&&i+length<=input.size();unsigned value=c&((1u<<(7-length))-1);
        for(unsigned j=1;valid&&j<length;++j){const auto next=static_cast<unsigned char>(input[i+j]);valid=(next&0xc0)==0x80;value=(value<<6)|(next&0x3f);}
        valid=valid&&value<=0x10ffff&&!(value>=0xd800&&value<=0xdfff)&&!(length==3&&value<0x800)&&!(length==4&&value<0x10000);
        if(valid){out.append(input,i,length);i+=length;}else{out+="\xef\xbf\xbd";++i;}
    }
    return out;
}
std::string quote(const std::string& value){
    const char hex[]="0123456789abcdef";std::string result="\"";
    for(const unsigned char c:utf8(value)){
        if(c=='"')result+="\\\"";else if(c=='\\')result+="\\\\";
        else if(c<0x20){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}else result+=static_cast<char>(c);
    }
    return result+'"';
}
std::string escape(const std::string& value){
    std::string result;
    for(const char c:utf8(value))switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;case '"':result+="&quot;";break;case '\'':result+="&#39;";break;default:if(static_cast<unsigned char>(c)<32&&c!='\n'&&c!='\t'&&c!='\r')result+="\xef\xbf\xbd";else result+=c;}
    return result;
}
std::string object(std::initializer_list<std::pair<std::string,std::string>> fields){
    std::string out="{";bool first=true;for(const auto& [key,value]:fields){if(!first)out+=',';first=false;out+=quote(key)+':'+value;}return out+'}';
}
template<class Range,class F> std::string array(const Range& values,F format){
    std::string out="[";bool first=true;for(const auto& value:values){context.check();if(!first)out+=',';first=false;out+=format(value);}return out+']';
}
std::string strings(const std::vector<std::string>& values){return array(values,[this](const auto& s){return quote(s);});}
std::string boolean(bool value){return value?"true":"false";}
template<class N> std::string number(N value){return std::to_string(value);}
std::string issue_json(const Issue& i){return object({{"rule_id",quote(i.rule_id)},{"severity",quote(i.severity)},{"file",quote(i.file)},{"line",number(i.line)},{"column",number(i.column)},{"message",quote(i.message)},{"evidence",quote(i.evidence)},{"suggestion",quote(i.suggestion)},{"symbol_id",quote(i.symbol_id)},{"detector",quote(i.detector)},{"suppression_reason",quote(i.suppression_reason)}});}
std::string snapshot_json(const ScanResult& scan){
    const auto& a=scan.analysis;
    return object({{"id",number(scan.id)},{"root",quote(scan.root)},{"scanned_at",quote(scan.scanned_at)},{"inventory_policy",quote(scan.inventory_policy)},{"build_logs_loaded",boolean(scan.build_logs_loaded)},
        {"file_changes",object({{"added",number(scan.added)},{"changed",number(scan.changed)},{"unchanged",number(scan.unchanged)},{"removed",number(scan.removed)}})},
        {"files",array(scan.files,[this](const FileRecord& f){return object({{"file",quote(f.path)},{"language",quote(f.language)},{"hash",quote(f.hash)},{"size",number(f.size)},{"mtime",number(f.mtime)},{"lines",number(f.lines)}});})},
        {"analysis",object({{"status",quote(a.status)},{"compile_commands",quote(a.compile_commands)},{"configuration",quote(a.configuration)},{"analyzer_revision",quote(a.analyzer_revision)},{"workers",number(a.workers)},{"elapsed_ms",number(a.elapsed_ms)},
            {"covered_files",strings(a.covered_files)},{"rule_diagnostics",strings(a.rule_diagnostics)},
            {"units",array(a.units,[this](const TranslationUnitResult& u){return object({{"file",quote(u.file)},{"status",quote(u.status)},{"diagnostics",quote(u.diagnostics)},{"indirect_calls",number(u.indirect_calls)},{"command_fingerprint",quote(u.command_fingerprint)},{"covered_files",strings(u.covered_files)}});})},
            {"issues",array(a.issues,[this](const Issue& issue){return issue_json(issue);})},{"suppressed_issues",array(a.suppressed_issues,[this](const Issue& issue){return issue_json(issue);})},
            {"symbols",array(a.symbols,[this](const Symbol& s){return object({{"id",quote(s.id)},{"kind",quote(s.kind)},{"name",quote(s.name)},{"file",quote(s.file)},{"line",number(s.line)},{"column",number(s.column)},{"definition",boolean(s.definition)},{"external",boolean(s.external)}});})},
            {"metrics",array(a.metrics,[this](const FunctionMetric& m){return object({{"symbol_id",quote(m.symbol_id)},{"lines",number(m.lines)},{"parameters",number(m.parameters)},{"complexity",number(m.complexity)}});})}})},
        {"build_runs",array(scan.build_runs,[this](const BuildRun& r){return object({{"id",number(r.id)},{"scan_id",number(r.scan_id)},{"started_at",quote(r.started_at)},{"status",quote(r.status)},{"workspace",quote(r.workspace)},{"target",quote(r.target)},{"git_revision",quote(r.git_revision)},{"git_log",quote(r.git_log)},{"source_unchanged",boolean(r.source_unchanged)},{"compile_commands",quote(r.compile_commands)},{"configuration",quote(r.configuration)},
            {"steps",array(r.steps,[this](const BuildStep& s){return object({{"name",quote(s.name)},{"command",quote(s.command)},{"working_directory",quote(s.working_directory)},{"status",quote(s.result.status)},{"exit_code",number(s.result.exit_code)},{"duration_ms",number(s.result.duration_ms)},{"tests_total",number(s.tests_total)},{"tests_failed",number(s.tests_failed)},{"tests_skipped",number(s.tests_skipped)},{"stdout",quote(s.result.stdout_text)},{"stderr",quote(s.result.stderr_text)},{"output_truncated",boolean(s.result.output_truncated)}});})}});})}
    });
}
void validate(const ScanResult& scan){
    if(scan.id<=0||scan.root.empty()||!scan.diagnostics.empty())throw std::invalid_argument("report requires a saved, complete file inventory");
    for(const auto& run:scan.build_runs)if(run.scan_id!=scan.id||run.root!=scan.root)throw std::invalid_argument("report build does not belong to the selected snapshot");
}
std::string location(const Issue& i){return i.file+":"+number(i.line)+":"+number(i.column);}
std::string change_label(const std::string& status){
    static const std::map<std::string,std::string> labels={
        {"added","新增观测"},{"persistent","持续存在"},{"persistent_suppressed","持续抑制"},{"suppressed","转为抑制"},{"reactivated","恢复活动"},{"new_suppressed","新增抑制观测"},
        {"file_removed","文件移出清单"},{"inventory_scope_changed","扫描范围变化"},{"inventory_unknown","清单范围未知"},{"analysis_disabled","未执行分析"},{"rule_disabled","规则停用"},
        {"parse_failed","解析失败"},{"coverage_lost","覆盖丢失"},{"configuration_unknown","配置未知"},{"configuration_changed","编译配置变化"},{"analyzer_changed","分析器变化"},{"provenance_unknown","历史溯源不足"},{"coverage_unknown","覆盖来源未知"},{"not_detected","未再检出（待复核）"}};
    const auto found=labels.find(status);return found==labels.end()?status:found->second;
}
std::string pre(const std::string& s){return "<pre>"+escape(s)+"</pre>";}
std::string detail(const std::string& title,const std::string& text){return "<details><summary>"+escape(title)+"</summary>"+pre(text)+"</details>";}
std::string cell(const std::string& text){return "<td>"+escape(text)+"</td>";}
std::string table(std::initializer_list<std::string> headers){std::string out="<div class=table-wrap><table><thead><tr>";for(const auto& title:headers)out+="<th>"+escape(title)+"</th>";return out+"</tr></thead><tbody>";}
const std::string table_end="</tbody></table></div>";
std::string issue_html(const Issue& issue){return "<strong>"+escape(issue.rule_id+" · "+issue.severity)+"</strong><p class=location>"+escape(location(issue))+"</p><p>"+escape(issue.message)+"</p>"+detail("源码证据",issue.evidence)+detail("建议",issue.suggestion)+(issue.suppression_reason.empty()?"":"<p><b>抑制理由：</b>"+escape(issue.suppression_reason)+"</p>");}
std::string snapshot_html(const ScanResult& scan){
    const auto& a=scan.analysis;std::string out;
    out+="<p class=location>"+escape(scan.root)+"</p><p>扫描 #"+number(scan.id)+" · "+escape(report_time(scan.scanned_at))+" · 分析状态 <strong>"+escape(a.status)+"</strong></p>";
    const auto parsed=std::count_if(a.units.begin(),a.units.end(),[this](const auto& u){context.check();return u.status=="success";});
    out+="<div class=stats>";
    for(const auto& [title,value]:std::vector<std::pair<std::string,std::string>>{{"活动问题",number(a.issues.size())},{"已抑制",number(a.suppressed_issues.size())},{"成功解析 TU",number(parsed)+" / "+number(a.units.size())},{"关联构建",number(scan.build_runs.size())}})
        out+="<div><span>"+escape(title)+"</span><b>"+escape(value)+"</b></div>";
    out+="</div><p class=note>无活动问题不代表无缺陷。抑制不算修复，解析失败不算通过；源文件指纹用于变化检测。报告展示保存快照中的证据，原源码和日志目录可以已不存在。</p>";
    out+="<details><summary>扫描配置与版本</summary><p>分析线程："+number(a.workers)+" · 分析耗时："+number(a.elapsed_ms)+" ms</p><p class=location>编译数据库："+escape(a.compile_commands)+"</p>";
    out+=detail("当次工程配置",a.configuration.empty()?"旧快照未记录":a.configuration)+detail("分析器版本",a.analyzer_revision.empty()?"旧快照未记录":a.analyzer_revision)+detail("实际扫描范围",scan.inventory_policy.empty()?"旧快照未记录":scan.inventory_policy)+"</details>";
    out+="<h3>问题与证据</h3>";
    if(a.issues.empty())out+="<p>本快照没有活动规则问题，请结合解析覆盖查看。</p>";
    for(const auto& issue:a.issues)out+="<article class=issue>"+issue_html(issue)+"</article>";
    out+="<details><summary>已抑制问题（"+number(a.suppressed_issues.size())+"）</summary>";
    for(const auto& issue:a.suppressed_issues)out+="<article class=issue>"+issue_html(issue)+"</article>";
    out+="</details><details><summary>规则配置诊断（"+number(a.rule_diagnostics.size())+"）</summary>";
    for(const auto& message:a.rule_diagnostics)out+="<p>"+escape(message)+"</p>";
    out+="</details><h3>解析覆盖</h3><p>清单文件 "+number(scan.files.size())+" · 已覆盖文件 "+number(a.covered_files.size())+"。TU 成功仅代表选定编译配置，不代表全部宏配置或孤立头文件。</p>";
    out+=table({"翻译单元","状态","诊断与溯源"});
    for(const auto& unit:a.units){out+="<tr>"+cell(unit.file)+cell(unit.status)+"<td>"+detail("诊断",unit.diagnostics)+detail("编译命令指纹",unit.command_fingerprint.empty()?"旧快照未记录或没有选定命令":unit.command_fingerprint);std::string covered;for(const auto& path:unit.covered_files){context.check();covered+=path+'\n';}out+=detail("本 TU 成功覆盖文件",covered)+"</td></tr>";}
    out+=table_end;
    std::set<std::string> covered;for(const auto& path:a.covered_files){context.check();covered.insert(path);}
    out+="<details><summary>未覆盖文件</summary><ul>";for(const auto& f:scan.files)if((context.check(),!covered.contains(f.path)))out+="<li>"+escape(f.path)+"</li>";out+="</ul></details>";
    out+="<h3>函数复杂度</h3><p>复杂度为 CFG 圈复杂度；未知值保持未知，不换成零。</p>"+table({"函数","位置","复杂度","函数行数","参数"});
    std::map<std::string,const Symbol*> symbols;for(const auto& s:a.symbols){context.check();symbols[s.id]=&s;}
    auto metrics=a.metrics;std::stable_sort(metrics.begin(),metrics.end(),[this](const auto& x,const auto& y){context.check();return x.complexity>y.complexity;});
    for(const auto& m:metrics){auto found=symbols.find(m.symbol_id);const auto name=found==symbols.end()?m.symbol_id:found->second->name;const auto where=found==symbols.end()?"未记录":found->second->file+":"+number(found->second->line);out+="<tr>"+cell(name)+cell(where)+cell(m.complexity<0?"未知":number(m.complexity))+cell(number(m.lines))+cell(number(m.parameters))+"</tr>";}
    out+=table_end+"<h3>构建与测试</h3>";
    if(scan.build_runs.empty())out+="<p>该扫描没有关联构建记录，不能据此认为测试通过。</p>";
    else if(!scan.build_logs_loaded)out+="<p class=note>本次输入仅载入构建摘要，未载入完整输出日志；空日志字段不代表构建没有输出。</p>";
    for(const auto& run:scan.build_runs){
        out+="<article class=issue><h4>构建 #"+number(run.id)+" · "+escape(run.status)+"</h4><p>关联扫描 #"+number(run.scan_id)+" · "+escape(report_time(run.started_at))+" · 目标："+escape(run.target.empty()?"全部":run.target)+"</p><p>构建时 Git 提交："+escape(run.git_revision.empty()?"未知":run.git_revision)+" · 源码一致性检查："+(run.source_unchanged?"通过":"未通过或未确认")+"</p>";
        out+=detail("构建配置",run.configuration)+detail("Git 记录",run.git_log)+table({"阶段","状态 / 退出码","耗时 ms","测试数 / 失败 / 跳过"});
        for(const auto& step:run.steps){const auto tests=step.tests_total<0?"未知":number(step.tests_total)+" / "+number(step.tests_failed)+" / "+number(step.tests_skipped);out+="<tr>"+cell(step.name)+cell(step.result.status+" / "+number(step.result.exit_code))+cell(number(step.result.duration_ms))+cell(tests)+"</tr>";}
        out+=table_end;
        for(const auto& step:run.steps)out+=detail(step.name+" 命令与日志"+(step.result.output_truncated?"（已截断）":""),step.command+"\n工作目录："+step.working_directory+"\n\nstdout:\n"+step.result.stdout_text+"\nstderr:\n"+step.result.stderr_text);
        out+="</article>";
    }
    out+="<details><summary>文件清单与内容指纹</summary>"+table({"文件","语言","物理行","字节数","变化检测指纹"});
    for(const auto& f:scan.files)out+="<tr>"+cell(f.path)+cell(f.language)+cell(number(f.lines))+cell(number(f.size))+cell(f.hash)+"</tr>";
    return out+table_end+"</details>";
}
std::string report_time(const std::string& value){
    if(value.empty())return "未知时间";
    std::int64_t millis=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),millis);
    if(error!=std::errc{}||end!=value.data()+value.size()||millis<0||millis>253402300799999)return value;
    const std::chrono::sys_time<std::chrono::milliseconds> time{std::chrono::milliseconds{millis}};
    const auto day=std::chrono::floor<std::chrono::days>(time);const std::chrono::year_month_day date{day};const std::chrono::hh_mm_ss clock{time-day};
    std::ostringstream out;out<<std::setfill('0')<<std::setw(4)<<static_cast<int>(date.year())<<'-'<<std::setw(2)<<static_cast<unsigned>(date.month())<<'-'<<std::setw(2)<<static_cast<unsigned>(date.day())<<' '<<std::setw(2)<<clock.hours().count()<<':'<<std::setw(2)<<clock.minutes().count()<<':'<<std::setw(2)<<clock.seconds().count()<<" UTC";return out.str();
}
std::string report_json(const ScanResult& current,const ScanResult* baseline){
    context.report("report_json");
    validate(current);std::string comparison="null";
    if(baseline){validate(*baseline);const auto diff=compare_scans(*baseline,current,context);comparison=object({{"before_id",number(diff.before_id)},{"after_id",number(diff.after_id)},{"notes",strings(diff.notes)},{"changes",array(diff.changes,[this](const IssueChange& c){return object({{"status",quote(c.status)},{"reason",quote(c.reason)},{"before",c.before?issue_json(*c.before):"null"},{"after",c.after?issue_json(*c.after):"null"}});})}});}
    return object({{"format","\"CodeGuardReport\""},{"version","1"},{"snapshot",snapshot_json(current)},{"baseline",baseline?snapshot_json(*baseline):"null"},{"comparison",comparison}})+'\n';
}
std::string report_html(const ScanResult& current,const ScanResult* baseline){
    context.report("report_html");
    validate(current);
    std::string out=R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"><title>CodeGuard 工程质量报告</title><style>
:root{color-scheme:light;font-family:"Segoe UI","Microsoft YaHei",sans-serif;color:#172a42;background:#eef2f7}*{box-sizing:border-box}body{margin:0}main{max-width:1180px;margin:auto;padding:36px 28px 70px}header{border-bottom:3px solid #245cea;padding-bottom:20px;margin-bottom:26px}header small{letter-spacing:.12em;color:#245cea;font-weight:700}h1{font-size:32px;margin:12px 0}h2{margin-top:32px;font-size:24px}h3{margin-top:28px}p,li{line-height:1.65}section,.issue{background:white;border:1px solid #d7e0ec;border-radius:8px;padding:20px;margin:14px 0}.issue p{margin:8px 0}.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}.stats div{border-top:3px solid #245cea;background:#f5f8ff;padding:14px}.stats span{display:block;color:#53647b;font-size:13px}.stats b{display:block;font-size:25px;margin-top:5px}.note{background:#fff7df;padding:14px;border-left:4px solid #d89a12}.location{overflow-wrap:anywhere;font-family:Consolas,"Microsoft YaHei",monospace;font-size:13px;color:#405776}.table-wrap{overflow-x:auto;margin:12px 0}table{border-collapse:collapse;width:100%;text-align:left}th,td{border-bottom:1px solid #dae2ee;padding:10px;vertical-align:top;overflow-wrap:anywhere}th{background:#edf3fd;white-space:nowrap}td{min-width:80px}summary{cursor:pointer;padding:9px 0;color:#2456a2;font-weight:600}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#f5f7fa;border:1px solid #e0e6ef;border-radius:4px;padding:12px;font:13px/1.55 Consolas,"Microsoft YaHei",monospace}details{margin:8px 0}footer{font-size:13px;color:#53647b;margin-top:30px}.diff td{width:33.33%}.tag{display:inline-block;background:#edf3fd;border-radius:4px;padding:6px 10px;margin:4px}@media(max-width:720px){main{padding:20px 12px}h1{font-size:26px}section,.issue{padding:14px}.stats{grid-template-columns:repeat(2,1fr)}.diff{min-width:700px}}@media print{body{background:white}main{padding:0;max-width:none}section{border:0;padding:0}h2,h3,h4{break-after:avoid}tr,.stats{break-inside:avoid}.table-wrap{overflow:visible}details[open]{break-inside:auto}}
</style><body><main><header><small>CODEGUARD / QUALITY REPORT</small><h1>工程质量报告</h1><p>问题、解析覆盖、复杂度与构建测试，关联到保存的扫描版本。</p></header>)HTML";
    if(baseline){validate(*baseline);const auto diff=compare_scans(*baseline,current,context);out+="<section><h2>版本对比 · #"+number(baseline->id)+" → #"+number(current.id)+"</h2>";
        std::map<std::string,std::size_t> counts;for(const auto& c:diff.changes)++counts[c.status];for(const auto& [status,count]:counts)out+="<span class=tag>"+escape(change_label(status))+" "+number(count)+"</span>";
        for(const auto& note:diff.notes)out+="<p class=note>"+escape(note)+"</p>";
        out+="<div class=table-wrap><table class=diff><thead><tr><th>变化及依据</th><th>基线问题</th><th>本次问题</th></tr></thead><tbody>";
        for(const auto& c:diff.changes)out+="<tr><td><b>"+escape(change_label(c.status))+"</b><p>"+escape(c.reason)+"</p></td><td>"+(c.before?issue_html(*c.before):"无匹配记录")+"</td><td>"+(c.after?issue_html(*c.after):"无匹配记录")+"</td></tr>";
        out+=table_end+"</section>";
    }
    out+="<section><h2>当前快照</h2>"+snapshot_html(current)+"</section>";
    if(baseline)out+="<section><details><summary>基线快照 #"+number(baseline->id)+" · 完整证据</summary>"+snapshot_html(*baseline)+"</details></section>";
    return out+"<footer>CodeGuard 报告格式 v1 · 本 HTML 可独立查看，无网络依赖。报告依据已保存数据；外部头文件内容及完整构建环境未全部保存，测试通过也不等于修复正确。</footer></main></body></html>\n";
}
};
} // namespace
std::string report_time(const std::string& value) { return Renderer{ScanContext{}}.report_time(value); }
std::string report_json(const ScanResult& current,const ScanResult* baseline,const ScanContext& context) { return Renderer{context}.report_json(current,baseline); }
std::string report_html(const ScanResult& current,const ScanResult* baseline,const ScanContext& context) { return Renderer{context}.report_html(current,baseline); }
ReportFiles export_report(const ScanResult& current,const fs::path& output,const ScanResult* baseline,const ScanContext& context){
    context.check();
    const auto destination=fs::weakly_canonical(fs::absolute(output));
    if(output.empty()||project_path_inside(destination,from_utf8(current.root)))throw std::invalid_argument("report output must be outside the source tree");
    const auto json=report_json(current,baseline,context),html=report_html(current,baseline,context);
    if(!fs::is_directory(destination.parent_path()))throw std::invalid_argument("report parent directory must already exist");
    context.report("report_ready");
    if(context.control)context.control->begin_commit();
    if(!fs::create_directory(destination))throw std::invalid_argument("report directory already exists; choose a new directory");
    ReportFiles files{destination/"report.html",destination/"report.json"};
    auto write=[&](const fs::path& path,const std::string& contents){std::ofstream file(path,std::ios::binary);file.write(contents.data(),static_cast<std::streamsize>(contents.size()));file.close();if(!file)throw std::runtime_error("report write failed; incomplete output preserved at "+utf8_path(destination));};
    write(files.json,json);write(files.html,html);return files;
}
} // namespace codeguard
