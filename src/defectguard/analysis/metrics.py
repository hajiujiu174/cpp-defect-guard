from __future__ import annotations

import re
from collections import Counter

from defectguard.analysis.source_utils import mask_comments_and_strings
from defectguard.domain import FileMetrics, ParsedFile, ProjectMetrics


def _file_metrics(parsed_file: ParsedFile) -> FileMetrics:
    lines = parsed_file.text.splitlines()
    sanitized_lines = mask_comments_and_strings(parsed_file.text).splitlines()
    blank_lines = sum(1 for line in lines if not line.strip())
    code_lines = sum(1 for line in sanitized_lines if line.strip())
    comment_lines = sum(
        1
        for original, sanitized in zip(lines, sanitized_lines, strict=False)
        if original.strip() and not sanitized.strip()
    )
    max_depth = 0
    depth = 0
    for line in sanitized_lines:
        depth += line.count("{")
        max_depth = max(max_depth, depth)
        depth = max(0, depth - line.count("}"))

    complexities = [function.cyclomatic_complexity for function in parsed_file.functions]
    complexity = sum(complexities)
    normalized = [
        re.sub(r"\s+", "", line)
        for line in sanitized_lines
        if len(re.sub(r"\s+", "", line)) >= 4
        and re.sub(r"\s+", "", line) not in {"{", "}", "};"}
    ]
    counts = Counter(normalized)
    duplicated = sum(count - 1 for count in counts.values() if count > 1)
    return FileMetrics(
        file=parsed_file.relative_path,
        physical_lines=len(lines),
        code_lines=code_lines,
        comment_lines=comment_lines,
        blank_lines=blank_lines,
        function_count=len(parsed_file.function_names),
        cyclomatic_complexity=complexity,
        average_function_complexity=round(
            complexity / max(len(parsed_file.functions), 1), 2
        ),
        max_brace_depth=max_depth,
        comment_ratio=round(comment_lines / max(len(lines), 1), 4),
        duplicate_line_ratio=round(duplicated / max(len(normalized), 1), 4),
        include_count=len(set(parsed_file.include_targets)),
    )


def calculate_project_metrics(
    parsed_files: list[ParsedFile], finding_count: int
) -> ProjectMetrics:
    file_metrics = tuple(_file_metrics(parsed_file) for parsed_file in parsed_files)
    code_lines = sum(item.code_lines for item in file_metrics)
    physical_lines = sum(item.physical_lines for item in file_metrics)
    comment_lines = sum(item.comment_lines for item in file_metrics)
    function_count = sum(item.function_count for item in file_metrics)
    complexity = sum(item.cyclomatic_complexity for item in file_metrics)
    all_normalized = [
        re.sub(r"\s+", "", line)
        for parsed_file in parsed_files
        for line in mask_comments_and_strings(parsed_file.text).splitlines()
        if len(re.sub(r"\s+", "", line)) >= 4
        and re.sub(r"\s+", "", line) not in {"{", "}", "};"}
    ]
    counts = Counter(all_normalized)
    duplicated = sum(count - 1 for count in counts.values() if count > 1)
    include_targets = {
        target for parsed_file in parsed_files for target in parsed_file.include_targets
    }
    return ProjectMetrics(
        file_count=len(file_metrics),
        physical_lines=physical_lines,
        code_lines=code_lines,
        comment_lines=comment_lines,
        blank_lines=sum(item.blank_lines for item in file_metrics),
        function_count=function_count,
        cyclomatic_complexity=complexity,
        average_function_complexity=round(complexity / max(function_count, 1), 2),
        max_brace_depth=max((item.max_brace_depth for item in file_metrics), default=0),
        comment_ratio=round(comment_lines / max(physical_lines, 1), 4),
        duplicate_line_ratio=round(duplicated / max(len(all_normalized), 1), 4),
        include_dependency_count=len(include_targets),
        warning_density_per_kloc=round(finding_count * 1000 / max(code_lines, 1), 2),
        files=file_metrics,
    )
