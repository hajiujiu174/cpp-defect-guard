"""CPU 确定性实验：训练集拟合特征，验证集选轮次，测试集只做最终评估。"""
from __future__ import annotations

import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import random
import re
import sys

import torch
from torch.nn import functional as F
from safetensors.torch import load_file, save_file
from safetensors import SafetensorError

from .data import load_dataset, validate_graph
from .encoder import FrozenEncoder
from .evaluation import evaluate_predictions
from .network import DetectorNetwork, NUMERIC_FEATURES, feature_names, fit_vocabulary, tensor_graph


def _write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")


def _seed(seed: int) -> None:
    random.seed(seed)
    torch.manual_seed(seed)
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)


def _predictions(model, records, graphs, localization=True) -> list[dict]:
    if len(records) != len(graphs):
        raise ValueError("推理输入与程序图数量不一致")
    model.eval()
    result = []
    with torch.no_grad():
        for record, graph in zip(records, graphs):
            logit, lines = model(graph)
            if logit.numel() != 1 or not torch.isfinite(logit).all() or any(
                score.numel() != 1 or not torch.isfinite(score).all() for score in lines.values()
            ):
                raise ValueError(f"模型推理产生非有限分数或非标量输出：{record.get('sample_id', record.get('graph_id'))}；不会生成模型告警")
            ranked = sorted(lines, key=lambda line: (-float(lines[line]), line)) if lines and localization else None
            result.append({"sample_id": record.get("sample_id", record.get("graph_id")),
                           "file": record.get("file"), "function": record.get("function"),
                           "probability": float(logit.sigmoid()), "ranked_lines": ranked,
                           "line_scores": {str(line): float(lines[line].sigmoid()) for line in ranked} if ranked else {}})
    return result


def _integer(value, low: int, high: int) -> bool:
    return type(value) is int and low <= value <= high


def _probability(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 1


def _metadata_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"检查点元数据包含重复字段：{key}")
        value[key] = item
    return value


def _reject_constant(value):
    raise ValueError(f"检查点元数据包含非有限数值：{value}")


def _validate_metadata(metadata: dict) -> None:
    """Validate before allocating the network or trusting its feature mapping."""
    if not isinstance(metadata, dict) or metadata.get("schema_version") != "1.0" or metadata.get("model_kind") != "defectguard-binary-detector":
        raise ValueError("检查点类型或版本不支持")
    network = metadata.get("network")
    if not isinstance(network, dict) or set(network) not in ({"mode", "vocabulary_size", "sequence_dim", "hidden"}, {"mode", "vocabulary_size", "sequence_dim", "hidden", "feature_version"}):
        raise ValueError("检查点 network 字段不完整或包含未知架构参数")
    if network["mode"] not in {"sequence", "gnn", "fusion"}:
        raise ValueError("检查点模型模式无效")
    if (not _integer(network["vocabulary_size"], 1, 100000) or
        not _integer(network["hidden"], 4, 1024) or
        type(network["sequence_dim"]) is not int or network["sequence_dim"] != 768):
        raise ValueError("检查点网络维度无效：词表 1..100000、隐藏层 4..1024、序列维度 768")
    vocabulary = metadata.get("vocabulary")
    if (not isinstance(vocabulary, dict) or len(vocabulary) != network["vocabulary_size"] or
        any(not isinstance(key, str) or not key or type(value) is not int for key, value in vocabulary.items()) or
        set(vocabulary.values()) != set(range(1, len(vocabulary) + 1))):
        raise ValueError("检查点词表必须与网络大小一致且映射到唯一连续编号 1..N（0 保留给未知类型）")
    if metadata.get("numeric_features") != feature_names(network.get("feature_version", "legacy-v1")):
        raise ValueError("节点特征版本不兼容")
    excluded = metadata.get("excluded_edges")
    if (not isinstance(excluded, list) or any(type(edge) is not int or edge not in range(4) for edge in excluded) or
        len(excluded) != len(set(excluded))):
        raise ValueError("检查点消融边类型必须是 0..3 的不重复整数")
    if not _probability(metadata.get("threshold")):
        raise ValueError("检查点模型阈值必须是 0 到 1 的有限数值")
    for field in ("weights_sha256", "dataset_sha256", "splits_sha256"):
        if not isinstance(metadata.get(field), str) or not re.fullmatch(r"[0-9a-f]{64}", metadata[field]):
            raise ValueError(f"检查点 {field} 必须是有效的 SHA-256")
    if network["mode"] != "gnn":
        encoding = metadata.get("encoding")
        if (not isinstance(encoding, dict) or not _integer(encoding.get("max_length"), 8, 512) or
            encoding.get("pooling") != "attention-masked-mean" or encoding.get("frozen") is not True or
            not isinstance(encoding.get("encoder_sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", encoding["encoder_sha256"])):
            raise ValueError("检查点编码器参数、指纹或特征提取约定无效")
        if not isinstance(metadata.get("encoder_directory"), str) or not metadata["encoder_directory"]:
            raise ValueError("检查点缺少本地编码器目录")


def _classification_loss(model, records, graphs):
    return F.binary_cross_entropy_with_logits(torch.stack([model(graph)[0] for graph in graphs]),
                                              torch.tensor([record["label"] for record in records], dtype=torch.float32))


def _graphs(records, vocabulary, sequences, excluded_edges, feature_version="legacy-v1"):
    graphs = []
    for record, sequence in zip(records, sequences):
        try:
            graphs.append(tensor_graph(record, vocabulary, sequence, excluded_edges, feature_version))
        except (KeyError, TypeError, ValueError, RuntimeError, OverflowError) as error:
            raise ValueError(
                f"程序图无法用于模型：{record.get('file')}:{record.get('start_line')} "
                f"{record.get('function')} ({record.get('sample_id', record.get('graph_id'))})：{error}") from error
    return graphs


def train_experiment(dataset: Path, output: Path, *, mode: str = "fusion", encoder: Path | None = None,
                     epochs: int = 40, seed: int = 42, learning_rate: float = 0.003,
                     max_length: int = 256, hidden: int = 48, excluded_edges: tuple[int, ...] = (),
                     threshold: float = 0.5, cache: Path | None = None,
                     feature_version: str = "legacy-v1", localization_weight: float | None = None) -> dict:
    feature_names(feature_version)
    if localization_weight is not None and (type(localization_weight) not in {int, float} or not math.isfinite(localization_weight) or not 0 <= localization_weight <= 1):
        raise ValueError("定位损失权重必须在 0..1 内")
    if (not _integer(epochs, 1, 1000000) or not _integer(hidden, 4, 1024) or
        type(learning_rate) not in (float, int) or not math.isfinite(learning_rate) or not 0 < learning_rate <= 1 or
        not _probability(threshold) or not _integer(max_length, 8, 512) or not _integer(seed, 0, 2**63 - 1)):
        raise ValueError("训练轮数、维度、学习率或阈值无效")
    if mode not in {"sequence", "gnn", "fusion"} or any(type(edge) is not int or edge not in range(4) for edge in excluded_edges) or len(excluded_edges) != len(set(excluded_edges)):
        raise ValueError("模型模式或消融边类型无效")
    if output.resolve().is_relative_to(dataset.resolve()) or dataset.resolve().is_relative_to(output.resolve()):
        raise ValueError("实验输出和数据集目录必须彼此独立")
    if output.exists() and any(output.iterdir()):
        raise ValueError("实验输出目录非空，请使用新目录以保留已有检查点")
    records, splits = load_dataset(dataset)
    _seed(seed)
    by_id = {record["sample_id"]: index for index, record in enumerate(records)}
    positions = {name: [by_id[key] for key in subset] for name, subset in splits["splits"].items()}
    train_records = [records[index] for index in positions["train"]]
    vocabulary = fit_vocabulary(train_records, feature_version)
    encoding = None
    sequence_dim = 768
    if mode != "gnn":
        if encoder is None:
            raise ValueError("sequence/fusion 需要本地 GraphCodeBERT --encoder 目录")
        print("读取固定编码器并提取序列特征（权重不参与训练）…", flush=True)
        frozen = FrozenEncoder(encoder, max_length)
        sequences, encoding = frozen.encode(records, cache)
        sequence_dim = frozen.dimension
        del frozen
    else:
        sequences = [torch.zeros(sequence_dim) for _ in records]
    graphs = _graphs(records, vocabulary, sequences, excluded_edges, feature_version)
    subsets = {name: ([records[index] for index in indices], [graphs[index] for index in indices])
               for name, indices in positions.items()}
    # Frozen encoder initialization must not change the trainable head's seed.
    _seed(seed)
    network_parameters = {"mode": mode, "vocabulary_size": len(vocabulary), "sequence_dim": sequence_dim, "hidden": hidden}
    if feature_version != "legacy-v1":
        network_parameters["feature_version"] = feature_version
    model = DetectorNetwork(**network_parameters)
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=0.01)
    train_records, train_graphs = subsets["train"]
    known_lines = [(record, graph) for record, graph in zip(train_records, train_graphs)
                   if record["label"] == 0 or record.get("defect_lines")]
    positive_lines = sum(line in record.get("defect_lines", []) for record, graph in known_lines for line in graph.line_nodes)
    # Unknown positive locations cannot support negative-only localization training.
    effective_localization_weight = (0.25 if localization_weight is None else localization_weight) if positive_lines and mode != "sequence" else 0.0
    negative_lines = sum(len(graph.line_nodes) for _, graph in known_lines) - positive_lines
    line_weight = torch.tensor(min(20.0, negative_lines / max(1, positive_lines)))
    history, best_loss, best_epoch, best_state = [], float("inf"), 0, None
    for epoch in range(1, epochs + 1):
        model.train()
        optimizer.zero_grad()
        logits, line_losses = [], []
        for record, graph in zip(train_records, train_graphs):
            logit, lines = model(graph)
            logits.append(logit)
            if effective_localization_weight and lines and (record["label"] == 0 or record.get("defect_lines")):
                target = torch.tensor([float(line in record.get("defect_lines", [])) for line in lines])
                line_losses.append(F.binary_cross_entropy_with_logits(torch.stack(list(lines.values())), target, pos_weight=line_weight))
        classification = F.binary_cross_entropy_with_logits(torch.stack(logits), torch.tensor([record["label"] for record in train_records], dtype=torch.float32))
        line_loss = torch.stack(line_losses).mean() if line_losses else classification.new_zeros(())
        loss = classification + effective_localization_weight * line_loss
        if not torch.isfinite(loss):
            raise ValueError("训练损失非有限数值，已终止")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
        optimizer.step()
        model.eval()
        with torch.no_grad():
            validation_loss = float(_classification_loss(model, *subsets["validation"]))
        if not torch.isfinite(torch.tensor(validation_loss)):
            raise ValueError("验证损失非有限数值，已终止")
        if validation_loss < best_loss:
            best_loss, best_epoch = validation_loss, epoch
            best_state = {key: value.detach().clone().contiguous() for key, value in model.state_dict().items()}
        history.append({"epoch": epoch, "train_loss": float(loss.detach()),
                        "train_classification_loss": float(classification.detach()),
                        "train_localization_loss": float(line_loss.detach()),
                        "validation_classification_loss": validation_loss})
        if epoch == 1 or epoch % 10 == 0 or epoch == epochs:
            print(f"{mode} 第 {epoch}/{epochs} 轮：训练损失 {float(loss.detach()):.4f}，验证损失 {validation_loss:.4f}", flush=True)
    assert best_state is not None
    model.load_state_dict(best_state)
    output.mkdir(parents=True, exist_ok=True)
    weights = output / "model.safetensors"
    save_file(best_state, str(weights))
    metadata = {
        "schema_version": "1.0", "model_kind": "defectguard-binary-detector", "network": network_parameters,
        "vocabulary": vocabulary, "numeric_features": feature_names(feature_version), "excluded_edges": list(excluded_edges),
        "seed": seed, "epochs": epochs, "learning_rate": learning_rate, "threshold": threshold,
        "selection": "minimum validation classification BCE; fixed threshold; test unused during selection",
        "best_epoch": best_epoch, "best_validation_loss": best_loss, "localization_loss_weight": effective_localization_weight,
        "localization_supervised_positive_nodes": positive_lines,
        "localization_positive_weight": float(line_weight),
        "encoder_directory": str(encoder.resolve()) if encoder and mode != "gnn" else None,
        "encoding": encoding, "dataset_sha256": splits["dataset_sha256"],
        "splits_sha256": hashlib.sha256((dataset / "splits.json").read_bytes()).hexdigest(),
        "weights_sha256": hashlib.sha256(weights.read_bytes()).hexdigest(),
        "runtime": {"python": sys.version.split()[0], "device": "cpu", "torch_threads": 2,
                    **{name: importlib.metadata.version(name) for name in ("torch", "torch-geometric", "transformers", "safetensors")}},
        "limitations": ["binary defect detection, no defect category head", "no original GraphCodeBERT graph-guided attention",
                        "AST-node line ranking, not semantic proof", "source groups only as trustworthy as supplied provenance"],
    }
    _write_json(output / "metadata.json", metadata)
    _write_json(output / "history.json", history)
    _write_json(output / "splits.json", splits)
    results = {}
    for name, (subset_records, subset_graphs) in subsets.items():
        predictions = _predictions(model, subset_records, subset_graphs, effective_localization_weight > 0)
        results[name] = evaluate_predictions(subset_records, predictions, threshold)
        _write_json(output / f"{name}-predictions.json", predictions)
    _write_json(output / "metrics.json", results)
    return {"output": str(output.resolve()), "mode": mode, "best_epoch": best_epoch, "metrics": results}


class TrainedPredictor:
    def __init__(self, checkpoint: Path, encoder_override: Path | None = None):
        self.checkpoint = checkpoint.resolve()
        self.last_encoding = None
        try:
            self.metadata = json.loads((checkpoint / "metadata.json").read_text(encoding="utf-8"),
                                       object_pairs_hook=_metadata_object, parse_constant=_reject_constant)
            metadata = self.metadata
            _validate_metadata(metadata)
            weights = checkpoint / "model.safetensors"
            if hashlib.sha256(weights.read_bytes()).hexdigest() != metadata["weights_sha256"]:
                raise ValueError("模型权重哈希不一致")
            self.model = DetectorNetwork(**metadata["network"])
            state = load_file(str(weights))
            expected = self.model.state_dict()
            if state.keys() != expected.keys():
                raise ValueError("检查点权重参数名与网络不一致")
            for name, tensor in state.items():
                if tensor.shape != expected[name].shape or tensor.dtype != expected[name].dtype:
                    raise ValueError(f"检查点权重形状或类型不兼容：{name}")
                if not torch.isfinite(tensor).all():
                    raise ValueError(f"检查点权重包含非有限值：{name}")
            self.model.load_state_dict(state, strict=True)
            self.model.eval()
            self.encoder = None
            if metadata["network"]["mode"] != "gnn":
                directory = encoder_override or Path(metadata["encoder_directory"])
                self.encoder = FrozenEncoder(directory, metadata["encoding"]["max_length"])
                if self.encoder.identity != metadata["encoding"]["encoder_sha256"]:
                    raise ValueError("推理编码器与训练编码器指纹不一致")
            torch.set_num_threads(2)
        except (KeyError, TypeError, RuntimeError, OSError, ValueError, SafetensorError) as error:
            raise ValueError(f"模型检查点不完整或不兼容：{error}") from error

    def predict(self, records: list[dict]) -> list[dict]:
        self.last_encoding = None
        if not records:
            return []
        seen = set()
        for record in records:
            try:
                validate_graph(record)
            except ValueError as error:
                raise ValueError(f"推理程序图无效：{record.get('file') if isinstance(record, dict) else '<非对象>'}：{error}") from error
            identity = record.get("sample_id", record.get("graph_id"))
            if not isinstance(identity, str) or not identity or identity in seen:
                raise ValueError("推理样本需要非空且唯一的 sample_id 或 graph_id")
            seen.add(identity)
        if self.encoder:
            sequences, self.last_encoding = self.encoder.encode(records)
        else:
            sequences, self.last_encoding = [torch.zeros(self.metadata["network"]["sequence_dim"]) for _ in records], None
        graphs = _graphs(records, self.metadata["vocabulary"], sequences, tuple(self.metadata["excluded_edges"]),
                         self.metadata["network"].get("feature_version", "legacy-v1"))
        return _predictions(self.model, records, graphs, self.metadata.get('localization_loss_weight', 0.25) > 0)


def evaluate_checkpoint(dataset: Path, checkpoint: Path, output: Path, encoder: Path | None = None) -> dict:
    records, splits = load_dataset(dataset)
    predictor = TrainedPredictor(checkpoint, encoder)
    if splits["dataset_sha256"] != predictor.metadata["dataset_sha256"] or hashlib.sha256((dataset / "splits.json").read_bytes()).hexdigest() != predictor.metadata["splits_sha256"]:
        raise ValueError("复评需要训练时的原始数据集和划分；不允许静默替换测试集")
    if output.resolve().is_relative_to(dataset.resolve()) or output.resolve().is_relative_to(checkpoint.resolve()):
        raise ValueError("复评输出不能位于数据集或检查点目录内")
    if output.exists() and any(output.iterdir()):
        raise ValueError("复评目录非空，请使用新目录")
    test = [record for record in records if record["sample_id"] in splits["splits"]["test"]]
    predictions = predictor.predict(test)
    metrics = evaluate_predictions(test, predictions, predictor.metadata["threshold"])
    output.mkdir(parents=True, exist_ok=True)
    _write_json(output / "predictions.json", predictions)
    _write_json(output / "metrics.json", metrics)
    return metrics
