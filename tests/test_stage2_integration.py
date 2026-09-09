from __future__ import annotations

from contextlib import redirect_stdout
from dataclasses import replace
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from defectguard.cli import main
from defectguard.config import DEFAULT_CONFIG, load_config
from defectguard.domain import Finding, Severity, SourceLocation
from defectguard.pipeline import AnalysisPipeline


class Stage2IntegrationTests(unittest.TestCase):
    def test_doctor_json_exit_code_reflects_enabled_model_preflight(self):
        config = replace(DEFAULT_CONFIG, model_backend="trained", model_checkpoint="not-loaded-by-doctor")
        result = {"layer1": {"fallback_scan_ready": True, "native_clang_ready": True},
                  "layer2": {"model_preflight_ready": False}, "model_preflight": {"status": "blocked"}}
        for expected in (2, 0):
            result["layer2"]["model_preflight_ready"] = expected == 0
            output = io.StringIO()
            with patch("defectguard.cli.load_config", return_value=config), patch("defectguard.cli.inspect_environment", return_value=result), redirect_stdout(output):
                self.assertEqual(expected, main(["doctor", ".", "--json"]))
            self.assertEqual(result, json.loads(output.getvalue()))

    def test_doctor_strict_native_config_does_not_claim_ready_on_fallback_only(self):
        result = {"layer1": {"fallback_scan_ready": True, "native_clang_ready": False},
                  "layer2": {"model_preflight_ready": False}}
        with patch("defectguard.cli.load_config", return_value=replace(DEFAULT_CONFIG, parser_backend="clang")), patch("defectguard.cli.inspect_environment", return_value=result), redirect_stdout(io.StringIO()):
            self.assertEqual(2, main(["doctor", ".", "--json"]))

    def test_model_threshold_rejects_coerced_toml_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "config.toml"
            for value in ('true', '"0.5"', '[0.5]', 'nan', 'inf'):
                path.write_text(f"[models]\nthreshold={value}\n", encoding="utf-8")
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, "models.threshold"):
                    load_config(path)

    def test_model_coverage_diagnostics_reach_report_and_severity_filter_remains_active(self):
        class Detector:
            def detect(self, files):
                return [Finding("ML001", "model_suspected_defect", Severity.MEDIUM,
                                SourceLocation(files[0].relative_path, 1), "suspected", "", "review", "model-gnn", 0.7)]

            def status(self):
                return "trained:gnn"

            def diagnostics(self):
                return ["模型未覆盖文件 unused.h：缺少 Clang 原生程序图；不能将其视为模型检查通过。"]

        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            config = replace(DEFAULT_CONFIG, parser_backend="fallback", model_backend="trained", minimum_severity="high")
            with patch("defectguard.pipeline.create_detector", return_value=Detector()):
                report = AnalysisPipeline(config).scan(source)
            self.assertEqual([], report.findings)
            self.assertTrue(any("unused.h" in line for line in report.diagnostics))
            self.assertTrue(report.source_unchanged)


if __name__ == "__main__":
    unittest.main()
