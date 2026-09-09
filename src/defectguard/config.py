from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import tomllib


@dataclass(frozen=True)
class AppConfig:
    project_name: str
    exclude: tuple[str, ...]
    enabled_rules: tuple[str, ...]
    minimum_severity: str
    max_file_bytes: int
    report_formats: tuple[str, ...]
    build_command: tuple[str, ...]
    test_command: tuple[str, ...]
    timeout_seconds: int
    parser_backend: str
    clang_tool: str
    compilation_database: str
    model_backend: str
    repair_backend: str
    model_checkpoint: str = ""
    model_encoder: str = ""
    model_threshold: float | None = None


DEFAULT_CONFIG = AppConfig(
    project_name="project",
    exclude=(".git", ".venv", ".venv-ml", "build", "dist", "artifacts", "third_party", "vendor"),
    enabled_rules=("DG001", "DG002", "DG003", "DG004", "DG005", "DG006", "DG007"),
    minimum_severity="low",
    max_file_bytes=1_048_576,
    report_formats=("json", "markdown", "html"),
    build_command=(),
    test_command=(),
    timeout_seconds=120,
    parser_backend="auto",
    clang_tool="",
    compilation_database="",
    model_backend="disabled",
    repair_backend="disabled",
)


def _section(data: dict[str, Any], name: str) -> dict[str, Any]:
    value = data.get(name, {})
    return value if isinstance(value, dict) else {}


def _string_tuple(value: Any, default: tuple[str, ...], key: str) -> tuple[str, ...]:
    if value is None:
        return default
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"{key} 必须是字符串数组")
    return tuple(value)


def load_config(path: Path | None) -> AppConfig:
    if path is None:
        return DEFAULT_CONFIG
    with path.open("rb") as stream:
        data = tomllib.load(stream)
    project = _section(data, "project")
    analysis = _section(data, "analysis")
    parser = _section(data, "parser")
    reporting = _section(data, "reporting")
    verification = _section(data, "verification")
    models = _section(data, "models")
    repair = _section(data, "repair")
    threshold = models.get("threshold")
    if "threshold" in models and (type(threshold) not in (int, float) or not 0 <= threshold <= 1):
        raise ValueError("models.threshold 必须是位于 0 到 1 的数值，不能是布尔值、字符串或数组")
    def model_path(key: str) -> str:
        value = models.get(key, "")
        if not isinstance(value, str):
            raise ValueError(f"models.{key} 必须为路径字符串")
        if not value:
            return ""
        candidate = Path(value)
        return str((candidate if candidate.is_absolute() else path.parent / candidate).resolve())
    config = AppConfig(
        project_name=str(project.get("name", DEFAULT_CONFIG.project_name)),
        exclude=_string_tuple(
            project.get("exclude"), DEFAULT_CONFIG.exclude, "project.exclude"
        ),
        enabled_rules=tuple(
            item.upper()
            for item in _string_tuple(
                analysis.get("enabled_rules"),
                DEFAULT_CONFIG.enabled_rules,
                "analysis.enabled_rules",
            )
        ),
        minimum_severity=str(
            analysis.get("minimum_severity", DEFAULT_CONFIG.minimum_severity)
        ).lower(),
        max_file_bytes=int(analysis.get("max_file_bytes", DEFAULT_CONFIG.max_file_bytes)),
        report_formats=tuple(
            item.lower()
            for item in _string_tuple(
                reporting.get("formats"), DEFAULT_CONFIG.report_formats, "reporting.formats"
            )
        ),
        build_command=_string_tuple(
            verification.get("build_command"), (), "verification.build_command"
        ),
        test_command=_string_tuple(
            verification.get("test_command"), (), "verification.test_command"
        ),
        timeout_seconds=int(verification.get("timeout_seconds", DEFAULT_CONFIG.timeout_seconds)),
        parser_backend=str(parser.get("backend", DEFAULT_CONFIG.parser_backend)).lower(),
        clang_tool=str(parser.get("clang_tool", DEFAULT_CONFIG.clang_tool)),
        compilation_database=str(
            parser.get("compilation_database", DEFAULT_CONFIG.compilation_database)
        ),
        model_backend=str(models.get("backend", DEFAULT_CONFIG.model_backend)).lower(),
        repair_backend=str(repair.get("backend", DEFAULT_CONFIG.repair_backend)).lower(),
        model_checkpoint=model_path("checkpoint"),
        model_encoder=model_path("encoder"),
        model_threshold=float(threshold) if threshold is not None else None,
    )
    if config.minimum_severity not in {"low", "medium", "high", "critical"}:
        raise ValueError("analysis.minimum_severity 必须是 low、medium、high 或 critical")
    if config.max_file_bytes <= 0:
        raise ValueError("analysis.max_file_bytes 必须大于 0")
    if config.timeout_seconds <= 0:
        raise ValueError("verification.timeout_seconds 必须大于 0")
    if config.parser_backend not in {"auto", "fallback", "clang"}:
        raise ValueError("parser.backend 必须是 auto、fallback 或 clang")
    if config.model_backend not in {"disabled", "trained"}:
        raise ValueError("models.backend 必须是 disabled 或 trained")
    if config.model_backend == "trained" and not config.model_checkpoint:
        raise ValueError("启用模型需要 models.checkpoint")
    if config.model_threshold is not None and not 0 <= config.model_threshold <= 1:
        raise ValueError("models.threshold 必须位于 0 到 1")
    invalid_formats = set(config.report_formats) - {"json", "markdown", "md", "html"}
    if invalid_formats:
        raise ValueError(f"不支持的报告格式：{', '.join(sorted(invalid_formats))}")
    return config
