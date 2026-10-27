#include "codeguard/application.hpp"
#include "codeguard/graph_explorer.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace cg=codeguard;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid input accepted");}
cg::ScanResult fixture() {
    cg::ScanResult scan;scan.analysis.status="partial";
    for(const auto* id:{"a","b","c","d","isolated"})scan.analysis.symbols.push_back({id,"function",id,"main.cpp",3,5,true,false});
    scan.analysis.edges={{"call","a","b","main.cpp",8,4},{"call","a","b","main.cpp",4,2},
        {"call","a","b","main.cpp",4,2},{"call","a","c","main.cpp",9,1},
        {"call","b","d","main.cpp",10,1},{"call","c","d","main.cpp",11,1},
        {"call","d","a","main.cpp",12,1},{"call","d","d","main.cpp",13,1},
        {"call","d","external","main.cpp",14,1},{"include","main.cpp","api.h","main.cpp",1,1}};
    scan.files.push_back({"main.cpp","C++","h",1,0,5});return scan;
}
int main(int argc,char** argv){try{
    check(argc==2,"mode required");const std::string mode=argv[1];auto scan=fixture();const auto graph=cg::build_graph_index(scan,"call");
    if(mode=="index") {
        check(graph.nodes.size()==6&&graph.links.size()==7,"index kind/dedup mismatch");
        check(graph.links[0].location_count==2&&graph.links[0].evidence.line==4,"edge evidence/count mismatch");
        check(graph.nodes.at("external").external&&graph.nodes.at("external").file.empty(),"external endpoint fabricated");
        auto copy=scan;std::reverse(copy.analysis.edges.begin(),copy.analysis.edges.end());const auto reversed=cg::build_graph_index(copy,"call");
        check(reversed.links.front().evidence.line==4,"evidence depends on input order");
        const auto includes=cg::build_graph_index(scan,"include");check(includes.nodes.size()==2&&includes.links.size()==1,"include graph mismatch");
        check(includes.nodes.at("api.h").external&&includes.nodes.at("main.cpp").line==1,"file endpoint mapping mismatch");
        const auto search=cg::search_graph_nodes(graph,"main.cpp",2);check(search.total==5&&search.nodes.size()==2,"search silently truncated");
    } else if(mode=="path") {
        auto path=cg::shortest_graph_path(graph,"a","d");
        check(path.found&&path.nodes.size()==3&&path.nodes[1].id=="b"&&path.links[0].evidence.line==4,"shortest stable path mismatch");
        check(!cg::shortest_graph_path(graph,"external","a").found,"edge direction ignored");
        check(!cg::shortest_graph_path(graph,"a","isolated").found,"disconnected node reached");
        path=cg::shortest_graph_path(graph,"d","d");check(path.found&&path.nodes.size()==1&&path.links.empty(),"zero-edge path mistaken for cycle");
        check(cg::shortest_graph_path(graph,"d","b").links.size()==2,"cycle traversal failed");
    } else if(mode=="slice") {
        auto view=cg::graph_neighborhood(graph,"a",cg::GraphDirection::outgoing,1,10,20);
        check(view.nodes.size()==3&&view.links.size()==2&&!view.node_limit_reached,"depth boundary ignored");
        view=cg::graph_neighborhood(graph,"a",cg::GraphDirection::incoming,1,10,20);
        check(view.nodes.size()==2&&view.nodes[1].node.id=="d"&&view.nodes[1].layer==-1,"incoming neighborhood mismatch");
        view=cg::graph_neighborhood(graph,"a",cg::GraphDirection::both,2,3,1);
        check(view.nodes.size()==3&&view.node_limit_reached&&view.edge_limit_reached&&view.links.size()==1,"bounded graph lost truncation evidence");
        view=cg::graph_neighborhood(graph,"a",cg::GraphDirection::outgoing,0,1,1);check(view.nodes.size()==1&&view.links.empty(),"depth zero mismatch");
    } else if(mode=="validation") {
        rejects([&]{cg::build_graph_index(scan,"unknown");});scan.analysis.status="not_requested";
        rejects([&]{cg::build_graph_index(scan,"call");});
        rejects([&]{cg::shortest_graph_path(graph,"missing","missing");});
        rejects([&]{cg::graph_neighborhood(graph,"a",cg::GraphDirection::outgoing,2,0);});
        rejects([&]{cg::search_graph_nodes(graph,"",0);});
        cg::ScanContext cancelled;cancelled.control=std::make_shared<cg::ScanControl>();cancelled.control->request_cancel();
        int rejected=0;for(int i=0;i<4;++i)try{if(i==0)cg::build_graph_index(scan,"call",cancelled);else if(i==1)cg::search_graph_nodes(graph,"",100,cancelled);else if(i==2)cg::graph_neighborhood(graph,"a",cg::GraphDirection::both,2,100,400,cancelled);else cg::shortest_graph_path(graph,"a","d",cancelled);}catch(const cg::ScanCancelled&){++rejected;}
        check(rejected==4,"graph operation ignored cancellation");
    } else if(mode=="deep") {
        scan.analysis.edges.clear();scan.analysis.symbols.clear();scan.analysis.status="complete";
        for(int i=0;i<30000;++i)scan.analysis.edges.push_back({"call",std::to_string(i),std::to_string(i+1),"main.cpp",i+1,1});
        const auto large=cg::build_graph_index(scan,"call");const auto path=cg::shortest_graph_path(large,"0","30000");
        check(path.found&&path.links.size()==30000,"deep path truncated or recursive");
        const auto view=cg::graph_neighborhood(large,"0",cg::GraphDirection::outgoing,20,10,20);check(view.nodes.size()==10&&view.node_limit_reached,"large view exceeded budget");
    } else throw std::runtime_error("unknown mode");
    std::cout<<"GRAPH_EXPLORER_OK "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
