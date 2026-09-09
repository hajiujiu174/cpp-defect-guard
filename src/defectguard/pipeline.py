from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import uuid
from collections import defaultdict
from dataclasses import replace
from typing import Any

from defectguard.analysis import RuleEngine, calculate_project_metrics
from defectguard.analysis.dataflow import reaching_definitions
from defectguard.analysis.native_rules import NATIVE_RULE_IDS, native_findings
from defectguard.config import AppConfig
from defectguard.domain import (
    AnalysisReport,
    ASTNode,
    CFGEdge,
    CFGNode,
    FileStructure,
    FunctionStructure,
    ParsedFile,
    ProgramStructureSummary,
    ProgramEdge,
    ProjectInfo,
    VerificationResult,
    VariableEvent,
)
from defectguard.models import create_detector
from defectguard.analysis.rules import SEVERITY_RANK
from defectguard.domain import Severity
from defectguard.parser import FallbackProjectParser
from defectguard.parser.clang_tool import ClangToolClient
from defectguard.repair import DisabledPatchGenerator
from defectguard.verification import IsolatedVerifier


def _fingerprint(parsed_files: list[ParsedFile]) -> str:
    digest = hashlib.sha256()
    for parsed_file in parsed_files:
        digest.update(parsed_file.relative_path.encode("utf-8"))
        digest.update(b"\0")
        try:
            digest.update(parsed_file.path.read_bytes())
        except OSError:
            digest.update(b"__SOURCE_READ_ERROR__")
            digest.update(parsed_file.text.encode("utf-8", errors="replace"))
        digest.update(b"\0")
    return digest.hexdigest()


def _find_compilation_database(source_root: Path, configured: str) -> Path | None:
    candidates: list[Path] = []
    if configured:
        path = Path(configured)
        candidates.append(path if path.is_absolute() else source_root / path)
    candidates.extend(
        [
            source_root / "compile_commands.json",
            source_root / "build" / "compile_commands.json",
            source_root / "out" / "compile_commands.json",
        ]
    )
    return next((path.resolve() for path in candidates if path.is_file()), None)


def _project_info(
    source_root: Path,
    parsed_files: list[ParsedFile],
    project_name: str,
    compilation_database: Path | None,
) -> ProjectInfo:
    build_markers = {
        "CMake": source_root / "CMakeLists.txt",
        "Make": source_root / "Makefile",
        "Meson": source_root / "meson.build",
    }
    systems = [name for name, path in build_markers.items() if path.is_file()]
    if any(source_root.glob("*.sln")):
        systems.append("Visual Studio")
    language_counts = {
        language: sum(1 for item in parsed_files if item.language == language)
        for language in sorted({item.language for item in parsed_files})
    }
    name = source_root.name if project_name == "project" else project_name
    return ProjectInfo(
        name=name,
        source_root=str(source_root),
        source_files=len(parsed_files),
        language_counts=language_counts,
        build_systems=tuple(systems),
        compilation_database=str(compilation_database) if compilation_database else None,
    )


def _fallback_structure(parsed_files: list[ParsedFile]) -> ProgramStructureSummary:
    files = tuple(
        FileStructure(
            file=item.relative_path,
            language=item.language,
            backend=item.backend,
            functions=item.functions,
        )
        for item in parsed_files
    )
    functions = [function for item in files for function in item.functions]
    return ProgramStructureSummary(
        representation="lexical-function-boundary-and-approximate-cfg",
        function_count=len(functions),
        cfg_node_count=sum(len(function.cfg_nodes) for function in functions),
        cfg_edge_count=sum(len(function.cfg_edges) for function in functions),
        dfg_status="unavailable-fallback",
        files=files,
    )


def _native_structure(
    payload: dict[str, Any], source_root: Path, allowed_files: set[str]
) -> ProgramStructureSummary:
    grouped: dict[str, list[FunctionStructure]] = defaultdict(list)
    for item in payload.get("functions", []):
        raw_file = str(item.get("file", ""))
        file_path = Path(raw_file)
        try:
            relative = file_path.resolve().relative_to(source_root).as_posix()
        except (OSError, ValueError):
            relative = file_path.as_posix()
        if relative not in allowed_files:
            continue
        nodes = tuple(
            CFGNode(
                node_id=str(node.get("id", "")),
                line=int(node.get("line", 0)),
                kind=str(node.get("kind", "block")),
                label=str(node.get("label", "")),
                ast_nodes=tuple(node.get("ast_nodes", [])),
                events=tuple(VariableEvent(**event) for event in node.get("events", [])),
            )
            for node in item.get("cfg", {}).get("nodes", [])
        )
        edges = tuple(
            CFGEdge(
                source=str(edge.get("source", "")),
                target=str(edge.get("target", "")),
                kind=str(edge.get("kind", "successor")),
            )
            for edge in item.get("cfg", {}).get("edges", [])
        )
        ast_nodes = tuple(ASTNode(
            node_id=str(node["id"]), kind=str(node["kind"]),
            line=int(node["line"]), column=int(node["column"]), end_line=int(node["end_line"]),
            name=str(node.get("name", "")), type_name=str(node.get("type", "")),
            symbol=str(node.get("symbol", "")),
        ) for node in item.get("ast", {}).get("nodes", []))
        ast_edges = tuple(ProgramEdge(**edge) for edge in item.get("ast", {}).get("edges", []))
        ast_ids = {node.node_id for node in ast_nodes}
        if len(ast_ids) != len(ast_nodes) or any(
            edge.source not in ast_ids or edge.target not in ast_ids for edge in ast_edges
        ):
            raise ValueError("AST 中存在重复节点或悬空边")
        dfg_status = (
            "intraprocedural-local-scalar" if item.get("dataflow_status") == "local-scalar-events"
            else "unavailable-cfg-or-native-schema"
        )
        dfg_edges = reaching_definitions(nodes, edges, ast_ids) if ast_nodes else ()
        grouped[relative].append(
            FunctionStructure(
                name=str(item.get("name", "")),
                return_type=str(item.get("return_type", "")),
                parameter_count=int(item.get("parameter_count", 0)),
                start_line=int(item.get("start_line", item.get("line", 0))),
                end_line=int(item.get("end_line", item.get("line", 0))),
                cyclomatic_complexity=int(item.get("cyclomatic_complexity", 1)),
                cfg_nodes=nodes,
                cfg_edges=edges,
                signature=str(item.get("signature", "")),
                source_start=int(item.get("source_start", -1)), source_end=int(item.get("source_end", -1)),
                ast_nodes=ast_nodes, ast_edges=ast_edges,
                dfg_edges=dfg_edges, dfg_status=dfg_status,
            )
        )
    files = tuple(
        FileStructure(
            file=file,
            language="c" if Path(file).suffix.lower() == ".c" else "cpp",
            backend="clang-libtooling",
            functions=tuple(sorted(functions, key=lambda fn: (fn.start_line, fn.name, fn.signature))),
        )
        for file, functions in sorted(grouped.items())
    )
    functions = [function for item in files for function in item.functions]
    return ProgramStructureSummary(
        representation="clang-ast-cfg-and-local-dfg" if any(fn.ast_nodes for fn in functions) else "clang-ast-and-cfg",
        function_count=len(functions),
        cfg_node_count=sum(len(function.cfg_nodes) for function in functions),
        cfg_edge_count=sum(len(function.cfg_edges) for function in functions),
        dfg_status=("intraprocedural-local-scalar" if functions and all(
            fn.dfg_status == "intraprocedural-local-scalar" for fn in functions
        ) else "partial-or-unavailable"),
        files=files,
    )


class AnalysisPipeline:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        self.parser = FallbackProjectParser(config.exclude, config.max_file_bytes)
        self.rule_engine = RuleEngine(config.enabled_rules, config.minimum_severity)
        self.model_backend = create_detector(config)
        self.patch_generator = DisabledPatchGenerator()

    def scan(self, source_root: Path, run_verification: bool = False) -> AnalysisReport:
        source_root = source_root.resolve()
        if not source_root.is_dir():
            raise ValueError(f"源码目录不存在或不是目录：{source_root}")
        parsed_files = self.parser.parse(source_root)
        source_fingerprint = _fingerprint(parsed_files)
        compilation_database = _find_compilation_database(
            source_root, self.config.compilation_database
        )
        parser_backend = self.parser.name
        program_structure = _fallback_structure(parsed_files)
        diagnostics: list[str] = []

        configured_tool = Path(self.config.clang_tool) if self.config.clang_tool else None
        discovered_tool = shutil.which("defectguard-clang")
        clang_tool = configured_tool or (Path(discovered_tool) if discovered_tool else None)
        can_run_clang = bool(
            clang_tool
            and clang_tool.is_file()
            and compilation_database
            and parsed_files
        )
        if self.config.parser_backend in {"auto", "clang"} and can_run_clang:
            assert clang_tool is not None and compilation_database is not None
            try:
                payload = ClangToolClient(clang_tool, self.config.timeout_seconds).analyze(
                    [item.path for item in parsed_files if item.text], compilation_database.parent
                )
                native_structure = _native_structure(payload, source_root, {file.relative_path for file in parsed_files})
                findings_by_file = native_findings(payload, parsed_files)
                functions_by_file = {file.file: file.functions for file in native_structure.files}
                supported_rules = tuple(rule for rule in payload.get("native_rules", []) if rule in NATIVE_RULE_IDS)
                translation_units = {Path(path).resolve() for path in payload.get("translation_units", [])}
                covered_files = set(functions_by_file) | {
                    file.relative_path for file in parsed_files if file.path.resolve() in translation_units
                }
                native_files = [replace(
                    file, functions=functions_by_file.get(file.relative_path, ()), backend="clang-libtooling",
                    native_findings=findings_by_file.get(file.relative_path, ()), native_rule_ids=supported_rules,
                ) if file.relative_path in covered_files else file for file in parsed_files]
                program_structure = native_structure
                parsed_files = native_files
                diagnostics.extend(payload.get("diagnostics", []))
                uncovered = [file.relative_path for file in parsed_files if file.relative_path not in covered_files]
                if uncovered:
                    diagnostics.append("以下文件没有 Clang 函数图，保留词法分析结果：" + ", ".join(uncovered))
                if supported_rules:
                    diagnostics.append("DG001/DG002/DG004 使用 Clang AST；其余四类规则仍为词法启发式。DFG 仅覆盖函数内局部标量，不建模指针别名、堆内存和调用副作用。")
                parser_backend = "clang-libtooling"
            except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
                if self.config.parser_backend == "clang":
                    raise ValueError(f"Clang 解析失败：{error}") from error
                diagnostics.append(f"Clang 解析失败，已降级：{error}")
        elif self.config.parser_backend == "clang":
            missing: list[str] = []
            if not clang_tool or not clang_tool.is_file():
                missing.append("defectguard-clang 可执行文件")
            if not compilation_database:
                missing.append("compile_commands.json")
            raise ValueError(f"Clang 解析条件不满足：{', '.join(missing)}")
        elif self.config.parser_backend == "auto":
            diagnostics.append(
                "未同时找到 defectguard-clang 与 compile_commands.json，已使用可运行降级解析器"
            )

        rule_findings = self.rule_engine.inspect(parsed_files)
        model_findings = self.model_backend.detect(parsed_files)
        if hasattr(self.model_backend, "diagnostics"):
            diagnostics.extend(self.model_backend.diagnostics())
        model_findings = [finding for finding in model_findings
                          if SEVERITY_RANK[finding.severity] >= SEVERITY_RANK[Severity(self.config.minimum_severity)]]
        findings = rule_findings + model_findings
        diagnostics.extend(
            [
            f"{item.relative_path}: {message}"
            for item in parsed_files
            for message in item.diagnostics
            ]
        )
        if not parsed_files:
            diagnostics.append("未发现可分析的 C/C++ 源文件")
        if self.config.model_backend != "disabled":
            diagnostics.append(
                "已启用实验性二分类模型；概率未经校准，不能视为安全证明或已确认缺陷。" + self.model_backend.status()
            )
        if self.config.repair_backend != "disabled":
            diagnostics.append(
                f"修复后端 {self.config.repair_backend} 属于第三层，当前第一层运行中未启用"
            )
        metrics = calculate_project_metrics(parsed_files, len(findings))
        verification = VerificationResult(status="skipped", steps=(), workspace_removed=True)
        if run_verification:
            verification = IsolatedVerifier(self.config.timeout_seconds).verify(
                source_root,
                self.config.build_command,
                self.config.test_command,
                self.config.exclude,
            )
            if verification.status == "skipped":
                diagnostics.append("已请求验证，但配置中没有构建或测试命令")
        source_unchanged = source_fingerprint == _fingerprint(parsed_files)
        if not source_unchanged:
            diagnostics.append("扫描或验证期间检测到原始源码内容发生变化")
        return AnalysisReport(
            run_id=uuid.uuid4().hex,
            source_root=str(source_root),
            source_fingerprint=source_fingerprint,
            parser_backend=parser_backend,
            project=_project_info(
                source_root,
                parsed_files,
                self.config.project_name,
                compilation_database,
            ),
            program_structure=program_structure,
            findings=findings,
            metrics=metrics,
            verification=verification,
            source_unchanged=source_unchanged,
            model_status=self.model_backend.status(),
            repair_status=self.patch_generator.status(),
            diagnostics=diagnostics,
        )
