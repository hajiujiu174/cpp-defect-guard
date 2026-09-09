from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from defectguard.analysis.rules import RuleEngine
from defectguard.parser import FallbackProjectParser


class RuleEngineTests(unittest.TestCase):
    def test_baseline_rules_find_expected_sample_defects(self) -> None:
        source = """
        #include <cstring>
        int main(int argc, char** argv) {
            char name[4] = {};
            strcpy(name, argv[1]);
            int values[2] = {0, 1};
            values[2] = 3;
            int* data = new int[3];
            if (argc = 2) { return data[0]; }
            return 0;
        }
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "main.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)
            findings = RuleEngine(("DG001", "DG002", "DG003", "DG004")).inspect(files)

        self.assertEqual({"DG001", "DG002", "DG003", "DG004"}, {item.rule_id for item in findings})

    def test_all_layer1_rules_and_comment_masking(self) -> None:
        source = r'''
        #include <cstdio>
        #include <cstring>
        int main(int argc, char** argv) {
            // strcpy(comment_only, value);
            const char* text = "gets(in_a_string)";
            char name[4] = {};
            strcpy(name, argv[1]);
            int values[2] = {0, 1};
            values[2] = 3;
            int* data = new int[3];
            int* pointer = nullptr;
            if (argc > 8) { *pointer = 1; }
            int result;
            if (argc > 8) { return result; }
            FILE* input = fopen("missing.txt", "r");
            if (argc = 2) { return data[0] + static_cast<int>(text[0]); }
            return 0;
        }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "main.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)
            findings = RuleEngine(
                ("DG001", "DG002", "DG003", "DG004", "DG005", "DG006", "DG007")
            ).inspect(files)

        ids = {item.rule_id for item in findings}
        self.assertEqual(
            {"DG001", "DG002", "DG003", "DG004", "DG005", "DG006", "DG007"}, ids
        )
        self.assertEqual(1, sum(item.rule_id == "DG001" for item in findings))

    def test_release_initialization_and_reassignment_suppress_findings(self) -> None:
        source = r'''
        #include <cstdio>
        int main() {
            int* data = new int[2];
            delete[] data;
            int value;
            value = 1;
            int target = 0;
            int* pointer = nullptr;
            pointer = &target;
            *pointer = value;
            FILE* input = fopen("missing.txt", "r");
            if (input) { fclose(input); }
            return 0;
        }
        '''
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "main.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)
            findings = RuleEngine(("DG003", "DG005", "DG006", "DG007")).inspect(files)

        self.assertEqual([], findings)

    def test_minimum_severity_filters_lower_findings(self) -> None:
        source = "int main(int argc) { if (argc = 2) { return 0; } return 1; }\n"
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "main.cpp").write_text(source, encoding="utf-8")
            files = FallbackProjectParser((), 1024 * 1024).parse(root)
            findings = RuleEngine(("DG004",), minimum_severity="high").inspect(files)

        self.assertEqual([], findings)

    def test_repository_sample_pair_has_expected_rule_delta(self) -> None:
        project_root = Path(__file__).resolve().parents[1]
        rule_ids = ("DG001", "DG002", "DG003", "DG004", "DG005", "DG006", "DG007")
        parser = FallbackProjectParser((), 1024 * 1024)
        engine = RuleEngine(rule_ids)

        vulnerable = engine.inspect(parser.parse(project_root / "samples" / "vulnerable"))
        fixed = engine.inspect(parser.parse(project_root / "samples" / "fixed"))

        self.assertEqual(set(rule_ids), {item.rule_id for item in vulnerable})
        self.assertEqual([], fixed)


if __name__ == "__main__":
    unittest.main()
