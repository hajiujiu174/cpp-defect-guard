from __future__ import annotations

from dataclasses import dataclass
import math
import re

import torch
from torch import nn
from torch_geometric.nn import RGCNConv


NUMERIC_FEATURES = ["is_ast", "is_cfg", "relative_line", "relative_span", "pointer_type", "array_type"]
SEMANTIC_FEATURES = NUMERIC_FEATURES + ["literal_present", "literal_signed_log", "array_extent_log", "binding_id"]
ARCHITECTURES = ('concat-v1', 'regularized-v2')


def feature_names(version):
    if version not in {"legacy-v1", "semantic-v2"}:
        raise ValueError("未知图特征版本")
    return SEMANTIC_FEATURES if version == "semantic-v2" else NUMERIC_FEATURES


def node_kind(node, version):
    kind = node["kind"]
    if version == "semantic-v2" and kind in {"BinaryOperator", "CompoundAssignOperator", "UnaryOperator"}:
        return kind + ":" + node.get("name", "")
    return kind


def fit_vocabulary(records: list[dict], feature_version: str = "legacy-v1") -> dict[str, int]:
    # Train only. Identifiers, paths, labels and rule findings are deliberately excluded.
    feature_names(feature_version)
    kinds = sorted({node_kind(node, feature_version) for record in records for node in record["nodes"]})
    return {kind: index + 1 for index, kind in enumerate(kinds)}  # 0 = unseen kind


@dataclass
class TensorGraph:
    kinds: torch.Tensor
    numeric: torch.Tensor
    edge_index: torch.Tensor
    edge_type: torch.Tensor
    line_nodes: dict[int, torch.Tensor]
    sequence: torch.Tensor


def tensor_graph(record: dict, vocabulary: dict[str, int], sequence: torch.Tensor,
                 excluded_edges: tuple[int, ...] = (), feature_version: str = "legacy-v1") -> TensorGraph:
    feature_names(feature_version)
    original_nodes = record["nodes"]
    retained = [index for index, node in enumerate(original_nodes)
                if not ({1, 2}.issubset(excluded_edges) and node["layer"] == "cfg")]
    mapping = {old: new for new, old in enumerate(retained)}
    nodes = [original_nodes[index] for index in retained]
    if not nodes or len(nodes) > 20000:
        raise ValueError("当前实验后端支持 1 至 20000 个图节点；不会静默截断程序图")
    start, end = record["start_line"], record["end_line"]
    span = max(1, end - start)
    numeric, line_nodes = [], {}
    for index, node in enumerate(nodes):
        ast = node["layer"] == "ast"
        numeric.append([float(ast), float(not ast), max(0.0, min(1.0, (node["line"] - start) / span)),
                        max(0.0, min(1.0, (node["end_line"] - node["line"]) / span)),
                        float("*" in node.get("type_name", "")), float("[" in node.get("type_name", ""))])
        if feature_version == "semantic-v2":
            literal = node["kind"] in {"IntegerLiteral", "CharacterLiteral"}
            value = node.get("name", "")
            if literal and not re.fullmatch(r"-?\d{1,160}", value):
                raise ValueError("semantic-v2 需要带常量值的新原生图，请重新构图")
            number = int(value) if literal else 0
            extent = re.search(r"\[(\d{1,160})\]", node.get("type_name", ""))
            binding = re.fullmatch(r"V(\d+)", node.get("symbol", ""))
            numeric[-1].extend([float(literal), math.copysign(math.log1p(abs(number)), number) / 10,
                                math.log1p(int(extent[1])) / 10 if extent else 0.0,
                                (int(binding[1]) + 1) / len(nodes) if binding else 0.0])
        if ast and start <= node["line"] <= end:
            line_nodes.setdefault(node["line"], []).append(index)
    edges, types = [], []
    for left, right, kind in zip(*record["edge_index"], record["edge_type"]):
        if kind not in excluded_edges and left in mapping and right in mapping:
            left, right = mapping[left], mapping[right]
            edges.extend([(left, right), (right, left)])
            types.extend([kind, kind + 4])
    return TensorGraph(
        kinds=torch.tensor([vocabulary.get(node_kind(node, feature_version), 0) for node in nodes], dtype=torch.long),
        numeric=torch.tensor(numeric, dtype=torch.float32),
        edge_index=torch.tensor(edges, dtype=torch.long).t().contiguous() if edges else torch.empty((2, 0), dtype=torch.long),
        edge_type=torch.tensor(types, dtype=torch.long),
        line_nodes={line: torch.tensor(indices, dtype=torch.long) for line, indices in sorted(line_nodes.items())},
        sequence=sequence,
    )


class DetectorNetwork(nn.Module):
    def __init__(self, mode: str, vocabulary_size: int, sequence_dim: int = 768, hidden: int = 48, feature_version: str = "legacy-v1", architecture: str = 'concat-v1'):
        super().__init__()
        if mode not in {"sequence", "gnn", "fusion"}:
            raise ValueError("模型模式必须是 sequence、gnn 或 fusion")
        self.mode = mode
        if architecture not in ARCHITECTURES:
            raise ValueError('未知模型架构')
        self.architecture = architecture
        if mode != "gnn":
            self.sequence_projection = nn.Sequential(nn.LayerNorm(sequence_dim), nn.Linear(sequence_dim, hidden), nn.ReLU())
        if mode != "sequence":
            self.kind_embedding = nn.Embedding(vocabulary_size + 1, 16)
            self.input_projection = nn.Linear(16 + len(feature_names(feature_version)), hidden)
            self.conv1 = RGCNConv(hidden, hidden, num_relations=8, num_bases=4)
            self.conv2 = RGCNConv(hidden, hidden, num_relations=8, num_bases=4)
            self.line_head = nn.Linear(hidden, 1)
        self.classifier = nn.Sequential(nn.Linear(hidden * (2 if mode == "fusion" else 1), hidden),
                                        nn.ReLU(), nn.Linear(hidden, 1))
        if architecture == 'regularized-v2':
            del self.classifier
            self.dropout = nn.Dropout(0.2)
            if mode != 'gnn':
                self.register_buffer('sequence_center', torch.zeros(sequence_dim))
                self.register_buffer('sequence_scale', torch.ones(sequence_dim))
                self.sequence_projection = nn.Sequential(nn.Linear(sequence_dim, hidden), nn.Tanh(), nn.Dropout(0.2))
                self.sequence_head = nn.Linear(hidden, 1)
            if mode != 'sequence':
                self.graph_norm = nn.LayerNorm(hidden)
                self.graph_head = nn.Linear(hidden, 1)
            if mode == 'fusion':
                self.fusion_gate = nn.Parameter(torch.zeros(()))

    def fit_sequence_normalizer(self, train_vectors):
        if self.architecture != 'regularized-v2' or self.mode == 'gnn':
            return
        values = torch.stack(train_vectors)
        if values.ndim != 2 or values.shape[1] != self.sequence_center.numel() or not torch.isfinite(values).all():
            raise ValueError('训练集序列特征无效')
        with torch.no_grad():
            self.sequence_center.copy_(values.mean(0))
            self.sequence_scale.copy_(values.std(0, unbiased=False).clamp_min(0.05))

    def forward(self, graph: TensorGraph) -> tuple[torch.Tensor, dict[int, torch.Tensor]]:
        score, lines, _ = self.forward_branches(graph)
        return score, lines

    def forward_branches(self, graph: TensorGraph):
        parts, lines = [], {}
        regularized = self.architecture == 'regularized-v2'
        if self.mode != "gnn":
            sequence = ((graph.sequence - self.sequence_center) / self.sequence_scale).clamp(-5, 5) if regularized else graph.sequence
            parts.append(self.sequence_projection(sequence))
        if self.mode != "sequence":
            x = torch.relu(self.input_projection(torch.cat([self.kind_embedding(graph.kinds), graph.numeric], dim=1)))
            x = torch.relu(self.conv1(x, graph.edge_index, graph.edge_type))
            if regularized:
                x = self.dropout(x)
            x = torch.relu(self.conv2(x, graph.edge_index, graph.edge_type))
            parts.append(self.dropout(self.graph_norm(0.5 * (x.mean(dim=0) + x.amax(dim=0)))) if regularized else x.mean(dim=0))
            node_logits = self.line_head(x).squeeze(-1)
            lines = {line: node_logits[indices].mean() for line, indices in graph.line_nodes.items()}
        if not regularized:
            return self.classifier(torch.cat(parts)).squeeze(-1), lines, []
        logits = []
        if self.mode != 'gnn':
            logits.append(self.sequence_head(parts[0]).squeeze(-1))
        if self.mode != 'sequence':
            logits.append(self.graph_head(parts[-1]).squeeze(-1))
        gate = self.fusion_gate.sigmoid() if self.mode == 'fusion' else None
        score = gate * logits[0] + (1 - gate) * logits[1] if gate is not None else logits[0]
        return score, lines, logits if self.mode == 'fusion' else []
