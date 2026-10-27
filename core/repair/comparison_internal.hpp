#pragma once
#include "repair_internal.hpp"
#include "codeguard/repair_comparison.hpp"
#include <set>

namespace codeguard {
struct RepairLineCorrespondence {
    std::map<int,int> original_to_patched;
    std::set<int> modified_original,modified_patched;
};
RepairLineCorrespondence repair_line_correspondence(const SourceReplacement& change,const std::vector<RepairHint>& edits);
void summarize_repair_coverage(RepairCoverageComparison& result,const RepairPlan& plan,const ScanContext& context={});
std::string normalized_test_catalog(const fs::path& metadata,const fs::path& source,const fs::path& build);
}
