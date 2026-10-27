#include "codeguard/application.hpp"
#include "../../core/project/path_filter.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace cg=codeguard;
namespace fs=std::filesystem;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"expected rejection");}
bool make_alias(const fs::path& target,const fs::path& alias) {
    std::error_code error;fs::create_directory_symlink(target,alias,error);
    if(!error)return true;
#ifdef _WIN32
    // Directory junctions exercise real Windows reparse resolution without
    // changing privilege or Developer Mode when symbolic links are denied.
    struct MountPoint {DWORD tag;WORD length,reserved;WORD substitute_offset,substitute_length,print_offset,print_length;wchar_t paths[4096];} data{};
    const auto print=fs::absolute(target).native(),substitute=L"\\??\\"+print;
    if(substitute.size()+print.size()+2>4096)return false;
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
    data.print_offset=static_cast<WORD>((substitute.size()+1)*sizeof(wchar_t));
    data.print_length=static_cast<WORD>(print.size()*sizeof(wchar_t));
    data.length=static_cast<WORD>(8+(substitute.size()+print.size()+2)*sizeof(wchar_t));
    std::copy(substitute.begin(),substitute.end(),data.paths);
    std::copy(print.begin(),print.end(),data.paths+substitute.size()+1);
    if(!fs::create_directory(alias,error)||error)return false;
    const auto handle=CreateFileW(alias.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    DWORD count=0;const bool ok=handle!=INVALID_HANDLE_VALUE&&DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,8+data.length,nullptr,0,&count,nullptr);
    if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
    if(!ok)fs::remove(alias,error);else std::cout<<"alias_kind=junction\n";
    return ok;
#else
    return false;
#endif
}
struct Fixture {
    fs::path directory=fs::current_path()/("exclusion-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root=directory/fs::path(u8"中文 source");
    Fixture(){fs::create_directories(root);}
    ~Fixture(){std::error_code error;fs::remove_all(directory,error);}
    void write(const std::string& file){const auto p=root/cg::from_utf8(file);fs::create_directories(p.parent_path());std::ofstream out(p);out<<"int a;\n";check(bool(out),"write");}
};
int main(int argc,char** argv){try{
    check(argc==2,"case required");const std::string name=argv[1];Fixture f;
    if(name=="scope") {
        for(const auto& path:{"skip/a.c","skip-neighbor/a.c","keep.c","one.c","one.cpp","nested/one.c","nested/two.c"})f.write(path);
        cg::ScanOptions o;o.ignored_paths={"skip","one.c","nested/one.c","absent/deeper","skip"};
        const auto scan=cg::scan_project(f.root,o);check(scan.diagnostics.empty(),"scan diagnostics");
        std::vector<std::string> paths;for(const auto& file:scan.files)paths.push_back(file.path);
        check(paths==std::vector<std::string>{"keep.c","nested/two.c","one.cpp","skip-neighbor/a.c"},"exact directory/file and prefix scope");
        cg::detail::PathExclusions filter(f.root,o.ignored_paths);
        for(const auto& entry:fs::recursive_directory_iterator(f.root)) {
            const auto expected=std::any_of(o.ignored_paths.begin(),o.ignored_paths.end(),[&](const auto& p){return cg::project_path_inside(entry.path(),f.root/cg::from_utf8(p));});
            check(filter.contains(entry.path())==expected,"resolved filter agrees with uncached public predicate");
        }
        filter.check_unchanged();
#ifdef _WIN32
        cg::ScanOptions mixed;mixed.ignored_paths={"SKIP","ONE.C"};
        check(cg::scan_project(f.root,mixed).files.size()==5,"Windows exclusion case insensitive");
#endif
    } else if(name=="invalid") {
        f.write("a.c");
        for(const auto& value:{"../outside",".git","nested/../a.c",".",""}){cg::ScanOptions o;o.ignored_paths={value};rejects([&]{cg::scan_project(f.root,o);});}
    } else if(name=="cancel") {
        f.write("a.c");f.write("b.c");cg::ScanOptions o;o.ignored_paths={"missing"};o.context.control=std::make_shared<cg::ScanControl>();
        o.context.progress=[&](const cg::ScanProgress& p){if(p.completed)o.context.control->request_cancel();};
        bool cancelled=false;try{cg::scan_project(f.root,o);}catch(const cg::ScanCancelled&){cancelled=true;}
        check(cancelled,"cancelled inventory must not succeed");
    } else if(name=="replacement") {
#ifdef _WIN32
        f.write("skip/a.c");cg::detail::PathExclusions existing(f.root,{"skip"});
        fs::rename(f.root/"skip",f.root/"previous");fs::create_directory(f.root/"skip");
        rejects([&]{existing.check_unchanged();});
        cg::detail::PathExclusions missing(f.root,{"absent/deeper"});missing.check_unchanged();
        fs::create_directory(f.root/"absent");rejects([&]{missing.check_unchanged();});
#else
        std::cout<<"SKIP: Windows native file identity check\n";return 77;
#endif
    } else if(name=="alias") {
        f.write("a/a.c");f.write("b/b.c");const auto alias=f.root/"alias";
        if(!make_alias(f.root/"a",alias)){std::cout<<"SKIP: directory aliases unavailable\n";return 77;}
        cg::detail::PathExclusions filter(f.root,{"alias"});
        const auto resolves=cg::project_path_inside(f.root/"a/a.c",alias);
        check(filter.contains(f.root/"a/a.c")==resolves&&!filter.contains(f.root/"b/b.c")&&filter.contains(alias),"alias agrees with uncached containment");
        cg::ScanOptions o;o.ignored_paths={"alias"};const auto scan=cg::scan_project(f.root,o);
        check(scan.files.size()==(resolves?1:2),"same target scope and excluded alias not traversed");
        fs::remove(alias);check(make_alias(f.root/"b",alias),"retarget alias");rejects([&]{filter.check_unchanged();});
        // Changing resolution through the progress callback cannot yield success.
        bool changed=false;
        o.context.progress=[&](const cg::ScanProgress& p){if(p.completed&&!changed){fs::remove(alias);check(make_alias(f.root/"a",alias),"callback retarget alias");changed=true;}};
        rejects([&]{cg::scan_project(f.root,o);});
    } else throw std::invalid_argument("unknown case");
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
