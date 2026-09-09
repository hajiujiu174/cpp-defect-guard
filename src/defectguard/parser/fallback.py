from __future__ import annotations

from pathlib import Path
import re

from defectguard.analysis.source_utils import line_number_at, mask_comments_and_strings
from defectguard.domain import CFGEdge, CFGNode, FunctionStructure, ParsedFile


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
FUNCTION_PATTERN = re.compile(
    r"(?:^|\n)\s*(?P<return>[A-Za-z_][\w:<>,\s*&]*?(?:[*&]|\s))"
    r"(?P<name>[A-Za-z_]\w*(?:::\w+)*)\s*\((?P<params>[^;{}]*)\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?\{",
    re.MULTILINE,
)
INCLUDE_PATTERN = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE)
BRANCH_PATTERN = re.compile(r"\b(?:if|for|while|case|catch)\b|&&|\|\|")


class FallbackProjectParser:
    """无额外依赖的降级解析器，用于让第一层骨架可直接运行。"""

    name = "fallback-regex"

    def __init__(self, exclude: tuple[str, ...], max_file_bytes: int) -> None:
        self.exclude = {item.casefold() for item in exclude}
        self.max_file_bytes = max_file_bytes

    def parse(self, source_root: Path) -> list[ParsedFile]:
        parsed: list[ParsedFile] = []
        for path in sorted(source_root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(source_root)
            if any(part.casefold() in self.exclude for part in relative.parts):
                continue
            diagnostics: list[str] = []
            try:
                file_size = path.stat().st_size
            except OSError as error:
                parsed.append(
                    ParsedFile(
                        path=path,
                        relative_path=relative.as_posix(),
                        language=self._language(path),
                        text="",
                        diagnostics=(f"无法读取文件元数据：{error}",),
                    )
                )
                continue
            if file_size > self.max_file_bytes:
                diagnostics.append(f"文件超过大小限制 {self.max_file_bytes} 字节，已跳过")
                parsed.append(
                    ParsedFile(
                        path=path,
                        relative_path=relative.as_posix(),
                        language=self._language(path),
                        text="",
                        diagnostics=tuple(diagnostics),
                    )
                )
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                try:
                    text = path.read_text(encoding="gb18030", errors="replace")
                    diagnostics.append("源码不是 UTF-8，已按 GB18030 降级读取")
                except OSError as error:
                    text = ""
                    diagnostics.append(f"源码读取失败：{error}")
            except OSError as error:
                text = ""
                diagnostics.append(f"源码读取失败：{error}")
            sanitized = mask_comments_and_strings(text)
            functions = self._functions(sanitized, relative.as_posix())
            parsed.append(
                ParsedFile(
                    path=path,
                    relative_path=relative.as_posix(),
                    language=self._language(path),
                    text=text,
                    functions=functions,
                    include_targets=tuple(INCLUDE_PATTERN.findall(text)),
                    backend=self.name,
                    diagnostics=tuple(diagnostics),
                )
            )
        return parsed

    def _functions(
        self, sanitized_text: str, relative_path: str
    ) -> tuple[FunctionStructure, ...]:
        functions: list[FunctionStructure] = []
        for match in FUNCTION_PATTERN.finditer(sanitized_text):
            open_brace = sanitized_text.find("{", match.start(), match.end())
            close_brace = self._matching_brace(sanitized_text, open_brace)
            start_line = line_number_at(sanitized_text, match.start())
            end_line = line_number_at(
                sanitized_text, close_brace if close_brace is not None else len(sanitized_text)
            )
            parameters = match.group("params").strip()
            parameter_count = 0 if not parameters or parameters == "void" else parameters.count(",") + 1
            body_end = close_brace + 1 if close_brace is not None else len(sanitized_text)
            body = sanitized_text[open_brace:body_end]
            complexity = 1 + len(BRANCH_PATTERN.findall(body))
            nodes, edges = self._build_cfg(
                relative_path, match.group("name"), start_line, end_line, body
            )
            functions.append(
                FunctionStructure(
                    name=match.group("name"),
                    return_type=" ".join(match.group("return").split()),
                    parameter_count=parameter_count,
                    start_line=start_line,
                    end_line=end_line,
                    cyclomatic_complexity=complexity,
                    cfg_nodes=nodes,
                    cfg_edges=edges,
                )
            )
        return tuple(functions)

    @staticmethod
    def _matching_brace(text: str, open_brace: int) -> int | None:
        if open_brace < 0:
            return None
        depth = 0
        for index in range(open_brace, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    return index
        return None

    @staticmethod
    def _build_cfg(
        relative_path: str,
        function_name: str,
        start_line: int,
        end_line: int,
        body: str,
    ) -> tuple[tuple[CFGNode, ...], tuple[CFGEdge, ...]]:
        prefix = f"{relative_path}:{function_name}:{start_line}"
        entry_id = f"{prefix}:entry"
        exit_id = f"{prefix}:exit"
        nodes: list[CFGNode] = [CFGNode(entry_id, start_line, "entry", "entry")]
        statements: list[CFGNode] = []
        for offset, raw_line in enumerate(body.splitlines(), 0):
            line = raw_line.strip().strip("{} ")
            if not line:
                continue
            line_number = start_line + offset
            if re.search(r"\breturn\b", line):
                kind = "return"
            elif re.search(r"\b(?:if|switch)\s*\(", line):
                kind = "branch"
            elif re.search(r"\b(?:for|while)\s*\(", line):
                kind = "loop"
            else:
                kind = "statement"
            node_id = f"{prefix}:n{len(statements) + 1}"
            statements.append(CFGNode(node_id, line_number, kind, line[:160]))
        nodes.extend(statements)
        nodes.append(CFGNode(exit_id, end_line, "exit", "exit"))

        edges: list[CFGEdge] = []
        if statements:
            edges.append(CFGEdge(entry_id, statements[0].node_id, "next"))
        else:
            edges.append(CFGEdge(entry_id, exit_id, "next"))
        for index, node in enumerate(statements):
            following = statements[index + 1].node_id if index + 1 < len(statements) else exit_id
            if node.kind == "return":
                edges.append(CFGEdge(node.node_id, exit_id, "return"))
                continue
            edges.append(CFGEdge(node.node_id, following, "next"))
            if node.kind in {"branch", "loop"}:
                alternate = (
                    statements[index + 2].node_id
                    if index + 2 < len(statements)
                    else exit_id
                )
                edges.append(CFGEdge(node.node_id, alternate, "false-approx"))
        unique_edges = tuple(dict.fromkeys(edges))
        return tuple(nodes), unique_edges

    @staticmethod
    def _language(path: Path) -> str:
        return "c" if path.suffix.lower() == ".c" else "cpp"
