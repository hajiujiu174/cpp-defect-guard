#include "codeguard/analysis.hpp"
#include <stdexcept>
namespace codeguard {
bool clang_analysis_available() { return false; }
AnalysisResult analyze_project(const ScanResult&, const std::string&, const ScanContext&, unsigned, const std::map<std::string,std::string>&,const std::vector<std::string>&) {
    throw std::runtime_error("Clang analysis is disabled; rebuild with CODEGUARD_ENABLE_ANALYSIS=ON");
}
std::vector<CompileCommandInfo> inspect_compile_commands(const std::string&) {throw std::runtime_error("compile command inspection requires the Analysis build");}
std::string relocate_compile_commands(const std::string&,const std::string&,const std::string&,const std::string&) {throw std::runtime_error("compile command preparation requires the Analysis build");}
}
