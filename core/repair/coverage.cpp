#include "coverage_internal.hpp"
#include <algorithm>
#include <sqlite3.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codeguard {
namespace {
void save(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary);out.write(text.data(),static_cast<std::streamsize>(text.size()));
    if(!out)throw std::runtime_error("cannot write coverage record: "+utf8_path(path));
}
std::string quote(const std::string& text) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:text) {
        if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);
        else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;
        else out<<static_cast<char>(c);
    }
    return out.str()+'"';
}
std::string trim(std::string s) {
    const auto begin=s.find_first_not_of(" \t\r");if(begin==std::string::npos)return {};
    return s.substr(begin,s.find_last_not_of(" \t\r")-begin+1);
}
fs::path runtime_alias(const fs::path& path) {
#ifdef _WIN32
    const auto bytes=utf8_path(path);const auto length=MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    if(length<=0)return path;std::wstring value(static_cast<std::size_t>(length),L'\0');
    if(MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),value.data(),length)!=length)return path;
    return fs::path(value);
#else
    return path;
#endif
}
std::string signature(const std::vector<CoverageFile>& files) {
    std::uint64_t hash=14695981039346656037ULL;
    for(const auto& f:files)for(const auto& [line,hit]:f.lines){(void)hit;const auto text=f.file+':'+std::to_string(line)+'\n';for(unsigned char c:text){hash^=c;hash*=1099511628211ULL;}}
    std::ostringstream out;out<<"gcov-project-lines-v1:"<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return out.str();
}
std::vector<fs::path> profile_files(const fs::path& root,const std::string& extension,const BuildOptions& options) {
    std::vector<fs::path> result;std::size_t count=0;
    if(!fs::is_directory(root))return result;
    for(fs::recursive_directory_iterator it(root),end;it!=end;++it) {
        if(options.control)options.control->check();
        if(++count>100000)throw std::runtime_error("coverage traversal exceeds 100000 entries");
        if(it->is_symlink()){it.disable_recursion_pending();continue;}
        if(it->is_regular_file()&&it->path().extension()==extension) {
            if(!project_path_inside(it->path(),root))throw std::runtime_error("coverage profile escapes build directory");
            if(result.size()==512)throw std::runtime_error("coverage exceeds 512 object profiles");
            result.push_back(it->path());
        }
    }
    std::sort(result.begin(),result.end());return result;
}
void parse(const std::string& text,const fs::path& source,const std::set<std::string>& inventory,
    std::map<std::string,CoverageFile>& files,const std::shared_ptr<ScanControl>& control) {
    sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("cannot initialize coverage JSON reader");}
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw,sqlite3_close);
    auto statement=[&](const char* sql){
        sqlite3_stmt* raw_statement=nullptr;
        if(sqlite3_prepare_v2(raw,sql,-1,&raw_statement,nullptr)!=SQLITE_OK)throw std::runtime_error("coverage requires SQLite JSON support");
        auto result=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>(raw_statement,sqlite3_finalize);
        if(sqlite3_bind_text(raw_statement,1,text.data(),static_cast<int>(text.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("cannot bind coverage JSON");
        return result;
    };
    auto header=statement("SELECT json_valid(?1),json_extract(?1,'$.format_version'),json_type(?1,'$.files')");
    if(sqlite3_step(header.get())!=SQLITE_ROW||sqlite3_column_int(header.get(),0)!=1||
        !sqlite3_column_text(header.get(),1)||std::string(reinterpret_cast<const char*>(sqlite3_column_text(header.get(),1)))!="2"||
        !sqlite3_column_text(header.get(),2)||std::string(reinterpret_cast<const char*>(sqlite3_column_text(header.get(),2)))!="array")
        throw std::runtime_error("gcov JSON format 2 with a files array is required");
    auto rows=statement(R"sql(
        SELECT json_extract(f.value,'$.file'),json_extract(l.value,'$.line_number'),json_extract(l.value,'$.count'),
          json_type(l.value,'$.branches'),
          (SELECT count(*) FROM json_each(l.value,'$.branches')),
          (SELECT count(*) FROM json_each(l.value,'$.branches') b WHERE json_extract(b.value,'$.count')>0),
          (SELECT count(*) FROM json_each(l.value,'$.branches') b WHERE json_type(b.value,'$.count') IS NOT 'integer' OR json_extract(b.value,'$.count')<0)
        FROM json_each(?1,'$.files') f,json_each(f.value,'$.lines') l
    )sql");
    int code=SQLITE_OK;std::size_t scanned=0;std::string previous,relative;
    while((code=sqlite3_step(rows.get()))==SQLITE_ROW) {
        if(++scanned%1024==0&&control)control->check();
        if(sqlite3_column_type(rows.get(),0)!=SQLITE_TEXT||sqlite3_column_type(rows.get(),1)!=SQLITE_INTEGER||
            sqlite3_column_type(rows.get(),2)!=SQLITE_INTEGER||sqlite3_column_int64(rows.get(),1)<1||
            sqlite3_column_int64(rows.get(),1)>2147483647||sqlite3_column_int64(rows.get(),2)<0||
            !sqlite3_column_text(rows.get(),3)||std::string(reinterpret_cast<const char*>(sqlite3_column_text(rows.get(),3)))!="array"||
            sqlite3_column_int64(rows.get(),6)!=0)throw std::runtime_error("invalid gcov line/branch data");
        const std::string name=reinterpret_cast<const char*>(sqlite3_column_text(rows.get(),0));
        if(name!=previous){previous=name;relative.clear();const auto path=from_utf8(name);
            if(path.is_absolute()&&fs::is_regular_file(path)&&project_path_inside(path,source)){
                const auto candidate=utf8_path(fs::weakly_canonical(path).lexically_relative(fs::canonical(source)));
                if(inventory.contains(candidate))relative=candidate;
            }
        }
        if(relative.empty())continue;
        auto& file=files[relative];file.file=relative;auto& hit=file.lines[sqlite3_column_int(rows.get(),1)];
        hit=hit||sqlite3_column_int64(rows.get(),2)>0;
        file.branch_records+=static_cast<std::uint64_t>(sqlite3_column_int64(rows.get(),4));
        file.branch_records_taken+=static_cast<std::uint64_t>(sqlite3_column_int64(rows.get(),5));
    }
    if(code!=SQLITE_DONE)throw std::runtime_error("malformed gcov JSON: "+std::string(sqlite3_errmsg(raw)));
}
}
void prepare_repair_coverage(BuildOptions& options,const fs::path& directory) {
    if(options.gcov.empty())throw std::invalid_argument("coverage requires a gcov executable");
    const auto tool=from_utf8(options.gcov);
    if(tool.has_parent_path()&&!tool.is_absolute())throw std::invalid_argument("gcov requires a PATH command or absolute executable path");
#ifdef _WIN32
    const auto output_path=utf8_path(fs::absolute(directory));
    if(std::any_of(output_path.begin(),output_path.end(),[](unsigned char c){return c>=128;}))
        throw std::invalid_argument("GCC gcov counters on Windows require an ASCII output directory; source file names may be Unicode");
#endif
    for(const auto& definition:options.cmake_definitions)if(definition.substr(0,definition.find_first_of(":="))=="CMAKE_PROJECT_INCLUDE")
        throw std::invalid_argument("coverage cannot override an existing CMAKE_PROJECT_INCLUDE hook");
    fs::create_directories(directory);const auto inject=fs::absolute(directory/"instrument.cmake");
    save(inject,R"cmake(if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  foreach(lang C CXX)
    if(CMAKE_${lang}_COMPILER_LOADED AND NOT CMAKE_${lang}_COMPILER_ID STREQUAL "GNU")
      message(FATAL_ERROR "CodeGuard gcov coverage requires GNU C/C++ compilers")
    endif()
  endforeach()
  add_compile_options(--coverage -O0 -fprofile-abs-path -fprofile-update=atomic)
  add_link_options(--coverage)
endif()
)cmake");
    options.cmake_definitions.push_back("CMAKE_PROJECT_INCLUDE="+utf8_path(inject));
    const auto previous=options.before_test;
    options.before_test=[previous,options](const fs::path& work) {
        if(previous)previous(work);
        reset_repair_counters(work,work/"pre-test-profile",options);
    };
}
void reset_repair_counters(const fs::path& work,const fs::path& backup,const BuildOptions& options){
    const auto build=work/"build";
    if(fs::is_symlink(build)||!project_path_inside(build,work)||project_path_inside(backup,build)||(!project_path_inside(backup,work)&&!project_path_inside(backup,work.parent_path().parent_path()/"test-coverage")))throw std::runtime_error("coverage counter paths escape validation workspace");
    const auto files=profile_files(build,".gcda",options);std::ostringstream log;
    for(const auto& path:files){const auto relative=path.lexically_relative(build),destination=backup/relative;fs::create_directories(destination.parent_path());if(fs::exists(destination))throw std::runtime_error("counter backup already exists");fs::rename(path,destination);log<<utf8_path(relative)<<'\n';}
    fs::create_directories(backup);save(backup/"reset.txt",log.str());save(work/"coverage-reset.txt","Moved pre-CTest counters into "+utf8_path(backup)+"; original project files not removed.\n"+log.str());
}
RepairCoverage collect_repair_coverage(const BuildRun& run,const RepairCandidate& candidate,
    const ValidationInputs& snapshot,const BuildOptions& options,const fs::path& directory) {
    RepairCoverage result;result.status="failed";result.tool=options.gcov;result.workspace=utf8_path(directory);
    fs::create_directories(directory);
    try {
        const auto work=from_utf8(run.workspace),build=work/"build",source=work/"source";
        if(run.workspace.empty()||!fs::exists(work/"coverage-reset.txt"))throw std::runtime_error("coverage requires a completed pre-CTest counter reset");
        if(fs::is_symlink(build)||!project_path_inside(build,work))throw std::runtime_error("coverage build directory escapes its workspace");
        const auto version=run_process({{options.gcov,"--version"},utf8_path(build),options.timeout,65536,options.control});
        save(directory/"version.log",version.stdout_text+version.stderr_text);
        if(version.status=="cancelled")throw ScanCancelled();
        if(version.status!="passed"||version.output_truncated)throw std::runtime_error("cannot read gcov version");
        result.tool_version=trim(version.stdout_text.substr(0,version.stdout_text.find('\n')));
        const auto notes=profile_files(build,".gcno",options);result.notes_files=notes.size();result.data_files=profile_files(build,".gcda",options).size();
        if(notes.empty())throw std::runtime_error("no gcov notes found; selected targets may not be instrumented");
        std::set<std::string> inventory;for(const auto& file:snapshot.files)inventory.insert(file.path);
        std::map<std::string,CoverageFile> files;std::size_t output_bytes=0;std::uintmax_t profile_bytes=0;
        const auto profiles=directory/"profiles";fs::create_directory(profiles);std::ostringstream mappings;mappings<<'[';
        for(std::size_t n=0;n<notes.size();++n) {
            if(options.control)options.control->check();
            auto native=notes[n];native.replace_extension(".gcda");const auto alias=runtime_alias(native);
            const bool native_found=fs::is_regular_file(native),alias_found=fs::is_regular_file(alias);
            if(native_found&&alias_found&&!fs::equivalent(native,alias))throw std::runtime_error("ambiguous native and runtime-encoded coverage data paths");
            const auto data=native_found?native:alias;const bool found=native_found||alias_found;
            if(found&&!project_path_inside(data,build))throw std::runtime_error("coverage data escapes build directory");
            profile_bytes+=fs::file_size(notes[n])+(found?fs::file_size(data):0);
            if(profile_bytes>512ULL*1024*1024)throw std::runtime_error("coverage profiles exceed 512 MiB");
            auto staged=profiles/("object-"+std::to_string(n)+".gcno");fs::copy_file(notes[n],staged);
            if(found){auto destination=staged;destination.replace_extension(".gcda");fs::copy_file(data,destination);}
            if(n)mappings<<',';mappings<<"{\"notes\": "<<quote(utf8_path(notes[n]))<<", \"data\": "<<quote(found?utf8_path(data):"")
                <<", \"encoding\": "<<quote(!found?"missing":native_found?"native":"utf8_bytes_as_system_codepage")<<", \"staged\": "<<quote(utf8_path(staged))<<'}';
            auto process=run_process({{options.gcov,"--json-format","--stdout","--branch-probabilities","--branch-counts",utf8_path(staged)},utf8_path(build),options.timeout,32*1024*1024,options.control});
            save(directory/("object-"+std::to_string(n)+".log"),utf8_path(notes[n])+"\ndata_source="+(found?utf8_path(data):"missing")+"\nstatus="+process.status+"\nstdout:\n"+process.stdout_text+"\nstderr:\n"+process.stderr_text);
            save(directory/("object-"+std::to_string(n)+".json"),process.stdout_text);
            if(process.status=="cancelled")throw ScanCancelled();
            if(process.status!="passed"||process.output_truncated)throw std::runtime_error("gcov failed or output was truncated; inspect object-"+std::to_string(n)+".log");
            output_bytes+=process.stdout_text.size();if(output_bytes>128*1024*1024)throw std::runtime_error("coverage output exceeds 128 MiB");
            parse(process.stdout_text,source,inventory,files,options.control);
        }
        mappings<<']';save(directory/"profiles.json",mappings.str());
        for(auto& [name,file]:files)if(!file.lines.empty()) {
            result.lines_total+=file.lines.size();for(const auto& [line,hit]:file.lines){(void)line;result.lines_hit+=hit;}
            result.branch_records+=file.branch_records;result.branch_records_taken+=file.branch_records_taken;
            result.files.push_back(std::move(file));inventory.erase(name);
        }
        result.unmeasured_files.assign(inventory.begin(),inventory.end());
        if(result.lines_total==0)throw std::runtime_error("gcov reported no instrumented project lines");
        result.scope_signature=signature(result.files);result.status="collected";result.target_line_status="not_instrumented";
        for(const auto& file:result.files)if(file.file==candidate.edit.file)if(auto line=file.lines.find(candidate.edit.line);line!=file.lines.end())
            result.target_line_status=line->second?"recorded":"no_recorded_execution";
    }catch(const ScanCancelled& e){result.status="cancelled";result.error=e.what();}
    catch(const std::exception& e){result.status="failed";result.error=e.what();}
    save(directory/"summary.json",repair_coverage_json(result));return result;
}
std::string repair_coverage_json(const RepairCoverage& c) {
    std::ostringstream out;out<<"{\"status\": "<<quote(c.status)<<", \"error\": "<<quote(c.error)
        <<", \"tool\": "<<quote(c.tool)<<", \"tool_version\": "<<quote(c.tool_version)<<", \"workspace\": "<<quote(c.workspace)<<", \"scope_signature\": "<<quote(c.scope_signature)
        <<", \"target_line_status\": "<<quote(c.target_line_status)<<", \"lines_total\": "<<c.lines_total<<", \"lines_hit\": "<<c.lines_hit
        <<", \"branch_records\": "<<c.branch_records<<", \"branch_records_taken\": "<<c.branch_records_taken
        <<", \"notes_files\": "<<c.notes_files<<", \"data_files\": "<<c.data_files
        <<", \"scope\": \"Instrumented project lines only; headers unioned by file/line; branch records counted per object and emission; uninstrumented files excluded from denominator\", \"files\": [";
    for(std::size_t n=0;n<c.files.size();++n){if(n)out<<',';const auto& f=c.files[n];out<<"{\"file\": "<<quote(f.file)<<", \"branch_records\": "<<f.branch_records<<", \"branch_records_taken\": "<<f.branch_records_taken<<", \"lines\": [";
        std::size_t i=0;for(const auto& [line,hit]:f.lines){if(i++)out<<',';out<<"{\"line\": "<<line<<", \"recorded\": "<<(hit?"true":"false")<<'}';}out<<"]}";}
    out<<"], \"unmeasured_files\": [";for(std::size_t n=0;n<c.unmeasured_files.size();++n){if(n)out<<',';out<<quote(c.unmeasured_files[n]);}out<<"]}";return out.str();
}
}
