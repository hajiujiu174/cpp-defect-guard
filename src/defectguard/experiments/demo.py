"""生成演示汇总和可直接扫描的 TOML；指标只读取已完成实验产物。"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
from collections import Counter

from .data import load_dataset


def summarize(root: Path, encoder: Path, native_tool: Path) -> Path:
    records, splits = load_dataset(root / "dataset")
    rows = []
    for directory in sorted(root.iterdir()):
        if not (directory / "metadata.json").is_file():
            continue
        metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
        if metadata["dataset_sha256"] != splits["dataset_sha256"]:
            raise ValueError("不能比较不同数据集上的实验")
        if metadata["splits_sha256"] != hashlib.sha256((root / "dataset" / "splits.json").read_bytes()).hexdigest():
            raise ValueError("不能比较不同划分上的实验")
        metrics = json.loads((directory / "metrics.json").read_text(encoding="utf-8"))["test"]
        rows.append({"experiment": directory.name, "best_epoch": metadata["best_epoch"], **metrics})
    if not rows or not (root / "fusion" / "model.safetensors").is_file():
        raise ValueError("缺少完成的融合模型实验")
    payload = {"purpose": "controlled teaching demo, not a real-project benchmark", "sample_count": len(records),
               "class_counts": splits["class_counts"], "groups": splits["groups"], "experiments": rows}
    (root / "comparison.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    text = ["# 第二层演示实验结果", "", "本结果仅验证流程；24 个自建函数不足以衡量真实工程泛化性能。",
            "验证集选择最优轮次，阈值固定 0.5；不根据测试结果选择模型或调整阈值。", "",
            "| 实验 | 最优轮次 | Precision | Recall | F1 | Accuracy | Top-1 | 端到端 Top-1 |",
            "|---|---:|---:|---:|---:|---:|---:|---:|"]
    def percent(value):
        return "不支持" if value is None else f"{value:.1%}"
    for row in rows:
        scores = [percent(row[key]) for key in ("precision", "recall", "f1", "accuracy")]
        scores.extend(percent(row["localization"][key]["1"]) for key in ("top_k", "end_to_end_top_k"))
        text.append(f"| {row['experiment']} | {row['best_epoch']} | " + " | ".join(scores) + " |")
    text.extend(["", "Top-k 的分母是所有具有定位真值的测试正例（包括分类漏报），端到端 Top-k 还要求分类命中。",
                 "序列基线不提供行定位；不能将函数入口当作正确定位。详情参见各实验 metrics.json、test-predictions.json、history.json 和 metadata.json。", ""])
    (root / "comparison.md").write_text("\n".join(text), encoding="utf-8", newline="\n")
    # Absolute forward-slash paths keep the generated config independent of shell cwd.
    config = '\n'.join([
        '[project]', 'exclude = [".git", ".venv", ".venv-ml", "build", "artifacts", "dist"]',
        '', '[parser]', 'backend = "clang"', f'clang_tool = {json.dumps(native_tool.resolve().as_posix(), ensure_ascii=False)}',
        'compilation_database = "build/compile_commands.json"', '', '[models]', 'backend = "trained"',
        f'checkpoint = {json.dumps((root / "fusion").resolve().as_posix(), ensure_ascii=False)}',
        f'encoder = {json.dumps(encoder.resolve().as_posix(), ensure_ascii=False)}', '', '[repair]', 'backend = "disabled"', ''])
    path = root / "scan-model.toml"
    path.write_text(config, encoding="utf-8", newline="\n")
    return path


def audit(root: Path) -> dict:
    """验收真实检查点、扫描与存储产物，而不只检查命令返回码。"""
    def read(path):
        return json.loads((root / path).read_text(encoding="utf-8"))
    metrics = read("fusion/metrics.json")["test"]
    original = read("fusion/test-predictions.json")
    reloaded = read("reloaded-test/predictions.json")
    if original != reloaded or metrics != read("reloaded-test/metrics.json"):
        raise ValueError("检查点重载后的预测或指标不一致")
    report = read("model-scan/report.json")
    if not report["source_unchanged"] or report["parser_backend"] != "clang-libtooling":
        raise ValueError("模型扫描未满足原生解析或源码保护验收")
    predictions = original + read("fusion/train-predictions.json") + read("fusion/validation-predictions.json")
    # Controlled demo has exactly one function per file. Refuse ambiguous joins.
    if len({item["file"] for item in predictions}) != len(predictions):
        raise ValueError("演示验收需要每文件一个函数，不能只按路径匹配重载函数")
    threshold = read("fusion/metadata.json")["threshold"]
    expected = {item["file"]: item for item in predictions if item["probability"] >= threshold}
    actual = {item["location"]["file"]: item for item in report["findings"] if item["detector"] == "model-fusion"}
    if set(actual) != set(expected) or any(actual[key]["confidence"] != expected[key]["probability"]
                                          or actual[key]["location"]["line"] != expected[key]["ranked_lines"][0] for key in actual):
        raise ValueError("在线扫描与离线预测不一致")
    counts = Counter(item["detector"] for item in report["findings"])
    database = root / "model-scan" / "runs.sqlite3"
    with sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True) as connection:
        rows = dict(connection.execute("SELECT detector, COUNT(*) FROM findings WHERE run_id = ? GROUP BY detector", (report["run_id"],)))
    if dict(counts) != rows:
        raise ValueError("报告与 SQLite 告警数量不一致")
    model_count = counts.get("model-fusion", 0)
    for name in ("model-scan/report.md", "model-scan/report.html"):
        text = (root / name).read_text(encoding="utf-8")
        if text.count("model-fusion") < model_count or (model_count and "ML001" not in text):
            raise ValueError("可读报告缺少模型检测器标记")
    result = {"status": "passed", "checkpoint_reload_exact": True, "online_offline_predictions_exact": True,
              "sqlite_matches_report": True, "readable_reports_include_model": True, "source_unchanged": True,
              "finding_counts": dict(counts), "model_status": report["model_status"]}
    (root / "acceptance.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--encoder", type=Path)
    parser.add_argument("--native-tool", type=Path)
    parser.add_argument("--audit", action="store_true")
    args = parser.parse_args()
    if args.audit:
        print(json.dumps(audit(args.root), ensure_ascii=False, indent=2))
    else:
        if not args.encoder or not args.native_tool:
            parser.error("汇总需要 --encoder 和 --native-tool")
        print(summarize(args.root, args.encoder, args.native_tool))
