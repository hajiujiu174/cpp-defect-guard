#pragma once

#include "codeguard/application.hpp"
#include <cwctype>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace codeguard::detail {
// MinGW's weakly_canonical may leave a directory junction unresolved. Preserve
// the existing containment semantics, but also detect alias/ancestor replacement
// with the identity of the nearest existing target, without relying on mtimes.
inline std::string exclusion_identity(const fs::path& path) {
#ifdef _WIN32
    auto probe=path;
    while(!probe.empty()) {
        const auto handle=CreateFileW(probe.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(handle!=INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};const bool ok=GetFileInformationByHandle(handle,&info);const auto error=GetLastError();CloseHandle(handle);
            if(!ok)throw fs::filesystem_error("cannot identify excluded path",probe,std::error_code(error,std::system_category()));
            return utf8_path(probe)+":"+std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
        }
        const auto error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
            throw fs::filesystem_error("cannot identify excluded path",probe,std::error_code(error,std::system_category()));
        const auto parent=probe.parent_path();if(parent==probe)break;probe=parent;
    }
    throw std::runtime_error("excluded path has no accessible ancestor");
#else
    return {};
#endif
}
// Both operands have already been resolved for the current operation. Compare
// components, never string prefixes (foo must not exclude foobar).
inline bool resolved_path_inside(const fs::path& path, const fs::path& root) {
    auto a=path.begin();
    for(auto b=root.begin();b!=root.end();++b,++a) {
        if(a==path.end())return false;
#ifdef _WIN32
        auto x=a->wstring(),y=b->wstring();
        for(auto& c:x)c=std::towlower(c);
        for(auto& c:y)c=std::towlower(c);
        if(x!=y)return false;
#else
        if(*a!=*b)return false;
#endif
    }
    return true;
}

// Short-lived per traversal: resolve each configured exclusion once and each
// candidate at most once, rather than doing two filesystem walks per pair.
class PathExclusions {
public:
    PathExclusions(const fs::path& root,const std::vector<std::string>& paths) {
        for(const auto& path:paths) {
            validate_relative_directory(path);
            const auto original=root/from_utf8(path);
            roots_.push_back({original,fs::weakly_canonical(original),exclusion_identity(original)});
        }
    }
    bool contains(const fs::path& path) const {
        if(roots_.empty())return false;
        const auto resolved=fs::weakly_canonical(path);
        for(const auto& entry:roots_)
            if(resolved_path_inside(resolved,entry.resolved))return true;
        return false;
    }
    // Do not accept a successful inventory/copy after an exclusion alias was
    // retargeted. This is a boundary check, not an atomic filesystem snapshot.
    void check_unchanged() const {
        for(const auto& entry:roots_) {
            const auto now=fs::weakly_canonical(entry.original);
            if(!resolved_path_inside(now,entry.resolved)||!resolved_path_inside(entry.resolved,now)||exclusion_identity(entry.original)!=entry.identity)
                throw std::runtime_error("excluded path resolution changed during traversal; retry required");
        }
    }
private:
    struct Root {fs::path original,resolved;std::string identity;};
    std::vector<Root> roots_;
};
} // namespace codeguard::detail
