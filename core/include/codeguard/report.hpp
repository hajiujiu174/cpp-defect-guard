#pragma once
#include "codeguard/application.hpp"
#include <optional>

namespace codeguard {
struct IssueChange {
    std::string status, reason;
    std::optional<Issue> before, after;
};
struct ScanComparison {
    std::int64_t before_id=0, after_id=0;
    std::vector<IssueChange> changes;
    std::vector<std::string> notes;
};
// Comparison is based entirely on saved evidence; no current source/compile DB
// reads. "not_detected" is a repair candidate, never proof of a correct repair.
ScanComparison compare_scans(const ScanResult& before,const ScanResult& after,const ScanContext& context = {});
std::string report_time(const std::string& saved_timestamp); // epoch ms -> UTC; legacy text retained
std::string report_json(const ScanResult& current,const ScanResult* baseline=nullptr,const ScanContext& context = {});
std::string report_html(const ScanResult& current,const ScanResult* baseline=nullptr,const ScanContext& context = {});
struct ReportFiles { fs::path html,json; };
// Creates a fresh directory outside the source tree; never replaces a report.
// Cancellation is accepted until both formats are rendered, before creating the
// output directory. Then the control enters committing and writes finish.
ReportFiles export_report(const ScanResult& current,const fs::path& output,const ScanResult* baseline=nullptr,const ScanContext& context = {});
} // namespace codeguard
