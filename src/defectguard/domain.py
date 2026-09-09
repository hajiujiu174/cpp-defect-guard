from __future__ import annotations

from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from enum import StrEnum
from pathlib import Path
from typing import Any


class Severity(StrEnum):
    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"
    CRITICAL = "critical"


@dataclass(frozen=True)
class SourceLocation:
    file: str
    line: int
    column: int = 1


@dataclass(frozen=True)
class Finding:
    rule_id: str
    defect_type: str
    severity: Severity
    location: SourceLocation
    message: str
    evidence: str
    suggestion: str
    detector: str = "rule"
    confidence: float = 1.0

    def to_dict(self) -> dict[str, Any]:
        value = asdict(self)
        value["severity"] = self.severity.value
        return value


@dataclass(frozen=True)
class VariableEvent:
    kind: str
    symbol: str
    node: str


@dataclass(frozen=True)
class CFGNode:
    node_id: str
    line: int
    kind: str
    label: str
    ast_nodes: tuple[str, ...] = ()
    events: tuple[VariableEvent, ...] = ()


@dataclass(frozen=True)
class CFGEdge:
    source: str
    target: str
    kind: str


@dataclass(frozen=True)
class ASTNode:
    node_id: str
    kind: str
    line: int
    column: int
    end_line: int
    name: str = ""
    type_name: str = ""
    symbol: str = ""


@dataclass(frozen=True)
class ProgramEdge:
    source: str
    target: str
    kind: str
    symbol: str = ""


@dataclass(frozen=True)
class FunctionStructure:
    name: str
    return_type: str
    parameter_count: int
    start_line: int
    end_line: int
    cyclomatic_complexity: int
    cfg_nodes: tuple[CFGNode, ...] = ()
    cfg_edges: tuple[CFGEdge, ...] = ()
    signature: str = ""
    source_start: int = -1
    source_end: int = -1
    ast_nodes: tuple[ASTNode, ...] = ()
    ast_edges: tuple[ProgramEdge, ...] = ()
    dfg_edges: tuple[ProgramEdge, ...] = ()
    dfg_status: str = "unavailable-fallback"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class ParsedFile:
    path: Path
    relative_path: str
    language: str
    text: str
    functions: tuple[FunctionStructure, ...] = ()
    include_targets: tuple[str, ...] = ()
    backend: str = "fallback"
    diagnostics: tuple[str, ...] = ()
    native_findings: tuple[Finding, ...] = ()
    native_rule_ids: tuple[str, ...] = ()

    @property
    def function_names(self) -> tuple[str, ...]:
        return tuple(function.name for function in self.functions)


@dataclass(frozen=True)
class FileMetrics:
    file: str
    physical_lines: int
    code_lines: int
    comment_lines: int
    blank_lines: int
    function_count: int
    cyclomatic_complexity: int
    average_function_complexity: float
    max_brace_depth: int
    comment_ratio: float
    duplicate_line_ratio: float
    include_count: int


@dataclass(frozen=True)
class ProjectMetrics:
    file_count: int
    physical_lines: int
    code_lines: int
    comment_lines: int
    blank_lines: int
    function_count: int
    cyclomatic_complexity: int
    average_function_complexity: float
    max_brace_depth: int
    comment_ratio: float
    duplicate_line_ratio: float
    include_dependency_count: int
    warning_density_per_kloc: float
    files: tuple[FileMetrics, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class ProjectInfo:
    name: str
    source_root: str
    source_files: int
    language_counts: dict[str, int]
    build_systems: tuple[str, ...]
    compilation_database: str | None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class FileStructure:
    file: str
    language: str
    backend: str
    functions: tuple[FunctionStructure, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "file": self.file,
            "language": self.language,
            "backend": self.backend,
            "functions": [function.to_dict() for function in self.functions],
        }


@dataclass(frozen=True)
class ProgramStructureSummary:
    representation: str
    function_count: int
    cfg_node_count: int
    cfg_edge_count: int
    dfg_status: str
    files: tuple[FileStructure, ...]

    @property
    def ast_node_count(self) -> int:
        return sum(len(fn.ast_nodes) for file in self.files for fn in file.functions)

    @property
    def dfg_edge_count(self) -> int:
        return sum(len(fn.dfg_edges) for file in self.files for fn in file.functions)

    def to_dict(self) -> dict[str, Any]:
        return {
            "representation": self.representation,
            "function_count": self.function_count,
            "ast_node_count": self.ast_node_count,
            "cfg_node_count": self.cfg_node_count,
            "cfg_edge_count": self.cfg_edge_count,
            "dfg_status": self.dfg_status,
            "dfg_edge_count": self.dfg_edge_count,
            "files": [item.to_dict() for item in self.files],
        }


@dataclass(frozen=True)
class VerificationStep:
    name: str
    command: tuple[str, ...]
    return_code: int | None
    duration_seconds: float
    stdout: str
    stderr: str
    status: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class VerificationResult:
    status: str
    steps: tuple[VerificationStep, ...]
    workspace_removed: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "steps": [step.to_dict() for step in self.steps],
            "workspace_removed": self.workspace_removed,
        }


@dataclass
class AnalysisReport:
    run_id: str
    source_root: str
    source_fingerprint: str
    parser_backend: str
    project: ProjectInfo
    program_structure: ProgramStructureSummary
    findings: list[Finding]
    metrics: ProjectMetrics
    verification: VerificationResult
    source_unchanged: bool
    model_status: str = "disabled"
    repair_status: str = "disabled"
    diagnostics: list[str] = field(default_factory=list)
    created_at: str = field(
        default_factory=lambda: datetime.now(timezone.utc).isoformat()
    )

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": "1.2",
            "run_id": self.run_id,
            "created_at": self.created_at,
            "source_root": self.source_root,
            "source_fingerprint": self.source_fingerprint,
            "parser_backend": self.parser_backend,
            "source_unchanged": self.source_unchanged,
            "project": self.project.to_dict(),
            "program_structure": self.program_structure.to_dict(),
            "model_status": self.model_status,
            "repair_status": self.repair_status,
            "verification": self.verification.to_dict(),
            "summary": {
                "finding_count": len(self.findings),
                "severity_counts": {
                    severity.value: sum(
                        1 for finding in self.findings if finding.severity == severity
                    )
                    for severity in Severity
                },
            },
            "metrics": self.metrics.to_dict(),
            "findings": [finding.to_dict() for finding in self.findings],
            "diagnostics": self.diagnostics,
        }
