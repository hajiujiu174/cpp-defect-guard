from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from defectguard.config import load_config
from defectguard.experiments.data import canonical_hash, join_labels, load_dataset, prepare_dataset, read_jsonl, split_records, validate_graph, validate_record
from defectguard.experiments.evaluation import evaluate_predictions


def record(identifier="s0", group="g0", label=0):
    source = f"int process() {{\n    return {sum(map(ord, identifier))};\n}}"
    return {"sample_id": identifier, "group_id": group, "label": label, "label_source": "test-fixture",
            "file": identifier + ".cpp", "function": "process", "graph_id": identifier,
            "source": source, "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
            "start_line": 1, "end_line": 3, "defect_lines": [2] if label else [],
            "nodes": [{"kind": "FunctionDecl", "layer": "ast", "line": 1, "end_line": 3},
                      {"kind": "ReturnStmt", "layer": "ast", "line": 2, "end_line": 2},
                      {"kind": "block", "layer": "cfg", "line": 2, "end_line": 2}],
            "edge_index": [[0, 2], [1, 1]], "edge_type": [0, 2]}


def records():
    return [record(f"s{group}{label}", f"g{group}", label) for group in range(5) for label in (0, 1)]


def write_dataset(directory: Path):
    # Deliberately synthetic fixtures for serialization/training tests, not a benchmark.
    examples = records()
    for index, item in enumerate(examples):
        item["source"] = f"int process() {{\n    return {index};\n}}"
        item["source_sha256"] = hashlib.sha256(item["source"].encode()).hexdigest()
    data, manifest = split_records(examples)
    directory.mkdir(parents=True)
    content = "".join(json.dumps(item, sort_keys=True) + "\n" for item in data)
    manifest["dataset_sha256"] = hashlib.sha256(content.encode()).hexdigest()
    (directory / "dataset.jsonl").write_text(content, encoding="utf-8", newline="\n")
    (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
    return data, manifest


def _rewrite_dataset(directory: Path, data, manifest):
    """Recompute the file digest to exercise semantic checks beyond the checksum."""
    content = "".join(json.dumps(item, sort_keys=True) + "\n" for item in data)
    manifest["dataset_sha256"] = hashlib.sha256(content.encode()).hexdigest()
    (directory / "dataset.jsonl").write_text(content, encoding="utf-8", newline="\n")
    (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")


class DatasetTests(unittest.TestCase):
    def test_comment_whitespace_dedup_preserves_literals_and_operators(self):
        self.assertEqual(canonical_hash('int x = 2; // note'), canonical_hash('int /*note*/ x=2;'))
        self.assertNotEqual(canonical_hash('"a b"'), canonical_hash('"ab"'))
        self.assertNotEqual(canonical_hash('x++ + y'), canonical_hash('x + + + y'))
        self.assertNotEqual(canonical_hash('R"tag(a // b)tag"'), canonical_hash('R"tag(a // c)tag"'))
        self.assertNotEqual(canonical_hash('L"hi"'), canonical_hash('L "hi"'))

    def test_repeatable_group_split(self):
        data = records()
        # Avoid unintended duplicates in the synthetic source bodies.
        for index, item in enumerate(data):
            item["source"] += f" // {index}"  # comments cannot distinguish duplicate bodies
            item["source"] = item["source"].replace("return ", f"return {index} + ")
            item["source_sha256"] = hashlib.sha256(item["source"].encode()).hexdigest()
        first = split_records(data, 42)
        self.assertEqual(first, split_records(list(reversed(data)), 42))
        owners = {}
        for name, groups in first[1]["groups"].items():
            for group in groups:
                self.assertNotIn(group, owners)
                owners[group] = name
        self.assertEqual(5, len(owners))

    def test_duplicates_union_source_groups_and_conflicts_fail(self):
        data = [record(f"sample{'x' * n}", f"g{n}", n % 2) for n in range(8)]
        duplicate = copy.deepcopy(data[0])
        duplicate.update(sample_id="duplicate", group_id="g1")
        unique, manifest = split_records(data + [duplicate])
        self.assertEqual(1, manifest["duplicate_count"])
        groups = {group: item["split_group"] for item in unique for group in item["source_groups"]}
        self.assertEqual(groups["g0"], groups["g1"])
        duplicate["label"] = 1
        duplicate["defect_lines"] = [2]
        with self.assertRaisesRegex(ValueError, "冲突标签"):
            split_records(data + [duplicate])

    def test_rejects_unlabeled_rule_derived_and_bad_edges(self):
        for field, value in (("label", None), ("label_source", "rule"), ("edge_type", [7, 2]),
                             ("edge_index", [[0, 9], [1, 1]]), ("defect_lines", [9])):
            item = record()
            item[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_record(item)

    def test_missing_ambiguous_and_unused_labels_fail(self):
        item = record()
        with self.assertRaisesRegex(ValueError, "唯一外部标注"):
            join_labels([item], [])
        with self.assertRaisesRegex(ValueError, "唯一外部标注"):
            join_labels([item], [item, item])
        with self.assertRaisesRegex(ValueError, "未匹配"):
            join_labels([item], [item, record("unused")])

    def test_manifest_detects_tampering_and_group_leakage(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, manifest = write_dataset(directory)
            self.assertEqual(data, load_dataset(directory)[0])
            # Move only one of a paired group to another split without changing data.
            moved = manifest["splits"]["train"].pop(0)
            manifest["splits"]["test"].append(moved)
            (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "来源组跨集合泄漏"):
                load_dataset(directory)
            with (directory / "dataset.jsonl").open("a", encoding="utf-8") as stream:
                stream.write("\n")
            with self.assertRaisesRegex(ValueError, "哈希不一致"):
                load_dataset(directory)

    def test_dataset_cannot_overwrite_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaisesRegex(ValueError, "覆盖"):
                prepare_dataset(directory / "dataset.jsonl", directory / "labels.jsonl", directory)

    def test_relative_model_config_and_invalid_threshold(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "config.toml"
            path.write_text('[models]\nbackend="trained"\ncheckpoint="weights"\nthreshold=0.6\n', encoding="utf-8")
            config = load_config(path)
            self.assertEqual(str(directory / "weights"), config.model_checkpoint)
            self.assertEqual(0.6, config.model_threshold)
            path.write_text('[models]\nbackend="trained"\ncheckpoint="weights"\nthreshold=2\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "threshold"):
                load_config(path)

    def test_graph_validator_supports_unlabeled_inference(self):
        item = record()
        for key in ("sample_id", "group_id", "label", "label_source", "defect_lines"):
            del item[key]
        validate_graph(item)
        with self.assertRaisesRegex(ValueError, "sample_id"):
            validate_record(item)

    def test_graph_validator_rejects_malformed_types_and_layer_edges(self):
        for field, value in (("nodes", [None]), ("edge_index", [[], None]), ("edge_type", [False, 2]),
                             ("start_line", True), ("schema_version", 1), ("signature", [])):
            item = record()
            item[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_graph(item)
        for field, value in (("kind", " "), ("layer", []), ("type_name", None), ("name", {}),
                             ("line", -1), ("end_line", 0), ("column", True), ("node_id", 3)):
            item = record()
            item["nodes"][0][field] = value
            with self.subTest(node_field=field), self.assertRaises(ValueError):
                validate_graph(item)
        item = record()
        item["edge_type"] = [1, 2]
        with self.assertRaisesRegex(ValueError, "节点层"):
            validate_graph(item)
        for value in (None, [], "record"):
            with self.assertRaises(ValueError):
                validate_graph(value)

    def test_explicit_edges_and_node_ids_must_match_indices(self):
        item = record()
        for index, node in enumerate(item["nodes"]):
            node["node_id"] = f"n{index}"
        item["edges"] = [{"source": "n0", "target": "n1", "kind": "ast-child"},
                         {"source": "n2", "target": "n1", "kind": "cfg-contains"}]
        validate_graph(item)
        broken = copy.deepcopy(item)
        broken["edges"][1]["target"] = "n0"
        with self.assertRaisesRegex(ValueError, "显式图边"):
            validate_graph(broken)
        broken = copy.deepcopy(item)
        broken["nodes"][1]["node_id"] = "n0"
        with self.assertRaisesRegex(ValueError, "重复节点"):
            validate_graph(broken)

    def test_external_labels_cannot_inherit_graph_supervision(self):
        item = record(label=1)
        for field in ("sample_id", "group_id", "label", "label_source"):
            label = copy.deepcopy(item)
            del label[field]
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "外部标注缺少"):
                join_labels([item], [label])
        label = copy.deepcopy(item)
        del label["defect_lines"]
        joined = join_labels([item], [label])[0]
        self.assertNotIn("defect_lines", joined)
        joined = join_labels([item], [item])[0]
        joined["defect_lines"].append(3)
        self.assertEqual([2], item["defect_lines"])

    def test_label_selectors_and_ids_are_strict(self):
        item = record()
        for key, value in (("file", None), ("function", " "), ("signature", [])):
            malformed = dict(item, **{key: value})
            with self.subTest(key=key), self.assertRaises(ValueError):
                join_labels([malformed], [malformed])
        for value in (None, {}, [None]):
            with self.assertRaises(ValueError):
                join_labels([item], value)
        duplicate_id = record("different", "g1", 1)
        duplicate_id["sample_id"] = item["sample_id"]
        with self.assertRaisesRegex(ValueError, "重复样例 ID"):
            join_labels([item, duplicate_id], [item, duplicate_id])
        for value in (" RULE ", "Clang-Ast", "UNLABELED"):
            with self.assertRaisesRegex(ValueError, "真值"):
                validate_record(dict(item, label_source=value))
        with self.assertRaisesRegex(ValueError, "重复"):
            validate_record(dict(record(label=1), defect_lines=[2, 2]))

    def test_record_provenance_types_membership_and_hashes_are_strict(self):
        for values in ({"source_groups": []}, {"source_groups": "g0"}, {"source_groups": [None]},
                       {"source_groups": ["hidden"]}, {"duplicate_ids": ["s0"]},
                       {"duplicate_ids": ["copy", "copy"]}, {"duplicate_ids": "copy"},
                       {"source_groups": ["g0", "alias"]}, {"duplicate_ids": ["copy"]},
                       {"split_group": []}, {"canonical_sha256": "0" * 64}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                validate_record(dict(record(), **values))
        # v0.4.0 stores one source-group entry per duplicate, including repeats.
        validate_record(dict(record(), source_groups=["g0", "g0"], duplicate_ids=["copy"]))

    def test_empty_or_replaced_aliases_cannot_hide_original_group_leakage(self):
        for aliases in ([], ["unrelated"]):
            with self.subTest(aliases=aliases), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary) / "dataset"
                data, manifest = write_dataset(directory)
                moved = manifest["splits"]["train"].pop(0)
                manifest["splits"]["test"].append(moved)
                next(item for item in data if item["sample_id"] == moved)["source_groups"] = aliases
                _rewrite_dataset(directory, data, manifest)
                with self.assertRaisesRegex(ValueError, "source_groups"):
                    load_dataset(directory)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, manifest = write_dataset(directory)
            moved = manifest["splits"]["train"].pop(0)
            manifest["splits"]["test"].append(moved)
            for item in data:
                item.pop("source_groups")
            _rewrite_dataset(directory, data, manifest)
            with self.assertRaisesRegex(ValueError, "来源组跨集合泄漏"):
                load_dataset(directory)

    def test_manifest_schema_and_field_types_fail_cleanly(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, original = write_dataset(directory)
            for field, value in (("schema_version", "9.0"), ("schema_version", 1), ("seed", True),
                                 ("seed", "42"), ("method", "random"), ("fractions", []),
                                 ("fractions", {"train": 0.6, "validation": True}),
                                 ("duplicate_count", True), ("duplicate_count", -1),
                                 ("labels_sha256", None), ("dataset_sha256", []),
                                 ("splits", []), ("groups", []), ("class_counts", [])):
                manifest = dict(original, **{field: value})
                (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    load_dataset(directory)
            for field in ("splits", "groups", "class_counts"):
                for value in ("abc", [None], {"0": True, "1": 1}):
                    manifest = copy.deepcopy(original)
                    manifest[field]["train"] = value
                    (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
                    with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                        load_dataset(directory)

    def test_manifest_counts_groups_and_fractions_must_match_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, original = write_dataset(directory)
            changes = [("class_counts", "class_counts"), ("groups", "groups"),
                       ("fractions", "比例"), ("duplicate_count", "duplicate_count")]
            for field, message in changes:
                manifest = copy.deepcopy(original)
                if field == "class_counts":
                    manifest[field]["train"]["0"] += 1
                elif field == "groups":
                    manifest[field]["train"][0] = "fabricated"
                elif field == "fractions":
                    manifest[field] = {"train": 0.2, "validation": 0.2}
                else:
                    manifest[field] = 1
                (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, message):
                    load_dataset(directory)

    def test_duplicate_and_unknown_or_missing_assignments_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, original = write_dataset(directory)
            for action in ("duplicate", "unknown", "missing", "wrong_type"):
                manifest = copy.deepcopy(original)
                if action == "duplicate":
                    manifest["splits"]["test"].append(manifest["splits"]["train"][0])
                elif action == "unknown":
                    manifest["splits"]["train"][0] = "absent"
                elif action == "missing":
                    manifest["splits"]["train"].pop(0)
                else:
                    manifest["splits"]["train"] = "s00"
                (directory / "splits.json").write_text(json.dumps(manifest), encoding="utf-8")
                with self.subTest(action=action), self.assertRaises(ValueError):
                    load_dataset(directory)

    def test_dataset_rejects_false_split_group_and_stale_canonical_hash(self):
        for field, value, message in (("split_group", "invented", "split_group"),
                                      ("canonical_sha256", "0" * 64, "规范化源码哈希")):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary) / "dataset"
                data, manifest = write_dataset(directory)
                data[0][field] = value
                _rewrite_dataset(directory, data, manifest)
                with self.assertRaisesRegex(ValueError, message):
                    load_dataset(directory)

    def test_training_requires_both_classes_even_with_consistent_counts(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            data, manifest = write_dataset(directory)
            for item in data:
                if item["sample_id"] in manifest["splits"]["train"]:
                    item.update(label=0, defect_lines=[])
            manifest["class_counts"]["train"] = {"0": len(manifest["splits"]["train"]), "1": 0}
            _rewrite_dataset(directory, data, manifest)
            with self.assertRaisesRegex(ValueError, "同时含正负例"):
                load_dataset(directory)

    def test_duplicate_sources_and_conflicting_labels_cannot_be_reintroduced(self):
        for conflict in (False, True):
            with self.subTest(conflict=conflict), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary) / "dataset"
                data, manifest = write_dataset(directory)
                train = [item for item in data if item["sample_id"] in manifest["splits"]["train"]]
                left, right = train[:2]
                right.update(source=left["source"], source_sha256=left["source_sha256"],
                             canonical_sha256=left["canonical_sha256"], label=1 - left["label"] if conflict else left["label"],
                             defect_lines=[2] if (1 - left["label"] if conflict else left["label"]) else [])
                _rewrite_dataset(directory, data, manifest)
                with self.assertRaisesRegex(ValueError, "冲突标签" if conflict else "未去重"):
                    load_dataset(directory)

    def test_resplit_preserves_duplicate_provenance_and_legacy_repeated_groups(self):
        examples = [record(f"sample{'x' * n}", f"g{n}", n % 2) for n in range(8)]
        duplicate = copy.deepcopy(examples[0])
        duplicate.update(sample_id="duplicate", group_id="g1")
        same_group = copy.deepcopy(duplicate)
        same_group["sample_id"] = "same-group-duplicate"
        data, manifest = split_records(examples + [duplicate, same_group])
        again, repeated = split_records(data)
        self.assertEqual(data, again)
        self.assertEqual(manifest, repeated)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            _rewrite_dataset(directory, data, manifest)
            self.assertEqual(data, load_dataset(directory)[0])
        data[1]["duplicate_ids"] = [data[0]["sample_id"]]
        data[1]["source_groups"].append(data[1]["group_id"])
        with self.assertRaisesRegex(ValueError, "重复样例 ID"):
            split_records(data)

    def test_split_fraction_types_extremes_and_input_types(self):
        for train, validation in ((True, 0.2), ("0.6", 0.2), (float("nan"), 0.2),
                                  (0.6, float("inf")), (10 ** 400, 0.2), (0.8, 0.2)):
            with self.subTest(train=train, validation=validation), self.assertRaises(ValueError):
                split_records(records(), train_fraction=train, validation_fraction=validation)
        for values in (None, [], [None], [{}]):
            with self.assertRaises(ValueError):
                split_records(values)
        with self.assertRaisesRegex(ValueError, "seed"):
            split_records(records(), seed=True)
        examples = [record(f"sample{'x' * n}", f"g{n // 2}", n % 2) for n in range(6)]
        _, manifest = split_records(examples, train_fraction=0.01, validation_fraction=0.98)
        self.assertEqual([1, 1, 1], [len(groups) for groups in manifest["groups"].values()])

    def test_json_duplicate_keys_nonfinite_values_and_nonobjects_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "dataset"
            _, manifest = write_dataset(directory)
            malformed = ('[]', 'null', '{"seed":42,"seed":7}', '{"seed":NaN}', '{"seed":Infinity}', '{"seed":1e400}')
            for content in malformed:
                (directory / "splits.json").write_text(content, encoding="utf-8")
                with self.subTest(manifest=content), self.assertRaises(ValueError):
                    load_dataset(directory)
            for content in malformed + ('{"nested":{"label":0,"label":1}}',):
                (directory / "bad.jsonl").write_text(content, encoding="utf-8")
                with self.subTest(record=content), self.assertRaises(ValueError):
                    read_jsonl(directory / "bad.jsonl")


class EvaluationTests(unittest.TestCase):
    def test_classification_and_localization_denominators(self):
        data = [record("a", label=1), record("b", label=1), record("c", label=0), record("d", label=0)]
        predictions = [{"sample_id": key, "probability": prob, "ranked_lines": lines} for key, prob, lines in
                       [("a", 0.9, [2, 1]), ("b", 0.1, [1, 2]), ("c", 0.8, [1]), ("d", 0.2, [2])]]
        result = evaluate_predictions(data, predictions)
        self.assertEqual({"tp": 1, "tn": 1, "fp": 1, "fn": 1}, result["confusion_matrix"])
        self.assertEqual(0.5, result["f1"])
        self.assertEqual(1.0, result["localization"]["top_k"]["3"])
        self.assertEqual(0.5, result["localization"]["end_to_end_top_k"]["3"])

    def test_sequence_does_not_claim_localization(self):
        result = evaluate_predictions([record(label=1)], [{"sample_id": "s0", "probability": 0.7, "ranked_lines": None}])
        self.assertFalse(result["localization"]["supported"])
        self.assertIsNone(result["localization"]["top_k"]["1"])

    def test_invalid_predictions_and_zero_positive_case(self):
        for prediction in [{"sample_id": "s0", "probability": float("nan")},
                           {"sample_id": "s0", "probability": 0.8, "ranked_lines": [2, 2]},
                           {"sample_id": "s0", "probability": 0.8, "ranked_lines": [9]}]:
            with self.assertRaises(ValueError):
                evaluate_predictions([record()], [prediction])
        result = evaluate_predictions([record()], [{"sample_id": "s0", "probability": 0.1}])
        self.assertEqual(1, result["accuracy"])
        self.assertEqual(0, result["f1"])


if __name__ == "__main__":
    unittest.main()
