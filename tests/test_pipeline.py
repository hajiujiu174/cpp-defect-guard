from __future__ import annotations

import hashlib
from contextlib import closing
from dataclasses import replace
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

from defectguard.config import DEFAULT_CONFIG
from defectguard.pipeline import AnalysisPipeline
from defectguard.reporting import write_reports
from defectguard.storage import RunStore
from defectguard.verification import IsolatedVerifier


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PipelineTests(unittest.TestCase):
    def test_scan_exports_reports_and_keeps_source_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "source"
            output = root / "output"
            source.mkdir()
            main = source / "main.c"
            main.write_text("int main(void) { char s[2]; gets(s); return 0; }\n", encoding="utf-8")
            before = _digest(main)

            report = AnalysisPipeline(DEFAULT_CONFIG).scan(source)
            paths = write_reports(report, output, ("json", "markdown", "html"))
            RunStore(output / "runs.sqlite3").save(report)

            self.assertEqual(before, _digest(main))
            self.assertEqual(3, len(paths))
            self.assertTrue(all(path.is_file() for path in paths))
            self.assertTrue((output / "runs.sqlite3").is_file())
            self.assertGreaterEqual(len(report.findings), 1)
            self.assertTrue(report.source_unchanged)
            self.assertGreaterEqual(report.program_structure.function_count, 1)
            self.assertIn("verification", json.loads((output / "report.json").read_text(encoding="utf-8")))
            with closing(sqlite3.connect(output / "runs.sqlite3")) as connection:
                run_columns = {
                    row[1] for row in connection.execute("PRAGMA table_info(analysis_runs)")
                }
                finding_columns = {
                    row[1] for row in connection.execute("PRAGMA table_info(findings)")
                }
            self.assertIn("verification_status", run_columns)
            self.assertIn("confidence", finding_columns)

    def test_verification_runs_only_in_temporary_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source"
            source.mkdir()
            (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            build = (
                sys.executable,
                "-c",
                "from pathlib import Path; Path('generated.txt').write_text('ok')",
            )
            test = (
                sys.executable,
                "-c",
                "from pathlib import Path; raise SystemExit(0 if Path('generated.txt').exists() else 1)",
            )
            result = IsolatedVerifier(30).verify(source, build, test)

            self.assertEqual("passed", result.status)
            self.assertTrue(result.workspace_removed)
            self.assertFalse((source / "generated.txt").exists())

    def test_pipeline_can_run_integrated_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source"
            source.mkdir()
            (source / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            build = (
                sys.executable,
                "-c",
                "from pathlib import Path; Path('built.txt').write_text('ok')",
            )
            test = (
                sys.executable,
                "-c",
                "from pathlib import Path; raise SystemExit(0 if Path('built.txt').exists() else 1)",
            )
            config = replace(DEFAULT_CONFIG, build_command=build, test_command=test)
            report = AnalysisPipeline(config).scan(source, run_verification=True)

            self.assertEqual("passed", report.verification.status)
            self.assertTrue(report.verification.workspace_removed)
            self.assertTrue(report.source_unchanged)
            self.assertFalse((source / "built.txt").exists())



if __name__ == "__main__":
    unittest.main()
