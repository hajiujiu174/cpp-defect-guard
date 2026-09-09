#include "codeguard/application.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
int run(const std::vector<std::string>& args) {
    if (args.size()<2) return 2;
    if (args[1]=="echo") { for(std::size_t i=2;i<args.size();++i) std::cout << args[i].size() << ':' << args[i] << '\n'; std::cerr << "error-stream\n"; return 0; }
    if (args[1]=="fail") { std::cerr << "intentional failure\n"; return 7; }
    if (args[1]=="flood") { for(int i=0;i<5000;++i) {std::cout<<std::string(100,'o')<<'\n';std::cerr<<std::string(100,'e')<<'\n';} return 0; }
    if (args[1]=="sleep") { std::cout<<"started\n"<<std::flush; std::this_thread::sleep_for(std::chrono::seconds(10));return 0; }
    if (args[1]=="tree" && args.size()==3) {
#ifdef _WIN32
        auto command=L"\""+codeguard::from_utf8(args[0]).wstring()+L"\" marker \""+codeguard::from_utf8(args[2]).wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 3;
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
#else
        auto pid=fork();if(pid<0)return 3;
        if(!pid){execl(args[0].c_str(),args[0].c_str(),"marker",args[2].c_str(),nullptr);_exit(4);}
#endif
        std::this_thread::sleep_for(std::chrono::seconds(10));return 0;
    }
    if(args[1]=="marker" && args.size()==3){std::this_thread::sleep_for(std::chrono::milliseconds(1500));std::ofstream(codeguard::from_utf8(args[2]))<<"orphan";return 0;}
    return 2;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){std::vector<std::string> args;for(int i=0;i<argc;++i){const auto value=std::filesystem::path(argv[i]).u8string();args.emplace_back(value.begin(),value.end());}return run(args);}
#else
int main(int argc,char** argv){return run({argv,argv+argc});}
#endif
