#include "codeguard/application.hpp"
#include "codeguard/query.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <set>
#include <charconv>
#include <csignal>
#include <thread>
#include <fstream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }
class InterruptScope {
    void (*previous_)(int);
    std::jthread monitor_;
public:
    explicit InterruptScope(std::shared_ptr<codeguard::ScanControl> control) : previous_(std::signal(SIGINT,interrupt_handler)) {
        interrupted = 0;
        monitor_=std::jthread([control](std::stop_token stop) {
            while (!stop.stop_requested()) { if (interrupted) control->request_cancel(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        });
    }
    ~InterruptScope() { monitor_.request_stop(); monitor_.join(); std::signal(SIGINT,previous_); }
};
unsigned number(const std::string& text, unsigned maximum) {
    unsigned value=0; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(error!=std::errc{} || end!=text.data()+text.size() || value>maximum) throw std::invalid_argument("invalid numeric option: "+text);
    return value;
}
void display_build(const codeguard::BuildRun& run) {
    std::cout << "run_id=" << run.id << "\nscan_id=" << run.scan_id << "\nstatus=" << run.status << "\nworkspace=" << run.workspace
        << "\nsource_unchanged=" << run.source_unchanged << "\ngit_revision=" << run.git_revision << '\n';
    for (const auto& step:run.steps) std::cout << step.name << '\t' << step.result.status << '\t' << step.result.exit_code << '\t'
        << step.result.duration_ms << "ms\ttests=" << step.tests_total << " failed=" << step.tests_failed << " skipped=" << step.tests_skipped << '\n'
        << step.result.stdout_text << step.result.stderr_text << (step.result.output_truncated ? "\n[output truncated]\n" : "");
}
std::string tsv(std::string value) {
    std::string out;
    for (const char c : value) {
        if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}
void display(const codeguard::ScanResult& result) {
    std::cout << "project=" << result.root << "\nscan_id=" << result.id
              << "\nfiles=" << result.files.size() << "\nphysical_lines=" << result.total_lines()
              << "\nadded=" << result.added << "\nchanged=" << result.changed
              << "\nunchanged=" << result.unchanged << "\nremoved=" << result.removed << '\n';
    std::cout << "analysis=" << result.analysis.status << "\nsymbols=" << result.analysis.symbols.size()
              << "\nfunction_metrics=" << result.analysis.metrics.size() << "\ncovered_files=" << result.analysis.covered_files.size() << '\n';
    std::cout << "issues=" << result.analysis.issues.size() << "\nworkers=" << result.analysis.workers << "\nanalysis_ms=" << result.analysis.elapsed_ms << '\n';
    if (result.analysis.status != "not_requested") {
        const std::set<std::string> covered(result.analysis.covered_files.begin(), result.analysis.covered_files.end());
        for (const auto& file : result.files) if (!covered.contains(file.path)) std::cout << "uncovered\t" << file.path << '\n';
        for (const auto& unit : result.analysis.units) {
            std::cout << "translation_unit\t" << unit.file << '\t' << unit.status << "\tindirect_calls=" << unit.indirect_calls << '\n';
            if (!unit.diagnostics.empty()) std::cerr << unit.file << ": " << unit.diagnostics << '\n';
        }
    }
    for (const auto& file : result.files)
        std::cout << file.language << '\t' << file.lines << '\t' << file.size << '\t' << file.path << '\n';
    for (const auto& item : result.diagnostics)
        std::cerr << item.path << ": " << item.message << '\n';
}
int run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help") {
        std::cout << "CodeGuard - C++ project inventory and optional Clang analysis\n"
                     "codeguard-cli scan PROJECT --database DATABASE [--compile-commands DIRECTORY_OR_JSON] [--ignore DIRECTORY]...\n"
                     "codeguard-cli status PROJECT --database DATABASE\n"
                     "codeguard-cli symbols PROJECT --database DATABASE [--prefix NAME]\n"
                     "codeguard-cli metrics PROJECT --database DATABASE\n"
                     "codeguard-cli graph PROJECT --database DATABASE --kind call|include\n"
                     "codeguard-cli recent --database DATABASE\n"
                     "codeguard-cli rules\n"
                     "codeguard-cli issues PROJECT --database DATABASE [--severity error|warning]\n"
                     "codeguard-cli build PROJECT --database DATABASE --output DIRECTORY [--jobs 4] [--timeout 120] [--target NAME] [--c-compiler PATH] [--cxx-compiler PATH]\n"
                     "Build also supports repeated --cmake-define KEY=VALUE and --copy-include RELATIVE_DIRECTORY.\n"
                     "codeguard-cli builds PROJECT --database DATABASE\n"
                     "codeguard-cli git PROJECT\n"
                     "Scan supports --threads 0..64 (0=automatic). Ctrl+C cancels scan/build.\n"
                     "codeguard-cli query PROJECT --database DATABASE --query \"SELECT file, lines FROM files ORDER BY lines DESC LIMIT 20\"\n"
                     "Database must be outside PROJECT. Sources are never modified.\n";
        return 0;
    }
    const auto command = args[0];
    if (command=="rules") {
        if (args.size()!=1) throw std::invalid_argument("rules takes no options");
        for (const auto& rule:codeguard::rule_catalog()) std::cout << rule.id << '\t' << rule.severity << '\t' << rule.title << '\t' << rule.scope << '\n';
        return 0;
    }
    if (command=="git") {
        if(args.size()!=2) throw std::invalid_argument("git requires PROJECT");
        codeguard::BuildOptions options; options.control=std::make_shared<codeguard::ScanControl>(); InterruptScope scope(options.control);
        std::string revision; std::cout << codeguard::read_git(codeguard::fs::canonical(codeguard::from_utf8(args[1])),options,&revision);
        return revision.empty() ? 2 : 0;
    }
    if (command != "scan" && command != "status" && command != "recent" && command != "symbols" && command != "metrics" && command != "graph" && command != "query" && command != "issues" && command != "build" && command != "builds")
        throw std::invalid_argument("unknown command; use --help");
    std::string project, database;
    std::size_t position = 1;
    if (command != "recent") {
        if (args.size() < 2 || args[1].starts_with("--")) throw std::invalid_argument("missing PROJECT");
        project = args[position++];
    }
    codeguard::ScanOptions options;
    codeguard::BuildOptions build_options;
    std::string prefix, kind, query, severity;
    std::set<std::string> seen;
    for (; position < args.size(); position += 2) {
        if (position + 1 >= args.size()) throw std::invalid_argument("missing option value");
        if (args[position] != "--ignore" && args[position] != "--cmake-define" && args[position] != "--copy-include" && !seen.insert(args[position]).second) throw std::invalid_argument("repeated option");
        if (args[position] == "--database" && database.empty()) database = args[position + 1];
        else if (args[position] == "--ignore" && command == "scan") options.ignored_directories.push_back(args[position + 1]);
        else if (args[position] == "--compile-commands" && command == "scan") options.compile_commands = args[position + 1];
        else if (args[position] == "--prefix" && command == "symbols") prefix = args[position + 1];
        else if (args[position] == "--kind" && command == "graph") kind = args[position + 1];
        else if (args[position] == "--query" && command == "query") query = args[position + 1];
        else if (args[position] == "--threads" && command == "scan") options.threads=number(args[position+1],64);
        else if (args[position] == "--severity" && command == "issues") severity=args[position+1];
        else if (args[position] == "--output" && command == "build") build_options.output_directory=codeguard::from_utf8(args[position+1]);
        else if (args[position] == "--jobs" && command == "build") build_options.jobs=number(args[position+1],64);
        else if (args[position] == "--timeout" && command == "build") build_options.timeout=std::chrono::seconds(number(args[position+1],86400));
        else if (args[position] == "--target" && command == "build") build_options.target=args[position+1];
        else if (args[position] == "--c-compiler" && command == "build") build_options.c_compiler=args[position+1];
        else if (args[position] == "--cxx-compiler" && command == "build") build_options.cxx_compiler=args[position+1];
        else if (args[position] == "--cmake-define" && command == "build") build_options.cmake_definitions.push_back(args[position+1]);
        else if (args[position] == "--copy-include" && command == "build") build_options.copy_includes.push_back(args[position+1]);
        else throw std::invalid_argument("unknown or repeated option: " + args[position]);
    }
    if (database.empty()) throw std::invalid_argument("--database is required");
    if (command == "query" && query.empty()) throw std::invalid_argument("--query is required");
    if (!severity.empty() && severity!="error" && severity!="warning") throw std::invalid_argument("severity must be error or warning");
    if (command == "build") {
        build_options.control=std::make_shared<codeguard::ScanControl>(); InterruptScope scope(build_options.control);
        build_options.progress=[](const auto& phase){ std::cerr << "stage=" << phase << '\n'; };
        const auto result=codeguard::build_and_test(codeguard::from_utf8(project),codeguard::from_utf8(database),build_options);
        display_build(result); return result.status=="passed" ? 0 : (result.status=="cancelled" ? 130 : 2);
    }
    if (command == "scan") {
        options.context.control=std::make_shared<codeguard::ScanControl>(); InterruptScope scope(options.context.control);
        const auto result = codeguard::import_project(codeguard::from_utf8(project), codeguard::from_utf8(database), options);
        display(result);
        return result.diagnostics.empty() && (result.analysis.status == "not_requested" || result.analysis.status == "complete") ? 0 : 2;
    }
    codeguard::SqliteDatabase db(codeguard::from_utf8(database), true);
    if (command == "recent") {
        for (const auto& item : db.projects()) std::cout << item.last_scan << '\t' << item.root << '\n';
    } else {
        const auto result = db.latest(codeguard::utf8_path(codeguard::fs::canonical(codeguard::from_utf8(project))));
        if (!result.id) throw std::invalid_argument("project has no saved scan");
        if (command == "status") display(result);
        else if(command=="builds") { for(const auto& run:db.builds(result.root)) display_build(run); }
        else if (command == "query") {
            const auto answer = codeguard::execute_query(result, query);
            std::cerr << answer.plan << "\nscanned=" << answer.scanned_rows << " matched=" << answer.matched_rows << " returned=" << answer.rows.size() << '\n';
            for (std::size_t i = 0; i < answer.columns.size(); ++i) std::cout << (i ? "\t" : "") << answer.columns[i];
            std::cout << '\n';
            for (const auto& row : answer.rows) {
                for (std::size_t i = 0; i < row.size(); ++i) std::cout << (i ? "\t" : "") << tsv(codeguard::query_value_text(row[i]));
                std::cout << '\n';
            }
            if (!result.diagnostics.empty() || (result.analysis.status != "not_requested" && result.analysis.status != "complete")) return 2;
        }
        else {
            if (result.analysis.status == "not_requested") throw std::invalid_argument("saved scan has no Clang analysis; scan with --compile-commands first");
            std::cout << "analysis=" << result.analysis.status << '\n';
            if (command == "issues") {
                std::cout << "severity\trule\tfile\tline\tcolumn\tmessage\tevidence\tsuggestion\n";
                for(const auto& issue:result.analysis.issues) if(severity.empty() || issue.severity==severity)
                    std::cout << issue.severity << '\t' << issue.rule_id << '\t' << tsv(issue.file) << '\t' << issue.line << '\t' << issue.column << '\t'
                        << tsv(issue.message) << '\t' << tsv(issue.evidence) << '\t' << tsv(issue.suggestion) << '\n';
            } else if (command == "symbols") {
                for (const auto& symbol : codeguard::find_symbols(result.analysis, prefix))
                    std::cout << symbol.kind << '\t' << symbol.name << '\t' << symbol.file << ':' << symbol.line << ':' << symbol.column
                              << '\t' << (symbol.definition ? "definition" : "declaration") << '\t' << symbol.id << '\n';
            } else if (command == "metrics") {
                auto metrics = result.analysis.metrics;
                std::map<std::string, codeguard::Symbol> symbols;
                for (const auto& symbol : result.analysis.symbols) symbols[symbol.id] = symbol;
                std::sort(metrics.begin(), metrics.end(), [](const auto& a, const auto& b) {
                    return a.complexity != b.complexity ? a.complexity > b.complexity : a.symbol_id < b.symbol_id;
                });
                std::cout << "complexity\tlines\tparameters\tname\tlocation\n";
                for (const auto& metric : metrics) {
                    const auto& symbol = symbols.at(metric.symbol_id);
                    std::cout << metric.complexity << '\t' << metric.lines << '\t' << metric.parameters << '\t'
                              << symbol.name << '\t' << symbol.file << ':' << symbol.line << '\n';
                }
            } else {
                const auto graph = codeguard::project_graph(result, kind);
                std::cout << "nodes=" << graph.nodes.size() << "\nedges=" << graph.edge_count << "\nscc=" << graph.components.size()
                          << "\ncyclic_components=" << graph.cycles.size() << '\n';
                for (const auto& symbol : result.analysis.symbols) if (kind == "call" && symbol.kind == "function")
                    std::cout << "node\t" << symbol.id << '\t' << symbol.name << '\t' << symbol.file << ':' << symbol.line << '\n';
                for (const auto& edge : result.analysis.edges) if (edge.kind == kind)
                    std::cout << "edge\t" << edge.source << " -> " << edge.target << '\t' << edge.file << ':' << edge.line << '\n';
                for (const auto& component : graph.cycles) {
                    std::cout << "cyclic_scc"; for (const auto& node : component) std::cout << '\t' << node; std::cout << '\n';
                }
                if (graph.cycles.empty()) { std::cout << "topological_source_first"; for (const auto& node : graph.topological_order) std::cout << '\t' << node; std::cout << '\n'; }
            }
            if (result.analysis.status != "complete") return 2;
        }
    }
    return 0;
}
int guarded(const std::vector<std::string>& args) {
    try { return run(args); }
    catch (const codeguard::ScanCancelled&) { std::cerr << "CodeGuard: scan cancelled\n"; return 130; }
    catch (const std::exception& exception) {
        std::cerr << "CodeGuard: " << exception.what() << '\n';
        return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) { const auto value=codeguard::fs::path(argv[i]).u8string(); args.emplace_back(value.begin(),value.end()); }
    return guarded(args);
}
#else
int main(int argc, char** argv) {
    return guarded({argv + 1, argv + argc});
}
#endif
