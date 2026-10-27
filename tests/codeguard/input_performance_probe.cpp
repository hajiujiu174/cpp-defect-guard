// Read-only measurement of real saved project inputs; no AST scope is changed.
#include "codeguard/application.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>

namespace cg=codeguard;
int main(int argc,char** argv){try{
    if(argc!=4)throw std::invalid_argument("inventory|snapshot ROOT DB");
    const auto root=cg::fs::canonical(cg::from_utf8(argv[2]));const auto database=cg::from_utf8(argv[3]);
    cg::SqliteDatabase db(database,true);cg::ScanResult result;
    const auto settings=cg::load_project_config(root,database);
    if(!settings)throw std::runtime_error("saved project configuration required");
    const auto options=cg::configured_scan_options(root,*settings);
    const auto start=std::chrono::steady_clock::now();
    if(std::string(argv[1])=="inventory")result=cg::scan_project(root,options);
    else if(std::string(argv[1])=="snapshot")result=db.latest(cg::utf8_path(root));
    else throw std::invalid_argument("unknown probe mode");
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if(!result.diagnostics.empty())throw std::runtime_error("inventory incomplete");
    std::cout<<"seconds\t"<<std::setprecision(10)<<elapsed<<"\nfiles\t"<<result.files.size()<<"\nlines\t"<<result.total_lines()<<"\nsymbols\t"<<result.analysis.symbols.size()<<"\nreferences\t"<<result.analysis.references.size()<<'\n';
    for(const auto& f:result.files)std::cout<<"file\t"<<f.path<<'\t'<<f.hash<<'\t'<<f.size<<'\t'<<f.lines<<'\n';
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
