#include "codeguard/application.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>

namespace codeguard {
std::vector<Symbol> find_symbols(const AnalysisResult& analysis, const std::string& prefix) {
    std::vector<Symbol> result;
    for (const auto& symbol : analysis.symbols)
        if (!symbol.external && symbol.name.starts_with(prefix)) result.push_back(symbol);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.file, a.line, a.id) < std::tie(b.name, b.file, b.line, b.id);
    });
    return result;
}
using Adjacency = std::map<std::string, std::set<std::string>>;
GraphSummary summarize_graph(const std::vector<std::string>& nodes, const std::vector<GraphEdge>& edges) {
    Adjacency forward, reverse;
    for (const auto& node : nodes) { forward[node]; reverse[node]; }
    for (const auto& edge : edges) {
        if (edge.source.empty() || edge.target.empty()) throw std::invalid_argument("empty graph endpoint");
        forward[edge.source].insert(edge.target); forward[edge.target];
        reverse[edge.target].insert(edge.source); reverse[edge.source];
    }
    GraphSummary result;
    std::set<std::string> seen;
    std::vector<std::string> finish;
    // Iterative DFS postorder: safe for deep project dependency chains.
    for (const auto& [node, next] : forward) {
        result.nodes.push_back(node); result.edge_count += next.size();
        if (!seen.insert(node).second) continue;
        using Iterator = std::set<std::string>::const_iterator;
        std::vector<std::pair<std::string, Iterator>> stack{{node, forward.at(node).begin()}};
        while (!stack.empty()) {
            auto& [current, cursor] = stack.back();
            if (cursor == forward.at(current).end()) { finish.push_back(current); stack.pop_back(); }
            else {
                auto child = *cursor++;
                if (seen.insert(child).second) stack.emplace_back(child, forward.at(child).begin());
            }
        }
    }
    seen.clear();
    for (auto item = finish.rbegin(); item != finish.rend(); ++item) {
        if (!seen.insert(*item).second) continue;
        std::vector<std::string> component, stack{*item};
        while (!stack.empty()) {
            auto node = stack.back(); stack.pop_back(); component.push_back(node);
            for (const auto& child : reverse.at(node)) if (seen.insert(child).second) stack.push_back(child);
        }
        std::sort(component.begin(), component.end());
        result.components.push_back(component);
        if (component.size() > 1 || forward.at(component[0]).contains(component[0])) result.cycles.push_back(component);
    }
    std::sort(result.components.begin(), result.components.end());
    std::sort(result.cycles.begin(), result.cycles.end());
    std::map<std::string, std::size_t> incoming;
    std::set<std::string> ready;
    for (const auto& [node, previous] : reverse) {
        incoming[node] = previous.size(); if (previous.empty()) ready.insert(node);
    }
    while (!ready.empty()) {
        const auto node = *ready.begin(); ready.erase(ready.begin()); result.topological_order.push_back(node);
        for (const auto& child : forward.at(node)) if (--incoming[child] == 0) ready.insert(child);
    }
    if (result.topological_order.size() != result.nodes.size()) result.topological_order.clear();
    return result;
}
GraphSummary project_graph(const ScanResult& scan, const std::string& kind) {
    if (kind != "call" && kind != "include") throw std::invalid_argument("graph kind must be call or include");
    std::vector<std::string> nodes;
    if (kind == "call") {
        for (const auto& symbol : scan.analysis.symbols) if (symbol.kind == "function") nodes.push_back(symbol.id);
    } else for (const auto& file : scan.files) nodes.push_back(file.path);
    std::vector<GraphEdge> edges;
    for (const auto& edge : scan.analysis.edges) if (edge.kind == kind) edges.push_back(edge);
    return summarize_graph(nodes, edges);
}
std::vector<std::string> reachable(const std::string& start, const std::vector<GraphEdge>& edges) {
    Adjacency graph;
    for (const auto& edge : edges) graph[edge.source].insert(edge.target);
    std::set<std::string> seen{start};
    std::queue<std::string> queue; queue.push(start);
    std::vector<std::string> result;
    while (!queue.empty()) {
        const auto node = queue.front(); queue.pop(); result.push_back(node);
        for (const auto& child : graph[node]) if (seen.insert(child).second) queue.push(child);
    }
    return result;
}
} // namespace codeguard
