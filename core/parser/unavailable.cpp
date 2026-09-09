#include "codeguard/analysis.hpp"
#include <stdexcept>
namespace codeguard {
bool clang_analysis_available() { return false; }
AnalysisResult analyze_project(const ScanResult&, const std::string&, const ScanContext&, unsigned) {
    throw std::runtime_error("Clang analysis is disabled; rebuild with CODEGUARD_ENABLE_ANALYSIS=ON");
}
}
