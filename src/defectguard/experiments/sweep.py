"""固定数据划分的多模型、多种子实验；不把重复测试样本当独立观测。"""
from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics
import time
from typing import Callable

from .data import load_dataset
from .evaluation import evaluate_predictions


MODES = ("sequence", "gnn", "fusion")


def _json(path: Path, payload) -> None:
    text = json.dumps(payload, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    # Windows readers may briefly hold a non-share-delete handle on progress files.
    for attempt in range(5):
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if attempt == 4:
                raise
            time.sleep(0.05 * (attempt + 1))


def _read(path: Path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"JSON 重复字段：{key}")
            result[key] = value
        return result

    def constant(value):
        raise ValueError(f"JSON 包含非有限数值：{value}")

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("JSON 浮点数溢出")
        return number

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pairs,
                          parse_constant=constant, parse_float=finite_float)
    except (OSError, ValueError) as error:
        raise ValueError(f"实验产物无法读取：{path.name}：{error}") from error


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _stamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def _validate_plan(plan) -> None:
    if not isinstance(plan, dict) or plan.get("schema_version") != "1.0":
        raise ValueError("多种子计划必须是受支持版本的 JSON 对象")
    modes, seeds = plan.get("modes"), plan.get("seeds")
    if (not isinstance(modes, list) or not modes or any(mode not in MODES for mode in modes)
            or len(set(modes)) != len(modes)):
        raise ValueError("实验计划的模式无效")
    if (not isinstance(seeds, list) or len(seeds) < 2
            or any(type(seed) is not int or not 0 <= seed < 2**32 for seed in seeds)
            or len(set(seeds)) != len(seeds)):
        raise ValueError("实验计划的种子无效")
    for name in ("dataset_sha256", "splits_sha256"):
        value = plan.get(name)
        if not isinstance(value, str) or len(value) != 64 or any(char not in "0123456789abcdef" for char in value):
            raise ValueError("实验计划缺少有效数据指纹")
    for name in ("epochs", "hidden", "max_length", "split_seed"):
        if type(plan.get(name)) is not int:
            raise ValueError("实验计划的数值类型无效")
    if plan["epochs"] < 1 or plan["hidden"] < 4 or not 8 <= plan["max_length"] <= 512:
        raise ValueError("实验计划超参数无效")
    for name in ("learning_rate", "threshold"):
        if type(plan.get(name)) not in (float, int) or not math.isfinite(plan[name]) or not 0 <= plan[name] <= 1:
            raise ValueError("实验计划学习率或阈值无效")
    if plan["learning_rate"] == 0:
        raise ValueError("实验学习率必须大于零")
    if any(mode != "gnn" for mode in modes):
        value = plan.get("encoder_sha256")
        if not isinstance(value, str) or len(value) != 64 or any(char not in "0123456789abcdef" for char in value):
            raise ValueError("实验计划缺少有效编码器指纹")


def _overlap(left: Path, right: Path) -> bool:
    return left.resolve().is_relative_to(right.resolve()) or right.resolve().is_relative_to(left.resolve())


def describe_scores(values: list[float | None]) -> dict:
    supported = [value for value in values if value is not None]
    if any(type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1 for value in supported):
        raise ValueError("汇总分数必须为 0 到 1 的有限数值，或 null")
    if supported and len(supported) != len(values):
        raise ValueError("不能混合支持与不支持该指标的实验")
    return {"count": len(supported), "mean": statistics.mean(supported) if supported else None,
            "sample_std": statistics.stdev(supported) if len(supported) >= 2 else None,
            "min": min(supported) if supported else None, "max": max(supported) if supported else None,
            "values": values}


def _verify_run(directory: Path, mode: str, seed: int, plan: dict, records: list[dict], splits: dict) -> dict:
    metadata = _read(directory / "metadata.json")
    if not isinstance(metadata, dict):
        raise ValueError("检查点元数据必须为对象")
    checks = {"seed": seed, "epochs": plan["epochs"], "learning_rate": plan["learning_rate"],
              "threshold": plan["threshold"], "dataset_sha256": plan["dataset_sha256"],
              "splits_sha256": plan["splits_sha256"], "excluded_edges": []}
    if any(type(metadata.get(key)) is not type(value) or metadata.get(key) != value for key, value in checks.items()):
        raise ValueError(f"{directory.name} 与预先登记的实验参数不一致")
    if not isinstance(metadata.get("network"), dict) or metadata["network"].get("mode") != mode or metadata["network"].get("hidden") != plan["hidden"]:
        raise ValueError("实验模式或网络维度与计划不一致")
    if mode != "gnn" and (not isinstance(metadata.get("encoding"), dict) or metadata["encoding"].get("encoder_sha256") != plan["encoder_sha256"]
                           or metadata["encoding"].get("max_length") != plan["max_length"]):
        raise ValueError("编码器或截断长度与计划不一致")
    if _digest(directory / "model.safetensors") != metadata.get("weights_sha256"):
        raise ValueError("实验权重哈希不一致")
    if _read(directory / "splits.json") != splits:
        raise ValueError("检查点保存的划分不是本次固定划分")
    saved = _read(directory / "metrics.json")
    if type(metadata.get("best_epoch")) is not int or not 1 <= metadata["best_epoch"] <= plan["epochs"]:
        raise ValueError("检查点最优轮次无效")
    if not isinstance(saved, dict) or set(saved) != {"train", "validation", "test"}:
        raise ValueError("实验指标缺少训练、验证或测试集合")
    predictions = {}
    for name, identifiers in splits["splits"].items():
        selected = [record for record in records if record["sample_id"] in identifiers]
        predictions[name] = _read(directory / f"{name}-predictions.json")
        try:
            actual = evaluate_predictions(selected, predictions[name], plan["threshold"])
        except (KeyError, TypeError) as error:
            raise ValueError("实验预测结构不完整") from error
        if actual != saved[name]:
            raise ValueError(f"{mode}/{seed} 的 {name} 预测与保存指标不一致")
    return {"mode": mode, "seed": seed, "best_epoch": metadata["best_epoch"], "test": saved["test"],
            "predictions": predictions["test"], "weights_sha256": metadata["weights_sha256"]}


def aggregate_runs(runs: list[dict], test_records: list[dict]) -> dict:
    if not runs or not test_records:
        raise ValueError("多种子汇总需要非空实验和测试集合")
    seen = set()
    modes = {}
    for run in runs:
        key = (run["mode"], run["seed"])
        if key in seen:
            raise ValueError("同一模型/种子被重复计入")
        seen.add(key)
        expected = evaluate_predictions(test_records, run["predictions"], run["test"]["threshold"])
        if expected != run["test"]:
            raise ValueError("汇总输入指标与预测不一致")
        modes.setdefault(run["mode"], []).append(run)
    summaries, errors = {}, {}
    for mode, selected in modes.items():
        selected.sort(key=lambda item: item["seed"])
        thresholds = {item["test"]["threshold"] for item in selected}
        if len(thresholds) != 1:
            raise ValueError("同一模型的各随机种子必须采用相同阈值")
        threshold = next(iter(thresholds))
        summaries[mode] = {"seeds": [run["seed"] for run in selected],
                           "best_epochs": [run["best_epoch"] for run in selected],
                           "classification": {key: describe_scores([run["test"][key] for run in selected])
                                              for key in ("precision", "recall", "f1", "accuracy")},
                           "localization": {kind: {k: describe_scores([run["test"]["localization"][kind][k] for run in selected])
                                                   for k in ("1", "3", "5")}
                                            for kind in ("top_k", "end_to_end_top_k")}}
        by_run = [{item["sample_id"]: item for item in run["predictions"]} for run in selected]
        cases = []
        for record in test_records:
            scores = [predictions[record["sample_id"]]["probability"] for predictions in by_run]
            predicted = [int(score >= threshold) for score in scores]
            fp = sum(record["label"] == 0 and label == 1 for label in predicted)
            fn = sum(record["label"] == 1 and label == 0 for label in predicted)
            cases.append({"sample_id": record["sample_id"], "file": record.get("file", ""),
                          "function": record.get("function", ""), "group_id": record["group_id"],
                          "label": record["label"], "seeds": summaries[mode]["seeds"], "probabilities": scores,
                          "false_positive_runs": fp, "false_negative_runs": fn, "error_runs": fp + fn,
                          "total_runs": len(selected), "defect_lines": record.get("defect_lines", [])})
        errors[mode] = sorted(cases, key=lambda case: (-case["error_runs"], case["sample_id"]))
    return {"models": summaries, "cases": errors, "test_sample_count": len(test_records),
            "test_group_count": len({item.get("split_group", item["group_id"]) for item in test_records}),
            "test_class_counts": dict(Counter(str(item["label"]) for item in test_records)),
            "interpretation": "Fixed test examples reused across model seeds; std measures initialization sensitivity, not confidence intervals or independent new samples."}


def summarize_sweep(dataset: Path, output: Path) -> dict:
    records, splits = load_dataset(dataset)
    plan = _read(output / "plan.json")
    _validate_plan(plan)
    if plan.get("dataset_sha256") != splits["dataset_sha256"] or plan.get("splits_sha256") != _digest(dataset / "splits.json"):
        raise ValueError("汇总数据集或划分与预登记计划不一致")
    runs = [_verify_run(output / f"{mode}-seed{seed}", mode, seed, plan, records, splits)
            for mode in plan["modes"] for seed in plan["seeds"]]
    test_records = [record for record in records if record["sample_id"] in splits["splits"]["test"]]
    summary = aggregate_runs(runs, test_records)
    summary.update(schema_version="1.0", status="complete", dataset_sha256=plan["dataset_sha256"],
                   splits_sha256=plan["splits_sha256"], split_seed=splits["seed"],
                   threshold=plan["threshold"], run_count=len(runs))
    cases = summary.pop("cases")
    _json(output / "summary.json", summary)
    _json(output / "cases.json", cases)
    text = ["# 固定划分的多种子模型实验", "", f"测试集：{len(test_records)} 个函数，{summary['test_group_count']} 个来源组；共 {len(runs)} 次训练。",
            "模型初始化种子改变，数据划分、阈值和训练超参数固定。每次仍仅按验证集选训练轮次。",
            "本表为均值 ± 样本标准差；不是置信区间，也未把同一测试样本的多次预测当作新增样本。",
            "现有受控小样本不代表真实工程性能，不能据测试结果挑选种子或发布最佳模型。", "",
            "| 模型 | 种子 | Precision | Recall | F1 | Accuracy | Top-1 | 端到端 Top-1 |",
            "|---|---|---:|---:|---:|---:|---:|---:|"]
    def formatted(stat):
        if stat["mean"] is None:
            return "不支持"
        spread = "无法估计" if stat["sample_std"] is None else f"{stat['sample_std']:.3f}"
        return f"{stat['mean']:.3f} ± {spread}"
    for mode, model in summary["models"].items():
        values = [formatted(model["classification"][key]) for key in ("precision", "recall", "f1", "accuracy")]
        values.extend(formatted(model["localization"][kind]["1"]) for kind in ("top_k", "end_to_end_top_k"))
        text.append(f"| {mode} | {', '.join(map(str, model['seeds']))} | " + " | ".join(values) + " |")
    for mode, records_with_errors in cases.items():
        text.extend(["", f"## {mode} 逐样例错误复核", "", "| 样例 | 来源组 | 标签 | 误报轮次 | 漏报轮次 | 总轮次 |",
                     "|---|---|---:|---:|---:|---:|"])
        for case in records_with_errors:
            # Identifiers are external text, so neutralize Markdown table syntax.
            identifier = case["sample_id"].replace("|", "\\|").replace("\n", " ")
            group = case["group_id"].replace("|", "\\|").replace("\n", " ")
            text.append(f"| {identifier} | {group} | {case['label']} | {case['false_positive_runs']} | {case['false_negative_runs']} | {case['total_runs']} |")
    (output / "summary.md").write_text("\n".join(text) + "\n", encoding="utf-8", newline="\n")
    return summary


def run_sweep(dataset: Path, output: Path, *, modes: tuple[str, ...] = MODES, seeds: tuple[int, ...] = (42, 43, 44),
              encoder: Path | None = None, epochs: int = 40, max_length: int = 256,
              cache: Path | None = None, resume: bool = False,
              trainer: Callable | None = None, encoder_fingerprint: Callable | None = None) -> dict:
    # Dependency injection keeps orchestration tests offline and lightweight.
    if not modes or any(mode not in MODES for mode in modes) or len(set(modes)) != len(modes):
        raise ValueError("实验模式必须为不重复的 sequence、gnn、fusion")
    if len(seeds) < 2 or any(type(seed) is not int or not 0 <= seed < 2**32 for seed in seeds) or len(set(seeds)) != len(seeds):
        raise ValueError("多种子实验至少需要两个不重复的 0 至 2^32-1 整数种子")
    if type(epochs) is not int or epochs < 1 or type(max_length) is not int or not 8 <= max_length <= 512:
        raise ValueError("训练轮数或序列长度无效")
    if _overlap(dataset, output) or (encoder is not None and _overlap(encoder, output)):
        raise ValueError("实验输出必须与输入数据集、编码器目录相互独立")
    if cache is not None and (_overlap(cache, dataset) or _overlap(cache, output) or (encoder is not None and _overlap(cache, encoder))):
        raise ValueError("特征缓存必须与数据集、编码器和实验输出目录相互独立")
    if any(mode != "gnn" for mode in modes) and encoder is None:
        raise ValueError("sequence/fusion 实验需要本地 --encoder")
    records, splits = load_dataset(dataset)
    if output.exists() and (not output.is_dir() or any(output.iterdir())) and not resume:
        raise ValueError("输出目录非空，请使用新目录；继续未完成计划须显式 --resume")
    fingerprint = None
    if any(mode != "gnn" for mode in modes):
        if encoder_fingerprint is None:
            from .encoder import encoder_identity
            encoder_fingerprint = encoder_identity
        fingerprint = encoder_fingerprint(encoder)
    plan = {"schema_version": "1.0", "purpose": "fixed-split initialization sensitivity, no test-based selection",
            "dataset_sha256": splits["dataset_sha256"], "splits_sha256": _digest(dataset / "splits.json"),
            "split_seed": splits["seed"], "modes": list(modes), "seeds": list(seeds),
            "epochs": epochs, "max_length": max_length, "hidden": 48, "learning_rate": 0.003,
            "threshold": 0.5, "encoder_sha256": fingerprint}
    if resume:
        saved_plan = _read(output / "plan.json")
        _validate_plan(saved_plan)
        if saved_plan != plan:
            raise ValueError("恢复参数与原计划不同；不能改变数据划分、种子、权重或超参数")
    else:
        output.mkdir(parents=True, exist_ok=True)
        _json(output / "plan.json", plan)
    if trainer is None:
        from .runner import train_experiment
        trainer = train_experiment
    state = {"status": "running", "started_at": _stamp(), "runs": []}
    _json(output / "progress.json", state)
    for mode in modes:
        for seed in seeds:
            directory = output / f"{mode}-seed{seed}"
            item = {"mode": mode, "seed": seed, "status": "running", "started_at": _stamp()}
            state["runs"].append(item)
            _json(output / "progress.json", state)
            print(f"多种子实验：{mode} / seed={seed}", flush=True)
            try:
                if directory.exists() and any(directory.iterdir()):
                    if not resume:
                        raise ValueError("实验子目录已存在，拒绝覆盖")
                    _verify_run(directory, mode, seed, plan, records, splits)
                    item["reused"] = True
                else:
                    trainer(dataset, directory, mode=mode, seed=seed, encoder=encoder, epochs=epochs,
                            max_length=max_length, hidden=48, learning_rate=0.003, threshold=0.5, cache=cache)
                    _verify_run(directory, mode, seed, plan, records, splits)
                    item["reused"] = False
                item.update(status="complete", finished_at=_stamp())
                _json(output / "progress.json", state)
            except Exception as error:
                item.update(status="failed", error=str(error), finished_at=_stamp())
                state.update(status="failed", finished_at=_stamp())
                _json(output / "progress.json", state)
                raise ValueError(f"多种子实验失败：{mode}/{seed}：{error}；已保留产物，不自动覆盖不完整检查点") from error
    try:
        summary = summarize_sweep(dataset, output)
    except Exception as error:
        state.update(status="failed", error=str(error), finished_at=_stamp())
        _json(output / "progress.json", state)
        raise ValueError(f"实验汇总失败，已保留所有运行记录：{error}") from error
    state.update(status="complete", finished_at=_stamp())
    _json(output / "progress.json", state)
    return summary
