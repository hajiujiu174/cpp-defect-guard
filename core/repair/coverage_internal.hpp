#pragma once
#include "codeguard/repair.hpp"

namespace codeguard {
void prepare_repair_coverage(BuildOptions& options,const fs::path& directory);
RepairCoverage collect_repair_coverage(const BuildRun& run,const RepairCandidate& candidate,
    const ValidationInputs& snapshot,const BuildOptions& options,const fs::path& directory);
inline RepairCoverage collect_repair_coverage(const BuildRun& run,const RepairCandidate& candidate,const ScanResult& snapshot,const BuildOptions& options,const fs::path& directory){return collect_repair_coverage(run,candidate,validation_inputs_of(snapshot),options,directory);}
void reset_repair_counters(const fs::path& work,const fs::path& backup,const BuildOptions& options);
RepairTestCoverage collect_repair_test_coverage(const BuildRun& run,const std::vector<RepairTargetValidation>& targets,
    const ValidationInputs& snapshot,const RepairCoverage& aggregate,const BuildOptions& options,const fs::path& directory);
inline RepairTestCoverage collect_repair_test_coverage(const BuildRun& run,const std::vector<RepairTargetValidation>& targets,const ScanResult& snapshot,const RepairCoverage& aggregate,const BuildOptions& options,const fs::path& directory){return collect_repair_test_coverage(run,targets,validation_inputs_of(snapshot),aggregate,options,directory);}
}
