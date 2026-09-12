#include "codeguard/application.hpp"
#include "codeguard/thread_pool.hpp"
#include "../analyzer/clang_rules/rule.hpp"
#include <clang/Lex/Lexer.h>
#include <chrono>
#include <mutex>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace codeguard {
static_assert(CLANG_VERSION_MAJOR == 22, "Core parser requires Clang 22.x headers");
namespace {
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
std::string key(const fs::path& path) {
    auto value = utf8_path(fs::weakly_canonical(path));
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}
struct Collector {
    const std::map<std::string, std::string>& inventory;
    AnalysisResult result;
    std::map<std::string, Symbol> symbols;
    std::map<std::string, FunctionMetric> metrics;
    std::set<std::string> covered;
    int indirect = 0;
    mutable std::unordered_map<std::string, std::string> path_cache;
    fs::path working_directory;
    clang_rules::Engine rules;
    Collector(const std::map<std::string, std::string>& files, const std::string& directory,const std::vector<std::string>& disabled)
        : inventory(files), working_directory(from_utf8(directory)),rules(disabled) {}
    const std::string& normalized(llvm::StringRef filename) const {
        const auto name = filename.str();
        const auto found = path_cache.find(name);
        if (found != path_cache.end()) return found->second;
        auto path = from_utf8(name);
        if (!path.is_absolute()) path = working_directory / path;
        return path_cache.emplace(name, key(path)).first->second;
    }
    std::string path(llvm::StringRef filename) const {
        if (filename.empty()) return {};
        const auto& canonical = normalized(filename);
        const auto it = inventory.find(canonical);
        return it == inventory.end() ? canonical : it->second;
    }
    bool internal(llvm::StringRef filename) const {
        return !filename.empty() && inventory.contains(normalized(filename));
    }
    std::string id(const NamedDecl* declaration, const SourceManager& sources) const {
        llvm::SmallString<256> value;
        if (index::generateUSRForDecl(declaration->getCanonicalDecl(), value)) return {};
        std::string result = value.str().str();
        if (declaration->getFormalLinkage() == Linkage::Internal || declaration->getFormalLinkage() == Linkage::None) {
            const auto location = sources.getExpansionLoc(declaration->getCanonicalDecl()->getLocation());
            result += "@file=" + path(sources.getFilename(location));
        }
        return result;
    }
    std::string symbol(const NamedDecl* declaration, const SourceManager& sources, bool permit_external = false) {
        if (!declaration || declaration->isImplicit() || declaration->getNameAsString().empty()) return {};
        const auto location = sources.getExpansionLoc(declaration->getLocation());
        if (location.isInvalid()) return {};
        const bool external = !internal(sources.getFilename(location));
        if (external && !permit_external) return {};
        Symbol symbol;
        symbol.id = id(declaration, sources);
        if (symbol.id.empty()) return {};
        symbol.name = declaration->getQualifiedNameAsString();
        symbol.file = path(sources.getFilename(location));
        symbol.line = sources.getExpansionLineNumber(location);
        symbol.column = sources.getExpansionColumnNumber(location);
        symbol.external = external;
        if (const auto* function = dyn_cast<FunctionDecl>(declaration)) {
            symbol.kind = "function"; symbol.definition = function->isThisDeclarationADefinition();
        } else if (const auto* record = dyn_cast<RecordDecl>(declaration)) {
            symbol.kind = record->isUnion() ? "union" : (record->isClass() ? "class" : "struct");
            symbol.definition = record->isThisDeclarationADefinition();
        } else if (const auto* variable = dyn_cast<VarDecl>(declaration)) {
            symbol.kind = isa<ParmVarDecl>(variable) ? "parameter" : "variable";
            symbol.definition = variable->isThisDeclarationADefinition() != VarDecl::DeclarationOnly;
        } else if (isa<FieldDecl>(declaration)) {
            symbol.kind = "field"; symbol.definition = true;
        } else return {};
        const auto old = symbols.find(symbol.id);
        if (old == symbols.end() || (symbol.definition && !old->second.definition) ||
            (symbol.definition == old->second.definition && std::tie(symbol.file, symbol.line, symbol.column) <
                std::tie(old->second.file, old->second.line, old->second.column))) symbols[symbol.id] = symbol;
        return symbol.id;
    }
};
class Calls final : public RecursiveASTVisitor<Calls> {
    Collector& output;
    const SourceManager& sources;
    std::string caller;
public:
    Calls(Collector& out, const SourceManager& sm, std::string id) : output(out), sources(sm), caller(std::move(id)) {}
    bool TraverseLambdaExpr(LambdaExpr*) { return true; } // no outer-function attribution of lambda bodies
    bool TraverseFunctionDecl(FunctionDecl*) { return true; } // local class methods have their own callback
    void add(const FunctionDecl* target, SourceLocation input) {
        const auto location = sources.getExpansionLoc(input);
        if (!output.internal(sources.getFilename(location))) return;
        if (!target) { ++output.indirect; return; }
        const auto target_id = output.symbol(target, sources, true);
        if (target_id.empty()) { ++output.indirect; return; }
        output.result.edges.push_back({"call", caller, target_id, output.path(sources.getFilename(location)),
            static_cast<int>(sources.getExpansionLineNumber(location)), static_cast<int>(sources.getExpansionColumnNumber(location))});
    }
    bool VisitCallExpr(CallExpr* expression) { add(expression->getDirectCallee(), expression->getExprLoc()); return true; }
    bool VisitCXXConstructExpr(CXXConstructExpr* expression) { add(expression->getConstructor(), expression->getExprLoc()); return true; }
};
class Matches final : public MatchFinder::MatchCallback {
    Collector& output;
public:
    explicit Matches(Collector& out) : output(out) {}
    void run(const MatchFinder::MatchResult& match) override {
        const auto* declaration = match.Nodes.getNodeAs<NamedDecl>("symbol");
        if (!declaration) return;
        const auto id = output.symbol(declaration, *match.SourceManager);
        const auto* function = dyn_cast<FunctionDecl>(declaration);
        if (id.empty() || !function || !function->isThisDeclarationADefinition() || !function->getBody()) return;
        const auto& sources = *match.SourceManager;
        FunctionMetric metric;
        metric.symbol_id = id; metric.parameters = function->getNumParams();
        const auto start = sources.getExpansionLineNumber(function->getBeginLoc());
        const auto end = sources.getExpansionLineNumber(function->getEndLoc());
        metric.lines = end >= start ? end - start + 1 : 0;
        CFG::BuildOptions options;
        const auto cfg = CFG::buildCFG(function, const_cast<Stmt*>(function->getBody()), match.Context, options);
        if (cfg) {
            metric.complexity = 1;
            // Same CFG branch-slot convention as the existing standalone analyzer.
            for (const auto* block : *cfg) if (block && block->succ_size() > 1) metric.complexity += block->succ_size() - 1;
        }
        output.metrics[id] = metric;
        Calls calls(output, sources, id);
        calls.TraverseStmt(const_cast<Stmt*>(function->getBody()));
        output.rules.inspect(function->getBody(),*match.Context,[&](const Stmt* statement,const std::string& rule,const std::string& message,const std::string& suggestion){
            add_issue(statement,*match.Context,id,rule,message,suggestion);
        });
    }
private:
    void add_issue(const Stmt* statement, ASTContext& context, const std::string& id, const std::string& rule,
                   const std::string& message, const std::string& suggestion) {
        auto& sources = context.getSourceManager();
        const auto location = sources.getExpansionLoc(statement->getBeginLoc());
        if (location.isInvalid() || !output.internal(sources.getFilename(location))) return;
        const auto& info = rule_info(rule);
        auto evidence = Lexer::getSourceText(CharSourceRange::getTokenRange(statement->getSourceRange()), sources, context.getLangOpts()).str();
        if(evidence.empty()&&statement->getBeginLoc().isMacroID())
            evidence=Lexer::getSourceText(sources.getExpansionRange(statement->getSourceRange()),sources,context.getLangOpts()).str();
        if(evidence.empty())evidence="[AST node: "+std::string(statement->getStmtClassName())+"]";
        if (evidence.size() > 500) evidence = evidence.substr(0, 500) + "...";
        output.result.issues.push_back({info.id, info.severity, output.path(sources.getFilename(location)),
            static_cast<int>(sources.getExpansionLineNumber(location)), static_cast<int>(sources.getExpansionColumnNumber(location)),
            message, evidence, suggestion, id});
    }
};
class Includes final : public PPCallbacks {
    Collector& output;
    SourceManager& sources;
public:
    Includes(Collector& out, SourceManager& sm) : output(out), sources(sm) {}
    void FileChanged(SourceLocation location, FileChangeReason reason, SrcMgr::CharacteristicKind, FileID) override {
        if (reason != EnterFile) return;
        const auto filename = sources.getFilename(sources.getExpansionLoc(location));
        if (output.internal(filename)) output.covered.insert(output.path(filename));
    }
    void InclusionDirective(SourceLocation location, const Token&, llvm::StringRef, bool, CharSourceRange,
        OptionalFileEntryRef file, llvm::StringRef, llvm::StringRef, const Module*, bool, SrcMgr::CharacteristicKind) override {
        const auto filename = sources.getFilename(sources.getExpansionLoc(location));
        if (!output.internal(filename) || !file) return;
        const auto source = output.path(filename);
        output.result.edges.push_back({"include", source, output.path(file->getName()), source,
            static_cast<int>(sources.getExpansionLineNumber(location)), static_cast<int>(sources.getExpansionColumnNumber(location))});
    }
};
class Action final : public ASTFrontendAction {
    Collector& output;
    Matches callback;
    MatchFinder finder;
public:
    explicit Action(Collector& out) : output(out), callback(out) {
        finder.addMatcher(functionDecl(unless(isImplicit())).bind("symbol"), &callback);
        finder.addMatcher(recordDecl(unless(isImplicit())).bind("symbol"), &callback);
        finder.addMatcher(varDecl(unless(isImplicit())).bind("symbol"), &callback);
        finder.addMatcher(fieldDecl(unless(isImplicit())).bind("symbol"), &callback);
    }
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, llvm::StringRef) override {
        compiler.getPreprocessor().addPPCallbacks(std::make_unique<Includes>(output, compiler.getSourceManager()));
        return finder.newASTConsumer();
    }
};
class Factory final : public FrontendActionFactory {
    Collector& output;
public:
    explicit Factory(Collector& out) : output(out) {}
    std::unique_ptr<FrontendAction> create() override { return std::make_unique<Action>(output); }
};
class Diagnostics final : public DiagnosticConsumer {
public:
    std::string text;
    void HandleDiagnostic(DiagnosticsEngine::Level level, const clang::Diagnostic& diagnostic) override {
        DiagnosticConsumer::HandleDiagnostic(level, diagnostic);
        llvm::SmallString<256> message; diagnostic.FormatDiagnostic(message);
        if (diagnostic.hasSourceManager() && diagnostic.getLocation().isValid()) {
            const auto location = diagnostic.getSourceManager().getPresumedLoc(diagnostic.getLocation());
            if (location.isValid()) text += std::string(location.getFilename()) + ":" + std::to_string(location.getLine()) + ": ";
        }
        text += message.str().str() + "\n";
    }
};
class OneCommand final : public CompilationDatabase {
    CompileCommand command;
public:
    explicit OneCommand(CompileCommand value) : command(std::move(value)) {}
    std::vector<CompileCommand> getCompileCommands(llvm::StringRef) const override { return {command}; }
};
bool safe_arguments(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        for (const auto* prefix : {"@", "--config", "-cc1", "-Xclang", "-load", "-plugin", "-fplugin", "-fpass-plugin", "-fmodules", "-fmodule-",
            "-include-pch", "-include-pth", "-emit-", "-serialize-diagnostics", "--serialize-diagnostics",
            "-save-temps", "-MJ", "-fprofile", "-ftime-trace", "-gen-", "-ivfsoverlay"})
            if (arg.starts_with(prefix)) return false;
    }
    return true;
}
std::string command_id(const CompileCommand& command) {
    llvm::SHA256 hash;
    auto add=[&](const std::string& s){hash.update(s);hash.update(llvm::StringRef("\0",1));};
    add(command.Directory);add(command.Filename);add(command.Output);for(const auto& arg:command.CommandLine)add(arg);
    const auto value=hash.final();std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto c:value)out<<std::setw(2)<<static_cast<unsigned>(c);return out.str();
}
std::vector<CompileCommand> read_commands(const std::string& input) {
    auto path=fs::canonical(from_utf8(input));if(fs::is_directory(path))path/="compile_commands.json";
    std::string error;auto db=JSONCompilationDatabase::loadFromFile(utf8_path(path),error,JSONCommandLineSyntax::AutoDetect);
    if(!db)throw std::invalid_argument("invalid compilation database: "+error);
    auto commands=db->getAllCompileCommands();
    for(auto& command:commands) {
        auto dir=from_utf8(command.Directory);if(!dir.is_absolute())dir=path.parent_path()/dir;
        command.Directory=utf8_path(fs::weakly_canonical(dir));
        auto file=from_utf8(command.Filename);if(!file.is_absolute())file=dir/file;
        command.Filename=utf8_path(fs::weakly_canonical(file));
    }
    return commands;
}
} // namespace
bool clang_analysis_available() { return true; }
std::vector<CompileCommandInfo> inspect_compile_commands(const std::string& database) {
    std::vector<CompileCommandInfo> result;
    for(const auto& command:read_commands(database))result.push_back({key(from_utf8(command.Filename)),command_id(command),format_arguments(command.CommandLine)});
    return result;
}
std::string relocate_compile_commands(const std::string& input,const std::string& copied_source,const std::string& original_source,const std::string& output) {
    const auto from=utf8_path(fs::canonical(from_utf8(copied_source))),to=utf8_path(fs::canonical(from_utf8(original_source)));
    auto replace=[&](std::string value) {
        auto windows_from=from;std::replace(windows_from.begin(),windows_from.end(),'/','\\');
        for(const auto& prefix:std::vector<std::string>{from,windows_from})for(std::size_t p=0;(p=value.find(prefix,p))!=std::string::npos;) {
            const auto end=p+prefix.size();
            if(end==value.size()||value[end]=='/'||value[end]=='\\'){value.replace(p,prefix.size(),to);p+=to.size();}else p=end;
        }
        return value;
    };
    llvm::json::Array result;
    for(const auto& command:read_commands(input)) {
        llvm::json::Array args;for(const auto& arg:command.CommandLine)args.push_back(replace(arg));
        result.push_back(llvm::json::Object{{"directory",command.Directory},{"file",replace(command.Filename)},{"arguments",std::move(args)},{"output",command.Output}});
    }
    const auto path=from_utf8(output);if(fs::exists(path))throw std::invalid_argument("generated compilation database must use a new file");
    fs::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary);
    stream<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result))).str();
    if(!stream)throw std::runtime_error("cannot write prepared compilation database");return utf8_path(fs::absolute(path));
}
AnalysisResult analyze_project(const ScanResult& inventory, const std::string& database, const ScanContext& context, unsigned threads, const std::map<std::string,std::string>& choices,const std::vector<std::string>& disabled_rules) {
    std::map<std::string,std::string> selected_choices;
    for(const auto& [file,id]:choices)if(!selected_choices.emplace(key(from_utf8(file)),id).second)throw std::invalid_argument("duplicate canonical command choice");
    const auto started = std::chrono::steady_clock::now();
    if (threads > 64) throw std::invalid_argument("threads must be 0..64");
    context.report("analysis_setup");
    auto path = fs::canonical(from_utf8(database));
    if (fs::is_directory(path)) path /= "compile_commands.json";
    if (path.filename() != "compile_commands.json") throw std::invalid_argument("expected compile_commands.json or its directory");
    const auto commands=read_commands(utf8_path(path));
    AnalysisResult result; result.compile_commands = utf8_path(path);
    const auto packaged_resources=from_utf8(executable_directory())/"resources"/"clang";
    const auto resource_directory=fs::is_regular_file(packaged_resources/"include"/"stddef.h") ? utf8_path(packaged_resources) : std::string(CODEGUARD_RESOURCE_DIR);
    std::map<std::string, std::string> files;
    for (const auto& file : inventory.files) files[key(from_utf8(inventory.root) / from_utf8(file.path))] = file.path;
    std::map<std::string, std::vector<CompileCommand>> by_file;
    for (const auto& command : commands) {
        context.check();
        auto directory = from_utf8(command.Directory);
        if (!directory.is_absolute()) directory = path.parent_path() / directory;
        auto absolute = from_utf8(command.Filename);
        if (!absolute.is_absolute()) absolute = directory / absolute;
        auto adjusted = command; adjusted.Directory = utf8_path(fs::weakly_canonical(directory));
        by_file[key(absolute)].push_back(std::move(adjusted));
    }
    for(const auto& [file,id]:selected_choices) {
        const auto found=by_file.find(key(from_utf8(file)));
        if(found==by_file.end()||std::none_of(found->second.begin(),found->second.end(),[&](const auto& command){return command_id(command)==id;}))
            throw std::invalid_argument("saved compile command selection is stale; choose again: "+file);
    }
    std::map<std::string, Symbol> symbols;
    std::map<std::string, FunctionMetric> metrics;
    std::set<std::string> covered;
    int good = 0, bad = 0;
    const auto total = std::count_if(inventory.files.begin(), inventory.files.end(), [](const auto& file) { return file.language != "header"; });
    if (!threads) threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    result.workers = std::min<unsigned>(threads, std::max<std::size_t>(1, total));
    struct UnitOutput {
        TranslationUnitResult unit;
        std::map<std::string, Symbol> symbols;
        std::map<std::string, FunctionMetric> metrics;
        std::set<std::string> covered;
        std::vector<GraphEdge> edges;
        std::vector<Issue> issues;
    };
    std::mutex progress_mutex;
    std::size_t completed = 0;
    // Pool declared after shared state so workers are joined before it is destroyed.
    ThreadPool pool(result.workers, result.workers * 2);
    std::vector<std::future<UnitOutput>> pending;
    for (const auto& file : inventory.files) {
        if (file.language == "header") continue;
        context.check();
        pending.push_back(pool.submit([&, file] {
        { std::lock_guard lock(progress_mutex); context.report("analyzing", file.path, completed, total); }
        UnitOutput part;
        auto& unit = part.unit; unit.file = file.path;
        const auto absolute = from_utf8(inventory.root) / from_utf8(file.path);
        const auto found = by_file.find(key(absolute));
        const auto choice=selected_choices.find(key(absolute));
        const CompileCommand* selected=nullptr;
        if(found!=by_file.end()) {
            if(choice!=selected_choices.end()) {for(const auto& command:found->second)if(command_id(command)==choice->second){selected=&command;break;}}
            else if(found->second.size()==1)selected=&found->second.front();
        }
        if (found == by_file.end()) {
            unit.status = "missing_command"; unit.diagnostics = "No exact compilation database entry; no fallback flags guessed.";
        } else if (!selected) {
            unit.status = "ambiguous_command"; unit.diagnostics = "Multiple build configurations; select a command in project settings or provide a single-configuration compilation database.";
        } else if (!safe_arguments(selected->CommandLine)) {
            unit.status = "rejected_command"; unit.diagnostics = "Plugin, response-file, module or side-effect compiler option is not supported in read-only analysis.";
        } else {
            Collector output{files, selected->Directory,disabled_rules}; Factory factory(output); Diagnostics diagnostic;
            OneCommand command(*selected);
            // A private VFS working directory avoids mutating the GUI process CWD.
            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> filesystem(llvm::vfs::createPhysicalFileSystem().release());
            ClangTool tool(command, {utf8_path(absolute)}, std::make_shared<PCHContainerOperations>(), filesystem);
            tool.setDiagnosticConsumer(&diagnostic);
            tool.appendArgumentsAdjuster(getClangStripDependencyFileAdjuster());
            tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(CommandLineArguments{"-resource-dir="+resource_directory}, ArgumentInsertPosition::END));
            const auto code = tool.run(&factory);
            context.check(); // cancellation is cooperative at TU boundaries
            unit.status = code == 0 ? "success" : "parse_failed";
            unit.diagnostics = diagnostic.text;
            if(found->second.size()>1)unit.diagnostics="Selected configuration "+command_id(*selected)+"; "+std::to_string(found->second.size()-1)+" other command variants were not analyzed.\n"+unit.diagnostics;
            if (code == 0) {
                unit.indirect_calls = output.indirect;
                output.covered.insert(file.path);
                part.covered = std::move(output.covered);
                part.symbols = std::move(output.symbols); part.metrics = std::move(output.metrics);
                part.edges = std::move(output.result.edges); part.issues = std::move(output.result.issues);
            }
        }
        { std::lock_guard lock(progress_mutex); context.report("analyzing", file.path, ++completed, total); }
        return part;
        }));
    }
    // Deterministic merge in inventory order; no AST or SQLite objects cross workers.
    for (auto& future : pending) {
        auto part = future.get(); context.check(); const auto& unit = part.unit;
        if (unit.status == "success") ++good; else ++bad;
        result.units.push_back(unit);
        covered.insert(part.covered.begin(), part.covered.end());
        for (const auto& [id, symbol] : part.symbols) {
            const auto old = symbols.find(id);
            if (old == symbols.end() || (symbol.definition && !old->second.definition) ||
                (symbol.definition == old->second.definition && std::tie(symbol.file, symbol.line, symbol.column) <
                 std::tie(old->second.file, old->second.line, old->second.column))) symbols[id] = symbol;
        }
        for (const auto& [id, metric] : part.metrics) {
            auto old = metrics.find(id);
            if (old == metrics.end() || metric.complexity > old->second.complexity) metrics[id] = metric;
        }
        result.edges.insert(result.edges.end(), part.edges.begin(), part.edges.end());
        result.issues.insert(result.issues.end(), part.issues.begin(), part.issues.end());
    }
    result.status = good == 0 ? "failed" : (bad ? "partial" : "complete");
    for (const auto& [id, symbol] : symbols) result.symbols.push_back(symbol);
    for (const auto& [id, metric] : metrics) result.metrics.push_back(metric);
    result.covered_files.assign(covered.begin(), covered.end());
    auto tuple = [](const GraphEdge& edge) { return std::tie(edge.kind, edge.source, edge.target, edge.file, edge.line, edge.column); };
    std::sort(result.edges.begin(), result.edges.end(), [&](const auto& a, const auto& b) { return tuple(a) < tuple(b); });
    result.edges.erase(std::unique(result.edges.begin(), result.edges.end(), [&](const auto& a, const auto& b) { return tuple(a) == tuple(b); }), result.edges.end());
    auto issue_key = [](const Issue& issue) { return std::tie(issue.file, issue.line, issue.column, issue.rule_id, issue.symbol_id); };
    std::stable_sort(result.issues.begin(), result.issues.end(), [&](const auto& a, const auto& b) { return issue_key(a) < issue_key(b); });
    result.issues.erase(std::unique(result.issues.begin(), result.issues.end(), [&](const auto& a, const auto& b) { return issue_key(a) == issue_key(b); }), result.issues.end());
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    context.check();
    return result;
}
} // namespace codeguard
