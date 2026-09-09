#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using llvm::json::Array;
using llvm::json::Object;

namespace {
llvm::cl::OptionCategory Category("defectguard-clang options");

// Stable local IDs follow traversal order, not memory addresses.
class FunctionGraph {
    ASTContext& context;
    const SourceManager& sources;
    const FunctionDecl* function;
    std::map<const Stmt*, std::string> statements;
    std::map<const VarDecl*, std::string> declarations, symbols;
    Array astNodes, astEdges, findings;
    unsigned nodeCounter = 0;

    std::string symbol(const VarDecl* declaration) {
        declaration = declaration->getCanonicalDecl();
        auto found = symbols.find(declaration);
        if (found != symbols.end()) return found->second;
        std::string id = "V" + std::to_string(symbols.size());
        symbols.emplace(declaration, id);
        return id;
    }

    std::string addNode(llvm::StringRef kind, SourceRange range,
                        llvm::StringRef type = "", llvm::StringRef name = "",
                        llvm::StringRef variable = "") {
        std::string id = "A" + std::to_string(nodeCounter++);
        auto begin = sources.getExpansionLoc(range.getBegin());
        auto end = sources.getExpansionLoc(range.getEnd());
        astNodes.push_back(Object{
            {"id", id}, {"kind", kind.str()}, {"name", name.str()},
            {"type", type.str()}, {"symbol", variable.str()},
            {"line", begin.isValid() ? sources.getExpansionLineNumber(begin) : 0},
            {"column", begin.isValid() ? sources.getExpansionColumnNumber(begin) : 0},
            {"end_line", end.isValid() ? sources.getExpansionLineNumber(end) : 0}});
        return id;
    }

    void connect(const std::string& parent, const std::string& child) {
        astEdges.push_back(Object{{"source", parent}, {"target", child}, {"kind", "ast-child"}});
    }

    void finding(llvm::StringRef rule, const Stmt* statement, Object details) {
        SourceLocation location = sources.getExpansionLoc(statement->getBeginLoc());
        if (location.isInvalid()) return;
        details["rule_id"] = rule.str();
        details["file"] = sources.getFilename(location).str();
        details["line"] = sources.getExpansionLineNumber(location);
        details["column"] = sources.getExpansionColumnNumber(location);
        details["ast_node"] = statements.at(statement);
        findings.push_back(std::move(details));
    }

    void inspect(const Stmt* statement) {
        if (const auto* call = dyn_cast<CallExpr>(statement)) {
            const FunctionDecl* callee = call->getDirectCallee();
            if (callee && callee->getIdentifier()) {
                std::string name = callee->getNameAsString();
                const bool libraryFunction = callee->getBuiltinID() != 0 ||
                    sources.isInSystemHeader(callee->getLocation());
                if (libraryFunction && !callee->isCXXClassMember() &&
                    (name == "gets" || name == "strcpy" || name == "strcat" || name == "sprintf"))
                    finding("DG001", statement, Object{{"callee", name}});
            }
        }
        if (const auto* access = dyn_cast<ArraySubscriptExpr>(statement)) {
            const Expr* base = access->getBase()->IgnoreParenImpCasts();
            const auto* array = context.getAsConstantArrayType(base->getType());
            Expr::EvalResult result;
            if (array && !access->getIdx()->isValueDependent() &&
                access->getIdx()->EvaluateAsInt(result, context)) {
                const auto& index = result.Val.getInt();
                if (index.isNegative() || index.getLimitedValue() >= array->getSize().getLimitedValue()) {
                    llvm::SmallString<32> indexText, sizeText;
                    index.toString(indexText, 10);
                    array->getSize().toStringUnsigned(sizeText);
                    std::string name = "array";
                    if (const auto* reference = dyn_cast<DeclRefExpr>(base))
                        name = reference->getDecl()->getNameAsString();
                    finding("DG002", statement, Object{
                        {"variable", name}, {"index", indexText.str().str()},
                        {"size", sizeText.str().str()}});
                }
            }
        }
        const Expr* condition = nullptr;
        if (const auto* branch = dyn_cast<IfStmt>(statement)) condition = branch->getCond();
        if (const auto* loop = dyn_cast<WhileStmt>(statement)) condition = loop->getCond();
        if (const auto* loop = dyn_cast<DoStmt>(statement)) condition = loop->getCond();
        if (const auto* loop = dyn_cast<ForStmt>(statement)) condition = loop->getCond();
        // Extra parentheses or an explicit comparison express deliberate assignment.
        if (condition && !isa<ParenExpr>(condition->IgnoreImpCasts())) {
            const auto* operation = dyn_cast<BinaryOperator>(condition->IgnoreParenImpCasts());
            if (operation && operation->isAssignmentOp())
                finding("DG004", statement, Object{{"operator", operation->getOpcodeStr().str()}});
        }
    }

    std::string addDeclaration(const VarDecl* declaration, const std::string& parent) {
        std::string id = addNode(isa<ParmVarDecl>(declaration) ? "ParmVarDecl" : "VarDecl",
            declaration->getSourceRange(), declaration->getType().getAsString(),
            declaration->getNameAsString(), symbol(declaration));
        declarations[declaration->getCanonicalDecl()] = id;
        connect(parent, id);
        return id;
    }

    void walk(const Stmt* statement, const std::string& parent, bool evaluated = true) {
        if (!statement) return;
        auto existing = statements.find(statement);
        if (existing != statements.end()) {
            connect(parent, existing->second);
            return;
        }
        std::string type, name, variable;
        if (const auto* expression = dyn_cast<Expr>(statement)) type = expression->getType().getAsString();
        if (const auto* reference = dyn_cast<DeclRefExpr>(statement)) {
            name = reference->getDecl()->getNameAsString();
            if (const auto* declaration = dyn_cast<VarDecl>(reference->getDecl())) variable = symbol(declaration);
        }
        if (const auto* call = dyn_cast<CallExpr>(statement))
            if (const auto* callee = call->getDirectCallee()) name = callee->getQualifiedNameAsString();
        if (const auto* operation = dyn_cast<BinaryOperator>(statement)) name = operation->getOpcodeStr().str();
        if (const auto* operation = dyn_cast<UnaryOperator>(statement)) name = UnaryOperator::getOpcodeStr(operation->getOpcode()).str();
        if (const auto* literal = dyn_cast<IntegerLiteral>(statement)) {
            llvm::SmallString<40> value;
            literal->getValue().toStringUnsigned(value);
            name = value.str().str();
        }
        if (const auto* literal = dyn_cast<CharacterLiteral>(statement)) name = std::to_string(literal->getValue());
        std::string id = addNode(statement->getStmtClassName(), statement->getSourceRange(), type, name, variable);
        statements[statement] = id;
        connect(parent, id);
        if (evaluated) inspect(statement);
        const bool childrenEvaluated = evaluated && !isa<UnaryExprOrTypeTraitExpr>(statement) &&
            !isa<CXXNoexceptExpr>(statement) && !isa<CXXTypeidExpr>(statement);
        if (const auto* declarationStatement = dyn_cast<DeclStmt>(statement)) {
            for (const Decl* declaration : declarationStatement->decls()) {
                if (const auto* variableDecl = dyn_cast<VarDecl>(declaration)) {
                    auto variableId = addDeclaration(variableDecl, id);
                    walk(variableDecl->getInit(), variableId, childrenEvaluated);
                }
            }
        } else if (const auto* lambda = dyn_cast<LambdaExpr>(statement)) {
            // Its call operator is collected as a separate function.
            for (const Expr* capture : lambda->capture_inits()) walk(capture, id, childrenEvaluated);
        } else {
            for (const Stmt* child : statement->children()) walk(child, id, childrenEvaluated);
        }
    }

    const VarDecl* directLocal(const Expr* expression) const {
        if (!expression) return nullptr;
        const auto* reference = dyn_cast<DeclRefExpr>(expression->IgnoreParenImpCasts());
        if (!reference) return nullptr;
        const auto* variable = dyn_cast<VarDecl>(reference->getDecl());
        if (!variable || !variable->hasLocalStorage() || !variable->getType()->isScalarType()) return nullptr;
        return variable->getCanonicalDecl();
    }

    void event(Array& events, llvm::StringRef kind, const VarDecl* variable,
               const std::string& astId) {
        if (variable) events.push_back(Object{{"kind", kind.str()},
            {"symbol", symbol(variable)}, {"node", astId}});
    }

    void collectEvents(const Stmt* statement, Array& events) {
        // CFG splits multi-variable DeclStmt into synthetic single declarations.
        if (const auto* declarationStatement = dyn_cast<DeclStmt>(statement)) {
            for (const Decl* declaration : declarationStatement->decls()) {
                const auto* variable = dyn_cast<VarDecl>(declaration);
                if (!variable || !variable->hasLocalStorage() || !variable->getType()->isScalarType()) continue;
                auto found = declarations.find(variable->getCanonicalDecl());
                if (found != declarations.end())
                    event(events, variable->hasInit() ? "def" : "undef", variable, found->second);
            }
            return;
        }
        auto position = statements.find(statement);
        if (position == statements.end()) return;
        const std::string& id = position->second;
        if (const auto* cast = dyn_cast<ImplicitCastExpr>(statement))
            if (cast->getCastKind() == CK_LValueToRValue)
                event(events, "use", directLocal(cast->getSubExpr()), id);
        if (const auto* assignment = dyn_cast<BinaryOperator>(statement)) {
            if (assignment->isAssignmentOp()) {
                const auto* variable = directLocal(assignment->getLHS());
                if (assignment->isCompoundAssignmentOp()) event(events, "use", variable, id);
                event(events, "def", variable, id);
            }
        }
        if (const auto* operation = dyn_cast<UnaryOperator>(statement)) {
            if (operation->isIncrementDecrementOp()) {
                const auto* variable = directLocal(operation->getSubExpr());
                event(events, "use", variable, id);
                event(events, "def", variable, id);
            }
        }

    }

public:
    FunctionGraph(ASTContext& context, const FunctionDecl* function)
        : context(context), sources(context.getSourceManager()), function(function) {}

    Object build() {
        std::string root = addNode("FunctionDecl", function->getSourceRange(),
            function->getType().getAsString(), function->getQualifiedNameAsString());
        for (const ParmVarDecl* parameter : function->parameters()) addDeclaration(parameter, root);
        walk(function->getBody(), root);
        CFG::BuildOptions options;
        options.setAllAlwaysAdd();
        std::unique_ptr<CFG> cfg = CFG::buildCFG(function, function->getBody(), &context, options);
        Array cfgNodes, cfgEdges;
        unsigned complexity = 1;
        if (cfg) {
            for (const CFGBlock* block : *cfg) {
                if (!block) continue;
                std::string id = "B" + std::to_string(block->getBlockID());
                std::string kind = block == &cfg->getEntry() ? "entry" :
                    (block == &cfg->getExit() ? "exit" : "block");
                Array mapped, events;
                unsigned line = sources.getExpansionLineNumber(function->getBeginLoc());
                if (block == &cfg->getEntry())
                    for (const ParmVarDecl* parameter : function->parameters())
                        if (parameter->getType()->isScalarType())
                            event(events, "def", parameter, declarations.at(parameter->getCanonicalDecl()));
                for (const CFGElement& element : *block) {
                    if (auto cfgStatement = element.getAs<CFGStmt>()) {
                        const Stmt* statement = cfgStatement->getStmt();
                        auto found = statements.find(statement);
                        if (found != statements.end()) {
                            if (mapped.empty()) line = sources.getExpansionLineNumber(statement->getBeginLoc());
                            mapped.push_back(found->second);
                        } else if (const auto* synthetic = dyn_cast<DeclStmt>(statement)) {
                            for (const Decl* declaration : synthetic->decls()) {
                                if (const auto* variable = dyn_cast<VarDecl>(declaration)) {
                                    auto node = declarations.find(variable->getCanonicalDecl());
                                    if (node != declarations.end()) mapped.push_back(node->second);
                                }
                            }
                        }
                        collectEvents(statement, events);
                    }
                }
                cfgNodes.push_back(Object{{"id", id}, {"line", line}, {"kind", kind},
                    {"label", id}, {"ast_nodes", std::move(mapped)}, {"events", std::move(events)}});
                if (block->succ_size() > 1) complexity += block->succ_size() - 1;
                for (auto successor = block->succ_begin(); successor != block->succ_end(); ++successor)
                    if (const CFGBlock* target = successor->getReachableBlock())
                        cfgEdges.push_back(Object{{"source", id},
                            {"target", "B" + std::to_string(target->getBlockID())}, {"kind", "successor"}});
            }
        }
        auto location = sources.getExpansionLoc(function->getLocation());
        auto sourceBegin = sources.getExpansionLoc(function->getBeginLoc());
        auto sourceEnd = Lexer::getLocForEndOfToken(
            sources.getExpansionLoc(function->getEndLoc()), 0, sources, context.getLangOpts());
        const bool hasSourceRange = sourceBegin.isValid() && sourceEnd.isValid() &&
            sources.getFileID(sourceBegin) == sources.getFileID(sourceEnd);
        return Object{
            {"name", function->getQualifiedNameAsString()}, {"signature", function->getType().getAsString()},
            {"return_type", function->getReturnType().getAsString()},
            {"file", sources.getFilename(location).str()},
            {"start_line", sources.getExpansionLineNumber(function->getBeginLoc())},
            {"end_line", sources.getExpansionLineNumber(function->getEndLoc())},
            {"source_start", hasSourceRange ? static_cast<int64_t>(sources.getFileOffset(sourceBegin)) : -1},
            {"source_end", hasSourceRange ? static_cast<int64_t>(sources.getFileOffset(sourceEnd)) : -1},
            {"parameter_count", function->getNumParams()}, {"cyclomatic_complexity", complexity},
            {"ast", Object{{"nodes", std::move(astNodes)}, {"edges", std::move(astEdges)}}},
            {"cfg", Object{{"nodes", std::move(cfgNodes)}, {"edges", std::move(cfgEdges)}}},
            {"dataflow_status", cfg ? "local-scalar-events" : "unavailable-cfg"},
            {"findings", std::move(findings)}};
    }
};

class Collector final : public MatchFinder::MatchCallback {
    Array functions;
    std::set<std::string> seen;
public:
    void run(const MatchFinder::MatchResult& result) override {
        const auto* declaration = result.Nodes.getNodeAs<FunctionDecl>("function");
        if (!declaration || !result.SourceManager || !result.Context || !declaration->hasBody()) return;
        auto location = result.SourceManager->getExpansionLoc(declaration->getLocation());
        if (location.isInvalid() || result.SourceManager->isInSystemHeader(location)) return;
        std::string key = result.SourceManager->getFilename(location).str() + ":" +
            std::to_string(result.SourceManager->getFileOffset(location)) + ":" + declaration->getType().getAsString();
        if (!seen.insert(key).second) return;
        functions.push_back(FunctionGraph(*result.Context, declaration).build());
    }
    void write() {
        llvm::outs() << llvm::json::Value(Object{
            {"schema_version", "1.1"}, {"native_rules", Array{"DG001", "DG002", "DG004"}},
            {"functions", std::move(functions)}}) << '\n';
    }
};
}  // namespace

int main(int argc, const char** argv) {
    llvm::InitLLVM initialization(argc, argv);
    auto options = CommonOptionsParser::create(argc, argv, Category);
    if (!options) {
        llvm::errs() << llvm::toString(options.takeError()) << '\n';
        return 2;
    }
    ClangTool tool(options->getCompilations(), options->getSourcePathList());
#ifdef DEFECTGUARD_RESOURCE_DIR
    tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        "-resource-dir=" DEFECTGUARD_RESOURCE_DIR, ArgumentInsertPosition::END));
#endif
    Collector collector;
    MatchFinder finder;
    finder.addMatcher(functionDecl(isDefinition()).bind("function"), &collector);
    int result = tool.run(newFrontendActionFactory(&finder).get());
    if (result != 0) return result;
    collector.write();
    return 0;
}
