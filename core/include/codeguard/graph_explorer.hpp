#pragma once
#include "codeguard/analysis.hpp"
#include <cstddef>
#include <map>

namespace codeguard {
struct GraphNode {
    std::string id, label, file;
    int line=0, column=0;
    bool external=false;
};
struct GraphLink {
    GraphEdge evidence; // deterministic first stored source position
    std::size_t location_count=0; // distinct file/line/column positions, not TU or runtime counts
};
struct GraphIndex {
    std::string kind, analysis_status;
    std::map<std::string,GraphNode> nodes;
    std::vector<GraphLink> links;
    std::map<std::string,std::vector<std::size_t>> outgoing,incoming;
};
enum class GraphDirection { outgoing, incoming, both };
struct GraphSearch {
    std::vector<GraphNode> nodes;
    std::size_t total=0;
};
struct GraphViewNode { GraphNode node; int layer=0; };
struct GraphSlice {
    std::vector<GraphViewNode> nodes;
    std::vector<GraphLink> links;
    bool node_limit_reached=false, edge_limit_reached=false;
    unsigned depth=0;
};
struct GraphPath {
    bool found=false;
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    std::size_t visited=0;
};
GraphIndex build_graph_index(const ScanResult& scan,const std::string& kind,const ScanContext& context={});
GraphSearch search_graph_nodes(const GraphIndex& graph,const std::string& query,std::size_t limit=100,const ScanContext& context={});
GraphSlice graph_neighborhood(const GraphIndex& graph,const std::string& focus,GraphDirection direction,
    unsigned depth=2,std::size_t max_nodes=100,std::size_t max_edges=400,const ScanContext& context={});
// Shortest directed path in the stored graph. No path is not a runtime
// unreachability proof; from==to is a zero-edge path, not cycle detection.
GraphPath shortest_graph_path(const GraphIndex& graph,const std::string& from,const std::string& to,const ScanContext& context={});
}
