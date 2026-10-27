#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

int protocol(const std::vector<std::string>& args) {
    const auto value=std::getenv("CODEGUARD_GIT_PROTOCOL_MODE");
    const std::string mode=value?value:"";std::string root;
    bool owner=false,head=false;
    for(const auto& arg:args) {
        if(arg.starts_with("--work-tree="))root=arg.substr(12);
        if(arg=="--show-toplevel")owner=true;
        if(arg=="HEAD")head=true;
    }
    if(owner) {
        if(mode=="failed_probe"){std::cerr<<"fixture probe rejected\n";return 7;}
        if(mode=="invalid_owner"){std::cout<<'\xff'<<'\n';return 0;}
        std::cout<<(mode=="mismatch"?root+"/absent":root)<<'\n';return 0;
    }
    if(head) {
        if(mode=="invalid_head"){std::cout<<"HEAD\n";return 0;}
        std::cout<<std::string(mode=="valid_sha256"?64:40,'a')<<'\n';
        if(mode=="truncated_head")for(int i=0;i<1025;++i)std::cout<<std::string(4096,'x');
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=1;i<argc;++i){const auto count=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(count,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),count,nullptr,nullptr);value.pop_back();args.push_back(std::move(value));}
    return protocol(args);
}
#else
int main(int argc,char** argv){return protocol(std::vector<std::string>(argv+1,argv+argc));}
#endif
