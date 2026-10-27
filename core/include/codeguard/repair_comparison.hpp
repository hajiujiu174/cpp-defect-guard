#pragma once
#include "codeguard/repair.hpp"

namespace codeguard {
struct CoverageObservation {
    unsigned round=0;
    std::string version,status="not_run",workspace,error,validation_record;
    BuildRun build;
    RepairCoverage coverage;
    RepairTestCoverage test_coverage;
    std::string normalized_catalog,catalog_signature;
    bool material_verified=false,standard_validation_passed=false;
    bool all_group_line_union_matches_aggregate=false;
};
struct CoverageLineHistory {
    unsigned observations=0,instrumented=0,recorded=0;
    std::string state="unknown";
};
struct CoverageLineChange {
    int original_line=0,patched_line=0; // zero means no asserted correspondence
    std::string mapping,transition="not_comparable";
    CoverageLineHistory original,patched;
};
struct CoverageFileComparison {std::string file;std::vector<CoverageLineChange> lines;};
struct CoverageGroupComparison {
    std::string name; // empty = whole-suite aggregate, otherwise a requested CTest group
    std::vector<std::string> selected_tests;
    bool diagnostic_only=false;
    std::vector<CoverageFileComparison> files;
};
struct RepairCoverageComparison {
    std::string id,workspace,status="not_run",error,input_signature;
    unsigned rounds=0;
    bool comparison_complete=false,coverage_stable=false,tests_stable=false;
    bool catalogs_match=false,tools_match=false,all_materials_verified=false;
    bool all_patched_validations_passed=false,ready_for_review=false;
    bool original_cpp_unchanged=false,known_original_inputs_unchanged=false;
    std::vector<CoverageObservation> observations;
    std::vector<CoverageGroupComparison> groups;
};
// Fresh independent build copies, alternating original/patched order by round.
// All proposed components share one saved snapshot; never writes them to source.
RepairCoverageComparison compare_repair_coverage(const fs::path& root,const fs::path& database,
    std::vector<RepairCandidate> components,BuildOptions options,unsigned rounds=2);
std::string repair_coverage_comparison_json(const RepairCoverageComparison& comparison);
}
