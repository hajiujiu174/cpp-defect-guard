#include "codeguard/application.hpp"
#include "codeguard/configuration.hpp"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <regex>
#include <set>
#include <cwctype>

namespace codeguard {
ScanOptions configured_scan_options(const fs::path& root,const ProjectConfig& config) {
    validate_config(config);auto effective=config;ScanOptions options;
    if(effective.analysis_enabled) {
        if(effective.compile_commands.empty()&&effective.auto_discover&&clang_analysis_available()) {
            const auto candidates=discover_compilation_databases(root);
            if(candidates.size()>1)throw std::invalid_argument("multiple compilation databases found; choose one in project settings");
            if(candidates.size()==1)effective.compile_commands=candidates.front();
        }
        options.compile_commands=effective.compile_commands;options.command_choices=effective.command_choices;
    }
    options.threads=effective.threads;options.disabled_rules=effective.disabled_rules;options.ignored_paths=effective.build.copy_excludes;
    options.configuration=encode_config(effective);return options;
}
bool project_path_inside(const fs::path& path, const fs::path& root) {
    const auto p=fs::weakly_canonical(path), r=fs::weakly_canonical(root);
    auto a=p.begin();
    for(auto b=r.begin();b!=r.end();++b,++a) {
        if(a==p.end())return false;
#ifdef _WIN32
        auto x=a->wstring(),y=b->wstring();for(auto& c:x)c=std::towlower(c);for(auto& c:y)c=std::towlower(c);
        if(x!=y)return false;
#else
        if(*a!=*b)return false;
#endif
    }
    return true;
}
void validate_relative_directory(const std::string& value) {
    const auto p=from_utf8(value);
    if(value.empty()||value.find('\0')!=std::string::npos||p.has_root_path())throw std::invalid_argument("directory must be project-relative: "+value);
    for(const auto& part:p) {
        auto s=utf8_path(part);std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});
        if(s==".."||s=="."||s==".git")throw std::invalid_argument("directory cannot contain dot segments or .git: "+value);
    }
}
void validate_config(const ProjectConfig& c) {
    if(c.threads>64||c.build.jobs>64||c.build.timeout.count()<1||c.build.timeout>std::chrono::hours(24))throw std::invalid_argument("threads/jobs must be 0..64; timeout must be positive and <= 24h");
    if(c.build.target.starts_with('-')||c.build.cmake.empty()||c.build.ctest.empty()||c.build.git.empty()||c.build.generator.empty())throw std::invalid_argument("invalid target or empty build tool/generator");
    for(const auto& value:{c.build.cmake,c.build.ctest,c.build.git,c.build.c_compiler,c.build.cxx_compiler}) {
        const auto path=from_utf8(value);
        if(path.has_parent_path()&&!path.is_absolute())throw std::invalid_argument("build tools/compilers require a command name in PATH or an absolute path: "+value);
    }
    for(const auto* list:{&c.build.cmake_definitions,&c.build.copy_includes,&c.build.copy_excludes,&c.disabled_rules})
        if(list->size()>10000)throw std::invalid_argument("configuration list exceeds 10000 entries");
    if(c.command_choices.size()>10000)throw std::invalid_argument("too many command selections");
    if(c.build.build_type!="Debug"&&c.build.build_type!="Release"&&c.build.build_type!="RelWithDebInfo"&&c.build.build_type!="MinSizeRel")throw std::invalid_argument("unsupported CMake build type");
    for(const auto& item:c.build.cmake_definitions) {
        const auto equal=item.find('=');
        if(equal==std::string::npos||!std::regex_match(item.substr(0,equal),std::regex("[A-Za-z_][A-Za-z0-9_]*(:[A-Za-z]+)?")))throw std::invalid_argument("CMake definition must be KEY[:TYPE]=VALUE: "+item);
        const auto name=item.substr(0,item.find_first_of(":="));
        if(name=="CMAKE_BUILD_TYPE"||name=="CMAKE_C_COMPILER"||name=="CMAKE_CXX_COMPILER"||name=="CMAKE_EXPORT_COMPILE_COMMANDS")throw std::invalid_argument("use dedicated setting instead of overriding "+name);
    }
    for(const auto& item:c.build.copy_includes)validate_relative_directory(item);
    for(const auto& item:c.build.copy_excludes)validate_relative_directory(item);
    for(const auto& id:c.disabled_rules) {
        const auto& catalog=rule_catalog();
        if(std::none_of(catalog.begin(),catalog.end(),[&](const auto& r){return r.id==id;}))throw std::invalid_argument("unknown rule: "+id);
    }
    for(const auto& [file,id]:c.command_choices)if(file.empty()||!std::regex_match(id,std::regex("[0-9a-f]{64}")))throw std::invalid_argument("invalid compile command selection");
}
std::string encode_config(const ProjectConfig& c) {
    validate_config(c);std::ostringstream out;
    out<<"CodeGuardProjectConfig 1\n"<<c.analysis_enabled<<' '<<c.auto_discover<<' '<<c.threads<<' '<<c.build.jobs<<' '<<c.build.timeout.count()<<'\n';
    for(const auto& s:std::vector<std::string>{c.compile_commands,utf8_path(c.build.output_directory),c.build.cmake,c.build.ctest,c.build.git,c.build.generator,c.build.c_compiler,c.build.cxx_compiler,c.build.target,c.build.build_type})out<<std::quoted(s)<<'\n';
    for(const auto* list:{&c.build.cmake_definitions,&c.build.copy_includes,&c.build.copy_excludes,&c.disabled_rules}) {
        out<<list->size()<<'\n';for(const auto& s:*list)out<<std::quoted(s)<<'\n';
    }
    out<<c.command_choices.size()<<'\n';for(const auto& [file,id]:c.command_choices)out<<std::quoted(file)<<' '<<std::quoted(id)<<'\n';
    const auto payload=out.str();if(payload.size()>1024*1024||payload.find('\0')!=std::string::npos)throw std::invalid_argument("project configuration is too large or contains NUL");
    return payload;
}
ProjectConfig decode_config(const std::string& payload) {
    if(payload.size()>1024*1024||payload.find('\0')!=std::string::npos)throw std::invalid_argument("invalid project configuration size or NUL");
    std::istringstream in(payload);std::string magic;int version=0;in>>magic>>version;
    if(magic!="CodeGuardProjectConfig"||version!=1)throw std::invalid_argument("unsupported project configuration version; existing settings preserved");
    ProjectConfig c;int enabled=-1,discover=-1;std::int64_t timeout=0;
    in>>enabled>>discover>>c.threads>>c.build.jobs>>timeout;
    if((enabled!=0&&enabled!=1)||(discover!=0&&discover!=1))throw std::invalid_argument("invalid configuration switches");
    c.analysis_enabled=enabled;c.auto_discover=discover;c.build.timeout=std::chrono::milliseconds(timeout);
    std::string output;
    for(auto* s:{&c.compile_commands,&output,&c.build.cmake,&c.build.ctest,&c.build.git,&c.build.generator,&c.build.c_compiler,&c.build.cxx_compiler,&c.build.target,&c.build.build_type})in>>std::quoted(*s);
    c.build.output_directory=from_utf8(output);
    auto count=[&]{std::size_t n=10001;in>>n;if(!in||n>10000)throw std::invalid_argument("invalid configuration list");return n;};
    for(auto* list:{&c.build.cmake_definitions,&c.build.copy_includes,&c.build.copy_excludes,&c.disabled_rules})for(auto n=count();n;--n){std::string s;in>>std::quoted(s);list->push_back(s);}
    for(auto n=count();n;--n){std::string file,id;in>>std::quoted(file)>>std::quoted(id);if(!c.command_choices.emplace(file,id).second)throw std::invalid_argument("duplicate command selection");}
    if(!in)throw std::invalid_argument("truncated project configuration");in>>std::ws;
    if(!in.eof())throw std::invalid_argument("unexpected trailing configuration data");
    validate_config(c);return c;
}
namespace {
std::string config_key(const fs::path& root) {
#ifdef _WIN32
    auto path=fs::canonical(root).wstring();for(auto& c:path)c=std::towlower(c);return utf8_path(fs::path(path));
#else
    return utf8_path(fs::canonical(root));
#endif
}
}
std::optional<ProjectConfig> load_project_config(const fs::path& root,const fs::path& database) {
    if(project_path_inside(database,root))throw std::invalid_argument("configuration database must be outside project");
    if(!fs::exists(database))return {};
    SqliteDatabase db(database,true);return db.configuration(config_key(root));
}
void save_project_config(const fs::path& root,const fs::path& database,const ProjectConfig& config) {
    if(project_path_inside(database,root))throw std::invalid_argument("configuration database must be outside project");
    validate_config(config);
    if(!config.build.output_directory.empty()&&project_path_inside(config.build.output_directory,root))throw std::invalid_argument("build workspace must be outside project");
    auto stored=config;
    if(!stored.compile_commands.empty())stored.compile_commands=utf8_path(fs::absolute(from_utf8(stored.compile_commands)));
    if(!stored.build.output_directory.empty())stored.build.output_directory=fs::absolute(stored.build.output_directory);
    SqliteDatabase db(database);db.save_configuration(config_key(root),stored);
}
std::vector<std::string> discover_compilation_databases(const fs::path& input) {
    const auto root=fs::canonical(input);std::vector<std::pair<fs::path,int>> pending{{root,0}};std::set<std::string> found;
    std::size_t visited=0;
    while(!pending.empty()) {
        const auto [dir,depth]=pending.back();pending.pop_back();
        if(++visited>2048)throw std::runtime_error("compile database discovery exceeded 2048 directories; select a path manually");
        std::error_code error;
        for(fs::directory_iterator it(dir,error),end;!error&&it!=end;it.increment(error)) {
            if(it->is_symlink(error))continue;
            if(it->is_regular_file(error)&&it->path().filename()=="compile_commands.json")found.insert(utf8_path(fs::canonical(it->path())));
            if(depth<3&&it->is_directory(error)) {
                auto name=utf8_path(it->path().filename());
                if(name!=".git"&&name!=".venv"&&name!=".venv-ml"&&name!="node_modules")pending.push_back({it->path(),depth+1});
            }
        }
        if(error)throw std::runtime_error("cannot inspect "+utf8_path(dir)+": "+error.message());
    }
    return {found.begin(),found.end()};
}
} // namespace codeguard
