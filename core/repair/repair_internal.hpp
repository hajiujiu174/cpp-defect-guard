#pragma once
#include "codeguard/repair.hpp"

namespace codeguard {
struct RepairAnchor {
    const RepairCandidate* component=nullptr;
    std::uint64_t revised_offset=0;
    int revised_line=0,revised_column=0;
};
struct RepairPlan {
    std::vector<SourceReplacement> files;
    std::vector<RepairAnchor> anchors;
};
RepairPlan make_repair_plan(const std::vector<RepairCandidate>& components);
std::vector<RepairTargetValidation> repair_plan_targets(const RepairPlan& plan);
std::string repair_file_diff(const std::string& file,const std::string& before,const std::string& after);
RepairValidation validate_repair_plan(const fs::path& root,const fs::path& database,const RepairPlan& plan,
    const std::string& id,BuildOptions options,const std::function<fs::path()>& export_artifact);
}
