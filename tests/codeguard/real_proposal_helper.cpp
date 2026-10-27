// Benchmark driver for the existing public multi-issue Core API.
#include "codeguard/repair.hpp"
#include <charconv>
#include <iostream>

namespace cg=codeguard;
int number(const char* value){int n=0;const std::string s=value;const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),n);if(error!=std::errc{}||end!=s.data()+s.size()||n<=0)throw std::invalid_argument("positive issue coordinate required");return n;}
int main(int argc,char** argv){try{
    if(argc<8||(argc-4)%4||argc>68)throw std::invalid_argument("ROOT DB OUTPUT [RULE FILE LINE COLUMN] (1..16)");
    const auto root=cg::fs::canonical(cg::from_utf8(argv[1]));cg::SqliteDatabase db(cg::from_utf8(argv[2]),true);const auto snapshot=db.validation_inputs(cg::utf8_path(root));
    if(snapshot.analysis.status!="complete")throw std::invalid_argument("complete saved scan required for this benchmark");
    std::vector<cg::Issue> selected;
    for(int n=4;n<argc;n+=4){const auto line=number(argv[n+2]),column=number(argv[n+3]);bool found=false;
        for(const auto& i:snapshot.analysis.issues)if(i.rule_id==argv[n]&&i.file==argv[n+1]&&i.line==line&&i.column==column){selected.push_back(i);found=true;break;}
        if(!found)throw std::invalid_argument("selected active issue missing");
    }
    cg::ScanContext context;context.progress=[](const cg::ScanProgress& p){std::cerr<<"phase="<<p.phase<<'\n';};
    const auto all=cg::propose_repairs(snapshot,selected,context);
    for(const auto& c:all){const auto directory=cg::export_repair_candidate(c,cg::from_utf8(argv[3]));std::cout<<c.id<<'\t'<<c.edit.recipe<<'\t'<<cg::utf8_path(directory/"candidate.cgfix")<<'\n';}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
