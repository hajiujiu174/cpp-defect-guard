from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

from defectguard.experiments.holdout import freeze_cohort, plan_holdout
from defectguard.experiments.data import canonical_hash
from defectguard.experiments.juliet import template_hash


def fixture():
    rows = []
    for group in range(4):
        for label in (0, 1):
            ident = f"{group}-{label}"
            rows.append(dict(sample_id=ident, component=f"g{group}", source_file=f"case{group}.c",
                             cwe=121 + group % 2, label=label, eligible=True,
                             canonical_sha256=hashlib.sha256(ident.encode()).hexdigest(),
                             template_sha256=hashlib.sha256((ident + 'template').encode()).hexdigest()))
    return rows


class HoldoutTests(unittest.TestCase):
    def test_import_does_not_load_model_dependencies(self):
        source = str(Path(__file__).resolve().parents[1] / 'src')
        code = (f'import sys;sys.path.insert(0,{source!r});'
                'import defectguard.experiments.holdout;'
                'assert not any(x in sys.modules for x in ("torch","transformers","torch_geometric"))')
        subprocess.run([sys.executable, '-I', '-B', '-c', code], check=True, capture_output=True)

    def test_deterministic_and_order_independent(self):
        self.assertEqual(plan_holdout(fixture(), 'frozen-seed'), plan_holdout(list(reversed(fixture())), 'frozen-seed'))

    def test_partition_invariants(self):
        plan = plan_holdout(fixture(), 's')
        sides = {s: [r for r in plan['assignments'] if r['partition'] == s] for s in ('calibration', 'test')}
        self.assertEqual(len(plan['assignments']), 8)
        for side in sides.values():
            self.assertEqual({r['label'] for r in side}, {0, 1})
            self.assertEqual({r['cwe'] for r in side}, {121, 122})
            self.assertEqual(len({r['component'] for r in side}), 2)
        for key in ('component', 'source_file', 'canonical_sha256', 'template_sha256'):
            self.assertFalse({r[key] for r in sides['calibration']} & {r[key] for r in sides['test']})
        self.assertFalse(plan['calibration_fitted'])
        self.assertFalse(plan['model_scored'])

    def test_cross_group_keys_rejected(self):
        for key in ('source_file', 'canonical_sha256', 'template_sha256'):
            with self.subTest(key=key):
                rows = fixture(); rows[2][key] = rows[0][key]
                with self.assertRaises(ValueError): plan_holdout(rows, 's')

    def test_development_exposure_rejected(self):
        for changes in ({'eligible': False}, {'eligible': 1}, {'component': 'development:old'}):
            rows = fixture(); rows[0].update(changes)
            with self.assertRaises(ValueError): plan_holdout(rows, 's')

    def test_incomplete_pair_rejected(self):
        with self.assertRaises(ValueError): plan_holdout(fixture()[:-1], 's')

    def test_duplicate_identity_rejected(self):
        rows = fixture(); rows[2]['sample_id'] = rows[0]['sample_id']
        with self.assertRaises(ValueError): plan_holdout(rows, 's')

    def test_invalid_labels_hashes_seed_and_scores(self):
        for changes in ({'label': True}, {'label': 2}, {'cwe': True}, {'canonical_sha256': 'bad'}, {'score': .5}):
            rows = fixture(); rows[0].update(changes)
            with self.assertRaises(ValueError): plan_holdout(rows, 's')
        with self.assertRaises(ValueError): plan_holdout(fixture(), '')

    def test_infeasible_cwe_coverage_rejected(self):
        rows = fixture()
        for r in rows: r['cwe'] = 122 if r['component'] == 'g0' else 121
        with self.assertRaises(ValueError): plan_holdout(rows, 's')

    def test_too_few_groups_rejected(self):
        with self.assertRaises(ValueError): plan_holdout(fixture()[:6], 's')

    def test_too_many_groups_rejected(self):
        rows = fixture()
        for i in range(9):
            for label in (0, 1):
                sid = f'extra-{i}-{label}'
                rows.append(dict(sample_id=sid, component=f'extra{i}', source_file=f'extra{i}.c',
                                 cwe=121, label=label, eligible=True,
                                 canonical_sha256=hashlib.sha256(sid.encode()).hexdigest(),
                                 template_sha256=hashlib.sha256((sid+'t').encode()).hexdigest()))
        with self.assertRaises(ValueError): plan_holdout(rows, 's')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.audit = self.root / 'audit'
        self.output = self.root / 'frozen'
        self.graphs, audit, labels, provenance = [], [], [], []
        for i, op in enumerate(('+', '-', '*', '/')):
            original = dict(source_file=f'upstream/{i}.c', original_sha256='a' * 64,
                            component=f'g{i}', cwe=121 + i % 2,
                            eligible_under_current_exclusion_policy=True, previous_source=False, functions=[])
            for label in (0, 1):
                source = f'int process(int x) {{ return x {op} {label + 1}; }}'
                name, sid = f'functions/{i}-{label}.c', f'{i}-{label}'
                path = self.audit / 'candidate-cohort/corpus' / name
                path.parent.mkdir(parents=True, exist_ok=True); path.write_text(source)
                function = f'original_{label}'
                original['functions'].append(dict(original_function=function, label=label,
                                                  canonical_sha256=canonical_hash(source), template_sha256=template_hash(source)))
                self.graphs.append(dict(file=name, function='process', graph_id=sid, source=source))
                provenance.append(dict(sample_id=sid, file=name, source_file=original['source_file'],
                                       original_function=function, original_sha256='a' * 64,
                                       generated_file_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
                labels.append(dict(sample_id=sid, file=name, function='process', label=label,
                                   provenance=dict(source_file=original['source_file'], original_function=function, cwe=original['cwe'])))
            audit.append(original)
        for rel, rows in [('attempt2/candidate-audit.jsonl', audit), ('candidate-cohort/labels.jsonl', labels),
                          ('candidate-cohort/provenance.jsonl', provenance)]:
            path = self.audit / rel; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(''.join(json.dumps(x) + '\n' for x in rows))
        for rel in ('candidate-graphs/graphs.jsonl', 'candidate-graphs/manifest.json'):
            path = self.audit / rel; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('{}')
        self.loader = patch('defectguard.experiments.holdout.load_graph_bundle', return_value=(self.graphs, {}, {}))
        self.loader.start(); self.addCleanup(self.loader.stop)

    def test_freeze_receipt_and_refuse_overwrite(self):
        result = freeze_cohort(self.audit, self.output, 's')
        raw = (self.output / 'partition.json').read_bytes()
        self.assertEqual(json.loads(raw), result)
        self.assertIn(hashlib.sha256(raw).hexdigest(), (self.output / 'SHA256SUMS.txt').read_text())
        with self.assertRaises(ValueError): freeze_cohort(self.audit, self.output, 's')

    def test_source_tamper_produces_no_output(self):
        (self.audit / 'candidate-cohort/corpus/functions/0-0.c').write_text('tampered')
        with self.assertRaises(ValueError): freeze_cohort(self.audit, self.output, 's')
        self.assertFalse(self.output.exists())

    def test_graph_identity_drift_produces_no_output(self):
        self.graphs[0]['source'] += ' + changed'
        with self.assertRaises(ValueError): freeze_cohort(self.audit, self.output, 's')
        self.assertFalse(self.output.exists())

    def test_output_cannot_contain_or_be_inside_inputs(self):
        for output in (self.audit, self.audit / 'new', self.root):
            with self.assertRaises(ValueError): freeze_cohort(self.audit, output, 's')

    def test_input_change_during_planning_is_rejected(self):
        def changed(*args):
            plan = plan_holdout(*args)
            with (self.audit / 'candidate-cohort/labels.jsonl').open('a') as stream: stream.write('\n')
            return plan
        with patch('defectguard.experiments.holdout.plan_holdout', side_effect=changed):
            with self.assertRaises(ValueError): freeze_cohort(self.audit, self.output, 's')
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
