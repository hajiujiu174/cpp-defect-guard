from __future__ import annotations

from typing import Protocol

from defectguard.domain import Finding, ParsedFile


class DetectorBackend(Protocol):
    name: str

    def detect(self, parsed_files: list[ParsedFile]) -> list[Finding]: ...

    def status(self) -> str: ...


class DisabledDetectorBackend:
    """默认禁用模型，保持基础规则扫描不依赖 ML 环境。"""

    name = "disabled"

    def detect(self, parsed_files: list[ParsedFile]) -> list[Finding]:
        del parsed_files
        return []

    def status(self) -> str:
        return "disabled: GraphCodeBERT/GNN 尚未配置"
