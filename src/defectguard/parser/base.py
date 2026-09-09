from __future__ import annotations

from pathlib import Path
from typing import Protocol

from defectguard.domain import ParsedFile


class ProjectParser(Protocol):
    name: str

    def parse(self, source_root: Path) -> list[ParsedFile]: ...

