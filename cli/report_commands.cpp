#include "report_commands.hpp"
#include "codeguard/report.hpp"
#include <charconv>
#include <cwctype>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace cg=codeguard;
std::int64_t integer(const std::string& text){
    std::int64_t result=-1;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
    if(error!=std::errc{}||end!=text.data()+text.size()||result<0)throw std::invalid_argument("invalid nonnegative ID or count: "+text);
    return result;
}
auto normalized(const std::string& path){
    auto value=cg::fs::absolute(cg::from_utf8(path)).lexically_normal().wstring();
#ifdef _WIN32
    for(auto& c:value)c=std::towlower(c);
#endif
    return value;
}
std::string saved_root(cg::SqliteDatabase& db,const std::string& input){
    const auto wanted=normalized(input);
    for(const auto& project:db.projects())if(normalized(project.root)==wanted)return project.root;
    throw std::invalid_argument("project has no saved scan; use a saved root shown by recent");
}
}
std::optional<int> report_command(const std::vector<std::string>& args){
    if(args.empty()||(args[0]!="scans"&&args[0]!="report"))return {};
    if(args.size()<2||args[1].starts_with("--"))throw std::invalid_argument("missing saved PROJECT root");
    const bool history=args[0]=="scans";
    const std::set<std::string> allowed=history?std::set<std::string>{"--database","--before","--limit"}:std::set<std::string>{"--database","--scan","--baseline","--output"};
    std::map<std::string,std::string> options;
    for(std::size_t i=2;i<args.size();i+=2){
        if(i+1==args.size()||!allowed.contains(args[i])||!options.emplace(args[i],args[i+1]).second)throw std::invalid_argument("missing, unknown or repeated report/history option: "+args[i]);
    }
    if(!options.contains("--database")||options["--database"].empty())throw std::invalid_argument("--database is required");
    cg::SqliteDatabase db(cg::from_utf8(options["--database"]),true);const auto root=saved_root(db,args[1]);
    if(history){
        const auto before=options.contains("--before")?integer(options["--before"]):0;
        const auto limit=options.contains("--limit")?integer(options["--limit"]):50;
        if(limit<1||limit>500)throw std::invalid_argument("history limit must be from 1 to 500");
        const auto scans=db.scans(root,before,static_cast<unsigned>(limit));
        std::cout<<"scan_id\tscanned_at\tanalysis\tfiles\tissues\tsuppressed\n";
        for(const auto& scan:scans)std::cout<<scan.id<<'\t'<<scan.scanned_at<<'\t'<<scan.analysis_status<<'\t'<<scan.file_count<<'\t'<<scan.issue_count<<'\t'<<scan.suppressed_count<<'\n';
        if(scans.size()==static_cast<std::size_t>(limit))std::cerr<<"next_before="<<scans.back().id<<'\n';
    }else{
        if(!options.contains("--output")||options["--output"].empty())throw std::invalid_argument("report requires --output NEW_DIRECTORY");
        const auto id=options.contains("--scan")?integer(options["--scan"]):db.scans(root,0,1).front().id;
        const auto scan=db.snapshot(root,id,true);
        std::optional<cg::ScanResult> baseline;if(options.contains("--baseline"))baseline=db.snapshot(root,integer(options["--baseline"]),true);
        const auto files=cg::export_report(scan,cg::from_utf8(options["--output"]),baseline?&*baseline:nullptr);
        std::cout<<"scan_id="<<scan.id<<"\nanalysis="<<scan.analysis.status<<"\nreport_html="<<cg::utf8_path(files.html)<<"\nreport_json="<<cg::utf8_path(files.json)<<'\n';
    }
    return 0;
}
