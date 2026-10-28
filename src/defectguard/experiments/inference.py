"""无标签程序图包的只读研究推理；导入此模块不加载模型依赖。"""
from __future__ import annotations

from collections import Counter
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re

from .data import _json_object, validate_graph
from defectguard.graphs import EDGE_TYPES


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _hash(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _relative_file(value) -> bool:
    return (isinstance(value, str) and bool(value) and "\\" not in value and ":" not in value
            and not PurePosixPath(value).is_absolute() and ".." not in value.split("/")
            and all(part not in ("", ".") for part in value.split("/")))


def _snapshot(directory: Path, names: tuple[str, ...]) -> dict:
    result = {}
    for name in names:
        raw = (directory / name).read_bytes()
        result[name] = {"bytes": len(raw), "sha256": _digest(raw)}
    return result


def load_graph_bundle(directory: Path) -> tuple[list[dict], dict, dict]:
    """只接受导出器的 1.0 无标签协议，不猜测旧的训练样例格式。"""
    raw_manifest = (directory / "manifest.json").read_bytes()
    raw_graphs = (directory / "graphs.jsonl").read_bytes()
    manifest = _json_object(raw_manifest.decode("utf-8-sig"), "程序图清单")
    if manifest.get("schema_version") != "1.0" or manifest.get("parser_backend") != "clang-libtooling":
        raise ValueError("不支持的程序图包版本或解析后端")
    edge_types = manifest.get("edge_types")
    if edge_types != EDGE_TYPES or any(type(value) is not int for value in edge_types.values()):
        raise ValueError("程序图包边类型词表不兼容")
    if not _hash(manifest.get("source_fingerprint")):
        raise ValueError("程序图包缺少源码指纹")
    if manifest.get("graphs_sha256") != _digest(raw_graphs):
        raise ValueError("程序图文件哈希不一致")
    count = manifest.get("function_count")
    if type(count) is not int or count < 1:
        raise ValueError("程序图包函数数量无效")
    missing = manifest.get("files_without_function_graphs")
    if (not isinstance(missing, list) or not all(_relative_file(name) for name in missing)
            or len(missing) != len(set(missing))):
        raise ValueError("未覆盖文件列表无效")
    limits = manifest.get("dfg_limitations")
    if not isinstance(limits, list) or not limits or not all(isinstance(x, str) and x.strip() for x in limits):
        raise ValueError("程序图包缺少数据流范围说明")
    if not isinstance(manifest.get("label_status"), str) or not manifest["label_status"].startswith("unlabeled:"):
        raise ValueError("研究推理需要无标签程序图包")
    records, seen, locations = [], set(), set()
    for number, line in enumerate(raw_graphs.decode("utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        record = _json_object(line, f"graphs.jsonl:{number}")
        validate_graph(record)
        required = ("graph_id", "file", "function", "signature", "source_slice", "dfg_status", "edges")
        if record.get("schema_version") != "1.0" or any(key not in record for key in required):
            raise ValueError("函数图缺少 1.0 协议字段")
        if (not _relative_file(record["file"]) or record["source_slice"] not in ("token-range", "line-range-fallback")
                or not isinstance(record["dfg_status"], str) or not record["dfg_status"].strip()):
            raise ValueError("函数图路径或覆盖状态无效")
        if record.get("label", "missing") is not None or record.get("label_source") != "unlabeled":
            raise ValueError("研究推理不接收训练标签；请使用无标签导出包")
        if any(key in record for key in ("sample_id", "group_id", "defect_lines")):
            raise ValueError("研究推理不接收训练样例身份或标注")
        identity = f"{record['file']}:{record['function']}:{record['signature']}:{record['start_line']}:{record['source_sha256']}"
        if record["graph_id"] != _digest(identity.encode("utf-8")) or record["graph_id"] in seen:
            raise ValueError("函数图标识不匹配或重复")
        location = tuple(record[key] for key in ("file", "function", "signature", "start_line"))
        if location in locations:
            raise ValueError("相同函数位置存在多个源码版本")
        seen.add(record["graph_id"])
        locations.add(location)
        records.append(record)
    if len(records) != count or {r["file"] for r in records}.intersection(missing):
        raise ValueError("程序图数量或未覆盖文件清单不一致")
    receipt = {"manifest.json": {"bytes": len(raw_manifest), "sha256": _digest(raw_manifest)},
               "graphs.jsonl": {"bytes": len(raw_graphs), "sha256": _digest(raw_graphs)}}
    return records, manifest, receipt


def _load_predictor(checkpoint: Path, encoder: Path | None):
    try:
        from .runner import TrainedPredictor
    except ImportError as error:
        raise ValueError("模型依赖未安装，请使用现有 .venv-ml 模型环境") from error
    return TrainedPredictor(checkpoint, encoder)


def _encoder_unchanged(predictor) -> None:
    if predictor.encoder is not None:
        from .encoder import encoder_identity
        if encoder_identity(predictor.encoder.directory) != predictor.encoder.identity:
            raise ValueError("编码器文件在推理期间发生变化")


def _probability(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 1


def _validate_predictions(records: list[dict], predictions) -> None:
    if not isinstance(predictions, list) or len(predictions) != len(records):
        raise ValueError("预测数量与函数图不一致")
    for record, prediction in zip(records, predictions):
        if (not isinstance(prediction, dict) or prediction.get("sample_id") != record["graph_id"]
                or any(prediction.get(key) != record[key] for key in ("file", "function"))
                or not _probability(prediction.get("probability"))):
            raise ValueError("预测身份或分数无效")
        ranked, scores = prediction.get("ranked_lines"), prediction.get("line_scores")
        if ranked is not None and (not isinstance(ranked, list) or any(
                type(line) is not int or not record["start_line"] <= line <= record["end_line"] for line in ranked)
                or len(ranked) != len(set(ranked))):
            raise ValueError("预测行排名无效")
        if (not isinstance(scores, dict) or set(scores) != {str(line) for line in ranked or []}
                or not all(_probability(value) for value in scores.values())):
            raise ValueError("预测行分数无效")


def _encoding_coverage(predictor, records: list[dict]) -> dict:
    if predictor.encoder is None:
        if predictor.last_encoding is not None:
            raise ValueError("纯图模型不应声明序列编码覆盖")
        return {"status": "not_used", "truncated_functions": [], "functions": []}
    info = predictor.last_encoding
    if (not isinstance(info, dict) or info.get("encoder_sha256") != predictor.encoder.identity
            or type(info.get("max_length")) is not int or info["max_length"] != predictor.encoder.max_length):
        raise ValueError("序列编码覆盖缺失或指纹不匹配")
    details = info.get("functions")
    if not isinstance(details, list) or len(details) != len(records):
        raise ValueError("序列编码覆盖数量不一致")
    for record, detail in zip(records, details):
        if (not isinstance(detail, dict) or detail.get("sample_id") != record["graph_id"]
                or any(detail.get(key) != record[key] for key in ("file", "function", "start_line", "end_line"))):
            raise ValueError("序列编码覆盖身份不一致")
        count, retained, truncated = (detail.get(key) for key in ("token_count", "retained_token_count", "truncated"))
        if (type(count) is not int or count < 1 or type(retained) is not int
                or retained != min(count, info["max_length"]) or type(truncated) is not bool
                or truncated != (count > info["max_length"])):
            raise ValueError("序列截断记录无效")
    truncated = [detail for detail in details if detail["truncated"]]
    if type(info.get("truncated_functions")) is not int or info["truncated_functions"] != len(truncated):
        raise ValueError("序列截断数量不一致")
    return {**info, "status": "truncated" if truncated else "not_truncated", "truncated_functions": truncated}


def infer_graphs(bundle: Path, checkpoint: Path, output: Path, encoder: Path | None = None) -> dict:
    bundle, checkpoint, output = bundle.resolve(), checkpoint.resolve(), output.resolve()
    if output.exists():
        raise ValueError("推理输出目录已存在，请使用新目录")
    for directory in (bundle, checkpoint, encoder.resolve() if encoder else None):
        if directory is not None and output.is_relative_to(directory):
            raise ValueError("推理输出不能位于程序图、检查点或编码器目录内部")
    records, graph_manifest, inputs = load_graph_bundle(bundle)
    checkpoint_inputs = _snapshot(checkpoint, ("metadata.json", "model.safetensors"))
    metadata = _json_object((checkpoint / "metadata.json").read_text(encoding="utf-8"), "模型元数据")
    predictor = _load_predictor(checkpoint, encoder)
    if predictor.metadata != metadata or checkpoint_inputs["model.safetensors"]["sha256"] != metadata["weights_sha256"]:
        raise ValueError("检查点加载内容与输入摘要不一致")
    threshold = metadata.get("threshold")
    if not _probability(threshold):
        raise ValueError("检查点阈值无效")
    if predictor.encoder is not None and output.is_relative_to(predictor.encoder.directory):
        raise ValueError("推理输出不能位于编码器目录内部")
    _encoder_unchanged(predictor)
    predictions = predictor.predict(records)
    _validate_predictions(records, predictions)
    encoding = _encoding_coverage(predictor, records)
    _encoder_unchanged(predictor)
    if (_snapshot(bundle, ("manifest.json", "graphs.jsonl")) != inputs
            or _snapshot(checkpoint, ("metadata.json", "model.safetensors")) != checkpoint_inputs):
        raise ValueError("程序图包或检查点在推理期间发生变化")
    results = []
    for record, prediction in zip(records, predictions):
        results.append({**prediction, "graph_id": record["graph_id"], "source_sha256": record["source_sha256"],
                        "start_line": record["start_line"], "end_line": record["end_line"],
                        "decision": "suspected" if prediction["probability"] >= threshold else "below_threshold",
                        "calibration": "uncalibrated", "correctness": "not_assessed"})
    encoded = (json.dumps(results, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    result = {
        "format": "CodeGuardGraphInference 1", "status": "research_only",
        "graph_schema_version": graph_manifest["schema_version"], "function_count": len(records),
        "inputs": {"graph_bundle": inputs, "checkpoint": checkpoint_inputs},
        "declared_source_fingerprint": graph_manifest["source_fingerprint"],
        "checkpoint": {"mode": metadata["network"]["mode"], "threshold": threshold,
                       "threshold_source": "checkpoint", "feature_version": metadata["network"].get("feature_version", "legacy-v1"),
                       "excluded_edge_types": metadata["excluded_edges"]},
        "coverage": {"scope": "exported_functions_only", "repository_coverage": "not_assessed",
                     "files_without_function_graphs": graph_manifest["files_without_function_graphs"],
                     "source_slices": dict(Counter(r["source_slice"] for r in records)),
                     "dfg_statuses": dict(Counter(r["dfg_status"] for r in records)), "sequence": encoding},
        "dfg_limitations": graph_manifest["dfg_limitations"],
        "calibration": "uncalibrated", "correctness": "not_assessed",
        "training": False, "source_auto_applied": False,
        "files": [{"path": "predictions.json", "bytes": len(encoded), "sha256": _digest(encoded)}],
    }
    # The result manifest is written last; incomplete writes cannot claim success.
    output.mkdir(parents=True, exist_ok=False)
    with (output / "predictions.json").open("xb") as stream:
        stream.write(encoded)
    with (output / "manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    return result
