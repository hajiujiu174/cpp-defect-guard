from __future__ import annotations

from dataclasses import replace
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from defectguard.config import DEFAULT_CONFIG
from defectguard.graphs import graph_records, write_graphs
from defectguard.parser.clang_tool import ClangToolClient
from defectguard.pipeline import AnalysisPipeline


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "native" / "clang_tool" / "build" / ("defectguard-clang.exe" if os.name == "nt" else "defectguard-clang")


class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("clang++")
        if not TOOL.is_file() or not cls.compiler:
            if os.environ.get("DEFECTGUARD_REQUIRE_NATIVE") == "1":
                raise RuntimeError("要求原生验收，但未找到 Clang 编译器或 defectguard-clang")
            raise unittest.SkipTest("未安装原生工具，设置 DEFECTGUARD_REQUIRE_NATIVE=1 可强制验收")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="defectguard-native-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "中文 source"
        self.root.mkdir()
        self.config = replace(DEFAULT_CONFIG, parser_backend="clang", clang_tool=str(TOOL))

    def scan(self, files):
        commands = []
        for name, source in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            if path.suffix in {".cpp", ".c"}:
                compiler = shutil.which("clang") if path.suffix == ".c" else self.compiler
                commands.append({"directory": str(self.root), "file": str(path), "arguments": [
                    compiler, "-std=c11" if path.suffix == ".c" else "-std=c++17",
                    "-I", str(self.root), "-c", str(path),
                ]})
        (self.root / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
        return AnalysisPipeline(self.config).scan(self.root)

    def test_constant_bounds_macros_negative_index_and_variable_scope(self):
        report = self.scan({"sample.cpp": """
#define CAPACITY 2
void bad() { int items[CAPACITY] = {}; items[1 + 1] = 3; items[-1] = 4; }
void good() { int items[8] = {}; items[4] = 1; (void)sizeof(items[99]); }
"""})
        bounds = [finding for finding in report.findings if finding.rule_id == "DG002"]
        self.assertEqual(2, len(bounds))
        self.assertTrue(all(finding.detector == "clang-ast" for finding in bounds))
        self.assertTrue(any("-1" in finding.message for finding in bounds))

    def test_library_call_is_distinguished_from_same_named_method(self):
        report = self.scan({"sample.cpp": """
#include <cstring>
struct Custom { void strcpy(const char*) {} };
void copy(char* output, const char* input) {
    std::strcpy(output, input);
    Custom object; object.strcpy(input);
    const char* text = "strcpy(in_a_string)";
    // strcpy(in_a_comment);
}
"""})
        calls = [finding for finding in report.findings if finding.rule_id == "DG001"]
        self.assertEqual(1, len(calls))
        self.assertEqual(5, calls[0].location.line)
        self.assertEqual("clang-ast", calls[0].detector)

    def test_multiline_conditions_and_explicit_assignment(self):
        report = self.scan({"sample.cpp": """
int next();
void check(int value) {
    if (
        value = next()
    ) {}
    while (value = next()) { break; }
    for (; value = next(); ) { break; }
    if ((value = next())) {}
    if ((value = next()) != 0) {}
    if (value == 1) {}
}
"""})
        assignments = [item for item in report.findings if item.rule_id == "DG004"]
        self.assertEqual({4, 7, 8}, {item.location.line for item in assignments})
        self.assertEqual(3, len(assignments))

    def test_shared_header_is_not_parsed_twice_and_templates_have_types(self):
        report = self.scan({
            "common.hpp": "#pragma once\ninline int twice(int v) { return v * 2; }\n",
            "a.cpp": '#include "common.hpp"\nint first() { return twice(1); }\n',
            "b.cpp": '#include "common.hpp"\nint second() { return twice(2); }\n',
        })
        functions = [fn for file in report.program_structure.files for fn in file.functions]
        self.assertEqual({"first", "second", "twice"}, {fn.name for fn in functions})
        self.assertEqual(3, len(functions))
        self.assertEqual(3, report.metrics.function_count)
        self.assertTrue(all(fn.ast_nodes for fn in functions))

    def test_template_and_c_translation_units(self):
        report = self.scan({
            "template.cpp": "template<class T> T identity(T value) { return value; }\nint use() { return identity(2); }\n",
            "plain.c": "int add(int a, int b) { return a + b; }\n",
        })
        functions = [fn for file in report.program_structure.files for fn in file.functions]
        self.assertTrue(any(fn.name == "identity" for fn in functions))
        self.assertTrue(any(fn.name == "add" and fn.signature for fn in functions))
        self.assertTrue(all(fn.ast_nodes for fn in functions))

    def test_literal_values_and_binding_metadata_survive_native_export(self):
        report = self.scan({'literal.c': "void f(void) { int a[50]; int b[100]; a[2] = 100 - 1; b[0] = 'A'; }"})
        nodes = [n for f in report.program_structure.files for fn in f.functions for n in fn.ast_nodes]
        self.assertIn('100', {n.name for n in nodes if n.kind == 'IntegerLiteral'})
        self.assertIn('65', {n.name for n in nodes if n.kind == 'CharacterLiteral'})
        self.assertIn('-', {n.name for n in nodes if n.kind == 'BinaryOperator'})
        self.assertEqual(2, len({n.symbol for n in nodes if n.kind == 'VarDecl'}))

    def test_unreferenced_header_keeps_lexical_analysis_with_diagnostic(self):
        report = self.scan({
            "unused.hpp": "inline void unchecked(char* p) { gets(p); }\n",
            "sample.cpp": "int main() { return 0; }\n",
        })
        findings = [finding for finding in report.findings if finding.location.file == "unused.hpp"]
        self.assertTrue(any(finding.rule_id == "DG001" and finding.detector == "rule" for finding in findings))
        self.assertTrue(any("unused.hpp" in diagnostic for diagnostic in report.diagnostics))

    def test_branch_definitions_reach_join_and_metrics_use_native_cfg(self):
        report = self.scan({"sample.cpp": """int choose(bool flag) {
    int value = 0;
    if (flag) {
        value = 1;
    } else {
        value = 2;
    }
    return value;
}
"""})
        function = report.program_structure.files[0].functions[0]
        nodes = {node.node_id: node for node in function.ast_nodes}
        reaching_lines = {nodes[edge.source].line for edge in function.dfg_edges if nodes[edge.target].line == 8}
        self.assertEqual({4, 6}, reaching_lines)
        self.assertEqual(function.cyclomatic_complexity, report.metrics.cyclomatic_complexity)

    def test_shadowed_variables_do_not_share_definitions(self):
        report = self.scan({"sample.cpp": """void sink(int);
int check() {
    int value = 1;
    { int value = 2; sink(value); }
    return value;
}
"""})
        function = next(fn for file in report.program_structure.files for fn in file.functions if fn.name == "check")
        nodes = {node.node_id: node for node in function.ast_nodes}
        declarations = [node for node in nodes.values() if node.kind == "VarDecl" and node.name == "value"]
        self.assertEqual(2, len({node.symbol for node in declarations}))
        self.assertEqual({3}, {nodes[edge.source].line for edge in function.dfg_edges if nodes[edge.target].line == 5})

    def test_loop_definitions_include_initial_and_updated_value(self):
        report = self.scan({"sample.cpp": """int sum(int count) {
    int total = 0;
    while (count-- > 0) {
        total += count;
    }
    return total;
}
"""})
        function = report.program_structure.files[0].functions[0]
        nodes = {node.node_id: node for node in function.ast_nodes}
        self.assertEqual({2, 4}, {nodes[edge.source].line for edge in function.dfg_edges if nodes[edge.target].line == 6})

    def test_multiple_declarations_preserve_initializer_dependencies(self):
        report = self.scan({"sample.cpp": """int chain(int input) {
    int first = input, second = first + 1;
    return second;
}
"""})
        function = report.program_structure.files[0].functions[0]
        nodes = {node.node_id: node for node in function.ast_nodes}
        self.assertEqual({"first", "second"}, {
            nodes[edge.source].name for edge in function.dfg_edges if nodes[edge.source].kind == "VarDecl"
        })

    def test_graph_exports_are_deterministic_unlabeled_and_have_valid_indices(self):
        report = self.scan({"sample.cpp": "int plus(int value) { int result = value + 1; return result; }\n"})
        output = Path(self.temp.name) / "graphs"
        paths = write_graphs(report, output)
        first = paths[0].read_bytes()
        repeated = AnalysisPipeline(self.config).scan(self.root)
        write_graphs(repeated, output)
        self.assertEqual(first, paths[0].read_bytes())
        graph = json.loads(first.decode("utf-8"))
        self.assertIsNone(graph["label"])
        self.assertEqual(len(graph["edges"]), len(graph["edge_type"]))
        self.assertTrue(all(0 <= index < len(graph["nodes"]) for row in graph["edge_index"] for index in row))
        self.assertEqual({"ast-child", "cfg-successor", "cfg-contains", "def-use"}, {edge["kind"] for edge in graph["edges"]})
        (self.root / "sample.cpp").write_text("int changed() { return 0; }", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "发生变化"):
            graph_records(report)

    def test_same_line_functions_have_exact_source_slices(self):
        report = self.scan({"sample.cpp": 'const char* title = "中文"; int first() { return 1; } int second() { return 2; }\n'})
        records = graph_records(report)
        self.assertEqual({"int first() { return 1; }", "int second() { return 2; }"},
                         {record["source"] for record in records})
        self.assertTrue(all(record["source_slice"] == "token-range" for record in records))

    def test_native_rules_still_respect_enable_and_severity_filters(self):
        self.config = replace(self.config, enabled_rules=("DG002",), minimum_severity="high")
        report = self.scan({"sample.cpp": "int check(int x) { int a[2]; a[2] = 1; if (x = 2) {} return 0; }"})
        self.assertEqual(["DG002"], [finding.rule_id for finding in report.findings])

    def test_parse_failure_is_explicit_or_recorded_on_fallback(self):
        with self.assertRaisesRegex(ValueError, "Clang 解析失败"):
            self.scan({"sample.cpp": "int broken( {"})
        report = AnalysisPipeline(replace(self.config, parser_backend="auto")).scan(self.root)
        self.assertEqual("fallback-regex", report.parser_backend)
        self.assertTrue(any("Clang 解析失败" in message for message in report.diagnostics))
        with self.assertRaisesRegex(ValueError, "图导出需要"):
            graph_records(report)


class NativeClientTests(unittest.TestCase):
    def test_large_projects_are_batched_and_shared_header_functions_deduplicated(self):
        client = ClangToolClient(Path(__file__))
        shared = {"file": "shared.hpp", "name": "shared", "source_start": 0, "source_end": 10}
        def respond(command, **kwargs):
            functions = [shared] + [{"file": path, "name": "process", "source_start": 0, "source_end": 20} for path in command[3:]]
            return subprocess.CompletedProcess(command, 0, json.dumps({"schema_version": "1.1", "functions": functions}), "batch diagnostic")
        files = [Path(f"source{index}.c") for index in range(70)] + [Path("shared.hpp")]
        with patch("defectguard.parser.clang_tool.subprocess.run", side_effect=respond) as run:
            result = client.analyze(files, Path("build"))
        self.assertEqual(3, run.call_count)
        self.assertEqual(71, len(result["functions"]))
        self.assertEqual(70, len(result["translation_units"]))
        self.assertEqual(3, len(result["diagnostics"]))
        self.assertTrue(all(len(call.args[0]) <= 35 for call in run.call_args_list))

    def test_batch_failure_does_not_return_partial_success(self):
        client = ClangToolClient(Path(__file__))
        success = subprocess.CompletedProcess([], 0, '{"schema_version":"1.1","functions":[]}', "")
        failure = subprocess.CompletedProcess([], 1, "", "injected parse failure")
        with patch("defectguard.parser.clang_tool.subprocess.run", side_effect=[success, failure]):
            with self.assertRaisesRegex(RuntimeError, "injected parse failure"):
                client.analyze([Path(f"source{i}.c") for i in range(40)], Path("build"))

    def test_timeout_has_readable_error(self):
        client = ClangToolClient(Path(__file__), timeout_seconds=1)
        with patch("defectguard.parser.clang_tool.subprocess.run", side_effect=subprocess.TimeoutExpired("tool", 1)):
            with self.assertRaisesRegex(RuntimeError, "超过 1 秒"):
                client.analyze([Path("source.cpp")], Path("build"))

    def test_invalid_protocol_is_not_accepted(self):
        client = ClangToolClient(Path(__file__))
        result = subprocess.CompletedProcess([], 0, stdout='{"schema_version":"9.9","functions":[]}', stderr="")
        with patch("defectguard.parser.clang_tool.subprocess.run", return_value=result):
            with self.assertRaisesRegex(ValueError, "协议版本"):
                client.analyze([Path("source.cpp")], Path("build"))


if __name__ == "__main__":
    unittest.main()
