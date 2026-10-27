#include "../../core/repair/comparison_internal.hpp"
#include <exception>
#include <fstream>
#include <iostream>

namespace cg=codeguard;namespace fs=std::filesystem;
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
struct Temp{fs::path dir=fs::temp_directory_path()/("cn-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Temp(){fs::create_directory(dir);}~Temp(){std::error_code ec;fs::remove_all(dir,ec);}};
int main(){try{Temp t;auto metadata=t.dir/"tests.json";
    auto normalize=[&](const std::string& text,const std::string& s,const std::string& b){std::ofstream(metadata,std::ios::binary)<<text;return cg::normalized_test_catalog(metadata,cg::from_utf8(s),cg::from_utf8(b));};
    const auto a=normalize(R"({"tests":[{"name":"b","command":["C:/a/build/demo.exe","--value=C:/a/source/a.cpp"],"properties":[{"name":"WORKING_DIRECTORY","value":"C:/a/build"}]},{"name":"a","command":["cmake","C:/a/source-candidate/keep"],"properties":[]}]})","C:/a/source","C:/a/build");
    const auto b=normalize(R"({"backtraceGraph":{"unused":1},"tests":[{"name":"a","command":["cmake","C:/a/source-candidate/keep"],"properties":[]},{"name":"b","command":["C:/b/build/demo.exe","--value=C:/b/source/a.cpp"],"properties":[{"name":"WORKING_DIRECTORY","value":"C:/b/build"}]}]})","C:/b/source","C:/b/build");
    check(a==b&&a.find("C:/a/source-candidate/keep")!=std::string::npos,"catalog root relocation or order normalization incorrect");
    const auto c=normalize(R"({"tests":[{"name":"b","command":["C:/a/build/demo.exe","different"],"properties":[]}]})","C:/a/source","C:/a/build");check(c!=a,"changed command hidden by normalization");
    bool rejected=false;try{normalize(R"({"tests":[{"name":"a","command":["$SOURCE/file"],"properties":[]}]})","C:/a/source","C:/a/build");}catch(const std::exception&){rejected=true;}check(rejected,"reserved marker collision accepted");
    std::cout<<"CATALOG_NORMALIZATION_OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
