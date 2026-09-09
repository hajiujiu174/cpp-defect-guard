from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from defectguard.analysis.metrics import calculate_project_metrics
from defectguard.parser import FallbackProjectParser


class ParserAndMetricsTests(unittest.TestCase):
    def test_function_boundaries_and_approximate_cfg_are_built(self) -> None:
        source = """
        #include <vector>
        const char* label() { return "ok"; }
        std::vector<int> make_values() { return {}; }
        int classify(int value) {
            if (value > 0) {
                return 1;
            }
            return 0;
        }
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "sample.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)

        self.assertEqual(1, len(files))
        self.assertEqual(("label", "make_values", "classify"), files[0].function_names)
        function = files[0].functions[-1]
        self.assertGreaterEqual(function.cyclomatic_complexity, 2)
        self.assertGreaterEqual(len(function.cfg_nodes), 3)
        self.assertGreaterEqual(len(function.cfg_edges), 2)
        self.assertEqual(("vector",), files[0].include_targets)

    def test_quality_metrics_include_comments_duplicates_and_dependencies(self) -> None:
        source = """
        #include <vector>
        // comment
        int first() { return 1; }
        int second() { return 1; }
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "sample.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)
            metrics = calculate_project_metrics(files, finding_count=2)

        self.assertEqual(2, metrics.function_count)
        self.assertEqual(1, metrics.include_dependency_count)
        self.assertGreater(metrics.comment_ratio, 0)
        self.assertGreater(metrics.warning_density_per_kloc, 0)


if __name__ == "__main__":
    unittest.main()
