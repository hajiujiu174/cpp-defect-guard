#include "coverage_internal.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace codeguard {
namespace {
void save(const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;if(!out)throw std::runtime_error("cannot save per-test coverage evidence");}
std::string read(const fs::path& path){if(!fs::is_regular_file(path)||fs::file_size(path)>8*1024*1024)throw std::runtime_error("missing or oversized CTest JUnit record");std::ifstream in(path,std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});if(!in)throw std::runtime_error("cannot read CTest JUnit record");return text;}
std::string quote(const std::string& value){std::ostringstream out;out<<'"';const char* digits="0123456789abcdef";for(unsigned char c:value){if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);else if(c<32)out<<"\\u00"<<digits[c/16]<<digits[c%16];else out<<static_cast<char>(c);}return out.str()+'"';}
void check(const BuildOptions& options){if(options.control)options.control->check();}
struct Json {
    sqlite3* db=nullptr;
    explicit Json(const std::string& text){
        if(sqlite3_open(":memory:",&db)!=SQLITE_OK){if(db)sqlite3_close(db);db=nullptr;throw std::runtime_error("cannot initialize CTest JSON reader");}
        sqlite3_stmt* statement=nullptr;const auto rc=sqlite3_prepare_v2(db,"CREATE TABLE document(value TEXT)",-1,&statement,nullptr);if(rc!=SQLITE_OK){sqlite3_close(db);db=nullptr;throw std::runtime_error("cannot prepare CTest JSON reader");}sqlite3_step(statement);sqlite3_finalize(statement);
        if(sqlite3_prepare_v2(db,"INSERT INTO document VALUES(?1)",-1,&statement,nullptr)!=SQLITE_OK){sqlite3_close(db);db=nullptr;throw std::runtime_error("cannot bind CTest JSON");}sqlite3_bind_text(statement,1,text.data(),static_cast<int>(text.size()),SQLITE_TRANSIENT);sqlite3_step(statement);sqlite3_finalize(statement);
    }
    ~Json(){if(db)sqlite3_close(db);}Json(const Json&)=delete;Json& operator=(const Json&)=delete;
    std::vector<std::vector<std::string>> rows(const char* sql){sqlite3_stmt* raw=nullptr;if(sqlite3_prepare_v2(db,sql,-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("CTest metadata requires SQLite JSON support");std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);std::vector<std::vector<std::string>> result;int code;
        while((code=sqlite3_step(raw))==SQLITE_ROW){if(result.size()>=4096)throw std::runtime_error("CTest metadata exceeds row limit");std::vector<std::string> row;for(int c=0;c<sqlite3_column_count(raw);++c){const auto* text=sqlite3_column_text(raw,c);row.emplace_back(text?reinterpret_cast<const char*>(text):"",text?static_cast<std::size_t>(sqlite3_column_bytes(raw,c)):0);}result.push_back(std::move(row));}if(code!=SQLITE_DONE)throw std::runtime_error("invalid CTest JSON metadata");return result;}
};
struct Definition{std::string name;std::vector<std::string> depends;};
std::vector<Definition> catalog(const std::string& text,bool require_dependencies=true){
    Json json(text);const auto header=json.rows("SELECT json_valid(value),json_extract(value,'$.kind'),json_extract(value,'$.version.major'),json_type(value,'$.tests') FROM document");
    if(header.size()!=1||header[0]!=std::vector<std::string>{"1","ctestInfo","1","array"})throw std::runtime_error("CTest json-v1 test catalog is required");
    auto rows=json.rows("SELECT json_type(t.value,'$.name'),json_extract(t.value,'$.name') FROM document,json_each(document.value,'$.tests') t");
    if(rows.empty()||rows.size()>256)throw std::runtime_error("per-test coverage requires 1..256 declared tests");std::vector<Definition> out;std::map<std::string,std::size_t> indices;
    for(const auto& row:rows){if(row[0]!="text"||row[1].empty()||row[1].size()>4096||row[1].find_first_of("\r\n\0",0,3)!=std::string::npos||!indices.emplace(row[1],out.size()).second)throw std::runtime_error("CTest test names must be unique nonempty single lines");out.push_back({row[1],{}});}
    const auto types=json.rows("SELECT json_type(p.value,'$.value') FROM document,json_each(document.value,'$.tests') t,json_each(t.value,'$.properties') p WHERE json_extract(p.value,'$.name')='DEPENDS'");
    for(const auto& row:types)if(row[0]!="array")throw std::runtime_error("unsupported CTest dependency representation");
    const auto dependencies=json.rows("SELECT json_extract(t.value,'$.name'),d.type,d.value FROM document,json_each(document.value,'$.tests') t,json_each(t.value,'$.properties') p,json_each(p.value,'$.value') d WHERE json_extract(p.value,'$.name')='DEPENDS'");
    for(const auto& row:dependencies){if(row[1]!="text"||!indices.contains(row[0])||(require_dependencies&&!indices.contains(row[2])))throw std::runtime_error("CTest dependency is outside declared test catalog");out[indices.at(row[0])].depends.push_back(row[2]);}return out;
}
std::string xml(const std::string& name){std::string out;for(char c:name){switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;case '\'':out+="&apos;";break;default:out+=c;}}return out;}
std::string attribute(const std::string& tag,const std::string& key){
    const auto marker=key+"=\"";std::size_t at=0;
    while((at=tag.find(marker,at))!=std::string::npos){if(at==0||std::isspace(static_cast<unsigned char>(tag[at-1]))){const auto begin=at+marker.size(),end=tag.find('"',begin);if(end==std::string::npos)break;return tag.substr(begin,end-begin);}at+=marker.size();}
    throw std::runtime_error("CTest JUnit attribute missing: "+key);
}
std::string tag_at(const std::string& text,std::size_t at){const auto end=text.find('>',at);if(end==std::string::npos||end-at>65536)throw std::runtime_error("invalid or oversized CTest JUnit tag");return text.substr(at,end-at+1);}
bool junit(RepairTestCoverageEntry& entry,const fs::path& file){
    const auto text=read(file);const auto suite=text.find("<testsuite");if(suite==std::string::npos||suite+10>=text.size()||!std::isspace(static_cast<unsigned char>(text[suite+10])))throw std::runtime_error("CTest JUnit suite missing");const auto tag=tag_at(text,suite);
    auto number=[&](const char* key){const auto value=attribute(tag,key);if(value.empty()||value.size()>6||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid CTest JUnit count");return std::stoi(value);};
    entry.tests_total=number("tests");entry.tests_failed=number("failures");entry.tests_skipped=number("skipped");entry.tests_disabled=number("disabled");entry.tests_not_run=0;
    std::set<std::string> expected;for(const auto& name:entry.selected_tests)expected.insert(xml(name));std::set<std::string> actual;bool ran=true;std::size_t at=0;
    while((at=text.find("<testcase",at))!=std::string::npos){if(at+9>=text.size()||!std::isspace(static_cast<unsigned char>(text[at+9])))throw std::runtime_error("invalid CTest JUnit case tag");const auto current=tag_at(text,at);at+=current.size();if(!actual.insert(attribute(current,"name")).second)throw std::runtime_error("duplicate CTest JUnit test name");const auto status=attribute(current,"status");const bool executed=status=="run"||status=="fail";entry.tests_not_run+=!executed;ran=ran&&executed;}
    if(actual!=expected||entry.tests_total!=static_cast<int>(actual.size()))throw std::runtime_error("CTest executed test set differs from selected group");
    return ran&&entry.tests_failed==0&&entry.tests_skipped==0&&entry.tests_disabled==0&&entry.tests_total>0;
}
using Lines=std::map<std::pair<std::string,int>,bool>;
Lines lines(const RepairCoverage& coverage){Lines result;for(const auto& file:coverage.files)for(const auto& [line,hit]:file.lines)result[{file.file,line}]=hit;return result;}
}
RepairTestCoverage collect_repair_test_coverage(const BuildRun& run,const std::vector<RepairTargetValidation>& targets,
    const ValidationInputs& snapshot,const RepairCoverage& aggregate,const BuildOptions& options,const fs::path& directory){
    RepairTestCoverage result;result.status="running";result.test_limit=options.coverage_test_limit;result.workspace=utf8_path(directory);fs::create_directories(directory);
    auto persist=[&]{save(directory/"summary.json",repair_test_coverage_json(result));};persist();std::size_t current=0;
    try{
        check(options);if(targets.empty())throw std::runtime_error("per-test coverage requires at least one target");if(!options.collect_coverage||aggregate.status!="collected")throw std::runtime_error("per-test replay needs successful aggregate gcov collection");
        if(options.coverage_test_limit<1||options.coverage_test_limit>256)throw std::runtime_error("per-test replay limit must be 1..256");
        const auto work=from_utf8(run.workspace),build=work/"build";
        const auto version=run_process({{options.ctest,"--version"},utf8_path(work),options.timeout,65536,options.control});save(directory/"ctest-version.log",version.stdout_text+version.stderr_text);check(options);
        result.ctest_version=version.stdout_text.substr(0,version.stdout_text.find('\n'));if(!result.ctest_version.empty()&&result.ctest_version.back()=='\r')result.ctest_version.pop_back();
        std::smatch match;if(version.status!="passed"||version.output_truncated||!std::regex_search(version.stdout_text,match,std::regex("ctest version ([0-9]{1,3})\\.([0-9]{1,3})"))||std::stoi(match[1])<3||(std::stoi(match[1])==3&&std::stoi(match[2])<29))throw std::runtime_error("per-test replay requires CTest 3.29 or later");
        const std::vector<std::string> common{options.ctest,"--test-dir",utf8_path(build),"-C",options.build_type};
        auto invoke=[&](std::vector<std::string> args,const fs::path& where){auto command=common;command.insert(command.end(),args.begin(),args.end());const auto p=run_process({command,utf8_path(work),options.timeout,8*1024*1024,options.control});save(where,format_arguments(command)+"\nstatus="+p.status+"\n"+p.stdout_text+"\nstderr:\n"+p.stderr_text);check(options);if(p.status!="passed"||p.output_truncated)throw std::runtime_error("CTest metadata command failed or was truncated");return p.stdout_text;};
        const auto original=invoke({"--show-only=json-v1"},directory/"discovery.log");save(directory/"tests.json",original);const auto definitions=catalog(original);
        for(std::size_t n=0;n<definitions.size();++n){RepairTestCoverageEntry e;e.name=definitions[n].name;e.workspace=utf8_path(directory/("t"+std::to_string(n)));for(const auto& target:targets)e.targets.push_back({target.file,target.revised_line,"unknown"});result.entries.push_back(std::move(e));}persist();
        if(definitions.size()>options.coverage_test_limit){result.status="limit_exceeded";result.error="Declared test count exceeds explicit replay limit; no tests replayed.";persist();return result;}
        std::map<std::string,const Definition*> by_name;for(const auto& d:definitions)by_name[d.name]=&d;Lines united;bool complete=true;std::uintmax_t total_bytes=0;
        for(current=0;current<result.entries.size();++current){
            check(options);auto& entry=result.entries[current];const auto row=from_utf8(entry.workspace);fs::create_directory(row);entry.status="running";persist();if(options.progress)options.progress("repair_test_coverage");check(options);
            try{
                std::set<std::string> selected;std::function<void(const std::string&)> dependency=[&](const std::string& name){if(!by_name.contains(name))throw std::runtime_error("selected dependency is not in original CTest catalog");if(!selected.insert(name).second)return;for(const auto& d:by_name.at(name)->depends)dependency(d);};dependency(entry.name);
                for(std::size_t attempt=0;;++attempt){
                    if(attempt>definitions.size())throw std::runtime_error("CTest fixture/dependency closure did not stabilize");
                    std::string selection;for(const auto& name:selected)selection+=name+'\n';save(row/"selection.txt",selection);
                    const auto group=invoke({"--show-only=json-v1","--tests-from-file",utf8_path(row/"selection.txt")},row/"selection.log");save(row/"tests.json",group);const auto planned=catalog(group,false);const auto before=selected.size();entry.selected_tests.clear();
                    for(const auto& d:planned){if(!by_name.contains(d.name))throw std::runtime_error("CTest added a test outside original catalog");entry.selected_tests.push_back(d.name);dependency(d.name);}
                    for(const auto& name:selected)if(std::find(entry.selected_tests.begin(),entry.selected_tests.end(),name)==entry.selected_tests.end()&&selected.size()==before)throw std::runtime_error("CTest did not select requested test/dependency");
                    if(selected.size()==before)break;
                }
                reset_repair_counters(work,row/"counters-before",options);
                auto command=common;const auto seconds=std::to_string(std::max<std::int64_t>(1,options.timeout.count()/1000));const std::vector<std::string> extra{"--tests-from-file",utf8_path(row/"selection.txt"),"--parallel","1","--output-on-failure","--no-tests=error","--timeout",seconds,"--output-junit",utf8_path(row/"tests.xml")};command.insert(command.end(),extra.begin(),extra.end());
                const auto process=run_process({command,utf8_path(work),options.timeout,4*1024*1024,options.control});save(row/"ctest.log",format_arguments(command)+"\nstatus="+process.status+"\n"+process.stdout_text+"\nstderr:\n"+process.stderr_text);check(options);
                const bool passed=junit(entry,row/"tests.xml")&&process.status=="passed"&&!process.output_truncated;
                RepairCandidate target;target.edit.file=targets.front().file;target.edit.line=targets.front().revised_line;entry.coverage=collect_repair_coverage(run,target,snapshot,options,row/"coverage");check(options);
                if(invoke({"--show-only=json-v1"},row/"discovery-after.log")!=original)throw std::runtime_error("CTest catalog changed during per-test replay");
                if(entry.coverage.status!="collected"||entry.coverage.tool_version!=aggregate.tool_version||entry.coverage.scope_signature!=aggregate.scope_signature)throw std::runtime_error("per-test coverage collection or scope differs from aggregate");
                const auto recorded=lines(entry.coverage);for(auto& t:entry.targets){const auto found=recorded.find({t.file,t.line});t.status=found==recorded.end()?"not_instrumented":found->second?"recorded":"no_recorded_execution";}
                entry.status=passed?"collected":"tests_incomplete";if(!passed)entry.error="CTest group did not execute and pass every selected test; coverage is diagnostic only.";
                if(passed)for(const auto& [line,hit]:recorded)united[line]=united[line]||hit;else complete=false;
            }catch(const ScanCancelled&){throw;}catch(const std::exception& e){entry.status="failed";entry.error=e.what();complete=false;}
            for(const auto& item:fs::recursive_directory_iterator(row)){check(options);if(item.is_symlink())throw std::runtime_error("per-test evidence contains a symlink");if(item.is_regular_file())total_bytes+=item.file_size();}
            persist();if(options.progress)options.progress("repair_test_coverage_collected");check(options);if(total_bytes>1024ULL*1024*1024)throw std::runtime_error("per-test evidence exceeds 1 GiB; remaining groups not run");
        }
        result.line_union_matches_aggregate=complete&&united==lines(aggregate);result.status=complete&&result.line_union_matches_aggregate?"collected":"incomplete";
        if(complete&&!result.line_union_matches_aggregate)result.error="Replay line union differs from aggregate; attribution is incomplete.";
    }catch(const ScanCancelled& e){result.status="cancelled";result.error=e.what();if(current<result.entries.size()&&result.entries[current].status=="running")result.entries[current].status="cancelled";}
    catch(const std::exception& e){result.status="failed";result.error=e.what();if(current<result.entries.size()&&result.entries[current].status=="running"){result.entries[current].status="failed";result.entries[current].error=e.what();}}
    persist();return result;
}
std::string repair_test_coverage_json(const RepairTestCoverage& coverage){
    std::ostringstream out;out<<"{\"status\": "<<quote(coverage.status)<<", \"workspace\": "<<quote(coverage.workspace)<<", \"error\": "<<quote(coverage.error)<<", \"ctest_version\": "<<quote(coverage.ctest_version)<<", \"test_limit\": "<<coverage.test_limit<<", \"declared_tests\": "<<coverage.entries.size()<<", \"line_union_matches_aggregate\": "<<(coverage.line_union_matches_aggregate?"true":"false")<<", \"semantics\": \"One requested CTest test with its transitive DEPENDS and automatic fixtures per group; counters reset between groups; repeated in one build workspace, not a clean environment per test\", \"entries\": [";
    for(std::size_t n=0;n<coverage.entries.size();++n){if(n)out<<',';const auto& e=coverage.entries[n];out<<"{\"name\": "<<quote(e.name)<<", \"status\": "<<quote(e.status)<<", \"workspace\": "<<quote(e.workspace)<<", \"error\": "<<quote(e.error)<<", \"tests_total\": "<<e.tests_total<<", \"tests_failed\": "<<e.tests_failed<<", \"tests_skipped\": "<<e.tests_skipped<<", \"tests_disabled\": "<<e.tests_disabled<<", \"tests_not_run\": "<<e.tests_not_run<<", \"selected_tests\": [";
        for(std::size_t i=0;i<e.selected_tests.size();++i){if(i)out<<',';out<<quote(e.selected_tests[i]);}out<<"], \"coverage\": "<<repair_coverage_json(e.coverage)<<", \"targets\": [";
        for(std::size_t i=0;i<e.targets.size();++i){if(i)out<<',';const auto& t=e.targets[i];out<<"{\"file\": "<<quote(t.file)<<", \"line\": "<<t.line<<", \"status\": "<<quote(t.status)<<'}';}out<<"]}";
    }out<<"]}";return out.str();
}
}
