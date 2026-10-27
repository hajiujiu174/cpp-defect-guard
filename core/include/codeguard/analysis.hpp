#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include "codeguard/scan_control.hpp"
#include "codeguard/rules.hpp"

namespace codeguard {
struct ScanResult;
struct Symbol {
    std::string id, kind, name, file;
    int line = 0, column = 0;
    bool definition = false, external = false;
};
struct FunctionMetric {
    std::string symbol_id;
    int lines = 0, parameters = 0, complexity = -1; // -1 = CFG unavailable
    int max_nesting = -1, fan_in = -1, fan_out = -1; // -1 = not recorded; fan counts observed direct endpoints
};
struct SymbolReference {
    std::string symbol_id, enclosing_symbol, kind, file;
    int line = 0, column = 0;
    std::string translation_unit; // retain each successful TU's evidence, including shared headers
    bool operator==(const SymbolReference&) const = default;
};
struct GraphEdge {
    std::string kind, source, target, file;
    int line = 0, column = 0;
};
struct TranslationUnitResult {
    std::string file, status, diagnostics;
    int indirect_calls = 0;
    std::string command_fingerprint = {};
    std::vector<std::string> covered_files = {}; // files contributed by this successful TU
};
struct CacheStatistics {
    std::size_t hits=0,misses=0,bypassed=0,errors=0;
    std::int64_t validation_ms=0; // sum across workers, not wall time
};
struct AnalysisResult {
    std::string status = "not_requested";
    std::string compile_commands;
    std::vector<Symbol> symbols;
    std::vector<FunctionMetric> metrics;
    std::vector<GraphEdge> edges;
    std::vector<TranslationUnitResult> units;
    std::vector<std::string> covered_files;
    std::vector<Issue> issues;
    std::vector<Issue> suppressed_issues;
    std::vector<std::string> rule_diagnostics;
    unsigned workers = 0;
    std::int64_t elapsed_ms = 0;
    std::string configuration;
    std::string analyzer_revision; // analyzer sources + linked Clang version; empty in old snapshots
    std::optional<CacheStatistics> cache; // telemetry of a live run; not historical evidence
    std::string reference_index; // empty = unavailable/old snapshot; resolved_ast_v1 = syntactic resolved uses
    std::vector<SymbolReference> references;
    std::vector<RepairHint> repair_hints; // opt-in fresh analysis only; never a historical repair claim
};
bool clang_analysis_available();
AnalysisResult analyze_project(const ScanResult& inventory, const std::string& compilation_database, const ScanContext& context = {}, unsigned threads = 0, const std::map<std::string,std::string>& choices = {},const std::vector<std::string>& disabled_rules = {},const std::string& cache_directory = {},bool collect_repair_hints=false);
struct CompileCommandInfo { std::string file, fingerprint, display; };
std::vector<CompileCommandInfo> inspect_compile_commands(const std::string& database);
// Rebase copied source paths, while keeping the generated build directory and headers.
// Optional stripping is limited to the three codegen-only gcov flags injected
// by repair validation; other profiling/plugin options remain subject to checks.
std::string relocate_compile_commands(const std::string& input,const std::string& copied_source,const std::string& original_source,const std::string& output,bool strip_gcov_codegen=false);
std::vector<Symbol> find_symbols(const AnalysisResult& analysis, const std::string& prefix);
std::vector<SymbolReference> find_references(const AnalysisResult& analysis, const std::string& symbol_id, const ScanContext& context = {});

struct GraphSummary {
    std::vector<std::string> nodes;
    std::size_t edge_count = 0; // unique endpoint pairs
    std::vector<std::vector<std::string>> components, cycles;
    std::vector<std::string> topological_order; // source before target; empty if cyclic
};
GraphSummary summarize_graph(const std::vector<std::string>& nodes, const std::vector<GraphEdge>& edges);
GraphSummary project_graph(const ScanResult& scan, const std::string& kind);
std::vector<std::string> reachable(const std::string& start, const std::vector<GraphEdge>& edges);
} // namespace codeguard
