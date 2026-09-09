from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from defectguard.experiments.juliet import _validate_members, clean_local_names, extract_pair, import_juliet, strip_comments, template_hash


def testcase(cwe, bad, good, *, extra="", newline="\n"):
    name = f"CWE{cwe}_Example__local_01"
    text = f'''/* TEMPLATE GENERATED TESTCASE FILE
Label Definition File: CWE{cwe}_Example.label.xml
Flow Variant: 01 Baseline
*/
#include "std_testcase.h"
#ifndef OMITBAD
void {name}_bad()
{{
    /* POTENTIAL FLAW: deliberate test */
    {bad}
}}
#endif
#ifndef OMITGOOD
static void goodG2B()
{{
    /* FIX: controlled negative */
    {good}
}}
void {name}_good() {{ goodG2B(); }}
{extra}
#endif
'''
    return name + ".c", text.replace("\n", newline)


class JulietTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="defectguard-juliet-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "downloads").mkdir()
        self.archive = self.root / "downloads/juliet.zip"
        self.files = {}
        for cwe, bad, good in ((121, "int x[2]; x[3] = 1;", "int x[4]; x[3] = 1;"),
                               (401, "char *p = malloc(4); (void)p;", "char *p = malloc(4); free(p);"),
                               (476, "int *p = 0; int x = *p;", "int x = 1; int *p = &x;")):
            name, content = testcase(cwe, bad, good, newline="\r\n")
            self.files[f"suite/testcases/CWE{cwe}_Example/{name}"] = content
        self.files["suite/testcasesupport/std_testcase.h"] = "#pragma once\n#include <stdlib.h>\n"
        self.files["suite/README.txt"] = "Synthetic unit fixture, not actual NIST data."
        self.write_archive()

    def write_archive(self):
        with zipfile.ZipFile(self.archive, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, value in self.files.items():
                archive.writestr(name, value)
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def run_import(self, **kwargs):
        return import_juliet(self.archive, self.root / "output", max_cases=3,
                             expected_sha256=self.digest, **kwargs)

    def test_import_preserves_originals_and_removes_explicit_answer_cues(self):
        result = self.run_import()
        self.assertEqual(6, result["imported_functions"])
        self.assertEqual({"0": 3, "1": 3}, result["class_counts"])
        self.assertFalse(result["official_archive_verified"])
        labels = [json.loads(line) for line in (self.root / "output/labels.jsonl").read_text(encoding="utf-8").splitlines()]
        origins = [json.loads(line) for line in (self.root / "output/provenance.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual(3, len({label["group_id"] for label in labels}))
        for label, origin in zip(labels, origins):
            code = (self.root / "output/corpus" / label["file"]).read_text(encoding="utf-8")
            self.assertNotRegex(code, r"CWE|good|bad|FLAW|FIX")
            self.assertIn("void process(void)", code)
            self.assertEqual([], label["defect_lines"])
            self.assertEqual(len(code.splitlines()), len(origin["line_map"]))
            raw = self.files[origin["source_file"]].encode()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), origin["original_sha256"])
            original_path = self.root / "output/originals" / (hashlib.sha256(origin["source_file"].encode()).hexdigest() + ".c")
            self.assertEqual(raw, original_path.read_bytes())
        self.assertIn("OBJECT", (self.root / "output/corpus/CMakeLists.txt").read_text())

    def test_repeated_import_is_deterministic_and_never_overwrites(self):
        self.run_import()
        output = self.root / "output"
        before = (output / "labels.jsonl").read_bytes()
        with self.assertRaisesRegex(ValueError, "拒绝覆盖"):
            self.run_import()
        import_juliet(self.archive, self.root / "repeat", max_cases=3, expected_sha256=self.digest)
        for name in ("labels.jsonl", "provenance.jsonl", "import-manifest.json"):
            self.assertEqual((output / name).read_bytes(), (self.root / "repeat" / name).read_bytes())
        self.assertEqual(before, (output / "labels.jsonl").read_bytes())

    def test_wrong_hash_and_overlap_fail_before_output(self):
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            import_juliet(self.archive, self.root / "output")
        self.assertFalse((self.root / "output").exists())
        with self.assertRaisesRegex(ValueError, "独立"):
            import_juliet(self.archive, self.archive.parent / "output", expected_sha256=self.digest)

    def test_unsafe_zip_members_fail_without_extraction(self):
        for unsafe in ("../escape.c", "C:/escape.c", "suite/../../escape.c", "suite/file.c:stream"):
            with self.subTest(unsafe=unsafe):
                self.files[unsafe] = "not extracted"
                self.write_archive()
                with self.assertRaisesRegex(ValueError, "不安全"):
                    self.run_import()
                del self.files[unsafe]
                self.assertFalse((self.root / "output").exists())
        # ZipInfo normalizes backslashes when constructed on Windows; test the
        # untrusted member name explicitly rather than accidentally testing '/'.
        member = zipfile.ZipInfo("placeholder")
        member.filename = "suite/evil\\x.c"
        with self.assertRaisesRegex(ValueError, "不安全"):
            _validate_members([member])

    def test_case_collisions_fail(self):
        self.files["suite/readme.txt"] = "collision"
        self.write_archive()
        with self.assertRaisesRegex(ValueError, "冲突"):
            self.run_import()

    def test_comments_are_removed_but_literal_content_is_preserved(self):
        original = 'char *s = "/* not a comment */"; /* answer\n clue */ int x = 1; // hint\n'
        clean = strip_comments(original)
        self.assertIn('"/* not a comment */"', clean)
        self.assertNotIn("answer", clean)
        self.assertEqual(original.count("\n"), clean.count("\n"))
        with self.assertRaisesRegex(ValueError, "spliced"):
            strip_comments('// comment\\\nint x;')

    def test_wrapper_not_used_as_negative_and_both_signatures_match(self):
        name, source = testcase(476, "int *p = 0; int x = *p;", "int x = 1;", extra="static void good1() { goodG2B(); }")
        pair, family = extract_pair(source, name, 42)
        self.assertEqual("CWE476_Example", family)
        self.assertEqual("goodG2B", pair[1]["original_function"])
        self.assertTrue(all("void process(void)" in item["function_source"] for item in pair))

    def test_leaks_nonleaf_cases_and_identical_pair_are_rejected(self):
        variants = [('printLine("bad clue");', "int x = 1;"), ("goodG2B();", "int x = 1;"),
                    ("int x = 1;", "int x = 1;")]
        for bad, good in variants:
            name, source = testcase(476, bad, good)
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                extract_pair(source, name, 42)

    def test_clone_hash_folds_literals_identifiers_but_not_operators_or_apis(self):
        left = "void process(void) { int x[2]; x[4] = 1; }"
        renamed = "void process(void) { int data[7]; data[9] = 3; }"
        self.assertEqual(template_hash(left), template_hash(renamed))
        self.assertNotEqual(template_hash(left), template_hash(left.replace("=", "+=")))
        self.assertNotEqual(template_hash("free(p);"), template_hash("printLine(p);"))

    def test_initial_profile_rejects_unknown_cwes_and_bad_limits(self):
        with self.assertRaisesRegex(ValueError, "CWE"):
            self.run_import(cwes=(999,))
        with self.assertRaisesRegex(ValueError, "max_cases"):
            import_juliet(self.archive, self.root / "output", max_cases=0, expected_sha256=self.digest)

    def test_requested_flow_subset_controls_pair_extraction(self):
        name, source = testcase(476, "int *p = 0; int x = *p;", "int x = 1;")
        with self.assertRaisesRegex(ValueError, "unsupported local-flow"):
            extract_pair(source, name, 42, flows=(2,))
        pair, _ = extract_pair(source, name, 42, flows=(1,))
        self.assertEqual(2, len(pair))

    def test_internal_answer_names_are_bijective_and_do_not_touch_literals(self):
        text = 'char dataBadBuffer[50]; char dataGoodBuffer[100]; int local_slot_0; data = dataGoodBuffer; printLine("dataBadBuffer");'
        clean, mapping = clean_local_names(text)
        self.assertEqual(2, len(set(mapping.values())))
        self.assertNotIn('local_slot_0', mapping.values())
        self.assertIn('data = ' + mapping['dataGoodBuffer'], clean)
        self.assertIn('"dataBadBuffer"', clean)
        self.assertEqual(text.count('\n'), clean.count('\n'))
        with self.assertRaises(ValueError):
            clean_local_names('int dataBad; obj.dataBad = 1;')

    def test_fixed_selection_requires_exact_case_membership(self):
        self.run_import()
        result = import_juliet(self.archive, self.root / 'repeat-selected', max_cases=3,
                               expected_sha256=self.digest, selection=self.root / 'output/provenance.jsonl')
        self.assertEqual(6, result['imported_functions'])
        with self.assertRaisesRegex(ValueError, 'max_cases'):
            import_juliet(self.archive, self.root / 'wrong-selected', max_cases=4,
                          expected_sha256=self.digest, selection=self.root / 'output/provenance.jsonl')


if __name__ == "__main__":
    unittest.main()
