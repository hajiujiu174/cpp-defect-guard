from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from defectguard.experiments.data import load_dataset
from defectguard.experiments.evaluation import evaluate_predictions
from defectguard.experiments.sweep import aggregate_runs, describe_scores, run_sweep, summarize_sweep
from test_experiments import write_dataset


class SweepTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="defectguard-sweep-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dataset = self.root / "dataset"
        self.records, self.splits = write_dataset(self.dataset)
        self.calls = []

    @staticmethod
    def write(path, value):
        path.write_text(json.dumps(value), encoding="utf-8", newline="\n")

    def fake_trainer(self, dataset, output, **kwargs):
        # Test-only serialization stub: no claim that these predictions come from ML.
        self.calls.append((kwargs["mode"], kwargs["seed"]))
        records, splits = load_dataset(dataset)
        output.mkdir()
        weights = b"synthetic-checkpoint-for-orchestration-unit-tests"
        (output / "model.safetensors").write_bytes(weights)
        metadata = {key: kwargs[key] for key in ("seed", "epochs", "learning_rate", "threshold")}
        metadata.update(network={"mode": kwargs["mode"], "hidden": kwargs["hidden"]}, best_epoch=1,
                        excluded_edges=[], dataset_sha256=splits["dataset_sha256"],
                        splits_sha256=hashlib.sha256((dataset / "splits.json").read_bytes()).hexdigest(),
                        weights_sha256=hashlib.sha256(weights).hexdigest())
        if kwargs["mode"] != "gnn":
            metadata["encoding"] = {"encoder_sha256": "f" * 64, "max_length": kwargs["max_length"]}
        self.write(output / "metadata.json", metadata)
        self.write(output / "splits.json", splits)
        metrics = {}
        for name, identifiers in splits["splits"].items():
            selected = [record for record in records if record["sample_id"] in identifiers]
            probabilities = [0.8 if record["label"] else 0.2 for record in selected]
            if kwargs["seed"] % 2:
                probabilities[0] = 1 - probabilities[0]
            predictions = [{"sample_id": item["sample_id"], "probability": probability,
                            "ranked_lines": None if kwargs["mode"] == "sequence" else [2, 1]}
                           for item, probability in zip(selected, probabilities)]
            metrics[name] = evaluate_predictions(selected, predictions)
            self.write(output / f"{name}-predictions.json", predictions)
        self.write(output / "metrics.json", metrics)

    def sweep(self, output=None, **kwargs):
        return run_sweep(self.dataset, output or self.root / "sweep", modes=("gnn",), seeds=(42, 43),
                         epochs=2, trainer=self.fake_trainer, **kwargs)

    def test_mean_and_sample_std_not_standard_error(self):
        result = describe_scores([0.2, 0.8])
        self.assertAlmostEqual(0.5, result["mean"])
        self.assertAlmostEqual(0.4242640687, result["sample_std"])
        self.assertEqual(0.2, result["min"])
        self.assertIsNone(describe_scores([None, None])["mean"])
        for bad in ([0.5, None], [float("nan")], [True], [1.2]):
            with self.assertRaises(ValueError):
                describe_scores(bad)

    def test_runs_all_registered_seeds_and_keeps_test_sample_denominator(self):
        result = self.sweep()
        self.assertEqual([("gnn", 42), ("gnn", 43)], self.calls)
        self.assertEqual(2, result["run_count"])
        self.assertEqual(len(self.splits["splits"]["test"]), result["test_sample_count"])
        self.assertEqual([42, 43], result["models"]["gnn"]["seeds"])
        self.assertEqual("complete", json.loads((self.root / "sweep/progress.json").read_text())["status"])
        cases = json.loads((self.root / "sweep/cases.json").read_text())["gnn"]
        self.assertEqual(1, sum(case["error_runs"] for case in cases))
        self.assertTrue((self.root / "sweep/summary.md").is_file())

    def test_sequence_retains_unsupported_localization(self):
        result = run_sweep(self.dataset, self.root / "sequence", modes=("sequence",), seeds=(42, 43),
                           encoder=self.root / "encoder", epochs=2, trainer=self.fake_trainer,
                           encoder_fingerprint=lambda _: "f" * 64)
        self.assertIsNone(result["models"]["sequence"]["localization"]["top_k"]["1"]["mean"])

    def test_resume_verifies_and_does_not_retrain_completed_checkpoints(self):
        first = self.sweep()
        self.calls.clear()
        second = self.sweep(resume=True)
        self.assertEqual(first, second)
        self.assertEqual([], self.calls)
        state = json.loads((self.root / "sweep/progress.json").read_text())
        self.assertTrue(all(run["reused"] for run in state["runs"]))

    def test_resume_rejects_changed_plan_without_replacing_it(self):
        self.sweep()
        path = self.root / "sweep/plan.json"
        before = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "恢复参数"):
            run_sweep(self.dataset, self.root / "sweep", modes=("gnn",), seeds=(42, 44), epochs=2,
                      trainer=self.fake_trainer, resume=True)
        self.assertEqual(before, path.read_bytes())

    def test_resume_rejects_corrupt_checkpoint_and_inconsistent_metrics(self):
        self.sweep()
        weights = self.root / "sweep/gnn-seed42/model.safetensors"
        weights.write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "权重哈希"):
            self.sweep(resume=True)
        self.assertEqual(b"corrupt", weights.read_bytes())

    def test_saved_scores_are_recomputed_from_predictions(self):
        self.sweep()
        path = self.root / "sweep/gnn-seed42/metrics.json"
        value = json.loads(path.read_text())
        value["test"]["f1"] = 0.123
        self.write(path, value)
        with self.assertRaisesRegex(ValueError, "预测与保存指标不一致"):
            summarize_sweep(self.dataset, self.root / "sweep")

    def test_summary_rejects_malformed_plan_and_metadata_types(self):
        self.sweep()
        path = self.root / "sweep/plan.json"
        original = json.loads(path.read_text())
        self.write(path, [])
        with self.assertRaisesRegex(ValueError, "JSON 对象"):
            summarize_sweep(self.dataset, self.root / "sweep")
        self.write(path, original)
        checkpoint = self.root / "sweep/gnn-seed42/metadata.json"
        metadata = json.loads(checkpoint.read_text())
        metadata["seed"] = 42.0
        self.write(checkpoint, metadata)
        with self.assertRaisesRegex(ValueError, "预先登记"):
            summarize_sweep(self.dataset, self.root / "sweep")

    def test_failure_records_status_and_preserves_outputs(self):
        def fail(dataset, output, **kwargs):
            if kwargs["seed"] == 43:
                output.mkdir()
                (output / "partial.txt").write_text("keep", encoding="utf-8")
                raise ValueError("injected failure")
            return self.fake_trainer(dataset, output, **kwargs)
        with self.assertRaisesRegex(ValueError, "injected failure"):
            run_sweep(self.dataset, self.root / "failed", modes=("gnn",), seeds=(42, 43), epochs=2, trainer=fail)
        state = json.loads((self.root / "failed/progress.json").read_text())
        self.assertEqual("failed", state["status"])
        self.assertEqual(["complete", "failed"], [run["status"] for run in state["runs"]])
        with self.assertRaisesRegex(ValueError, "保留产物"):
            run_sweep(self.dataset, self.root / "failed", modes=("gnn",), seeds=(42, 43), epochs=2,
                      trainer=self.fake_trainer, resume=True)
        self.assertEqual("keep", (self.root / "failed/gnn-seed43/partial.txt").read_text())

    def test_resume_rejects_ambiguous_json_and_coerced_plan_types(self):
        self.sweep()
        path = self.root / "sweep/plan.json"
        original = path.read_text()
        for bad in ('{"schema_version":"broken",' + original[1:],
                    original.replace('"threshold": 0.5', '"threshold": NaN'),
                    original.replace('"threshold": 0.5', '"threshold": 1e999')):
            self.assertNotEqual(original, bad)
            path.write_text(bad, encoding="utf-8")
            with self.assertRaises(ValueError):
                self.sweep(resume=True)
            self.assertEqual(bad, path.read_text())
        value = json.loads(original)
        value["seeds"][0] = 42.0
        self.write(path, value)
        with self.assertRaisesRegex(ValueError, "种子无效"):
            self.sweep(resume=True)

    def test_duplicate_and_single_seeds_or_modes_are_rejected(self):
        for kwargs in ({"seeds": (42,)}, {"seeds": (42, 42)}, {"seeds": (True, 42)}, {"seeds": (-1, 42)},
                       {"modes": ("gnn", "gnn")}, {"modes": ("other",)}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                run_sweep(self.dataset, self.root / "bad", trainer=self.fake_trainer, **kwargs)
        self.assertFalse((self.root / "bad").exists())

    def test_input_output_cache_overlap_and_existing_directory_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "独立"):
            self.sweep(output=self.dataset / "output")
        with self.assertRaisesRegex(ValueError, "缓存"):
            self.sweep(cache=self.dataset / "cache")
        self.sweep()
        with self.assertRaisesRegex(ValueError, "非空"):
            self.sweep()


if __name__ == "__main__":
    unittest.main()
