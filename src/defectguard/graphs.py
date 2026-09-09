"""可复现的函数图导出；规则输出是弱信号，不自动转换为训练标签。"""
from __future__ import annotations

from dataclasses import asdict
import hashlib
import json
from pathlib import Path
from typing import Any

from defectguard.domain import AnalysisReport, FileStructure, FunctionStructure


EDGE_TYPES = {"ast-child": 0, "cfg-successor": 1, "cfg-contains": 2, "def-use": 3}
DFG_LIMITATIONS = [
    "函数内、路径不敏感的局部标量到达定义分析",
    "不建模指针别名、堆对象、数组元素、引用写入或调用副作用",
    "未初始化读取可能没有定义边；没有边不等于没有缺陷",
]


def graph_records(report: AnalysisReport) -> list[dict[str, Any]]:
    if report.parser_backend != "clang-libtooling" or not report.program_structure.ast_node_count:
        raise ValueError("图导出需要新版 Clang 原生分析器和编译数据库；降级解析结果不能充当 AST/DFG")
    if not report.source_unchanged:
        raise ValueError("源码在分析期间发生变化，不能导出图数据")
    root = Path(report.source_root).resolve()
    sources: dict[str, str] = {}
    raw_sources: dict[str, bytes] = {}
    source_encodings: dict[str, str] = {}
    digest = hashlib.sha256()
    for file in report.metrics.files:
        path = (root / file.file).resolve()
        if not path.is_relative_to(root):
            raise ValueError("图数据引用了工程外源码")
        raw = path.read_bytes()
        raw_sources[file.file] = raw
        digest.update(file.file.encode("utf-8"))
        digest.update(b"\0")
        digest.update(raw)
        digest.update(b"\0")
        try:
            sources[file.file] = raw.decode("utf-8")
            source_encodings[file.file] = "utf-8"
        except UnicodeDecodeError:
            sources[file.file] = raw.decode("gb18030", errors="replace")
            source_encodings[file.file] = "gb18030"
    if digest.hexdigest() != report.source_fingerprint:
        raise ValueError("源码在分析后发生变化，请重新扫描后导出")
    return _structure_records(report.program_structure.files, sources, raw_sources, source_encodings, report.findings)


def function_record(file: str, function: FunctionStructure, raw: bytes) -> dict[str, Any]:
    """在线推理与训练导出共用图构造及字节范围切片。"""
    try:
        text, encoding = raw.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        text, encoding = raw.decode("gb18030", errors="replace"), "gb18030"
    structure = FileStructure(file=file, language="C++", backend="clang-libtooling", functions=(function,))
    return _structure_records((structure,), {file: text}, {file: raw}, {file: encoding}, [])[0]


def _structure_records(files, sources, raw_sources, source_encodings, findings) -> list[dict[str, Any]]:

    records: list[dict[str, Any]] = []
    for file in files:
        for function in file.functions:
            if not function.ast_nodes:
                raise ValueError(f"函数缺少 AST：{file.file}:{function.name}")
            source = "\n".join(sources[file.file].splitlines()[function.start_line - 1:function.end_line])
            source_slice = "line-range-fallback"
            if 0 <= function.source_start < function.source_end <= len(raw_sources[file.file]):
                source = raw_sources[file.file][function.source_start:function.source_end].decode(
                    source_encodings[file.file], errors="replace"
                )
                source_slice = "token-range"
            source_hash = hashlib.sha256(source.encode("utf-8")).hexdigest()
            identity = f"{file.file}:{function.name}:{function.signature}:{function.start_line}:{source_hash}"
            nodes = [dict(asdict(node), layer="ast") for node in function.ast_nodes]
            nodes.extend({
                "node_id": node.node_id, "kind": node.kind, "line": node.line,
                "column": 1, "end_line": node.line, "name": node.label,
                "type_name": "", "symbol": "", "layer": "cfg",
            } for node in function.cfg_nodes)
            index = {node["node_id"]: position for position, node in enumerate(nodes)}
            if len(index) != len(nodes):
                raise ValueError("AST/CFG 节点 ID 冲突")
            edges = [asdict(edge) for edge in function.ast_edges]
            edges.extend(dict(asdict(edge), kind="cfg-successor", symbol="") for edge in function.cfg_edges)
            edges.extend({"source": block.node_id, "target": ast_id,
                          "kind": "cfg-contains", "symbol": ""}
                         for block in function.cfg_nodes for ast_id in block.ast_nodes)
            edges.extend(asdict(edge) for edge in function.dfg_edges)
            if any(edge["source"] not in index or edge["target"] not in index for edge in edges):
                raise ValueError("程序图存在悬空边")
            records.append({
                "schema_version": "1.0", "graph_id": hashlib.sha256(identity.encode("utf-8")).hexdigest(),
                "file": file.file, "function": function.name, "signature": function.signature,
                "start_line": function.start_line, "end_line": function.end_line,
                "source": source, "source_sha256": source_hash,
                "source_slice": source_slice,
                "label": None, "label_source": "unlabeled",
                "nodes": nodes, "edges": edges,
                "edge_index": [[index[edge["source"]] for edge in edges], [index[edge["target"]] for edge in edges]],
                "edge_type": [EDGE_TYPES[edge["kind"]] for edge in edges],
                "dfg_status": function.dfg_status,
                "calls": [{"node_id": node.node_id, "callee": node.name, "line": node.line}
                          for node in function.ast_nodes if node.kind.endswith("CallExpr")],
                "rule_findings": [finding.to_dict() for finding in findings
                                  if finding.detector in {"rule", "clang-ast"} and finding.location.file == file.file and
                                  function.start_line <= finding.location.line <= function.end_line],
            })
    return records


def write_graphs(report: AnalysisReport, output: Path) -> tuple[Path, Path]:
    if output.resolve().is_relative_to(Path(report.source_root).resolve()):
        raise ValueError("图数据输出目录不能位于原始源码目录内部")
    records = graph_records(report)
    encoded = "".join(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n" for record in records)
    manifest = {
        "schema_version": "1.0", "parser_backend": report.parser_backend,
        "source_fingerprint": report.source_fingerprint,
        "graphs_sha256": hashlib.sha256(encoded.encode("utf-8")).hexdigest(),
        "function_count": len(records), "edge_types": EDGE_TYPES,
        "files_without_function_graphs": sorted(
            {file.file for file in report.metrics.files} - {record["file"] for record in records}
        ),
        "label_status": "unlabeled: 规则告警不能替代人工或数据集真值标签",
        "dfg_limitations": DFG_LIMITATIONS,
    }
    output.mkdir(parents=True, exist_ok=True)
    graphs_path, manifest_path = output / "graphs.jsonl", output / "manifest.json"
    graphs_path.write_text(encoded, encoding="utf-8", newline="\n")
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return graphs_path, manifest_path
