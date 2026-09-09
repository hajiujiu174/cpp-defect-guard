from __future__ import annotations

import copy
import gc
import hashlib
from dataclasses import replace
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import shutil
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from defectguard.config import DEFAULT_CONFIG
from defectguard.domain import ASTNode, FunctionStructure, ParsedFile
from defectguard.graphs import function_record
from test_experiments import record, write_dataset


class MLTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(importlib.util.find_spec(name) for name in ("torch", "torch_geometric", "transformers", "safetensors")):
            if os.environ.get("DEFECTGUARD_REQUIRE_ML") == "1":
                raise RuntimeError("要求模型验收但缺少依赖，请使用 .venv-ml")
            raise unittest.SkipTest("模型测试需要可选 ML 环境；DEFECTGUARD_REQUIRE_ML=1 强制验收")
        import torch
        from defectguard.experiments.runner import train_experiment
        cls.torch = torch
        torch.set_num_threads(2)
        cls.temp = tempfile.TemporaryDirectory(prefix="defectguard-ml-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.data, cls.splits = write_dataset(cls.root / "dataset")
        cls.training = train_experiment(cls.root / "dataset", cls.root / "model", mode="gnn", epochs=2, seed=7, hidden=8)

    def test_train_reload_and_held_out_evaluation(self):
        from defectguard.experiments.runner import TrainedPredictor, evaluate_checkpoint
        predictor = TrainedPredictor(self.root / "model")
        before = json.loads((self.root / "model" / "test-predictions.json").read_text())
        test = [item for item in self.data if item["sample_id"] in self.splits["splits"]["test"]]
        self.assertEqual(before, predictor.predict(test))
        metrics = evaluate_checkpoint(self.root / "dataset", self.root / "model", self.root / "reevaluation")
        self.assertEqual(self.training["metrics"]["test"], metrics)

    def test_same_seed_reproduces_parameters(self):
        from defectguard.experiments.runner import train_experiment
        from safetensors.torch import load_file
        train_experiment(self.root / "dataset", self.root / "repeat", mode="gnn", epochs=2, seed=7, hidden=8)
        left, right = (load_file(str(self.root / name / "model.safetensors")) for name in ("model", "repeat"))
        self.assertTrue(all(self.torch.equal(left[key], right[key]) for key in left))

    def test_model_does_not_encode_labels_paths_rules_or_test_only_kinds(self):
        from defectguard.experiments.network import fit_vocabulary, tensor_graph
        item = record()
        vocabulary = fit_vocabulary([item])
        modified = copy.deepcopy(item)
        modified.update(label=1, file="bug.cpp", rule_findings=[{"rule_id": "DG001"}], group_id="test")
        sequence = self.torch.zeros(768)
        left, right = (tensor_graph(value, vocabulary, sequence) for value in (item, modified))
        self.assertTrue(self.torch.equal(left.numeric, right.numeric))
        self.assertTrue(self.torch.equal(left.kinds, right.kinds))
        modified["nodes"][0]["kind"] = "UnseenKind"
        self.assertEqual(0, int(tensor_graph(modified, vocabulary, sequence).kinds[0]))

    def test_ablations_remove_forward_and_reverse_edges(self):
        from defectguard.experiments.network import fit_vocabulary, tensor_graph
        item = record()
        graph = tensor_graph(item, fit_vocabulary([item]), self.torch.zeros(768), (1, 2))
        self.assertEqual([0, 4], graph.edge_type.tolist())
        self.assertEqual([[0, 1], [1, 0]], graph.edge_index.tolist())

    def test_semantic_features_preserve_constants_extents_operators_and_bindings(self):
        from defectguard.experiments.network import fit_vocabulary, tensor_graph
        item = record()
        item['nodes'][0].update(kind='IntegerLiteral', name='50', type_name='int [50]', symbol='V0')
        changed = copy.deepcopy(item)
        changed['nodes'][0].update(name='100', type_name='int [100]', symbol='V1')
        vocab = fit_vocabulary([item], 'semantic-v2')
        seq = self.torch.zeros(768)
        old = [tensor_graph(x, vocab, seq) for x in (item, changed)]
        new = [tensor_graph(x, vocab, seq, feature_version='semantic-v2') for x in (item, changed)]
        self.assertTrue(self.torch.equal(old[0].numeric, old[1].numeric))
        self.assertFalse(self.torch.equal(new[0].numeric, new[1].numeric))
        changed['nodes'][0]['name'] = ''
        with self.assertRaisesRegex(ValueError, '重新构图'):
            tensor_graph(changed, vocab, seq, feature_version='semantic-v2')
        item['nodes'][0].update(kind='BinaryOperator', name='+')
        changed['nodes'][0].update(kind='BinaryOperator', name='-')
        vocab = fit_vocabulary([item, changed], 'semantic-v2')
        self.assertIn('BinaryOperator:+', vocab)
        self.assertIn('BinaryOperator:-', vocab)

    def test_classification_only_automatically_disables_negative_only_localization(self):
        from defectguard.experiments.runner import train_experiment
        from test_experiments import write_dataset
        from defectguard.experiments.data import load_dataset
        path = self.root / 'classification-data'
        data, _ = write_dataset(path)
        # Create a fresh valid dataset through the normal dataset writer.
        for item in data:
            item['defect_lines'] = []
        import hashlib
        contents = ''.join(json.dumps(item) + '\n' for item in data)
        (path / 'dataset.jsonl').write_text(contents, encoding='utf-8', newline='\n')
        manifest = json.loads((path / 'splits.json').read_text())
        manifest['dataset_sha256'] = hashlib.sha256(contents.encode()).hexdigest()
        (path / 'splits.json').write_text(json.dumps(manifest), encoding='utf-8')
        load_dataset(path)
        out = self.root / 'classification-model'
        train_experiment(path, out, mode='gnn', epochs=2, hidden=8, feature_version='semantic-v2')
        meta = json.loads((out / 'metadata.json').read_text())
        self.assertEqual(0, meta['localization_loss_weight'])
        self.assertTrue(all(x['ranked_lines'] is None for x in json.loads((out / 'test-predictions.json').read_text())))
        self.assertTrue(all(x['train_loss'] == x['train_classification_loss'] for x in json.loads((out / 'history.json').read_text())))
        from defectguard.experiments.runner import TrainedPredictor
        self.assertEqual(json.loads((out / 'test-predictions.json').read_text()),
                         TrainedPredictor(out).predict([x for x in data if x['sample_id'] in manifest['splits']['test']]))

    def test_all_network_modes_produce_finite_scores(self):
        from defectguard.experiments.network import DetectorNetwork, fit_vocabulary, tensor_graph
        item = record()
        vocabulary = fit_vocabulary([item])
        graph = tensor_graph(item, vocabulary, self.torch.zeros(768))
        for mode in ("sequence", "gnn", "fusion"):
            model = DetectorNetwork(mode, len(vocabulary), hidden=8)
            logit, lines = model(graph)
            self.assertTrue(self.torch.isfinite(logit))
            self.assertEqual(mode != "sequence", bool(lines))
            logit.backward()
            self.assertTrue(any(parameter.grad is not None for parameter in model.parameters()))

    def test_backend_emits_model_findings_and_refuses_fallback(self):
        from defectguard.models import create_detector
        backend = create_detector(replace(DEFAULT_CONFIG, model_backend="trained", model_checkpoint=str(self.root / "model"), model_threshold=0.0))
        path = self.root / "process.cpp"
        text = "int process() {\n    return 0;\n}"
        path.write_text(text, encoding="utf-8")
        function = FunctionStructure("process", "int", 0, 1, 3, 1, ast_nodes=(ASTNode("ast0", "ReturnStmt", 2, 5, 2),))
        file = ParsedFile(path, "process.cpp", "C++", text, functions=(function,), backend="clang-libtooling")
        findings = backend.detect([file])
        self.assertEqual(1, len(findings))  # Threshold zero is for wiring coverage, not an accuracy claim.
        self.assertEqual("model-gnn", findings[0].detector)
        self.assertEqual("ML001", findings[0].rule_id)
        self.assertEqual(2, findings[0].location.line)
        self.assertEqual([], function_record("process.cpp", function, text.encode())["rule_findings"])
        with self.assertRaisesRegex(ValueError, "Clang"):
            backend.detect([replace(file, backend="fallback")])

    def test_missing_checkpoint_and_nonempty_output_fail(self):
        from defectguard.experiments.runner import TrainedPredictor, train_experiment
        with self.assertRaisesRegex(ValueError, "检查点"):
            TrainedPredictor(self.root / "missing")
        with self.assertRaisesRegex(ValueError, "非空"):
            train_experiment(self.root / "dataset", self.root / "model", mode="gnn", epochs=1)

    def test_checkpoint_rejects_invalid_metadata(self):
        from defectguard.experiments.runner import TrainedPredictor
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            destination = Path(temporary) / "checkpoint"
            shutil.copytree(self.root / "model", destination)
            original = json.loads((destination / "metadata.json").read_text(encoding="utf-8"))
            changes = [
                ("threshold", float("nan")), ("threshold", True), ("threshold", -0.1),
                ("network", {**original["network"], "hidden": 2**32}),
                ("network", {**original["network"], "sequence_dim": 12}),
                ("vocabulary", {key: 0 for key in original["vocabulary"]}),
                ("excluded_edges", [True]), ("excluded_edges", [1, 1]),
                ("numeric_features", []), ("dataset_sha256", "invalid"),
            ]
            for field, value in changes:
                metadata = {**original, field: value}
                (destination / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "检查点|特征"):
                    TrainedPredictor(destination)
            (destination / "metadata.json").write_text('{"threshold":0.4,"threshold":0.8}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "重复字段"):
                TrainedPredictor(destination)

    def test_checkpoint_rejects_bad_weights_even_with_matching_hash(self):
        from defectguard.experiments.runner import TrainedPredictor
        from safetensors.torch import load_file, save_file
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            destination = Path(temporary) / "checkpoint"
            shutil.copytree(self.root / "model", destination)
            metadata = json.loads((destination / "metadata.json").read_text(encoding="utf-8"))
            original = load_file(str(destination / "model.safetensors"))
            key = next(iter(original))
            for mutation in ("nan", "inf", "shape", "dtype", "keys", "corrupt"):
                state = {name: value.clone() for name, value in original.items()}
                if mutation in {"nan", "inf"}:
                    state[key].flatten()[0] = float(mutation)
                elif mutation == "shape":
                    state[key] = state[key].flatten()[:1]
                elif mutation == "dtype":
                    state[key] = state[key].double()
                elif mutation == "keys":
                    state.pop(key)
                save_file(state, str(destination / "model.safetensors"))
                if mutation == "corrupt":
                    (destination / "model.safetensors").write_bytes(b"not a safetensors file")
                metadata["weights_sha256"] = hashlib.sha256((destination / "model.safetensors").read_bytes()).hexdigest()
                (destination / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
                with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "检查点"):
                    TrainedPredictor(destination)

    def test_prediction_rejects_nonfinite_logits_and_malformed_graph(self):
        from defectguard.experiments.runner import TrainedPredictor, _predictions
        class BadNetwork:
            def eval(self):
                return self
            def __call__(network, graph):
                return self.torch.tensor(float("inf")), {}
        with self.assertRaisesRegex(ValueError, "非有限"):
            _predictions(BadNetwork(), [record()], [None])
        predictor = TrainedPredictor(self.root / "model")
        invalid = record()
        invalid["edge_type"] = [0]
        with self.assertRaises(ValueError):
            predictor.predict([invalid])
        with self.assertRaisesRegex(ValueError, "唯一"):
            predictor.predict([record(), record()])

    def test_encoder_refuses_randomly_initialized_backbone(self):
        from defectguard.experiments.encoder import FrozenEncoder
        for loading in ({"missing_keys": ["encoder.layer.0.attention.self.query.weight"]},
                        {"mismatched_keys": ["embeddings.word_embeddings.weight"]},
                        {"unexpected_keys": ["unknown_backbone.weight"]}):
            with self.subTest(loading=loading), \
                 patch("defectguard.experiments.encoder.encoder_identity", return_value="0" * 64), \
                 patch("defectguard.experiments.encoder.AutoTokenizer.from_pretrained"), \
                 patch("defectguard.experiments.encoder.AutoModel.from_pretrained", return_value=(None, loading)), \
                 self.assertRaisesRegex(ValueError, "主干权重"):
                FrozenEncoder(self.root)

    def test_encoder_checks_weight_values_and_tokenizer_compatibility(self):
        from defectguard.experiments.encoder import FrozenEncoder
        model = self.torch.nn.Linear(1, 1)
        model.config = SimpleNamespace(model_type="roberta", hidden_size=768)
        model.get_input_embeddings = lambda: SimpleNamespace(num_embeddings=2)
        for invalid in ("weight", "tokenizer"):
            with self.subTest(invalid=invalid), \
                 patch("defectguard.experiments.encoder.encoder_identity", return_value="0" * 64), \
                 patch("defectguard.experiments.encoder.AutoTokenizer.from_pretrained", return_value=[0]), \
                 patch("defectguard.experiments.encoder.AutoModel.from_pretrained", return_value=(model, {})):
                with self.torch.no_grad():
                    model.weight.fill_(float("nan") if invalid == "weight" else 1.0)
                with self.assertRaisesRegex(ValueError, "非有限|词表大小"):
                    FrozenEncoder(self.root)

    def _fake_encoder(self):
        from defectguard.experiments.encoder import FrozenEncoder
        encoder = FrozenEncoder.__new__(FrozenEncoder)
        encoder.identity, encoder.max_length, encoder.dimension = "0" * 64, 8, 768
        def tokenize(text, **kwargs):
            if kwargs.get("return_tensors") == "pt":
                return {"input_ids": self.torch.ones((1, 8), dtype=self.torch.long),
                        "attention_mask": self.torch.ones((1, 8), dtype=self.torch.long)}
            return {"input_ids": list(range(20))}
        encoder.tokenizer = tokenize
        encoder.model = lambda **kwargs: SimpleNamespace(last_hidden_state=self.torch.ones((1, 8, 768)))
        return encoder

    def test_encoder_cache_tracks_identity_truncation_and_rejects_corruption(self):
        from safetensors.torch import save_file
        encoder = self._fake_encoder()
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            cache = Path(temporary)
            vectors, first = encoder.encode([record()], cache)
            self.assertEqual("miss", first["functions"][0]["cache_status"])
            self.assertEqual("s0.cpp", first["functions"][0]["file"])
            self.assertEqual(8, first["functions"][0]["retained_token_count"])
            self.assertEqual(1, first["truncated_functions"])
            self.assertEqual("hit", encoder.encode([record()], cache)[1]["functions"][0]["cache_status"])
            encoder.identity = "1" * 64
            self.assertEqual("miss", encoder.encode([record()], cache)[1]["functions"][0]["cache_status"])
            self.assertEqual(2, len(list(cache.iterdir())))
            encoder.identity = "0" * 64
            key = hashlib.sha256(json.dumps([encoder.identity, 8, "masked-mean-v1", record()["source"]],
                                            ensure_ascii=False).encode("utf-8")).hexdigest()
            cache_file = cache / f"{key}.safetensors"
            save_file({"embedding": vectors[0]}, str(cache_file))  # v0.4 legacy cache remains usable, explicitly marked.
            self.assertEqual("legacy-shape-checked", encoder.encode([record()], cache)[1]["functions"][0]["cache_status"])
            save_file({"embedding": vectors[0]}, str(cache_file), metadata={"cache_key": "wrong"})
            with self.assertRaisesRegex(ValueError, "缓存损坏"):
                encoder.encode([record()], cache)
            cache_file.write_bytes(b"broken")
            with self.assertRaisesRegex(ValueError, "缓存损坏"):
                encoder.encode([record()], cache)

    def test_nonfinite_encoder_output_is_never_cached(self):
        encoder = self._fake_encoder()
        encoder.model = lambda **kwargs: SimpleNamespace(last_hidden_state=self.torch.full((1, 8, 768), float("nan")))
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            with self.assertRaisesRegex(ValueError, "数值无效"):
                encoder.encode([record()], Path(temporary))
            self.assertEqual([], list(Path(temporary).iterdir()))

    def test_cache_retries_transient_windows_replace_lock(self):
        encoder = self._fake_encoder()
        real_replace = os.replace
        attempts = []
        def replace_with_transient_lock(source, target):
            attempts.append(target)
            if len(attempts) < 3:
                raise PermissionError("transient Windows handle")
            return real_replace(source, target)
        with tempfile.TemporaryDirectory(dir=self.root) as temporary, \
             patch("defectguard.experiments.encoder.os.replace", side_effect=replace_with_transient_lock), \
             patch("defectguard.experiments.encoder.time.sleep"):
            encoder.encode([record()], Path(temporary))
            self.assertEqual(3, len(attempts))
            self.assertEqual(1, len(list(Path(temporary).iterdir())))

    def test_backend_tracks_uncovered_files_functions_and_rejects_nan(self):
        from defectguard.models.trained import TrainedDetectorBackend
        backend = TrainedDetectorBackend(self.root / "model")
        path = self.root / "coverage.cpp"
        path.write_text("int process() { return 0; }", encoding="utf-8")
        function = FunctionStructure("process", "int", 0, 1, 1, 1,
                                     ast_nodes=(ASTNode("ast0", "ReturnStmt", 1, 1, 1),))
        native = ParsedFile(path, "coverage.cpp", "C++", path.read_text(), functions=(function,), backend="clang-libtooling")
        missing = replace(native, relative_path="missing.cpp", functions=(replace(function, ast_nodes=()),))
        fallback = replace(native, relative_path="header.h", backend="fallback")
        backend.detect([native, fallback, missing, replace(native, relative_path="empty.h", functions=())])
        details = "\n".join(backend.diagnostics())
        self.assertIn("header.h", details)
        self.assertIn("missing.cpp:1", details)
        self.assertIn("empty.h", details)
        identity = function_record(native.relative_path, function, path.read_bytes())["graph_id"]
        backend.predictor.last_encoding = {"truncated_functions": 1, "functions": [{
            "file": "coverage.cpp", "function": "process", "start_line": 1,
            "token_count": 800, "retained_token_count": 256, "truncated": True}]}
        with patch.object(backend.predictor, "predict", return_value=[{
            "sample_id": identity, "probability": 0.0, "ranked_lines": None}]):
            backend.detect([native])
        self.assertIn("coverage.cpp:1 process", "\n".join(backend.diagnostics()))
        self.assertIn("800", "\n".join(backend.diagnostics()))
        with patch.object(backend.predictor, "predict", return_value=[{"sample_id": identity, "probability": float("nan")}]), \
             self.assertRaisesRegex(ValueError, "概率无效"):
            backend.detect([native])
        backend.detect([])
        self.assertEqual([], backend.diagnostics())
        self.assertIsNone(backend.predictor.last_encoding)

    def test_existing_sequence_and_fusion_checkpoints_remain_compatible(self):
        from defectguard.experiments.data import load_dataset
        from defectguard.experiments.runner import TrainedPredictor
        root = Path(__file__).resolve().parents[1]
        demo = root / "artifacts/layer2/demo-20260906"
        encoder = root / "artifacts/models/graphcodebert-base"
        if not (demo / "fusion/metadata.json").is_file() or not (encoder / "pytorch_model.bin").is_file():
            self.skipTest("历史演示检查点和本地 GraphCodeBERT 权重未提供")
        records, splits = load_dataset(demo / "dataset")
        test = [item for item in records if item["sample_id"] in splits["splits"]["test"]]
        for mode in ("sequence", "fusion"):
            with self.subTest(mode=mode):
                predictor = TrainedPredictor(demo / mode, encoder)
                expected = json.loads((demo / mode / "test-predictions.json").read_text(encoding="utf-8"))
                self.assertEqual(expected, predictor.predict(test))
                del predictor
                gc.collect()


if __name__ == "__main__":
    unittest.main()
