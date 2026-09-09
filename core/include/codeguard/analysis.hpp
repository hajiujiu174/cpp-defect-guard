#pragma once
#include <cstdint>
#include <string>
#include <vector>
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
};
struct GraphEdge {
    std::string kind, source, target, file;
    int line = 0, column = 0;
};
struct TranslationUnitResult {
    std::string file, status, diagnostics;
    int indirect_calls = 0;
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
    unsigned workers = 0;
    std::int64_t elapsed_ms = 0;
};
bool clang_analysis_available();
AnalysisResult analyze_project(const ScanResult& inventory, const std::string& compilation_database, const ScanContext& context = {}, unsigned threads = 0);
std::vector<Symbol> find_symbols(const AnalysisResult& analysis, const std::string& prefix);

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
