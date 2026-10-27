#include "validation_inputs_wire.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
template<class Snapshot> void output(const Snapshot& s,double seconds) {
    std::cout<<"seconds="<<seconds<<"\nfiles="<<s.files.size()<<"\nlines="<<validation_test::lines(s)<<"\nunits="<<s.analysis.units.size()<<"\nissues="<<s.analysis.issues.size()<<'\n';
    if constexpr(requires{s.analysis.references;})std::cout<<"symbols="<<s.analysis.symbols.size()<<"\nreferences="<<s.analysis.references.size()<<'\n';
    else std::cout<<"graph=absent_by_type\n";
    std::cout<<"metadata_begin\n"<<validation_test::metadata(s)<<"\nmetadata_end\n";
}
int main(int argc,char** argv){try{
#ifdef _WIN32
    _setmode(_fileno(stdout),_O_BINARY);
#endif
    if(argc!=4)throw std::invalid_argument("full|inputs|parity ROOT DB");
    const auto root=codeguard::utf8_path(codeguard::fs::canonical(codeguard::from_utf8(argv[2])));codeguard::SqliteDatabase db(codeguard::from_utf8(argv[3]),true);
    const auto start=std::chrono::steady_clock::now();const std::string mode=argv[1];
    auto seconds=[&]{return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();};
    if(mode=="full"){const auto s=db.latest(root);output(s,seconds());}
    else if(mode=="inputs"){const auto s=db.validation_inputs(root);output(s,seconds());}
    else if(mode=="parity") {const auto full=db.latest(root);const auto inputs=db.validation_inputs(root);if(validation_test::metadata(full)!=validation_test::metadata(inputs))throw std::runtime_error("metadata differs");std::cout<<"metadata_equal=true\nfiles="<<inputs.files.size()<<"\nlines="<<validation_test::lines(inputs)<<"\nunits="<<inputs.analysis.units.size()<<"\nissues="<<inputs.analysis.issues.size()<<'\n';}
    else throw std::invalid_argument("unknown mode");
    // Retain a live process long enough for the external sampler to observe
    // the OS peak counters even when the lightweight read finishes quickly.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
