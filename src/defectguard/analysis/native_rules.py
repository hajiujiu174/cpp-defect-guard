from __future__ import annotations

from pathlib import Path
from typing import Any

from defectguard.domain import Finding, ParsedFile, Severity, SourceLocation


NATIVE_RULE_IDS = ("DG001", "DG002", "DG004")


def native_findings(payload: dict[str, Any], files: list[ParsedFile]) -> dict[str, tuple[Finding, ...]]:
    by_path = {file.path.resolve(): file for file in files}
    result: dict[str, set[Finding]] = {file.relative_path: set() for file in files}
    for function in payload.get("functions", []):
        for fact in function.get("findings", []):
            file = by_path.get(Path(fact["file"]).resolve())
            if file is None:
                continue
            line, column = int(fact["line"]), int(fact["column"])
            lines = file.text.splitlines()
            if not 1 <= line <= len(lines) or column < 1:
                raise ValueError("原生规则的源码位置无效")
            rule_id = fact["rule_id"]
            if rule_id == "DG001":
                defect_type, severity = "dangerous-function", Severity.HIGH
                message = f"AST 确认调用了缺少容量约束的库函数 {fact['callee']}"
                suggestion = "使用长度受控的接口，并检查目标缓冲区容量与返回值。"
            elif rule_id == "DG002":
                defect_type, severity = "literal-array-out-of-bounds", Severity.CRITICAL
                size, index = int(fact["size"]), int(fact["index"])
                message = f"数组 {fact['variable']} 的长度为 {size}，常量索引 {index} 越界"
                suggestion = f"将索引限制在 0 到 {size - 1}，或在访问前检查边界。"
            elif rule_id == "DG004":
                defect_type, severity = "assignment-in-condition", Severity.MEDIUM
                message = "AST 确认分支或循环直接将赋值结果用作条件"
                suggestion = "若意图是比较，请使用 ==；若确需赋值，请显式比较结果或拆分语句。"
            else:
                raise ValueError(f"原生分析器返回不支持的规则：{rule_id}")
            result[file.relative_path].add(Finding(
                rule_id=rule_id, defect_type=defect_type, severity=severity,
                location=SourceLocation(file.relative_path, line, column),
                message=message, evidence=lines[line - 1].strip(), suggestion=suggestion,
                detector="clang-ast", confidence=0.8 if rule_id == "DG004" else 1.0,
            ))
    return {file: tuple(sorted(findings, key=lambda finding: (
        finding.location.line, finding.location.column, finding.rule_id
    ))) for file, findings in result.items()}
