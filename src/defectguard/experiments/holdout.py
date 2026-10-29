"""评分前冻结来源组；不加载模型、不拟合校准参数。"""
from __future__ import annotations

from collections import Counter, defaultdict
import argparse
import hashlib
import itertools
import json
from pathlib import Path

from .data import canonical_hash, read_jsonl
from .inference import _hash, _relative_file, load_graph_bundle
from .juliet import template_hash


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def plan_holdout(records: list[dict], seed: str) -> dict:
    """有界穷举完整组；两侧覆盖所有 CWE、两类标签及至少两个组。"""
    if not isinstance(seed, str) or not seed.strip() or not records:
        raise ValueError("需要非空候选和预先固定的 seed")
    groups, pairs, owners, ids = defaultdict(list), defaultdict(list), {}, set()
    for r in records:
        if (not isinstance(r, dict) or any(not isinstance(r.get(k), str) or not r[k].strip()
                for k in ("sample_id", "component", "source_file"))
                or type(r.get("label")) is not int or r["label"] not in (0, 1)
                or type(r.get("cwe")) is not int or r["cwe"] <= 0
                or r.get("eligible") is not True or r["component"].startswith("development:")
                or any(k in r for k in ("probability", "prediction", "score"))
                or any(not _hash(r.get(k)) for k in ("canonical_sha256", "template_sha256"))):
            raise ValueError("候选身份、标签、隔离资格或指纹无效")
        if r["sample_id"] in ids:
            raise ValueError("重复样例身份")
        ids.add(r["sample_id"])
        for key in ("source_file", "canonical_sha256", "template_sha256"):
            token = (key, r[key])
            if token in owners and owners[token] != r["component"]:
                raise ValueError("来源或近克隆指纹跨组；必须先合并完整关联组件")
            owners[token] = r["component"]
        groups[r["component"]].append(r)
        pairs[r["source_file"]].append(r)
    if any(sorted(x["label"] for x in v) != [0, 1] or len({x["cwe"] for x in v}) != 1
           for v in pairs.values()):
        raise ValueError("每个上游用例必须有同一 CWE 的完整正负对")
    if not 4 <= len(groups) <= 12:
        raise ValueError("需要 4 到 12 个来源组；更多组应采用单独评审的分区算法")
    all_cwes = {r["cwe"] for r in records}
    names = sorted(groups)
    choices = []
    for count in range(2, len(names) - 1):
        for subset in itertools.combinations(names, count):
            chosen = set(subset)
            left = [r for n in subset for r in groups[n]]
            right = [r for n in names if n not in chosen for r in groups[n]]
            if any({r["cwe"] for r in side} != all_cwes or {r["label"] for r in side} != {0, 1}
                   for side in (left, right)):
                continue
            tie = digest(json.dumps([seed, subset], ensure_ascii=True).encode())
            choices.append((abs(5 * len(left) - 2 * len(records)), tie, subset))
    if not choices:
        raise ValueError("无法在不拆组的前提下让校准和测试均覆盖全部 CWE")
    calibration = set(min(choices)[2])
    assignment = [{**r, "partition": "calibration" if r["component"] in calibration else "test"}
                  for r in sorted(records, key=lambda x: x["sample_id"])]
    summary = {}
    for split in ("calibration", "test"):
        side = [r for r in assignment if r["partition"] == split]
        summary[split] = dict(functions=len(side), groups=len({r["component"] for r in side}),
                              cwe_counts=dict(sorted(Counter(str(r["cwe"]) for r in side).items())),
                              label_counts=dict(sorted(Counter(str(r["label"]) for r in side).items())))
    return dict(schema_version=1, seed=seed, algorithm="whole-component-exhaustive-40-percent-v1",
                assignments=assignment, partitions=summary, model_scored=False, calibration_fitted=False,
                independent_accuracy_assessed=False, semantic_independence_proven=False,
                limitation="Juliet constructed labels; five-group cohort does not establish general C/C++ accuracy")


def freeze_cohort(audit_root: Path, output: Path, seed: str) -> dict:
    audit_root, output = audit_root.resolve(), output.resolve()
    if output == audit_root or output.is_relative_to(audit_root) or audit_root.is_relative_to(output):
        raise ValueError("输出目录必须与候选库分离")
    if output.exists():
        raise ValueError("输出必须为新目录，不覆盖已冻结分区")
    cohort = audit_root / "candidate-cohort"
    paths = [audit_root / "attempt2/candidate-audit.jsonl", cohort / "labels.jsonl",
             cohort / "provenance.jsonl", audit_root / "candidate-graphs/graphs.jsonl",
             audit_root / "candidate-graphs/manifest.json"]
    inputs = {str(p.relative_to(audit_root)): digest(p.read_bytes()) for p in paths}
    audit = read_jsonl(paths[0])
    selected = {}
    for r in audit:
        if r.get("eligible_under_current_exclusion_policy") is True:
            if r["source_file"] in selected or r.get("previous_source") is not False:
                raise ValueError("审计存在重复用例或既有开发暴露")
            selected[r["source_file"]] = r
    labels, provenance = read_jsonl(paths[1]), read_jsonl(paths[2])
    graphs, _, _ = load_graph_bundle(audit_root / "candidate-graphs")
    by_id = {r["sample_id"]: r for r in provenance}
    by_file = {r["file"]: r for r in graphs}
    if len(by_id) != len(provenance) or len(by_file) != len(graphs):
        raise ValueError("重复来源身份或函数文件")
    records, matched_ids, matched_files = [], set(), set()
    for label in labels:
        p = by_id[label["sample_id"]]
        if not _relative_file(p["file"]) or label["file"] != p["file"]:
            raise ValueError("生成文件路径不一致")
        source = (cohort / "corpus" / p["file"]).resolve()
        if not source.is_relative_to((cohort / "corpus").resolve()):
            raise ValueError("生成文件逃逸候选库")
        rel = str(source.relative_to(audit_root))
        inputs[rel] = digest(source.read_bytes())
        if inputs[rel] != p["generated_file_sha256"]:
            raise ValueError("生成源码已变化")
        graph, original = by_file[p["file"]], selected[p["source_file"]]
        functions = [x for x in original["functions"] if x["original_function"] == p["original_function"]]
        if len(functions) != 1:
            raise ValueError("原始函数无法唯一追踪")
        expected = functions[0]
        if (label["provenance"]["source_file"] != p["source_file"]
                or label["provenance"]["original_function"] != p["original_function"]
                or label["provenance"]["cwe"] != original["cwe"]
                or label["function"] != graph["function"]
                or p["original_sha256"] != original["original_sha256"]
                or type(label["label"]) is not int or label["label"] != expected["label"]
                or canonical_hash(graph["source"]) != expected["canonical_sha256"]
                or template_hash(graph["source"]) != expected["template_sha256"]):
            raise ValueError("标签、原始来源或函数图与审计不一致")
        if label["sample_id"] in matched_ids or p["file"] in matched_files:
            raise ValueError("重复标签")
        matched_ids.add(label["sample_id"]); matched_files.add(p["file"])
        records.append(dict(sample_id=label["sample_id"], graph_id=graph["graph_id"],
                            file=p["file"], source_file=p["source_file"], component=original["component"],
                            cwe=original["cwe"], label=label["label"], eligible=True,
                            canonical_sha256=expected["canonical_sha256"], template_sha256=expected["template_sha256"]))
    if matched_ids != set(by_id) or matched_files != set(by_file) or {r["source_file"] for r in records} != set(selected):
        raise ValueError("候选、标签、来源及图未完整一一对应")
    plan = plan_holdout(records, seed)
    for rel, value in inputs.items():
        if digest((audit_root / rel).read_bytes()) != value:
            raise ValueError("冻结期间输入发生变化")
    plan["inputs_sha256"] = inputs
    raw = (json.dumps(plan, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode()
    output.mkdir(parents=True, exist_ok=False)
    (output / "partition.json").write_bytes(raw)
    (output / "SHA256SUMS.txt").write_text(digest(raw) + "  partition.json\n", encoding="utf8")
    return plan


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seed", required=True)
    args = parser.parse_args()
    try:
        plan = freeze_cohort(args.audit_root, args.output, args.seed)
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(2, f"冻结拒绝: {error}\n")
    print(json.dumps(plan["partitions"], ensure_ascii=False))


if __name__ == "__main__":
    main()
