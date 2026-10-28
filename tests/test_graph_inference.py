from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from defectguard.experiments.inference import infer_graphs, load_graph_bundle
from defectguard.graphs import EDGE_TYPES


def digest(value):
    return hashlib.sha256(value).hexdigest()


class GraphInferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="graph-interface-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "graphs"
        self.bundle.mkdir()
        source = "int process() {\n return 0;\n}"
        self.record = dict(
            schema_version="1.0", file="目录/sample.cpp", function="process", signature="int ()",
            start_line=1, end_line=3, source=source, source_sha256=digest(source.encode()),
            source_slice="line-range-fallback", dfg_status="unavailable: fixture", label=None, label_source="unlabeled",
            nodes=[dict(node_id="A0", kind="FunctionDecl", layer="ast", line=1, end_line=3),
                   dict(node_id="A1", kind="ReturnStmt", layer="ast", line=2, end_line=2)],
            edges=[dict(source="A0", target="A1", kind="ast-child")], edge_index=[[0], [1]], edge_type=[0])
        self.record["graph_id"] = digest(
            f"{self.record['file']}:process:int ():1:{self.record['source_sha256']}".encode())
        self.manifest = dict(schema_version="1.0", parser_backend="clang-libtooling", source_fingerprint="1" * 64,
                             edge_types=EDGE_TYPES, function_count=1, files_without_function_graphs=["other.cpp"],
                             label_status="unlabeled: fixture", dfg_limitations=["fixture limitation"])
        self.write_bundle()
        self.checkpoint = self.root / "checkpoint"
        self.checkpoint.mkdir()
        (self.checkpoint / "model.safetensors").write_bytes(b"fixture weights")
        self.metadata = dict(network={"mode": "gnn"}, threshold=0.5, excluded_edges=[],
                             weights_sha256=digest(b"fixture weights"))
        self.write_metadata()
        self.predictor = SimpleNamespace(metadata=self.metadata, encoder=None, last_encoding=None,
                                         predict=lambda rows: [self.prediction(r) for r in rows])
        self.output = self.root / "out"

    def write_metadata(self):
        (self.checkpoint / "metadata.json").write_text(json.dumps(self.metadata), encoding="utf-8")

    def write_bundle(self, rows=None):
        raw = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in (rows if rows is not None else [self.record])).encode()
        (self.bundle / "graphs.jsonl").write_bytes(raw)
        self.manifest["graphs_sha256"] = digest(raw)
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def prediction(self, record):
        return dict(sample_id=record["graph_id"], file=record["file"], function=record["function"],
                    probability=0.75, ranked_lines=[2], line_scores={"2": 0.8})

    def infer(self):
        with patch("defectguard.experiments.inference._load_predictor", return_value=self.predictor):
            return infer_graphs(self.bundle, self.checkpoint, self.output)

    def sequence(self, token_count):
        self.metadata["network"]["mode"] = "fusion"
        self.write_metadata()
        self.predictor.encoder = SimpleNamespace(identity="f" * 64, max_length=8, directory=self.root / "encoder")
        detail = dict(sample_id=self.record["graph_id"], file=self.record["file"], function="process",
                      start_line=1, end_line=3, token_count=token_count, retained_token_count=min(8, token_count),
                      truncated=token_count > 8, cache_status="disabled")
        self.predictor.last_encoding = dict(encoder_sha256="f" * 64, max_length=8,
                                           truncated_functions=int(token_count > 8), functions=[detail])

    def test_valid_bundle_binds_exact_input_bytes(self):
        records, manifest, receipt = load_graph_bundle(self.bundle)
        self.assertEqual([self.record], records)
        self.assertEqual(self.manifest, manifest)
        for name in ("manifest.json", "graphs.jsonl"):
            self.assertEqual(digest((self.bundle / name).read_bytes()), receipt[name]["sha256"])

    def test_bad_graph_hash_is_rejected_before_loading_model(self):
        with (self.bundle / "graphs.jsonl").open("ab") as f:
            f.write(b" ")
        with patch("defectguard.experiments.inference._load_predictor") as factory:
            with self.assertRaisesRegex(ValueError, "哈希"):
                infer_graphs(self.bundle, self.checkpoint, self.output)
            factory.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_protocol_manifest_types_versions_and_coverage_are_enforced(self):
        original = copy.deepcopy(self.manifest)
        for key, value in (
                ("schema_version", "9"), ("parser_backend", "fallback"), ("function_count", True),
                ("function_count", 2), ("source_fingerprint", "unknown"), ("edge_types", {"ast-child": 0}),
                ("edge_types", {**EDGE_TYPES, "ast-child": False}),
                ("files_without_function_graphs", ["../escape.cpp"]), ("files_without_function_graphs", ["目录/sample.cpp"]),
                ("files_without_function_graphs", ["same.cpp", "same.cpp"]),
                ("label_status", "reviewed"), ("dfg_limitations", [])):
            with self.subTest(key=key, value=value):
                self.manifest = {**original, key: value}
                self.write_bundle()
                with self.assertRaises(ValueError):
                    load_graph_bundle(self.bundle)

    def test_graph_protocol_identity_and_supervision_are_enforced(self):
        original = copy.deepcopy(self.record)
        for key, value in (("schema_version", "9"), ("graph_id", "0" * 64), ("file", "C:/outside.cpp"),
                           ("label", 0), ("label_source", "rule"), ("sample_id", "other"),
                           ("dfg_status", ""), ("source_slice", "unknown"), ("edge_type", [3]),
                           ("source_sha256", "0" * 64), ("nodes", [])):
            with self.subTest(key=key):
                self.record = {**original, key: value}
                self.write_bundle()
                with self.assertRaises(ValueError):
                    load_graph_bundle(self.bundle)
        self.record = original
        for key in ("signature", "graph_id", "edges", "label", "schema_version"):
            value = copy.deepcopy(original)
            value.pop(key)
            self.write_bundle([value])
            with self.assertRaises(ValueError):
                load_graph_bundle(self.bundle)

    def test_duplicate_graphs_cannot_silently_reduce_coverage(self):
        self.manifest["function_count"] = 2
        self.write_bundle([self.record, self.record])
        with self.assertRaisesRegex(ValueError, "重复"):
            load_graph_bundle(self.bundle)

    def test_duplicate_json_fields_and_nonfinite_values_are_rejected(self):
        for raw in (b'{"schema_version":"1.0","schema_version":"1.0"}', b'{"field":NaN}', b"[]"):
            (self.bundle / "manifest.json").write_bytes(raw)
            with self.assertRaises(ValueError):
                load_graph_bundle(self.bundle)

    def test_unlabeled_inference_emits_bound_research_result_and_honest_coverage(self):
        result = self.infer()
        self.assertEqual("research_only", result["status"])
        self.assertEqual("uncalibrated", result["calibration"])
        self.assertEqual("not_assessed", result["correctness"])
        self.assertFalse(result["training"])
        self.assertEqual("not_assessed", result["coverage"]["repository_coverage"])
        self.assertEqual(["other.cpp"], result["coverage"]["files_without_function_graphs"])
        self.assertEqual({"line-range-fallback": 1}, result["coverage"]["source_slices"])
        self.assertEqual({"unavailable: fixture": 1}, result["coverage"]["dfg_statuses"])
        entry = result["files"][0]
        self.assertEqual(digest((self.output / entry["path"]).read_bytes()), entry["sha256"])
        predictions = json.loads((self.output / "predictions.json").read_text(encoding="utf-8"))
        self.assertEqual("suspected", predictions[0]["decision"])
        self.assertEqual(self.record["graph_id"], predictions[0]["graph_id"])

    def test_below_threshold_is_not_a_no_defect_claim(self):
        self.predictor.predict = lambda rows: [{**self.prediction(r), "probability": 0.25} for r in rows]
        self.infer()
        row = json.loads((self.output / "predictions.json").read_text(encoding="utf-8"))[0]
        self.assertEqual("below_threshold", row["decision"])
        self.assertEqual("not_assessed", row["correctness"])

    def test_predictions_must_have_matching_identity_finite_scores_and_valid_lines(self):
        baseline = self.prediction(self.record)
        for key, value in (("sample_id", "other"), ("file", "other.cpp"), ("probability", float("nan")),
                           ("probability", True), ("probability", 1.1), ("ranked_lines", [4]),
                           ("ranked_lines", [2, 2]), ("line_scores", {"2": float("inf")}),
                           ("line_scores", {})):
            with self.subTest(key=key, value=value):
                self.predictor.predict = lambda rows: [{**baseline, key: value}]
                with self.assertRaises(ValueError):
                    self.infer()
                self.assertFalse(self.output.exists())
        self.predictor.predict = lambda rows: []
        with self.assertRaisesRegex(ValueError, "数量"):
            self.infer()

    def test_input_and_checkpoint_mutation_cannot_produce_success(self):
        for target in (self.bundle / "graphs.jsonl", self.checkpoint / "metadata.json"):
            before = target.read_bytes()
            def changed(rows):
                target.write_bytes(before + b" ")
                return [self.prediction(r) for r in rows]
            self.predictor.predict = changed
            with self.assertRaisesRegex(ValueError, "发生变化"):
                self.infer()
            self.assertFalse(self.output.exists())
            target.write_bytes(before)

    def test_existing_outputs_and_protected_directories_are_rejected(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("original")
        for directory in (self.output, self.bundle / "out", self.checkpoint / "out"):
            with self.subTest(directory=directory), self.assertRaises(ValueError):
                infer_graphs(self.bundle, self.checkpoint, directory)
        self.assertEqual("original", marker.read_text())

    def test_truncation_reports_every_function_and_keeps_graph_scope_separate(self):
        self.sequence(12)
        with patch("defectguard.experiments.inference._encoder_unchanged"):
            result = self.infer()
        info = result["coverage"]["sequence"]
        self.assertEqual("truncated", info["status"])
        self.assertEqual(8, info["truncated_functions"][0]["retained_token_count"])
        self.assertEqual("not_assessed", result["coverage"]["repository_coverage"])

    def test_missing_or_inconsistent_encoding_records_are_rejected(self):
        for field, value in (("truncated_functions", 0), ("functions", []), ("encoder_sha256", "0" * 64)):
            self.sequence(12)
            self.predictor.last_encoding[field] = value
            with patch("defectguard.experiments.inference._encoder_unchanged"), self.assertRaises(ValueError):
                self.infer()
            self.assertFalse(self.output.exists())
        for field, value in (("truncated", False), ("retained_token_count", 12), ("sample_id", "other")):
            self.sequence(12)
            self.predictor.last_encoding["functions"][0][field] = value
            with patch("defectguard.experiments.inference._encoder_unchanged"), self.assertRaises(ValueError):
                self.infer()

    def test_base_cli_registration_does_not_import_model_runtime(self):
        code = ("import sys; from defectguard.cli import build_parser; "
                "p=build_parser(); a=p.parse_args(['infer-graphs','--graph-bundle','a','--checkpoint','b','--output','c']); "
                "assert a.command=='infer-graphs'; "
                "assert not any(x in sys.modules for x in ('torch','transformers','torch_geometric'))")
        subprocess.run([sys.executable, "-B", "-c", code], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
