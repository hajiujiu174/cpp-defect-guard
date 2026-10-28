import json
from pathlib import Path, PurePosixPath
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from accept_real_repair import archive_path, test_expectations, verify_test_outputs, CASE_NAMES


class CasePolicyTests(unittest.TestCase):
    def test_exact_baseline_oracles_are_distinct(self):
        expected = []
        for name in CASE_NAMES:
            case = json.loads((ROOT / 'benchmarks' / name / 'manifest.json').read_text(encoding='utf-8'))
            values = test_expectations(case)
            expected.append((len(case['targets']), len(values), list(values.values()).count('failed')))
            for target in case['targets']:
                if target['regression']:
                    self.assertEqual(values['codeguard-real.' + target['regression']], 'failed')
        self.assertEqual(expected, [(4, 4, 3), (2, 3, 2)])

    def test_observed_native_return_is_required_even_for_a_failed_test(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'tests.xml'
            path.write_text('<testsuite><testcase name="codeguard-real.a"><failure/><system-out>setup failed</system-out></testcase></testsuite>', encoding='utf-8')
            with self.assertRaises(ValueError):
                verify_test_outputs(path, {'codeguard-real.a': 1})
            path.write_text('<testsuite><testcase name="codeguard-real.a"><system-out>api=rename return=-1</system-out></testcase></testsuite>', encoding='utf-8')
            verify_test_outputs(path, {'codeguard-real.a': -1})
            with self.assertRaises(ValueError):
                verify_test_outputs(path, {'codeguard-real.a': 1})

    def test_native_output_inventory_must_be_exact(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'tests.xml'
            path.write_text('<testsuite/>', encoding='utf-8')
            with self.assertRaises(ValueError):
                verify_test_outputs(path, {'codeguard-real.a': 1})

    def test_missing_negative_or_control_or_skipped_baseline_is_rejected(self):
        for values in ({}, {'codeguard-real.a': 'passed'}, {'codeguard-real.a': 'failed'},
                       {'codeguard-real.a': 'skipped', 'codeguard-real.b': 'passed'},
                       {'unrelated.a': 'failed', 'codeguard-real.b': 'passed'}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                test_expectations({'test_expectations': values})

    def test_git_archive_paths_are_unchanged(self):
        self.assertEqual(archive_path('src/libgit2/repository.c', 'git-archive'), PurePosixPath('src/libgit2/repository.c'))

    def test_exact_github_wrapper_is_removed(self):
        self.assertIsNone(archive_path('libgit2-fixed/', 'github-zipball', 'libgit2-fixed'))
        self.assertEqual(archive_path('libgit2-fixed/src/a.c', 'github-zipball', 'libgit2-fixed'), PurePosixPath('src/a.c'))

    def test_mixed_github_roots_are_rejected(self):
        for name in ('other/src/a.c', 'libgit2-fixed-other/src/a.c'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                archive_path(name, 'github-zipball', 'libgit2-fixed')

    def test_unsafe_paths_are_rejected_before_prefix_removal(self):
        for name in ('../a.c', '/a.c', 'C:/a.c', 'fixed/../a.c', 'fixed/src\\a.c'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                archive_path(name, 'github-zipball', 'fixed')

    def test_unknown_or_missing_layout_policy_is_rejected(self):
        for layout, prefix in [('unknown', None), ('github-zipball', None)]:
            with self.subTest(layout=layout), self.assertRaises(ValueError):
                archive_path('src/a.c', layout, prefix)


if __name__ == '__main__':
    unittest.main()
