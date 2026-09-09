#include "codeguard/application.hpp"
#include "codeguard/thread_pool.hpp"
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
    Collector(const std::map<std::string, std::string>& files, const std::string& directory)
        : inventory(files), working_directory(from_utf8(directory)) {}
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
        inspect_rules(function->getBody(), *match.Context, id);
    }
private:
    void add_issue(const Stmt* statement, ASTContext& context, const std::string& id, std::size_t rule,
                   const std::string& message, const std::string& suggestion) {
        auto& sources = context.getSourceManager();
        const auto location = sources.getExpansionLoc(statement->getBeginLoc());
        if (location.isInvalid() || !output.internal(sources.getFilename(location))) return;
        const auto& info = rule_catalog().at(rule);
        auto evidence = Lexer::getSourceText(CharSourceRange::getTokenRange(statement->getSourceRange()), sources, context.getLangOpts()).str();
        if (evidence.size() > 500) evidence = evidence.substr(0, 500) + "...";
        output.result.issues.push_back({info.id, info.severity, output.path(sources.getFilename(location)),
            static_cast<int>(sources.getExpansionLineNumber(location)), static_cast<int>(sources.getExpansionColumnNumber(location)),
            message, evidence, suggestion, id});
    }
    void inspect_rules(const Stmt* statement, ASTContext& context, const std::string& id, bool address = false) {
        if (!statement || isa<UnaryExprOrTypeTraitExpr>(statement) || isa<CXXNoexceptExpr>(statement) || isa<CXXTypeidExpr>(statement)) return;
        if (const auto* lambda = dyn_cast<LambdaExpr>(statement)) {
            for (const auto* capture : lambda->capture_inits()) inspect_rules(capture, context, id);
            return; // the call operator gets its own callback
        }
        if (const auto* call = dyn_cast<CallExpr>(statement)) {
            const auto* callee = call->getDirectCallee();
            if (callee && callee->getIdentifier() && !callee->isCXXClassMember() &&
                (callee->getBuiltinID() || context.getSourceManager().isInSystemHeader(callee->getLocation()))) {
                const auto name = callee->getNameAsString();
                if (name == "gets" || name == "strcpy" || name == "strcat" || name == "sprintf")
                    add_issue(statement, context, id, 0, "Unbounded library call: " + name,
                        "Use a capacity-aware operation and check destination size; this is an API-risk warning, not proof of overflow.");
            }
        }
        if (const auto* access = dyn_cast<ArraySubscriptExpr>(statement)) {
            const auto* array = context.getAsConstantArrayType(access->getBase()->IgnoreParenImpCasts()->getType());
            Expr::EvalResult value;
            if (array && !access->getIdx()->isValueDependent() && access->getIdx()->EvaluateAsInt(value, context)) {
                const auto& index = value.Val.getInt(); const auto size = array->getSize().getLimitedValue();
                if (index.isNegative() || index.getLimitedValue() > size || (!address && index.getLimitedValue() == size))
                    add_issue(statement, context, id, 1, "Constant index is outside the array extent " + std::to_string(size),
                        "Keep evaluated indices in [0, size); one-past address formation is permitted.");
            }
        }
        const Expr* condition = nullptr;
        if (const auto* s = dyn_cast<IfStmt>(statement)) condition = s->getCond();
        if (const auto* s = dyn_cast<WhileStmt>(statement)) condition = s->getCond();
        if (const auto* s = dyn_cast<DoStmt>(statement)) condition = s->getCond();
        if (const auto* s = dyn_cast<ForStmt>(statement)) condition = s->getCond();
        if (condition && !isa<ParenExpr>(condition->IgnoreImpCasts())) {
            const auto* assignment = dyn_cast<BinaryOperator>(condition->IgnoreParenImpCasts());
            if (assignment && assignment->isAssignmentOp()) add_issue(condition, context, id, 2,
                "Assignment directly controls a branch", "Use comparison if intended; otherwise parenthesize the deliberate assignment.");
        }
        if (const auto* ret = dyn_cast<ReturnStmt>(statement)) {
            const Expr* expression = ret->getRetValue();
            if (expression && expression->getType()->isPointerType()) {
                expression = expression->IgnoreParenImpCasts(); bool address_of = false;
                if (const auto* unary = dyn_cast<UnaryOperator>(expression); unary && unary->getOpcode() == UO_AddrOf) {
                    expression = unary->getSubExpr()->IgnoreParenImpCasts(); address_of = true;
                }
                if (const auto* ref = dyn_cast<DeclRefExpr>(expression)) {
                    const auto* var = dyn_cast<VarDecl>(ref->getDecl());
                    if (var && var->hasLocalStorage() && !var->getType()->isReferenceType() &&
                        (address_of || var->getType()->isArrayType())) add_issue(statement, context, id, 3,
                            "Returning storage owned by automatic local " + var->getNameAsString(), "Return a value or use storage whose lifetime outlives the call.");
                }
            }
        }
        const Expr* pointer = nullptr;
        if (const auto* unary = dyn_cast<UnaryOperator>(statement); unary && unary->getOpcode() == UO_Deref && !address) pointer = unary->getSubExpr();
        if (const auto* member = dyn_cast<MemberExpr>(statement); member && member->isArrow()) pointer = member->getBase();
        if (pointer && pointer->getType()->isPointerType() && !pointer->isValueDependent() &&
            pointer->IgnoreParenCasts()->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNotNull) != Expr::NPCK_NotNull)
            add_issue(statement, context, id, 4, "Dereference of a constant null pointer", "Provide a valid object before dereferencing this pointer.");
        if (const auto* declaration = dyn_cast<DeclStmt>(statement)) {
            for (const auto* decl : declaration->decls()) if (const auto* var = dyn_cast<VarDecl>(decl)) inspect_rules(var->getInit(), context, id);
            return;
        }
        bool child_address = address && (isa<ParenExpr>(statement) || isa<ImplicitCastExpr>(statement));
        if (const auto* unary = dyn_cast<UnaryOperator>(statement)) child_address = unary->getOpcode() == UO_AddrOf;
        for (const auto* child : statement->children()) inspect_rules(child, context, id, child_address);
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
} // namespace
bool clang_analysis_available() { return true; }
AnalysisResult analyze_project(const ScanResult& inventory, const std::string& database, const ScanContext& context, unsigned threads) {
    const auto started = std::chrono::steady_clock::now();
    if (threads > 64) throw std::invalid_argument("threads must be 0..64");
    context.report("analysis_setup");
    auto path = fs::canonical(from_utf8(database));
    if (fs::is_directory(path)) path /= "compile_commands.json";
    if (path.filename() != "compile_commands.json") throw std::invalid_argument("expected compile_commands.json or its directory");
    std::string error;
    const auto commands = JSONCompilationDatabase::loadFromFile(utf8_path(path), error, JSONCommandLineSyntax::AutoDetect);
    if (!commands) throw std::invalid_argument("invalid compilation database: " + error);
    AnalysisResult result; result.compile_commands = utf8_path(path);
    const auto packaged_resources=from_utf8(executable_directory())/"resources"/"clang";
    const auto resource_directory=fs::is_regular_file(packaged_resources/"include"/"stddef.h") ? utf8_path(packaged_resources) : std::string(CODEGUARD_RESOURCE_DIR);
    std::map<std::string, std::string> files;
    for (const auto& file : inventory.files) files[key(from_utf8(inventory.root) / from_utf8(file.path))] = file.path;
    std::map<std::string, std::vector<CompileCommand>> by_file;
    for (const auto& command : commands->getAllCompileCommands()) {
        context.check();
        auto directory = from_utf8(command.Directory);
        if (!directory.is_absolute()) directory = path.parent_path() / directory;
        auto absolute = from_utf8(command.Filename);
        if (!absolute.is_absolute()) absolute = directory / absolute;
        auto adjusted = command; adjusted.Directory = utf8_path(fs::weakly_canonical(directory));
        by_file[key(absolute)].push_back(std::move(adjusted));
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
        if (found == by_file.end()) {
            unit.status = "missing_command"; unit.diagnostics = "No exact compilation database entry; no fallback flags guessed.";
        } else if (found->second.size() != 1) {
            unit.status = "ambiguous_command"; unit.diagnostics = "Multiple build configurations; provide a single-configuration compilation database.";
        } else if (!safe_arguments(found->second[0].CommandLine)) {
            unit.status = "rejected_command"; unit.diagnostics = "Plugin, response-file, module or side-effect compiler option is not supported in read-only analysis.";
        } else {
            Collector output{files, found->second[0].Directory}; Factory factory(output); Diagnostics diagnostic;
            OneCommand command(found->second[0]);
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
