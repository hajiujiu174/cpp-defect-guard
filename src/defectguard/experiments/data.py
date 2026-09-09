from __future__ import annotations

import copy
import hashlib
import json
import math
from pathlib import Path
import random
import re
from typing import Any


_TOKENS = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?R"(?P<delim>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delim)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?\'(?:\\.|[^\'\\])*\''
    r'|[A-Za-z_]\w*|(?:\d+(?:\.\d*)?|\.\d+)(?:[eEpP][+-]?\d+)?[a-zA-Z0-9_]*'
    r'|>>=|<<=|->\*|<=>|\.\.\.|::|->|\.\*|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\|'
    r'|\+=|-=|\*=|/=|%=|&=|\|=|\^=|##|[^\s]'
)
_SPLITS = {"train", "validation", "test"}
_METHOD = "source-group-isolation-and-token-exact-deduplication"


def _nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _string_list(value: Any, *, nonempty: bool = False, unique: bool = False) -> bool:
    return (isinstance(value, list) and (bool(value) or not nonempty)
            and all(_nonempty_string(item) for item in value)
            and (not unique or len(value) == len(set(value))))


def _hash_string(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _json_object(text: str, context: str) -> dict[str, Any]:
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"重复 JSON 字段：{key}")
            result[key] = value
        return result

    def constant(value):
        raise ValueError(f"非有限 JSON 数值：{value}")

    def finite_float(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            constant(value)
        return parsed

    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant, parse_float=finite_float)
    except ValueError as error:
        raise ValueError(f"{context}: JSON 无效：{error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{context}: 必须为对象")
    return value


def _validate_fractions(train: Any, validation: Any) -> None:
    if (type(train) not in {int, float} or type(validation) not in {int, float}
            or not 0 < train < 1 or not 0 < validation < 1
            or not math.isfinite(train) or not math.isfinite(validation) or train + validation >= 1):
        raise ValueError("训练/验证比例必须为有限正数，且总和小于 1")


def _partition_sizes(count: int, train: float, validation: float) -> tuple[int, int]:
    train_count = max(1, int(count * train))
    validation_count = min(count - 2, max(1, int(count * validation)))
    if train_count + validation_count >= count:
        train_count = count - validation_count - 1
    return train_count, validation_count


def canonical_hash(source: str) -> str:
    # Preprocessor line boundaries and backslash continuations are semantic.
    # Keep exact bytes for those functions instead of risking a false merge.
    if "#" in source or "\\\n" in source or "\\\r\n" in source:
        return hashlib.sha256(source.encode("utf-8")).hexdigest()
    tokens = [match.group() for match in _TOKENS.finditer(source)
              if not match.group().startswith(("//", "/*"))]
    return hashlib.sha256(" ".join(tokens).encode("utf-8")).hexdigest()


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    records = []
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        records.append(_json_object(line, f"{path.name}:{number}"))
    if not records:
        raise ValueError("数据集为空")
    return records


def validate_graph(record: dict[str, Any]) -> None:
    """Validate graph input for both unlabeled inference and labeled experiments."""
    if not isinstance(record, dict):
        raise ValueError("程序图必须为对象")
    if "schema_version" in record and record["schema_version"] != "1.0":
        raise ValueError("不支持的程序图 schema_version")
    if not _nonempty_string(record.get("source")):
        raise ValueError("样例缺少 source")
    for key in ("file", "function", "graph_id", "source_slice"):
        if key in record and not _nonempty_string(record[key]):
            raise ValueError(f"程序图 {key} 必须为非空字符串")
    if "signature" in record and not isinstance(record["signature"], str):
        raise ValueError("程序图 signature 必须为字符串")
    begin, end = record.get("start_line"), record.get("end_line")
    if type(begin) is not int or type(end) is not int or not 1 <= begin <= end:
        raise ValueError("函数源码行范围无效")
    if record.get("source_sha256") != hashlib.sha256(record["source"].encode("utf-8")).hexdigest():
        raise ValueError("源码哈希不一致")
    nodes, indices, types = record.get("nodes"), record.get("edge_index"), record.get("edge_type")
    if not isinstance(nodes, list) or not nodes or not isinstance(indices, list) or len(indices) != 2:
        raise ValueError("训练样例缺少有效程序图")
    if not isinstance(types, list) or not all(isinstance(row, list) and len(row) == len(types) for row in indices):
        raise ValueError("图边索引与类型数量不一致")
    if any(type(index) is not int or not 0 <= index < len(nodes) for row in indices for index in row):
        raise ValueError("程序图包含越界索引")
    if any(type(kind) is not int or kind not in range(4) for kind in types):
        raise ValueError("未知图边类型")
    for node in nodes:
        if (not isinstance(node, dict) or not _nonempty_string(node.get("kind"))
                or not isinstance(node.get("layer"), str) or node["layer"] not in {"ast", "cfg"}
                or type(node.get("line")) is not int or type(node.get("end_line")) is not int
                or not 0 <= node["line"] <= node["end_line"]):
            raise ValueError("图节点必须提供 kind、layer 和有效整数源码位置")
        for key in ("type_name", "name", "symbol"):
            if key in node and not isinstance(node[key], str):
                raise ValueError(f"图节点 {key} 必须为字符串")
        if "column" in node and (type(node["column"]) is not int or node["column"] < 0):
            raise ValueError("图节点 column 必须为非负整数")
        if "node_id" in node and not _nonempty_string(node["node_id"]):
            raise ValueError("图节点 node_id 必须为非空字符串")
    node_ids = [node["node_id"] for node in nodes if "node_id" in node]
    if len(node_ids) != len(set(node_ids)):
        raise ValueError("程序图包含重复节点 ID")
    if not any(node["layer"] == "ast" for node in nodes):
        raise ValueError("程序图不能缺少 AST 节点")
    for left, right, kind in zip(*indices, types):
        expected = ("cfg", "cfg") if kind == 1 else ("cfg", "ast") if kind == 2 else ("ast", "ast")
        if (nodes[left]["layer"], nodes[right]["layer"]) != expected:
            raise ValueError("程序图边类型与节点层不一致")
    if "edges" in record:
        edges = record["edges"]
        if not isinstance(edges, list) or len(edges) != len(types) or len(node_ids) != len(nodes):
            raise ValueError("显式图边与索引数量或节点 ID 不一致")
        names = ("ast-child", "cfg-successor", "cfg-contains", "def-use")
        for edge, left, right, kind in zip(edges, *indices, types):
            if (not isinstance(edge, dict) or edge.get("source") != nodes[left]["node_id"]
                    or edge.get("target") != nodes[right]["node_id"] or edge.get("kind") != names[kind]
                    or ("symbol" in edge and not isinstance(edge["symbol"], str))):
                raise ValueError("显式图边与边索引/类型不一致")


def validate_record(record: dict[str, Any]) -> None:
    validate_graph(record)
    for key in ("sample_id", "group_id", "label_source"):
        if not _nonempty_string(record.get(key)):
            raise ValueError(f"样例缺少 {key}")
    if type(record.get("label")) is not int or record["label"] not in {0, 1}:
        raise ValueError("训练数据必须提供独立标注的二分类 label=0/1")
    if record["label_source"].strip().casefold() in {"unlabeled", "rule", "clang-ast"}:
        raise ValueError("规则输出或未标注图不能作为训练真值")
    begin, end = record["start_line"], record["end_line"]
    lines = record.get("defect_lines", [])
    if not isinstance(lines, list) or any(type(line) is not int or not begin <= line <= end for line in lines):
        raise ValueError("缺陷行标注超出函数范围")
    if len(lines) != len(set(lines)):
        raise ValueError("缺陷行标注不能重复")
    if record["label"] == 0 and lines:
        raise ValueError("无缺陷样例不能包含缺陷行标注")
    if "source_groups" in record:
        # Older v0.4.0 manifests may repeat a group for same-group duplicates.
        # Keep accepting those lists, but never let an empty/replaced list hide group_id.
        if not _string_list(record["source_groups"], nonempty=True) or record["group_id"] not in record["source_groups"]:
            raise ValueError("source_groups 必须为包含原 group_id 的非空字符串列表")
    if "split_group" in record and not _nonempty_string(record["split_group"]):
        raise ValueError("split_group 必须为非空字符串")
    if "duplicate_ids" in record:
        if not _string_list(record["duplicate_ids"], unique=True) or record["sample_id"] in record["duplicate_ids"]:
            raise ValueError("duplicate_ids 必须为不含自身 ID 的唯一字符串列表")
    if ("source_groups" in record and len(record["source_groups"]) != len(record.get("duplicate_ids", [])) + 1
            or record.get("duplicate_ids") and "source_groups" not in record):
        raise ValueError("source_groups 与 duplicate_ids 的来源数量不一致")
    if "canonical_sha256" in record and record["canonical_sha256"] != canonical_hash(record["source"]):
        raise ValueError("规范化源码哈希不一致")


def join_labels(graphs: list[dict[str, Any]], labels: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if not isinstance(graphs, list) or not graphs or not isinstance(labels, list):
        raise ValueError("函数图必须为非空列表，外部标注必须为列表")
    for value in graphs + labels:
        if not isinstance(value, dict) or any(not _nonempty_string(value.get(key)) for key in ("file", "function")):
            raise ValueError("函数图和外部标注必须提供非空 file、function")
        if "signature" in value and not isinstance(value["signature"], str):
            raise ValueError("函数图和外部标注的 signature 必须为字符串")
    for label in labels:
        # Never inherit supervision from a previously labeled graph when an
        # external label entry accidentally omits its authoritative fields.
        for key in ("sample_id", "group_id", "label", "label_source"):
            if key not in label:
                raise ValueError(f"外部标注缺少 {key}")
    matched: set[int] = set()
    records = []
    ids: set[str] = set()
    for graph in graphs:
        matches = [(index, label) for index, label in enumerate(labels)
                   if label.get("file") == graph.get("file") and label.get("function") == graph.get("function")
                   and ("signature" not in label or label["signature"] == graph.get("signature"))]
        if len(matches) != 1:
            raise ValueError(f"函数需要唯一外部标注：{graph.get('file')}:{graph.get('function')}")
        index, label = matches[0]
        if index in matched:
            raise ValueError("一个标注匹配到多个重载函数，请补充 signature")
        matched.add(index)
        record = copy.deepcopy(graph)
        for key in ("defect_lines", "rationale", "provenance"):
            record.pop(key, None)
        for key in ("sample_id", "group_id", "label", "label_source", "defect_lines", "rationale", "provenance"):
            if key in label:
                record[key] = copy.deepcopy(label[key])
        validate_record(record)
        if record["sample_id"] in ids:
            raise ValueError("重复样例 ID")
        ids.add(record["sample_id"])
        records.append(record)
    if len(matched) != len(labels):
        raise ValueError("部分标注未匹配到函数图")
    return records


def split_records(records: list[dict[str, Any]], seed: int = 42,
                  train_fraction: float = 0.6, validation_fraction: float = 0.2) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    _validate_fractions(train_fraction, validation_fraction)
    if type(seed) is not int:
        raise ValueError("切分 seed 必须为整数")
    if not isinstance(records, list) or not records:
        raise ValueError("数据集必须为非空列表")
    parents: dict[str, str] = {}
    all_ids = []
    for record in records:
        validate_record(record)
        all_ids.extend([record["sample_id"], *record.get("duplicate_ids", [])])
        for group in [record["group_id"], *record.get("source_groups", [])]:
            parents[group] = group
    if len(set(all_ids)) != len(all_ids):
        raise ValueError("重复样例 ID（包含 duplicate_ids）")

    def find(group):
        while parents[group] != group:
            parents[group] = parents[parents[group]]
            group = parents[group]
        return group

    # Re-splitting an already prepared dataset must retain its duplicate
    # provenance; otherwise previously merged source groups could leak apart.
    for record in records:
        for group in record.get("source_groups", []):
            left, right = find(record["group_id"]), find(group)
            parents[max(left, right)] = min(left, right)

    unique: dict[str, dict[str, Any]] = {}
    for original in sorted(records, key=lambda record: record["sample_id"]):
        record = copy.deepcopy(original)
        key = canonical_hash(record["source"])
        if key in unique:
            previous = unique[key]
            if previous["label"] != record["label"]:
                raise ValueError("重复源码具有冲突标签")
            left, right = find(previous["group_id"]), find(record["group_id"])
            parents[max(left, right)] = min(left, right)
            previous["duplicate_ids"].extend([record["sample_id"], *record.get("duplicate_ids", [])])
            previous["source_groups"].extend(record.get("source_groups", [record["group_id"]]))
        else:
            record["canonical_sha256"] = key
            record.setdefault("duplicate_ids", [])
            record.setdefault("source_groups", [record["group_id"]])
            unique[key] = record
    deduplicated = sorted(unique.values(), key=lambda record: record["sample_id"])
    grouped: dict[str, list[dict[str, Any]]] = {}
    for record in deduplicated:
        record["split_group"] = find(record["group_id"])
        grouped.setdefault(record["split_group"], []).append(record)
    groups = sorted(grouped)
    if len(groups) < 3:
        raise ValueError("去重并合并来源后至少需要三个独立组，才能划分训练/验证/测试集")
    train_count, validation_count = _partition_sizes(len(groups), train_fraction, validation_fraction)
    best = None
    generator = random.Random(seed)
    # Stratify at group level; labels never split a paired/duplicate group apart.
    for _ in range(128):
        order = list(groups)
        generator.shuffle(order)
        partitions = {"train": order[:train_count], "validation": order[train_count:train_count + validation_count],
                      "test": order[train_count + validation_count:]}
        classes = {name: {record["label"] for group in subset for record in grouped[group]}
                   for name, subset in partitions.items()}
        if classes["train"] != {0, 1}:
            continue
        score = sum(len(values) for values in classes.values())
        if best is None or score > best[0]:
            best = (score, partitions)
        if score == 6:
            break
    if best is None:
        raise ValueError("无法在保持组隔离的条件下获得同时含正负例的训练集")
    partitions = best[1]
    split_ids = {name: sorted(record["sample_id"] for group in subset for record in grouped[group])
                 for name, subset in partitions.items()}
    manifest = {
        "schema_version": "1.0", "seed": seed,
        "fractions": {"train": train_fraction, "validation": validation_fraction},
        "method": _METHOD,
        "duplicate_count": len(all_ids) - len(deduplicated),
        "groups": {key: sorted(value) for key, value in partitions.items()}, "splits": split_ids,
        "class_counts": {name: {str(label): sum(record["label"] == label for record in deduplicated
                                                 if record["sample_id"] in identifiers) for label in (0, 1)}
                         for name, identifiers in split_ids.items()},
    }
    return deduplicated, manifest


def prepare_dataset(graphs_path: Path, labels_path: Path, output: Path, seed: int = 42) -> dict[str, Any]:
    inputs = {graphs_path.resolve(), labels_path.resolve()}
    if output.resolve() in inputs or any((output / name).resolve() in inputs for name in ("dataset.jsonl", "splits.json")):
        raise ValueError("输出目录不能覆盖输入文件")
    if output.exists() and any(output.iterdir()):
        raise ValueError("数据集输出目录非空，请使用新目录以保留既有标签和划分")
    records = join_labels(read_jsonl(graphs_path), read_jsonl(labels_path))
    records, manifest = split_records(records, seed)
    encoded = "".join(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n" for record in records)
    manifest["dataset_sha256"] = hashlib.sha256(encoded.encode("utf-8")).hexdigest()
    manifest["labels_sha256"] = hashlib.sha256(labels_path.read_bytes()).hexdigest()
    output.mkdir(parents=True, exist_ok=True)
    (output / "dataset.jsonl").write_text(encoded, encoding="utf-8", newline="\n")
    (output / "splits.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest


def load_dataset(directory: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    path = directory / "dataset.jsonl"
    manifest = _json_object((directory / "splits.json").read_text(encoding="utf-8-sig"), "splits.json")
    _validate_manifest(manifest)
    if hashlib.sha256(path.read_bytes()).hexdigest() != manifest.get("dataset_sha256"):
        raise ValueError("数据集与划分清单哈希不一致")
    records = read_jsonl(path)
    for record in records:
        validate_record(record)
    ids = [record["sample_id"] for record in records]
    partitions = manifest["splits"]
    assigned = [identifier for subset in partitions.values() for identifier in subset]
    if len(set(ids)) != len(ids) or len(assigned) != len(set(assigned)) or set(assigned) != set(ids):
        raise ValueError("划分包含重复、未知或遗漏样例")
    duplicate_ids = [identifier for record in records for identifier in record.get("duplicate_ids", [])]
    if (len(duplicate_ids) != len(set(duplicate_ids)) or set(duplicate_ids) & set(ids)
            or len(duplicate_ids) != manifest["duplicate_count"]):
        raise ValueError("duplicate_count 或重复样例 ID 清单不一致")
    group_owners, hash_owners, source_labels = {}, {}, {}
    parents = {group: group for record in records for group in [record["group_id"], *record.get("source_groups", [])]}

    def find(group):
        while parents[group] != group:
            parents[group] = parents[parents[group]]
            group = parents[group]
        return group

    for record in records:
        for group in record.get("source_groups", []):
            left, right = find(record["group_id"]), find(group)
            parents[max(left, right)] = min(left, right)

    lookup = {record["sample_id"]: record for record in records}
    actual_groups: dict[str, set[str]] = {}
    actual_counts = {}
    for name, subset in partitions.items():
        actual_groups[name] = set()
        actual_counts[name] = {"0": 0, "1": 0}
        for identifier in subset:
            record = lookup[identifier]
            # group_id is always authoritative, regardless of optional aliases.
            for group in {record["group_id"], *record.get("source_groups", [])}:
                if group in group_owners and group_owners[group] != name:
                    raise ValueError("来源组跨集合泄漏")
                group_owners[group] = name
            key = canonical_hash(record["source"])
            if key in source_labels and source_labels[key] != record["label"]:
                raise ValueError("重复源码具有冲突标签")
            source_labels[key] = record["label"]
            if key in hash_owners and hash_owners[key] != name:
                raise ValueError("重复源码跨集合泄漏")
            if key in hash_owners:
                raise ValueError("数据集含未去重的重复源码")
            hash_owners[key] = name
            root = find(record["group_id"])
            if "split_group" in record and record["split_group"] != root:
                raise ValueError("split_group 与原始来源的连通分组不一致")
            actual_groups[name].add(root)
            actual_counts[name][str(record["label"])] += 1
    if actual_counts["train"]["0"] == 0 or actual_counts["train"]["1"] == 0:
        raise ValueError("训练集必须同时含正负例")
    if actual_counts != manifest["class_counts"]:
        raise ValueError("class_counts 与实际标签不一致")
    if any(actual_groups[name] != set(manifest["groups"][name]) for name in _SPLITS):
        raise ValueError("groups 与实际来源分组不一致")
    groups = [group for subset in manifest["groups"].values() for group in subset]
    if len(groups) != len(set(groups)):
        raise ValueError("来源组跨集合泄漏")
    train_count, validation_count = _partition_sizes(len(groups), **manifest["fractions"])
    if (len(actual_groups["train"]), len(actual_groups["validation"])) != (train_count, validation_count):
        raise ValueError("分组数量与划分比例不一致")
    return records, manifest


def _validate_manifest(manifest: dict[str, Any]) -> None:
    if manifest.get("schema_version") != "1.0":
        raise ValueError("不支持的划分清单 schema_version")
    if type(manifest.get("seed")) is not int:
        raise ValueError("划分清单 seed 必须为整数")
    if manifest.get("method") != _METHOD:
        raise ValueError("不支持的划分清单 method")
    fractions = manifest.get("fractions")
    if not isinstance(fractions, dict) or set(fractions) != {"train", "validation"}:
        raise ValueError("划分清单 fractions 必须包含 train、validation")
    _validate_fractions(**fractions)
    if type(manifest.get("duplicate_count")) is not int or manifest["duplicate_count"] < 0:
        raise ValueError("duplicate_count 必须为非负整数")
    if not _hash_string(manifest.get("dataset_sha256")):
        raise ValueError("数据集与划分清单哈希不一致或格式无效")
    if "labels_sha256" in manifest and not _hash_string(manifest["labels_sha256"]):
        raise ValueError("labels_sha256 格式无效")
    for field in ("splits", "groups"):
        values = manifest.get(field)
        if (not isinstance(values, dict) or set(values) != _SPLITS
                or any(not _string_list(subset, nonempty=True, unique=True) for subset in values.values())):
            raise ValueError(f"{field} 需要非空训练、验证和测试集，且各项为唯一字符串列表")
    counts = manifest.get("class_counts")
    if (not isinstance(counts, dict) or set(counts) != _SPLITS
            or any(not isinstance(values, dict) or set(values) != {"0", "1"}
                   or any(type(value) is not int or value < 0 for value in values.values())
                   for values in counts.values())):
        raise ValueError("class_counts 必须包含三组的非负整数 0/1 类计数")
