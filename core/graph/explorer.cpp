#include "codeguard/graph_explorer.hpp"
#include "codeguard/application.hpp"
#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <tuple>

namespace codeguard {
namespace {
void require_node(const GraphIndex& graph,const std::string& id) {
    if(!graph.nodes.contains(id))throw std::invalid_argument("Unknown graph node in this snapshot: "+id);
}
const std::vector<std::size_t>& neighbors(const std::map<std::string,std::vector<std::size_t>>& adjacency,const std::string& id) {
    static const std::vector<std::size_t> empty;
    const auto found=adjacency.find(id);return found==adjacency.end()?empty:found->second;
}
}
GraphIndex build_graph_index(const ScanResult& scan,const std::string& kind,const ScanContext& context) {
    context.check();
    if(kind!="call"&&kind!="include")throw std::invalid_argument("Graph kind must be call or include");
    if(scan.analysis.status!="complete"&&scan.analysis.status!="partial")
        throw std::invalid_argument("Graph analysis unavailable; rescan with Clang analysis");
    GraphIndex result;result.kind=kind;result.analysis_status=scan.analysis.status;
    if(kind=="call") {
        for(const auto& s:scan.analysis.symbols){context.check();if(s.kind=="function")result.nodes[s.id]={s.id,s.name,s.file,s.line,s.column,s.external};}
    } else {
        for(const auto& f:scan.files){context.check();result.nodes[f.path]={f.path,f.path,f.path,1,1,false};}
    }
    using Position=std::tuple<std::string,int,int>;
    struct Locations { GraphEdge first; std::set<Position> positions; };
    std::map<std::pair<std::string,std::string>,Locations> pairs;
    for(const auto& e:scan.analysis.edges) {
        context.check();if(e.kind!=kind)continue;
        if(e.source.empty()||e.target.empty())throw std::invalid_argument("Empty graph endpoint");
        for(const auto* id:{&e.source,&e.target})if(!result.nodes.contains(*id))
            result.nodes[*id]={*id,*id,kind=="include"?*id:"",kind=="include"?1:0,kind=="include"?1:0,true};
        const Position position{e.file,e.line,e.column};
        auto& locations=pairs[{e.source,e.target}];
        if(locations.positions.empty()||position<Position{locations.first.file,locations.first.line,locations.first.column})locations.first=e;
        locations.positions.insert(position);
    }
    for(auto& [endpoints,locations]:pairs) {
        context.check();const auto n=result.links.size();
        result.outgoing[endpoints.first].push_back(n);result.incoming[endpoints.second].push_back(n);
        result.links.push_back({std::move(locations.first),locations.positions.size()});
    }
    return result;
}
GraphSearch search_graph_nodes(const GraphIndex& graph,const std::string& query,std::size_t limit,const ScanContext& context) {
    context.check();if(limit==0||limit>10000)throw std::invalid_argument("Search limit must be 1..10000");
    GraphSearch result;
    for(const auto& [id,node]:graph.nodes) {
        context.check();if(id.find(query)==std::string::npos&&node.label.find(query)==std::string::npos&&node.file.find(query)==std::string::npos)continue;
        ++result.total;if(result.nodes.size()<limit)result.nodes.push_back(node);
    }
    return result;
}
GraphSlice graph_neighborhood(const GraphIndex& graph,const std::string& focus,GraphDirection direction,
    unsigned depth,std::size_t max_nodes,std::size_t max_edges,const ScanContext& context) {
    context.check();require_node(graph,focus);
    if(depth>20||max_nodes==0||max_nodes>10000||max_edges==0||max_edges>50000)
        throw std::invalid_argument("Invalid graph neighborhood limits");
    if(direction!=GraphDirection::outgoing&&direction!=GraphDirection::incoming&&direction!=GraphDirection::both)
        throw std::invalid_argument("Invalid graph direction");
    GraphSlice result;result.depth=depth;
    std::map<std::string,int> layers{{focus,0}};
    std::deque<std::pair<std::string,unsigned>> queue{{focus,0}};
    while(!queue.empty()) {
        context.check();const auto [id,distance]=queue.front();queue.pop_front();
        result.nodes.push_back({graph.nodes.at(id),layers.at(id)});
        if(distance==depth)continue;
        std::map<std::string,int> next;
        if(direction!=GraphDirection::incoming)for(const auto edge:neighbors(graph.outgoing,id)){
            context.check();next.emplace(graph.links.at(edge).evidence.target,1);
        }
        if(direction!=GraphDirection::outgoing)for(const auto edge:neighbors(graph.incoming,id)){
            context.check();next.emplace(graph.links.at(edge).evidence.source,-1);
        }
        for(const auto& [child,sign]:next) {
            context.check();if(layers.contains(child))continue;
            if(layers.size()==max_nodes){result.node_limit_reached=true;continue;}
            const int layer=layers.at(id);layers[child]=(layer<0?-1:layer>0?1:sign)*static_cast<int>(distance+1);
            queue.emplace_back(child,distance+1);
        }
    }
    for(const auto& link:graph.links) {
        context.check();if(!layers.contains(link.evidence.source)||!layers.contains(link.evidence.target))continue;
        if(result.links.size()==max_edges){result.edge_limit_reached=true;break;}
        result.links.push_back(link);
    }
    return result;
}
GraphPath shortest_graph_path(const GraphIndex& graph,const std::string& from,const std::string& to,const ScanContext& context) {
    context.check();require_node(graph,from);require_node(graph,to);
    GraphPath result;std::map<std::string,std::size_t> previous;
    std::set<std::string> seen{from};std::deque<std::string> queue{from};
    while(!queue.empty()) {
        context.check();auto id=std::move(queue.front());queue.pop_front();++result.visited;
        if(id==to){result.found=true;break;}
        for(const auto edge:neighbors(graph.outgoing,id)) {
            context.check();const auto& target=graph.links.at(edge).evidence.target;
            if(seen.insert(target).second){previous[target]=edge;queue.push_back(target);}
        }
    }
    if(!result.found)return result;
    for(auto id=to;;) {
        context.check();result.nodes.push_back(graph.nodes.at(id));if(id==from)break;
        const auto& link=graph.links.at(previous.at(id));result.links.push_back(link);id=link.evidence.source;
    }
    std::reverse(result.nodes.begin(),result.nodes.end());std::reverse(result.links.begin(),result.links.end());return result;
}
}
