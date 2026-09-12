#include "codeguard/application.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace codeguard {
std::string utf8_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path from_utf8(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::uint64_t ScanResult::total_lines() const {
    std::uint64_t result = 0;
    for (const auto& file : files) result += file.lines;
    return result;
}
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
FileRecord read_file(const fs::path& path, const fs::path& root, const ScanContext& context) {
    FileRecord result;
    result.path = utf8_path(path.lexically_relative(root));
    const auto ext = lower(utf8_path(path.extension()));
    result.language = ext == ".c" ? "C" : (ext[1] == 'h' ? "header" : "C++");
    const auto before_size = fs::file_size(path);
    const auto before_time = fs::last_write_time(path);
    result.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(
        before_time.time_since_epoch()).count();
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open source file");
    std::uint64_t hash = 14695981039346656037ULL;
    std::array<char, 65536> buffer{};
    char last = '\0';
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        context.check();
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            const auto c = static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash ^= c;
            hash *= 1099511628211ULL;
            if (c == '\n') ++result.lines;
            last = static_cast<char>(c);
            ++result.size;
        }
    }
    if (input.bad()) throw std::runtime_error("source read failed");
    if (result.size != before_size || fs::file_size(path) != before_size ||
        fs::last_write_time(path) != before_time)
        throw std::runtime_error("file changed during scan; retry required");
    if (result.size && last != '\n') ++result.lines;
    std::ostringstream text;
    text << "fnv1a64-v1:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    result.hash = text.str();
    return result;
}
} // namespace

ScanResult scan_project(const fs::path& input, const ScanOptions& options) {
    options.context.report(options.scan_phase);
    const auto root = fs::canonical(input);
    for(const auto& item:options.ignored_paths)validate_relative_directory(item);
    if (!fs::is_directory(root)) throw std::invalid_argument("project root must be a directory");
    ScanResult result;
    result.root = utf8_path(root);
    result.scanned_at = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    std::set<std::string> ignored;
    for (const auto& name : options.ignored_directories) ignored.insert(lower(name));
    const std::set<std::string> extensions{ ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx" };
    // Explicit stack permits one unreadable directory to fail without aborting siblings.
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        options.context.check();
        auto directory = pending.back();
        pending.pop_back();
        std::error_code error;
        fs::directory_iterator it(directory, error), end;
        if (error) {
            result.diagnostics.push_back({utf8_path(directory), error.message()});
            continue;
        }
        for (; it != end; it.increment(error)) {
            options.context.check();
            if (error) break;
            const auto path = it->path();
            if(std::any_of(options.ignored_paths.begin(),options.ignored_paths.end(),[&](const auto& p){return project_path_inside(path,root/from_utf8(p));}))continue;
            try {
                if (fs::is_symlink(it->symlink_status())) continue;
                if (it->is_directory()) {
                    const auto name = lower(utf8_path(path.filename()));
                    if (!ignored.contains(name) && name.rfind("cmake-build-", 0) != 0)
                        pending.push_back(path);
                } else if (it->is_regular_file() && extensions.contains(lower(utf8_path(path.extension())))) {
                    result.files.push_back(read_file(path, root, options.context));
                    options.context.report(options.scan_phase, result.files.back().path, result.files.size());
                }
            } catch (const ScanCancelled&) { throw;
            } catch (const std::exception& exception) {
                result.diagnostics.push_back({utf8_path(path), exception.what()});
            }
        }
        if (error) result.diagnostics.push_back({utf8_path(directory), error.message()});
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    options.context.report(options.scan_phase, {}, result.files.size(), result.files.size());
    return result;
}
} // namespace codeguard
