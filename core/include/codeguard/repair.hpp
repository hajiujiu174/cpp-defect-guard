#pragma once
#include "codeguard/application.hpp"
#include <map>

namespace codeguard {
struct RepairCandidate {
    std::string id,root,configuration,analyzer_revision;
    std::int64_t scan_id=0;
    RepairHint edit;
    std::string original_text,revised_text;
};
struct CoverageFile {
    std::string file;
    std::map<int,bool> lines; // union across instrumented objects, recorded execution
    std::uint64_t branch_records=0,branch_records_taken=0;
};
struct RepairCoverage {
    std::string status="not_measured",error,tool,tool_version,workspace,scope_signature;
    std::string target_line_status="unknown"; // recorded / no_recorded_execution / not_instrumented
    std::uint64_t lines_total=0,lines_hit=0,branch_records=0,branch_records_taken=0;
    std::size_t notes_files=0,data_files=0;
    std::vector<CoverageFile> files;
    std::vector<std::string> unmeasured_files;
};
struct RepairTargetValidation {
    std::string candidate_id,rule_id,file;
    int line=0,column=0,revised_line=0,revised_column=0;
    bool baseline_reproduced=false,target_not_detected=false;
    std::string coverage_line_status="unknown";
};
struct CoverageTargetLine {std::string file;int line=0;std::string status="unknown";};
struct RepairTestCoverageEntry {
    std::string name,status="not_run",workspace,error;
    std::vector<std::string> selected_tests; // includes explicit dependencies and CTest fixtures
    int tests_total=-1,tests_failed=-1,tests_skipped=-1,tests_disabled=-1,tests_not_run=-1;
    RepairCoverage coverage;
    std::vector<CoverageTargetLine> targets;
};
struct RepairTestCoverage {
    std::string status="not_measured",workspace,error;
    std::string ctest_version;
    unsigned test_limit=0;
    bool line_union_matches_aggregate=false;
    std::vector<RepairTestCoverageEntry> entries;
};
struct RepairValidation {
    std::string candidate_id,status="not_run",workspace,error;
    BuildRun build;
    std::string analysis_status="not_run";
    std::string baseline_status="not_run";
    bool baseline_target_reproduced=false;
    bool target_not_detected=false,gates_passed=false;
    std::int64_t before_issues=-1,after_issues=-1,new_or_unmasked_issues=-1;
    std::string correctness="not_assessed";
    RepairCoverage coverage;
    bool coverage_commands_normalized=false;
    bool proposed_copy_verified=false;
    std::vector<RepairTargetValidation> targets;
    RepairTestCoverage test_coverage;
};
// Reproduce the selected active issue in fresh ASTs; no cache-derived ranges.
std::vector<RepairCandidate> propose_repairs(const ScanResult& snapshot,const Issue& selected,const ScanContext& context={});
std::vector<RepairCandidate> propose_repairs(const ValidationInputs& snapshot,const Issue& selected,const ScanContext& context={});
// Reparse once for a bounded selection of active issues.
std::vector<RepairCandidate> propose_repairs(const ScanResult& snapshot,const std::vector<Issue>& selected,const ScanContext& context={});
std::vector<RepairCandidate> propose_repairs(const ValidationInputs& snapshot,const std::vector<Issue>& selected,const ScanContext& context={});
std::string repair_diff(const RepairCandidate& candidate);
std::string encode_repair_candidate(const RepairCandidate& candidate);
RepairCandidate decode_repair_candidate(const std::string& payload);
RepairCandidate read_repair_candidate(const fs::path& path);
// Always creates a unique directory outside the source tree. Never overwrites
// an original project file or applies a candidate to that project.
fs::path export_repair_candidate(const RepairCandidate& candidate,const fs::path& output);
RepairValidation validate_repair(const fs::path& root,const fs::path& database,const RepairCandidate& candidate,BuildOptions options);
std::string repair_validation_json(const RepairValidation& validation);
std::string repair_coverage_json(const RepairCoverage& coverage);
std::string repair_test_coverage_json(const RepairTestCoverage& coverage);

struct RepairBatchEntry {
    std::size_t input_index=0;
    std::string candidate_id,recipe,file,manifest;
    int line=0,column=0;
    std::uint64_t changed_bytes=0;
    RepairValidation validation;
};
struct RepairBatch {
    std::string status="running",workspace;
    std::size_t attempted=0;
    std::vector<RepairBatchEntry> entries; // input order; rank is a separate view
};
// Lexicographic evidence ranking, never a probability of repair correctness.
std::vector<std::size_t> rank_repairs(const std::vector<RepairBatchEntry>& entries);
bool comparable_repair_coverage(const std::vector<RepairBatchEntry>& entries);
std::string repair_batch_json(const RepairBatch& batch);
// Each candidate is validated independently, not combined into a patch.
// Keeps pending/failed/cancelled entries and immutable progress checkpoints.
RepairBatch evaluate_repairs(const fs::path& root,const fs::path& database,
    const std::vector<RepairCandidate>& candidates,BuildOptions options);

struct RepairSet {
    std::string id;
    std::vector<RepairCandidate> components; // canonical file/offset/id order
};
// Explicitly selected alternatives, never automatic combinatorial search.
// Requires 2..16 non-overlapping components from one scan/config/analyzer.
RepairSet compose_repairs(std::vector<RepairCandidate> components);
std::string encode_repair_set(const RepairSet& set);
RepairSet decode_repair_set(const std::string& payload);
RepairSet read_repair_set(const fs::path& path);
std::vector<SourceReplacement> repair_set_changes(const RepairSet& set);
std::string repair_set_diff(const RepairSet& set);
fs::path export_repair_set(const RepairSet& set,const fs::path& output);
RepairValidation validate_repair_set(const fs::path& root,const fs::path& database,const RepairSet& set,BuildOptions options);
}
