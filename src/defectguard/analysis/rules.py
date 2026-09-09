from __future__ import annotations

from dataclasses import dataclass
import re
from typing import Protocol

from defectguard.analysis.source_utils import mask_comments_and_strings
from defectguard.domain import Finding, ParsedFile, Severity, SourceLocation


class Rule(Protocol):
    rule_id: str
    title: str
    default_severity: Severity

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]: ...


def _location(parsed_file: ParsedFile, line_number: int, column: int = 1) -> SourceLocation:
    return SourceLocation(parsed_file.relative_path, line_number, column)


def _source_lines(parsed_file: ParsedFile) -> tuple[list[str], list[str]]:
    return parsed_file.text.splitlines(), mask_comments_and_strings(parsed_file.text).splitlines()


def _scope_end(parsed_file: ParsedFile, line_number: int, total_lines: int) -> int:
    for function in parsed_file.functions:
        if function.start_line <= line_number <= function.end_line:
            return function.end_line
    return total_lines


@dataclass(frozen=True)
class DangerousFunctionRule:
    rule_id: str = "DG001"
    title: str = "危险函数调用"
    default_severity: Severity = Severity.HIGH
    pattern: re.Pattern[str] = re.compile(r"\b(gets|strcpy|strcat|sprintf)\s*\(")

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        suggestions = {
            "gets": "改用带长度限制的 fgets，并显式处理换行符。",
            "strcpy": "改用长度受控的复制逻辑，并确保目标缓冲区可容纳终止符。",
            "strcat": "拼接前计算剩余容量，并使用带长度约束的实现。",
            "sprintf": "改用 snprintf，并检查返回值是否超过目标缓冲区容量。",
        }
        findings: list[Finding] = []
        original_lines, code_lines = _source_lines(parsed_file)
        for line_number, code in enumerate(code_lines, 1):
            for match in self.pattern.finditer(code):
                function = match.group(1)
                findings.append(
                    Finding(
                        rule_id=self.rule_id,
                        defect_type="dangerous-function",
                        severity=Severity.HIGH,
                        location=_location(parsed_file, line_number, match.start(1) + 1),
                        message=f"调用了缺少容量约束的函数 {function}",
                        evidence=original_lines[line_number - 1].strip(),
                        suggestion=suggestions[function],
                    )
                )
        return findings


@dataclass(frozen=True)
class LiteralArrayBoundsRule:
    rule_id: str = "DG002"
    title: str = "字面量数组越界"
    default_severity: Severity = Severity.CRITICAL
    declaration: re.Pattern[str] = re.compile(
        r"\b(?:char|short|int|long|float|double|bool|unsigned|signed)"
        r"(?:\s+\w+)*\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]"
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        arrays: dict[str, tuple[int, int]] = {}
        original_lines, code_lines = _source_lines(parsed_file)
        for line_number, line in enumerate(code_lines, 1):
            for match in self.declaration.finditer(line):
                arrays[match.group("name")] = (int(match.group("size")), line_number)

        findings: list[Finding] = []
        for name, (size, declaration_line) in arrays.items():
            access = re.compile(rf"\b{re.escape(name)}\s*\[\s*(\d+)\s*\]")
            for line_number, code in enumerate(code_lines, 1):
                if line_number == declaration_line:
                    continue
                for match in access.finditer(code):
                    index = int(match.group(1))
                    if index < size:
                        continue
                    findings.append(
                        Finding(
                            rule_id=self.rule_id,
                            defect_type="literal-array-out-of-bounds",
                            severity=Severity.CRITICAL,
                            location=_location(parsed_file, line_number, match.start() + 1),
                            message=f"数组 {name} 的固定长度为 {size}，字面量索引 {index} 越界",
                            evidence=original_lines[line_number - 1].strip(),
                            suggestion=f"将索引限制在 0 到 {size - 1}，或在访问前进行边界检查。",
                        )
                    )
        return findings


@dataclass(frozen=True)
class AllocationWithoutReleaseRule:
    rule_id: str = "DG003"
    title: str = "疑似动态内存泄漏"
    default_severity: Severity = Severity.MEDIUM
    allocation: re.Pattern[str] = re.compile(
        r"\b(?P<var>[A-Za-z_]\w*)\s*=\s*"
        r"(?:new\b|(?:malloc|calloc|realloc)\s*\()"
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        original_lines, code_lines = _source_lines(parsed_file)
        findings: list[Finding] = []
        for line_number, code in enumerate(code_lines, 1):
            for match in self.allocation.finditer(code):
                variable = match.group("var")
                release = re.compile(
                    rf"(?:\bdelete\s*(?:\[\s*\])?\s*{re.escape(variable)}\b|"
                    rf"\bfree\s*\(\s*{re.escape(variable)}\s*\))"
                )
                end_line = _scope_end(parsed_file, line_number, len(code_lines))
                remaining = "\n".join(code_lines[line_number - 1 : end_line])
                if release.search(remaining):
                    continue
                findings.append(
                    Finding(
                        rule_id=self.rule_id,
                        defect_type="possible-resource-leak",
                        severity=Severity.MEDIUM,
                        location=_location(parsed_file, line_number, match.start() + 1),
                        message=f"动态分配变量 {variable} 后未发现对应释放操作",
                        evidence=original_lines[line_number - 1].strip(),
                        suggestion="优先使用 RAII 容器或智能指针；该文件级结果需由 CFG/DFG 继续复核。",
                        confidence=0.55,
                    )
                )
        return findings


@dataclass(frozen=True)
class AssignmentInConditionRule:
    rule_id: str = "DG004"
    title: str = "条件表达式中的赋值"
    default_severity: Severity = Severity.MEDIUM
    pattern: re.Pattern[str] = re.compile(
        r"\b(?:if|while)\s*\([^)]*(?<![=!<>])=(?!=)[^)]*\)"
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        findings: list[Finding] = []
        original_lines, code_lines = _source_lines(parsed_file)
        for line_number, code in enumerate(code_lines, 1):
            match = self.pattern.search(code)
            if match:
                findings.append(
                    Finding(
                        rule_id=self.rule_id,
                        defect_type="assignment-in-condition",
                        severity=Severity.MEDIUM,
                        location=_location(parsed_file, line_number, match.start() + 1),
                        message="条件表达式中存在赋值操作，可能把比较写成了赋值",
                        evidence=original_lines[line_number - 1].strip(),
                        suggestion="若意图是比较，请使用 ==；若确需赋值，请拆分语句并写明意图。",
                        confidence=0.8,
                    )
                )
        return findings


@dataclass(frozen=True)
class NullDereferenceRule:
    rule_id: str = "DG005"
    title: str = "空指针解引用"
    default_severity: Severity = Severity.CRITICAL
    null_declaration: re.Pattern[str] = re.compile(
        r"\b[A-Za-z_]\w*(?:\s*::\s*\w+|[\w:<>,\s])*\s*\*+\s*"
        r"(?P<var>[A-Za-z_]\w*)\s*=\s*(?:nullptr|NULL)\s*;"
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        original_lines, code_lines = _source_lines(parsed_file)
        findings: list[Finding] = []
        for declaration_line, code in enumerate(code_lines, 1):
            for match in self.null_declaration.finditer(code):
                variable = match.group("var")
                dereference = re.compile(
                    rf"(?:\b{re.escape(variable)}\s*->|(?<![\w*])\*\s*{re.escape(variable)}\b)"
                )
                assignment = re.compile(
                    rf"(?<![*&.\w])\b{re.escape(variable)}\s*=\s*(?!(?:nullptr|NULL)\b)"
                )
                end_line = _scope_end(parsed_file, declaration_line, len(code_lines))
                for line_number in range(declaration_line + 1, end_line + 1):
                    later = code_lines[line_number - 1]
                    if assignment.search(later):
                        break
                    access = dereference.search(later)
                    if not access:
                        continue
                    findings.append(
                        Finding(
                            rule_id=self.rule_id,
                            defect_type="null-pointer-dereference",
                            severity=Severity.CRITICAL,
                            location=_location(parsed_file, line_number, access.start() + 1),
                            message=f"指针 {variable} 初始化为空后，在重新赋值前被解引用",
                            evidence=original_lines[line_number - 1].strip(),
                            suggestion="解引用前检查指针并保证非空，优先使用具有明确所有权的 RAII 类型。",
                            confidence=0.9,
                        )
                    )
                    break
        return findings


@dataclass(frozen=True)
class UninitializedVariableRule:
    rule_id: str = "DG006"
    title: str = "未初始化局部变量"
    default_severity: Severity = Severity.HIGH
    declaration: re.Pattern[str] = re.compile(
        r"^\s*(?:(?:unsigned|signed|long|short)\s+)*"
        r"(?:bool|char|wchar_t|short|int|long|float|double|size_t)\s+"
        r"(?P<var>[A-Za-z_]\w*)\s*;\s*$"
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        original_lines, code_lines = _source_lines(parsed_file)
        findings: list[Finding] = []
        for declaration_line, code in enumerate(code_lines, 1):
            match = self.declaration.match(code)
            if not match:
                continue
            variable = match.group("var")
            use = re.compile(rf"\b{re.escape(variable)}\b")
            direct_assignment = re.compile(rf"\b{re.escape(variable)}\s*=(?!=)")
            input_assignment = re.compile(
                rf"(?:>>\s*{re.escape(variable)}\b|&\s*{re.escape(variable)}\b)"
            )
            end_line = min(
                _scope_end(parsed_file, declaration_line, len(code_lines)),
                declaration_line + 80,
            )
            for line_number in range(declaration_line + 1, end_line + 1):
                later = code_lines[line_number - 1]
                if direct_assignment.search(later) or input_assignment.search(later):
                    break
                access = use.search(later)
                if not access:
                    continue
                findings.append(
                    Finding(
                        rule_id=self.rule_id,
                        defect_type="uninitialized-local-variable",
                        severity=Severity.HIGH,
                        location=_location(parsed_file, line_number, access.start() + 1),
                        message=f"局部变量 {variable} 可能在赋值前被使用",
                        evidence=original_lines[line_number - 1].strip(),
                        suggestion="声明时提供确定初值，或保证所有控制流路径在使用前完成赋值。",
                        confidence=0.75,
                    )
                )
                break
        return findings


@dataclass(frozen=True)
class FileHandleWithoutCloseRule:
    rule_id: str = "DG007"
    title: str = "文件资源未释放"
    default_severity: Severity = Severity.HIGH
    open_pattern: re.Pattern[str] = re.compile(
        r"\bFILE\s*\*\s*(?P<var>[A-Za-z_]\w*)\s*=\s*fopen\s*\("
    )

    def inspect(self, parsed_file: ParsedFile) -> list[Finding]:
        original_lines, code_lines = _source_lines(parsed_file)
        findings: list[Finding] = []
        for line_number, code in enumerate(code_lines, 1):
            for match in self.open_pattern.finditer(code):
                variable = match.group("var")
                close_pattern = re.compile(rf"\bfclose\s*\(\s*{re.escape(variable)}\s*\)")
                end_line = _scope_end(parsed_file, line_number, len(code_lines))
                remaining = "\n".join(code_lines[line_number - 1 : end_line])
                if close_pattern.search(remaining):
                    continue
                findings.append(
                    Finding(
                        rule_id=self.rule_id,
                        defect_type="file-resource-not-closed",
                        severity=Severity.HIGH,
                        location=_location(parsed_file, line_number, match.start() + 1),
                        message=f"文件句柄 {variable} 打开后未发现 fclose",
                        evidence=original_lines[line_number - 1].strip(),
                        suggestion="在所有退出路径关闭文件，C++ 中可使用带自定义删除器的 RAII 封装。",
                        confidence=0.8,
                    )
                )
        return findings


ALL_RULES: tuple[Rule, ...] = (
    DangerousFunctionRule(),
    LiteralArrayBoundsRule(),
    AllocationWithoutReleaseRule(),
    AssignmentInConditionRule(),
    NullDereferenceRule(),
    UninitializedVariableRule(),
    FileHandleWithoutCloseRule(),
)

SEVERITY_RANK = {
    Severity.LOW: 0,
    Severity.MEDIUM: 1,
    Severity.HIGH: 2,
    Severity.CRITICAL: 3,
}


class RuleEngine:
    def __init__(
        self, enabled_rule_ids: tuple[str, ...], minimum_severity: str = "low"
    ) -> None:
        known = {rule.rule_id for rule in ALL_RULES}
        enabled = set(enabled_rule_ids)
        unknown = enabled - known
        if unknown:
            raise ValueError(f"未知规则编号：{', '.join(sorted(unknown))}")
        self.rules = tuple(rule for rule in ALL_RULES if rule.rule_id in enabled)
        self.minimum_severity = Severity(minimum_severity)

    @staticmethod
    def catalog() -> tuple[dict[str, str], ...]:
        return tuple(
            {
                "rule_id": rule.rule_id,
                "title": rule.title,
                "default_severity": rule.default_severity.value,
            }
            for rule in ALL_RULES
        )

    def inspect(self, parsed_files: list[ParsedFile]) -> list[Finding]:
        enabled = {rule.rule_id for rule in self.rules}
        findings = [
            finding
            for parsed_file in parsed_files
            for rule in self.rules
            if rule.rule_id not in parsed_file.native_rule_ids
            for finding in rule.inspect(parsed_file)
        ]
        findings.extend(
            finding for parsed_file in parsed_files for finding in parsed_file.native_findings
            if finding.rule_id in enabled
        )
        visible = [
            finding
            for finding in findings
            if SEVERITY_RANK[finding.severity] >= SEVERITY_RANK[self.minimum_severity]
        ]
        return sorted(
            visible,
            key=lambda item: (
                item.location.file,
                item.location.line,
                item.location.column,
                item.rule_id,
            ),
        )
