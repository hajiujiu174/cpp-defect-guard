from __future__ import annotations

from pathlib import Path
import math

from defectguard.domain import Finding, ParsedFile, Severity, SourceLocation
from defectguard.experiments.runner import TrainedPredictor
from defectguard.graphs import function_record


class TrainedDetectorBackend:
    name = "trained"

    def __init__(self, checkpoint: Path, threshold: float | None = None, encoder: Path | None = None):
        self.predictor = TrainedPredictor(checkpoint, encoder)
        self.threshold = self.predictor.metadata["threshold"] if threshold is None else threshold
        if type(self.threshold) not in (int, float) or not math.isfinite(self.threshold) or not 0 <= self.threshold <= 1:
            raise ValueError("模型阈值必须位于 0 到 1")
        self.function_count, self.skipped_files, self.truncated_count = 0, [], 0
        self.coverage_details: dict = {"skipped_files": [], "skipped_functions": [], "truncated_functions": []}

    def detect(self, parsed_files: list[ParsedFile]) -> list[Finding]:
        self.function_count, self.skipped_files, self.truncated_count = 0, [], 0
        self.coverage_details = {"skipped_files": [], "skipped_functions": [], "truncated_functions": []}
        if parsed_files and not any(file.backend == "clang-libtooling" for file in parsed_files):
            raise ValueError("训练模型推理需要 Clang AST/CFG；不会把词法近似结果充当程序图")
        records = []
        for file in parsed_files:
            if file.backend != "clang-libtooling" or not file.functions:
                self.skipped_files.append(file.relative_path)
                self.coverage_details["skipped_files"].append({
                    "file": file.relative_path,
                    "reason": "缺少 Clang 原生程序图" if file.backend != "clang-libtooling" else "没有可分析的函数定义"})
                continue
            raw = file.path.read_bytes()
            for function in file.functions:
                if not function.ast_nodes:
                    self.coverage_details["skipped_functions"].append({
                        "file": file.relative_path, "function": function.name,
                        "start_line": function.start_line, "end_line": function.end_line, "reason": "函数缺少 AST 节点"})
                    continue
                records.append(function_record(file.relative_path, function, raw))
        predictions = self.predictor.predict(records)
        if len(predictions) != len(records):
            raise ValueError("模型预测数量与函数图不一致，不能生成不完整告警")
        self.function_count = len(records)
        self.truncated_count = (self.predictor.last_encoding or {}).get("truncated_functions", 0) if records else 0
        self.coverage_details["truncated_functions"] = [
            detail for detail in (self.predictor.last_encoding or {}).get("functions", []) if detail["truncated"]]
        findings = []
        mode = self.predictor.metadata["network"]["mode"]
        for record, prediction in zip(records, predictions):
            if prediction.get("sample_id") != record["graph_id"]:
                raise ValueError("模型预测与函数图标识不匹配")
            probability = prediction.get("probability")
            if type(probability) not in (int, float) or not math.isfinite(probability) or not 0 <= probability <= 1:
                raise ValueError(f"模型预测概率无效：{record['file']}:{record['function']}；不会生成告警")
            if prediction["probability"] < self.threshold:
                continue
            ranked = prediction["ranked_lines"]
            line = ranked[0] if ranked else record["start_line"]
            evidence = record["source"].splitlines()
            findings.append(Finding(
                rule_id="ML001", defect_type="model_suspected_defect", severity=Severity.MEDIUM,
                location=SourceLocation(record["file"], line),
                message=f"模型将函数 {record['function']} 判为疑似缺陷；二分类结果不代表已确认缺陷类型。",
                evidence=evidence[line - record["start_line"]] if 0 <= line - record["start_line"] < len(evidence) else "",
                suggestion="人工复核控制流、资源生命周期及测试；模型排名行仅为候选位置。" if ranked else "该检查点未提供经训练的行定位；此处显示函数入口，请人工复核。",
                detector=f"model-{mode}", confidence=prediction["probability"],
            ))
        return findings

    def diagnostics(self) -> list[str]:
        messages = []
        for detail in self.coverage_details["skipped_files"]:
            messages.append(f"模型未覆盖文件 {detail['file']}：{detail['reason']}；不能将其视为模型检查通过。")
        for detail in self.coverage_details["skipped_functions"]:
            messages.append(f"模型未覆盖函数 {detail['file']}:{detail['start_line']} {detail['function']}：{detail['reason']}。")
        for detail in self.coverage_details["truncated_functions"]:
            messages.append(
                f"模型序列截断 {detail.get('file')}:{detail.get('start_line')} {detail.get('function')}："
                f"{detail['token_count']} 个 token，截断后保留 {detail['retained_token_count']} 个（含特殊符号）；"
                "函数尾部未进入序列编码器，图分支是否存在取决于模型模式。")
        return messages

    def status(self) -> str:
        return (f"trained:{self.predictor.metadata['network']['mode']}; functions={self.function_count}; "
                f"threshold={self.threshold}; skipped_files={len(self.skipped_files)}; truncated_sequences={self.truncated_count}; "
                f"skipped_functions={len(self.coverage_details['skipped_functions'])}; "
                "experimental binary classifier; confidence is not calibrated")
