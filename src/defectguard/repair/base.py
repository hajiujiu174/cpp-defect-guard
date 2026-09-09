from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

from defectguard.domain import Finding, ParsedFile


@dataclass(frozen=True)
class PatchCandidate:
    candidate_id: str
    target_file: str
    unified_diff: str
    generator: str
    score: float


class PatchGenerator(Protocol):
    name: str

    def generate(
        self, findings: list[Finding], parsed_files: list[ParsedFile]
    ) -> list[PatchCandidate]: ...

    def status(self) -> str: ...


class DisabledPatchGenerator:
    """第三层扩展点：后续接模板、掩码填充或离散扩散模型。"""

    name = "disabled"

    def generate(
        self, findings: list[Finding], parsed_files: list[ParsedFile]
    ) -> list[PatchCandidate]:
        del findings, parsed_files
        return []

    def status(self) -> str:
        return "disabled: 自动修复候选生成尚未配置"

