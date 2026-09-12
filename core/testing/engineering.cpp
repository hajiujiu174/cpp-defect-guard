#include "codeguard/application.hpp"
#include "codeguard/thread_pool.hpp"
#include <fstream>
#include <map>
#include <regex>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cwctype>

namespace codeguard {
namespace {
bool inside(const fs::path& path, const fs::path& directory) {
    const auto p = fs::weakly_canonical(path), d = fs::weakly_canonical(directory);
    auto a = p.begin(), b = d.begin();
    for (; b != d.end(); ++b, ++a) {
        if (a == p.end()) return false;
#ifdef _WIN32
        auto x = a->wstring(), y = b->wstring();
        for (auto& c : x) c = std::towlower(c);
        for (auto& c : y) c = std::towlower(c);
        if (x != y) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return true;
}
std::string timestamp() {
    const auto now = std::time(nullptr); std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc,&now);
#else
    gmtime_r(&now,&utc);
#endif
    std::ostringstream out; out << std::put_time(&utc,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
bool same_source(const ScanResult& before, const ScanResult& after) {
    if (!after.diagnostics.empty() || before.files.size() != after.files.size()) return false;
    for (std::size_t i = 0; i < before.files.size(); ++i)
        if (before.files[i].path != after.files[i].path || before.files[i].hash != after.files[i].hash || before.files[i].size != after.files[i].size) return false;
    return true;
}
void check_cancel(const BuildOptions& options) { if (options.control) options.control->check(); }
void write_text(const fs::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::binary); stream << text;
    if (!stream) throw std::runtime_error("cannot write build log: " + utf8_path(file));
}
void copy_project(const fs::path& root, const fs::path& target, const BuildOptions& options) {
    fs::create_directories(target); std::size_t count = 0; std::uintmax_t bytes = 0;
    const auto ignored = ScanOptions{}.ignored_directories;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        check_cancel(options); const auto status = it->symlink_status();
        if (fs::is_symlink(status)) { it.disable_recursion_pending(); continue; }
        if(std::any_of(options.copy_excludes.begin(),options.copy_excludes.end(),[&](const auto& path){return inside(it->path(),root/from_utf8(path));})) {it.disable_recursion_pending();continue;}
        auto name = utf8_path(it->path().filename());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        const bool included = std::any_of(options.copy_includes.begin(), options.copy_includes.end(), [&](const auto& value) {
            return it->path().lexically_relative(root) == from_utf8(value).lexically_normal();
        });
        if (fs::is_directory(status) && !included && std::find(ignored.begin(), ignored.end(), name) != ignored.end()) { it.disable_recursion_pending(); continue; }
        const auto destination = target / it->path().lexically_relative(root);
        if (fs::is_directory(status)) fs::create_directory(destination);
        else if (fs::is_regular_file(status)) {
            if (++count > 100000 || (bytes += it->file_size()) > 2ULL * 1024 * 1024 * 1024) throw std::runtime_error("build source copy exceeds 100000 files or 2 GiB");
            std::ifstream input(it->path(),std::ios::binary); std::ofstream output(destination,std::ios::binary);
            if (!input || !output) throw std::runtime_error("cannot copy source file");
            char buffer[65536];
            while (input) { check_cancel(options); input.read(buffer,sizeof(buffer)); output.write(buffer,input.gcount()); }
            if (!input.eof() || !output) throw std::runtime_error("source copy I/O failure");
            fs::permissions(destination,status.permissions());
        }
    }
}
void test_counts(BuildStep& step) {
    if (step.result.output_truncated) return;
    const auto& text = step.result.stdout_text;
    std::smatch match;
    const std::regex summary(R"((\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+))");
    if (std::regex_search(text, match, summary)) {
        step.tests_failed = match[2].matched ? std::stoi(match[2]) : 0; step.tests_total = std::stoi(match[3]);
        step.tests_skipped = 0;
        const std::regex skipped(R"(\*\*\*Skipped)");
        for (std::sregex_iterator i(text.begin(),text.end(),skipped),end;i!=end;++i) ++step.tests_skipped;
    }
}
}
std::string read_git(const fs::path& root, const BuildOptions& options, std::string* revision) {
    std::string log;
    for (const auto& command : std::vector<std::vector<std::string>>{{"rev-parse","HEAD"},{"branch","--show-current"},
        {"status","--short","--untracked-files=no"},{"log","-5","--oneline"},{"diff","--no-ext-diff","--no-textconv","--"}}) {
        ProcessOptions process;
        process.arguments = {options.git,"--no-pager","--no-optional-locks","-c","core.fsmonitor=false","-C",utf8_path(root)};
        process.arguments.insert(process.arguments.end(),command.begin(),command.end());
        process.working_directory= utf8_path(root); process.control=options.control;
        process.timeout=std::min(options.timeout,std::chrono::milliseconds(10000));
        const auto result=run_process(process);
        log += "$ " + format_arguments(process.arguments) + "\n" + result.status + "\n" + result.stdout_text + result.stderr_text;
        if (result.output_truncated) log += "\n[output truncated]\n";
        if (result.status != "passed") break;
        if (revision && command.front()=="rev-parse") {
            *revision=result.stdout_text;
            while (!revision->empty() && (revision->back()=='\n' || revision->back()=='\r')) revision->pop_back();
        }
    }
    return log;
}
BuildRun build_and_test(const fs::path& input, const fs::path& database, const BuildOptions& options) {
    const auto root = fs::canonical(input); const auto dbpath = fs::weakly_canonical(database);
    if (options.output_directory.empty() || options.timeout.count() <= 0 || options.jobs > 64 || options.target.starts_with('-'))
        throw std::invalid_argument("build needs an output directory, positive timeout, jobs 0..64 and a non-option target");
    if (inside(dbpath,root) || inside(options.output_directory,root)) throw std::invalid_argument("build database and output must be outside source directory");
    ProjectConfig settings;settings.build=options;validate_config(settings);
    ScanOptions inventory_options;inventory_options.ignored_paths=options.copy_excludes;
    for (const auto& definition : options.cmake_definitions) {
        const auto equal = definition.find('=');
        if (equal == std::string::npos || !std::regex_match(definition.substr(0,equal),std::regex("[A-Za-z_][A-Za-z0-9_]*(:[A-Za-z]+)?")) || definition.find('\0') != std::string::npos)
            throw std::invalid_argument("CMake definition must be KEY[:TYPE]=VALUE");
    }
    for (const auto& value : options.copy_includes) {
        const auto relative = from_utf8(value);
        bool unsafe = value.empty() || relative.has_root_path();
        for (const auto& component : relative) {
            auto name=utf8_path(component); std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            unsafe = unsafe || name==".." || name=="." || name==".git";
        }
        if (unsafe || !inside(root/relative,root) || !fs::is_directory(root/relative) || fs::is_symlink(root/relative))
            throw std::invalid_argument("copy include must name an existing project-relative directory, without dot segments, symlinks or .git");
    }
    ScanResult snapshot;
    { SqliteDatabase db(dbpath,true); snapshot = db.latest(utf8_path(root)); }
    if (!snapshot.id) throw std::invalid_argument("scan the project before build/test");
    BuildRun run; run.root=snapshot.root; run.scan_id=snapshot.id; run.started_at=timestamp(); run.target=options.target;run.configuration=encode_config(settings);
    const auto base = fs::absolute(options.output_directory); fs::create_directories(base);
    fs::path work;
    for (unsigned attempt=0;attempt<100;++attempt) {
        work=base/("run-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(attempt));
        if (fs::create_directory(work)) break;
        if (attempt==99) throw std::runtime_error("cannot create unique build workspace");
    }
    work=fs::canonical(work); run.workspace=utf8_path(work);
    const auto source=work/"source", build=work/"build";
    BuildStep prepare; prepare.name="prepare"; prepare.working_directory=utf8_path(work);
    try {
        if (options.progress) options.progress("prepare");
        check_cancel(options);
        if (!same_source(snapshot,scan_project(root,inventory_options))) throw std::runtime_error("source or excluded directories differ from saved scan; rescan before building");
        copy_project(root,source,options); check_cancel(options);
        if (!same_source(snapshot,scan_project(root,inventory_options)) || !same_source(snapshot,scan_project(source,inventory_options)))
            throw std::runtime_error("source changed while copying; build stopped");
        if (!fs::is_regular_file(source/"CMakeLists.txt")) throw std::runtime_error("project has no CMakeLists.txt");
        prepare.result.status="passed"; prepare.result.exit_code=0; prepare.result.stdout_text="Source snapshot copied; original C/C++ inventory matches saved scan.\n";
        run.steps.push_back(prepare);
        run.git_log=read_git(root,options,&run.git_revision);
        const unsigned jobs=options.jobs ? options.jobs : std::min(8u,std::max(1u,std::thread::hardware_concurrency()));
        std::vector<std::string> configure{options.cmake,"-S",utf8_path(source),"-B",utf8_path(build),"-G",options.generator,
            "-DCMAKE_BUILD_TYPE="+options.build_type,"-DBUILD_TESTING=ON","-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
        if (!options.c_compiler.empty()) configure.push_back("-DCMAKE_C_COMPILER="+options.c_compiler);
        if (!options.cxx_compiler.empty()) configure.push_back("-DCMAKE_CXX_COMPILER="+options.cxx_compiler);
        for (const auto& definition : options.cmake_definitions) configure.push_back("-D"+definition);
        std::vector<std::string> compile{options.cmake,"--build",utf8_path(build),"--config",options.build_type,"--parallel",std::to_string(jobs)};
        if (!options.target.empty()) { compile.push_back("--target"); compile.push_back(options.target); }
        const auto seconds=std::max<std::int64_t>(1,options.timeout.count()/1000);
        std::vector<std::string> test{options.ctest,"--test-dir",utf8_path(build),"-C",options.build_type,"--output-on-failure","--no-tests=error",
            "--timeout",std::to_string(seconds),"--output-junit",utf8_path(work/"tests.xml")};
        run.status="passed";
        for (const auto& [name,args] : std::vector<std::pair<std::string,std::vector<std::string>>>{{"configure",configure},{"build",compile},{"test",test}}) {
            if(options.configure_only&&name!="configure")continue;
            BuildStep step; step.name=name; step.command=format_arguments(args); step.working_directory=utf8_path(work);
            if (run.status != "passed") { step.result.status="skipped"; step.result.stderr_text="Previous stage did not pass."; }
            else {
                if (options.progress) options.progress(name);
                step.result=run_process({args,utf8_path(work),options.timeout,4*1024*1024,options.control});
                if (name=="test") test_counts(step);
                if (step.result.status!="passed") run.status=step.result.status;
            }
            run.steps.push_back(std::move(step));
        }
        if(options.configure_only&&run.status=="passed") {
            if(!same_source(snapshot,scan_project(source,inventory_options)))throw std::runtime_error("CMake generated or changed source-tree C/C++ files; use out-of-source generated files or supply a matching compilation database manually");
            run.compile_commands=relocate_compile_commands(utf8_path(build/"compile_commands.json"),utf8_path(source),utf8_path(root),utf8_path(work/"analysis-input"/"compile_commands.json"));
            run.status="configured";
        }
    } catch (const std::exception& error) {
        run.status=options.control && options.control->state()==ScanControl::State::cancel_requested ? "cancelled" : "failed";
        BuildStep step; step.name=run.steps.empty() ? "prepare" : "manager"; step.result.status=run.status; step.result.stderr_text=error.what();
        run.steps.push_back(std::move(step));
    }
    try { run.source_unchanged=same_source(snapshot,scan_project(root,inventory_options)); }
    catch(const std::exception& error) {
        BuildStep verify;verify.name="source_check";verify.result.status="failed";verify.result.stderr_text=error.what();run.steps.push_back(std::move(verify));
        run.source_unchanged=false;
    }
    if (!run.source_unchanged && (run.status=="passed"||run.status=="configured")) run.status="source_changed";
    if (options.control) {
        try { options.control->begin_commit(); }
        catch (const ScanCancelled&) { run.status="cancelled"; }
    }
    std::ostringstream summary;
    summary << "scan=" << run.scan_id << "\nstatus=" << run.status << "\nsource_unchanged=" << run.source_unchanged << "\n";
    for (std::size_t i=0;i<run.steps.size();++i) {
        const auto& step=run.steps[i]; summary << step.name << '\t' << step.result.status << '\t' << step.result.exit_code << '\t' << step.result.duration_ms << " ms\n";
        write_text(work/(std::to_string(i)+"-"+step.name+".log"),step.command+"\nstatus="+step.result.status+"\nstdout:\n"+step.result.stdout_text+"\nstderr:\n"+step.result.stderr_text+
            (step.result.output_truncated ? "\n[output truncated]\n" : ""));
    }
    write_text(work/"summary.txt",summary.str()); write_text(work/"git.txt",run.git_log);
    // Cancellation stops processes; its logs still commit using a fresh writer context.
    ThreadPool writer(1,1);
    writer.submit([&]{SqliteDatabase db(dbpath); db.save_build(run);}).get();
    return run;
}
} // namespace codeguard
