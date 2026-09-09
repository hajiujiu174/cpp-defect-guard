from __future__ import annotations

import builtins
from dataclasses import replace
import importlib.metadata
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from defectguard.config import DEFAULT_CONFIG
from defectguard.environment import inspect_environment


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.native = self.root / "defectguard-clang.exe"
        self.native.write_bytes(b"not-executed-by-preflight")
        (self.source / "compile_commands.json").write_text("[]", encoding="utf-8")
        self.config = replace(DEFAULT_CONFIG, clang_tool=str(self.native), parser_backend="clang")
        self.which = self.enterContext(patch("defectguard.environment.shutil.which", return_value=None))
        self.spec = self.enterContext(patch("defectguard.environment.importlib.util.find_spec", return_value=object()))
        self.version = self.enterContext(patch("defectguard.environment.importlib.metadata.version", return_value="test-version"))

    def checkpoint(self, mode="gnn", **extra):
        directory = self.root / "checkpoint"
        directory.mkdir(exist_ok=True)
        value = {"schema_version": "1.0", "model_kind": "defectguard-binary-detector",
                 "network": {"mode": mode}, "threshold": 0.5, **extra}
        (directory / "metadata.json").write_text(json.dumps(value), encoding="utf-8")
        (directory / "model.safetensors").write_bytes(b"synthetic-bytes-not-real-weights")
        return replace(self.config, model_backend="trained", model_checkpoint=str(directory))

    def encoder(self):
        directory = self.root / "encoder"
        directory.mkdir(exist_ok=True)
        for name in ("config.json", "vocab.json", "merges.txt", "pytorch_model.bin"):
            (directory / name).write_bytes(b"file-existence-only")
        return directory

    def inspect(self, config=None):
        return inspect_environment(self.source, config or self.config)

    def test_disabled_preserves_old_fields_and_is_not_ready(self):
        result = self.inspect()
        for key in ("python", "source_root", "source_exists", "tools", "clang_libtooling", "verification", "layer1"):
            self.assertIn(key, result)
        self.assertTrue(result["layer1"]["native_clang_ready"])
        self.assertTrue(result["layer2"]["dependencies_ready"])
        self.assertFalse(result["layer2"]["model_enabled"])
        self.assertFalse(result["layer2"]["model_preflight_ready"])
        self.assertEqual("disabled", result["model_preflight"]["status"])
        self.assertEqual([], result["model_preflight"]["issues"])

    def test_inspection_does_not_import_ml_modules_or_read_weights(self):
        config = self.checkpoint()
        imported = builtins.__import__
        original_open = Path.open

        def guarded_import(name, *args, **kwargs):
            self.assertNotIn(name.split(".")[0], ("torch", "transformers", "torch_geometric", "safetensors"))
            return imported(name, *args, **kwargs)

        def guarded_open(path, *args, **kwargs):
            self.assertNotIn(path.suffix, (".bin", ".safetensors"))
            return original_open(path, *args, **kwargs)

        before = {str(path): path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
        with patch("builtins.__import__", side_effect=guarded_import), patch.object(Path, "open", guarded_open):
            result = self.inspect(config)
        self.assertEqual("preflight-passed", result["model_preflight"]["status"])
        self.assertEqual(before, {str(path): path.read_bytes() for path in self.root.rglob("*") if path.is_file()})
        self.assertTrue(any("未校验" in item and "推理" in item for item in result["model_preflight"]["limitations"]))

    def test_dependency_uses_distribution_name_and_current_interpreter(self):
        result = self.inspect()
        self.version.assert_any_call("torch-geometric")
        self.spec.assert_any_call("torch_geometric")
        geometric = next(item for item in result["model_preflight"]["dependencies"] if item["module"] == "torch_geometric")
        self.assertEqual("torch-geometric", geometric["distribution"])
        self.assertEqual("test-version", geometric["version"])
        self.assertEqual(result["python"]["executable"], result["model_preflight"]["interpreter"])

    def test_missing_dependency_and_missing_version_block_enabled_model(self):
        self.spec.side_effect = lambda name: None if name == "torch" else object()

        def installed(name):
            if name == "torch-geometric":
                raise importlib.metadata.PackageNotFoundError(name)
            return "test-version"

        self.version.side_effect = installed
        result = self.inspect(self.checkpoint())
        self.assertFalse(result["layer2"]["dependencies_ready"])
        self.assertFalse(result["layer2"]["model_preflight_ready"])
        self.assertEqual("blocked", result["model_preflight"]["status"])
        issues = " ".join(result["model_preflight"]["issues"])
        self.assertIn("torch 模块", issues)
        self.assertIn("torch-geometric", issues)

    def test_broken_module_spec_is_reported_without_crashing(self):
        self.spec.side_effect = ValueError("bad spec")
        result = self.inspect()
        self.assertFalse(result["layer2"]["dependencies_ready"])
        self.assertEqual("disabled", result["model_preflight"]["status"])
        self.assertIn("bad spec", result["model_preflight"]["dependencies"][0]["issues"][0])

    def test_missing_checkpoint_and_empty_weights_are_explicit(self):
        config = replace(self.config, model_backend="trained", model_checkpoint=str(self.root / "missing"))
        result = self.inspect(config)["model_preflight"]
        self.assertEqual("blocked", result["status"])
        self.assertFalse(result["checkpoint"]["exists"])
        self.assertTrue(any("目录不存在" in item for item in result["issues"]))
        config = self.checkpoint()
        (Path(config.model_checkpoint) / "model.safetensors").write_bytes(b"")
        result = self.inspect(config)["model_preflight"]
        self.assertFalse(result["checkpoint"]["files"]["model.safetensors"]["nonempty"])
        self.assertEqual("blocked", result["status"])

    def test_metadata_rejects_malformed_nonobject_version_mode_and_threshold(self):
        config = self.checkpoint()
        metadata = Path(config.model_checkpoint) / "metadata.json"
        base = json.loads(metadata.read_text(encoding="utf-8"))
        documents = ("not JSON", "[]", "null", '{"schema_version":"1.0","schema_version":"1.0"}',
                     json.dumps({**base, "schema_version": "2.0"}),
                     json.dumps({**base, "model_kind": "other"}),
                     json.dumps({**base, "network": {"mode": "diffusion"}}),
                     json.dumps({**base, "threshold": True}),
                     json.dumps({**base, "threshold": float("nan")}))
        for document in documents:
            with self.subTest(document=document):
                metadata.write_text(document, encoding="utf-8")
                result = self.inspect(config)["model_preflight"]
                self.assertFalse(result["metadata"]["basic_valid"])
                self.assertEqual("blocked", result["status"])
                self.assertTrue(result["metadata"]["issues"])

    def test_metadata_read_is_bounded(self):
        config = self.checkpoint()
        (Path(config.model_checkpoint) / "metadata.json").write_bytes(b" " * (4 * 1024 * 1024 + 1))
        result = self.inspect(config)["model_preflight"]
        self.assertEqual("blocked", result["status"])
        self.assertIn("4 MiB", result["metadata"]["issues"][0])

    def test_gnn_does_not_require_encoder_files(self):
        config = replace(self.checkpoint(), model_encoder=str(self.root / "missing-encoder"))
        result = self.inspect(config)["model_preflight"]
        self.assertEqual("preflight-passed", result["status"])
        self.assertFalse(result["encoder"]["required"])
        self.assertEqual({}, result["encoder"]["files"])

    def test_fusion_encoder_override_and_tokenizer_alternatives(self):
        encoder = self.encoder()
        config = replace(self.checkpoint("fusion", encoder_directory=str(self.root / "missing")), model_encoder=str(encoder))
        result = self.inspect(config)["model_preflight"]
        self.assertEqual("preflight-passed", result["status"])
        self.assertTrue(result["encoder"]["required"])
        self.assertEqual("models.encoder", result["encoder"]["path_source"])
        (encoder / "vocab.json").unlink()
        (encoder / "merges.txt").unlink()
        (encoder / "pytorch_model.bin").unlink()
        (encoder / "tokenizer.json").write_text("{}", encoding="utf-8")
        (encoder / "model.safetensors").write_bytes(b"placeholder")
        self.assertEqual("preflight-passed", self.inspect(config)["model_preflight"]["status"])

    def test_sequence_encoder_metadata_path_and_missing_files(self):
        encoder = self.encoder()
        config = self.checkpoint("sequence", encoder_directory=str(encoder))
        result = self.inspect(config)["model_preflight"]
        self.assertEqual("metadata.encoder_directory", result["encoder"]["path_source"])
        self.assertEqual("preflight-passed", result["status"])
        (encoder / "config.json").unlink()
        (encoder / "pytorch_model.bin").unlink()
        result = self.inspect(config)["model_preflight"]
        self.assertFalse(result["encoder"]["ready"])
        self.assertEqual("blocked", result["status"])
        self.assertTrue(any("config.json" in item for item in result["issues"]))
        self.assertTrue(any("pytorch_model.bin" in item for item in result["issues"]))

    def test_native_requirements_and_configured_tool_match_pipeline(self):
        config = replace(self.checkpoint(), parser_backend="fallback")
        self.assertFalse(self.inspect(config)["model_preflight"]["native"]["ready"])
        self.assertEqual("blocked", self.inspect(config)["model_preflight"]["status"])
        self.which.side_effect = lambda name: str(self.native) if name == "defectguard-clang" else None
        config = replace(config, parser_backend="clang", clang_tool=str(self.root / "missing.exe"))
        result = self.inspect(config)
        self.assertIsNone(result["clang_libtooling"]["executable"])
        self.assertEqual("blocked", result["model_preflight"]["status"])
        (self.source / "compile_commands.json").unlink()
        config = replace(config, clang_tool=str(self.native))
        self.assertFalse(self.inspect(config)["model_preflight"]["native"]["compilation_database_exists"])
        (self.source / "out").mkdir()
        (self.source / "out/compile_commands.json").write_text("[]", encoding="utf-8")
        self.assertEqual("preflight-passed", self.inspect(config)["model_preflight"]["status"])


if __name__ == "__main__":
    unittest.main()
