from __future__ import annotations

import math
from typing import Any


def evaluate_predictions(records: list[dict[str, Any]], predictions: list[dict[str, Any]],
                         threshold: float = 0.5, top_k: tuple[int, ...] = (1, 3, 5)) -> dict[str, Any]:
    if not 0 <= threshold <= 1 or not top_k or any(type(k) is not int or k <= 0 for k in top_k):
        raise ValueError("阈值或 Top-k 设置无效")
    expected = {record["sample_id"]: record for record in records}
    observed = {prediction["sample_id"]: prediction for prediction in predictions}
    if not records or len(expected) != len(records) or len(observed) != len(predictions) or set(expected) != set(observed):
        raise ValueError("预测必须与评估样例逐一匹配")
    tp = tn = fp = fn = eligible = 0
    hits = {str(k): 0 for k in top_k}
    gated_hits = dict(hits)
    localization_supported = any(item.get("ranked_lines") is not None for item in predictions)
    for sample_id, record in expected.items():
        prediction = observed[sample_id]
        probability = prediction["probability"]
        if not isinstance(probability, (int, float)) or not math.isfinite(probability) or not 0 <= probability <= 1:
            raise ValueError("预测概率必须位于 0 到 1，且为有限数值")
        actual, predicted = record["label"] == 1, probability >= threshold
        tp += actual and predicted
        tn += not actual and not predicted
        fp += not actual and predicted
        fn += actual and not predicted
        ranked = prediction.get("ranked_lines")
        if ranked is not None:
            if not isinstance(ranked, list) or any(type(line) is not int or not record["start_line"] <= line <= record["end_line"] for line in ranked):
                raise ValueError("预测定位超出函数范围")
            if len(ranked) != len(set(ranked)):
                raise ValueError("定位行不能重复")
        truth = set(record.get("defect_lines", []))
        if actual and truth:
            eligible += 1
            for k in top_k:
                hit = bool(truth & set((ranked or [])[:k]))
                hits[str(k)] += hit
                gated_hits[str(k)] += hit and predicted
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    return {
        "sample_count": len(records), "threshold": threshold,
        "confusion_matrix": {"tp": tp, "tn": tn, "fp": fp, "fn": fn},
        "precision": precision, "recall": recall,
        "f1": 2 * precision * recall / (precision + recall) if precision + recall else 0.0,
        "accuracy": (tp + tn) / len(records),
        "localization": {
            "supported": localization_supported, "eligible_positive_samples": eligible,
            "denominator": "all positively labeled samples with known defect lines, independent of classification",
            "top_k": {k: value / eligible if eligible and localization_supported else None for k, value in hits.items()},
            "end_to_end_top_k": {k: value / eligible if eligible and localization_supported else None for k, value in gated_hits.items()},
        },
    }
