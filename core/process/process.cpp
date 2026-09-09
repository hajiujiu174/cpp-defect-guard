#include "codeguard/process.hpp"
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#endif

namespace codeguard {
namespace {
using Clock = std::chrono::steady_clock;
void append(ProcessResult& result, std::string& out, const char* data, std::size_t size, std::size_t limit) {
    const auto remaining = limit - std::min(limit, out.size());
    out.append(data, std::min(size, remaining));
    if (size > remaining) result.output_truncated = true;
}
bool cancelled(const ProcessOptions& options) {
    return options.control && options.control->state() == ScanControl::State::cancel_requested;
}
#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    Handle(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE v = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = v; }
};
std::wstring wide(const std::string& input) {
    if (input.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!size) throw std::invalid_argument("process argument is not valid UTF-8");
    std::wstring out(size, L' ');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), out.data(), size);
    return out;
}
std::wstring quote(const std::wstring& input) {
    std::wstring out = L"\""; std::size_t slashes = 0;
    for (const auto c : input) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
void drain(HANDLE pipe, ProcessResult& result, std::string& output, std::size_t limit) {
    char buffer[8192];
    for (int i = 0; i < 16; ++i) {
        DWORD available = 0, read = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
        if (!ReadFile(pipe, buffer, std::min<DWORD>(sizeof(buffer), available), &read, nullptr) || !read) break;
        append(result, output, buffer, read, limit);
    }
}
#else
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
    void reset(int v = -1) { if (value >= 0) close(value); value = v; }
};
void drain(int pipe, ProcessResult& result, std::string& output, std::size_t limit) {
    char buffer[8192];
    for (int i = 0; i < 16; ++i) {
        const auto n = read(pipe, buffer, sizeof(buffer));
        if (n > 0) append(result, output, buffer, static_cast<std::size_t>(n), limit);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}
#endif
}
std::string format_arguments(const std::vector<std::string>& args) {
    std::ostringstream text;
    for (const auto& arg : args) { if (text.tellp() > 0) text << ' '; text << std::quoted(arg); }
    return text.str();
}
std::string executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length||length>=path.size()) throw std::runtime_error("cannot resolve executable path");
    const auto value=std::filesystem::path(std::wstring(path.data(),length)).parent_path().u8string();
#else
    std::vector<char> path(65536);const auto length=readlink("/proc/self/exe",path.data(),path.size());
    if(length<0||static_cast<std::size_t>(length)>=path.size())throw std::runtime_error("cannot resolve executable path");
    const auto value=std::filesystem::path(std::string(path.data(),length)).parent_path().u8string();
#endif
    return {value.begin(),value.end()};
}
ProcessResult run_process(const ProcessOptions& options) {
    if (options.arguments.empty() || options.arguments[0].empty() || options.timeout.count() <= 0)
        throw std::invalid_argument("process requires executable and positive timeout");
    for (const auto& arg : options.arguments) if (arg.find('\0') != std::string::npos) throw std::invalid_argument("NUL in process argument");
    if (options.working_directory.find('\0') != std::string::npos) throw std::invalid_argument("NUL in working directory");
    ProcessResult result; const auto start = Clock::now();
    if (cancelled(options)) { result.status = "cancelled"; return result; }
#ifdef _WIN32
    Handle out_read, out_write, err_read, err_write;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto pipe = [&](Handle& reader, Handle& writer) {
        if (!CreatePipe(&reader.value, &writer.value, &security, 0) || !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("CreatePipe failed");
    };
    pipe(out_read, out_write); pipe(err_read, err_write);
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("open NUL failed");
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("create process job failed");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = out_write.value; startup.StartupInfo.hStdError = err_write.value;
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) throw std::runtime_error("process attributes failed");
    struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{startup.lpAttributeList};
    HANDLE inherited[] = {input.value, out_write.value, err_write.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error("process handle list failed");
    std::wstring executable = wide(options.arguments[0]);
    if (executable.find_first_of(L"/\\") == std::wstring::npos) {
        std::vector<wchar_t> resolved(32768);
        const DWORD length = SearchPathW(nullptr, executable.c_str(), L".exe", static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        if (!length || length >= resolved.size()) { result.stderr_text = "Executable not found: " + options.arguments[0]; return result; }
        executable.assign(resolved.data(), length);
    }
    std::wstring command;
    for (const auto& arg : options.arguments) { if (!command.empty()) command += L' '; command += quote(wide(arg)); }
    const auto directory = wide(options.working_directory);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info)) {
        result.stderr_text = "CreateProcess failed: " + std::to_string(GetLastError()); return result;
    }
    Handle process(info.hProcess), thread(info.hThread);
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1); WaitForSingleObject(process.value, INFINITE);
        result.stderr_text = "Failed to attach/start managed process"; return result;
    }
    out_write.reset(); err_write.reset();
    result.status = "failed";
    for (;;) {
        drain(out_read.value, result, result.stdout_text, options.capture_limit);
        drain(err_read.value, result, result.stderr_text, options.capture_limit);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (cancelled(options) || Clock::now() - start >= options.timeout) {
            result.status = cancelled(options) ? "cancelled" : "timed_out";
            TerminateJobObject(job.value, 1); WaitForSingleObject(process.value, INFINITE); break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DWORD code = 0; GetExitCodeProcess(process.value, &code); result.exit_code = static_cast<int>(code);
    // A completed parent must not leave grandchildren running or pipe handles open.
    TerminateJobObject(job.value, 0);
    drain(out_read.value, result, result.stdout_text, options.capture_limit);
    drain(err_read.value, result, result.stderr_text, options.capture_limit);
#else
    int out[2], err[2], exec_error[2];
    if (pipe(out) < 0) throw std::runtime_error("stdout pipe failed");
    Fd out_read{out[0]}, out_write{out[1]};
    if (pipe(err) < 0) throw std::runtime_error("stderr pipe failed");
    Fd err_read{err[0]}, err_write{err[1]};
    if (pipe(exec_error) < 0) throw std::runtime_error("exec pipe failed");
    Fd error_read{exec_error[0]}, error_write{exec_error[1]};
    for (int fd : {out[0], out[1], err[0], err[1], exec_error[0], exec_error[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<char*> argv;
    for (const auto& arg : options.arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { result.stderr_text = std::strerror(errno); return result; }
    if (!pid) {
        int failure = 0;
        if (setpgid(0, 0) || (!options.working_directory.empty() && chdir(options.working_directory.c_str()))) failure = errno;
        const int null_input = open("/dev/null", O_RDONLY);
        if (!failure && (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 || dup2(err[1], STDERR_FILENO) < 0)) failure = errno;
        if (null_input > 2) close(null_input);
        if (!failure) { execvp(argv[0], argv.data()); failure = errno; }
        const auto ignored = write(exec_error[1], &failure, sizeof(failure)); (void)ignored;
        _exit(127);
    }
    setpgid(pid, pid); out_write.reset(); err_write.reset(); error_write.reset();
    struct ChildGuard {
        pid_t pid; bool reaped = false;
        ~ChildGuard() { kill(-pid,SIGKILL); if (!reaped) { int status; while (waitpid(pid,&status,0)<0 && errno==EINTR) {} } }
    } child{pid};
    for (int fd : {out[0], err[0], exec_error[0]}) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int status = 0; result.status = "failed";
    for (;;) {
        drain(out[0], result, result.stdout_text, options.capture_limit);
        drain(err[0], result, result.stderr_text, options.capture_limit);
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) { kill(-pid, SIGKILL); throw std::runtime_error("waitpid failed"); }
        if (cancelled(options) || Clock::now() - start >= options.timeout) {
            result.status = cancelled(options) ? "cancelled" : "timed_out";
            kill(-pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(-pid, SIGKILL);
    child.reaped = true;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    drain(out[0], result, result.stdout_text, options.capture_limit);
    drain(err[0], result, result.stderr_text, options.capture_limit);
    int failure = 0;
    if (read(exec_error[0], &failure, sizeof(failure)) == sizeof(failure)) { result.status = "start_failed"; result.stderr_text += std::strerror(failure); }
#endif
    if (result.status == "failed" && result.exit_code == 0) result.status = "passed";
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    return result;
}
} // namespace codeguard
