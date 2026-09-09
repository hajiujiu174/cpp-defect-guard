from __future__ import annotations

import json
from pathlib import Path
import subprocess
from typing import Any


class ClangToolClient:
    """Clang LibTooling 原生分析器的进程适配器。

    读取类型化 AST、CFG 和变量读写事件；同时保留诊断并验证协议版本。
    """

    def __init__(self, executable: Path, timeout_seconds: int = 120) -> None:
        self.executable = executable
        self.timeout_seconds = timeout_seconds

    def is_available(self) -> bool:
        return self.executable.is_file()

    def analyze(
        self, files: list[Path], compilation_database_dir: Path
    ) -> dict[str, Any]:
        # Bound argv size on Windows and memory/time per native invocation.
        # Headers still enter through translation units; duplicates are merged below.
        units = [path for path in files if path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}] or files
        if not units:
            raise ValueError("Clang 分析至少需要一个源文件")
        prefix = [str(self.executable), "-p", str(compilation_database_dir)]
        batches, batch = [], []
        for path in units:
            candidate = prefix + [str(p) for p in [*batch, path]]
            command_length = len(subprocess.list2cmdline(candidate).encode("utf-16-le")) // 2
            if batch and (len(batch) >= 32 or command_length > 24000):
                batches.append(batch)
                batch = []
            if len(subprocess.list2cmdline(prefix + [str(path)]).encode("utf-16-le")) // 2 > 24000:
                raise ValueError("单个 Clang 调用路径超过安全命令行长度")
            batch.append(path)
        batches.append(batch)
        if len(batches) == 1:
            return self._analyze_batch(batches[0], compilation_database_dir)
        combined, seen = None, set()
        for batch in batches:
            payload = self._analyze_batch(batch, compilation_database_dir)
            if combined is None:
                combined = {**payload, "functions": [], "diagnostics": [], "translation_units": []}
            if payload["schema_version"] != combined["schema_version"]:
                raise ValueError("Clang 分批结果协议不一致")
            for function in payload["functions"]:
                key = tuple(function.get(name) for name in ("file", "name", "signature", "source_start", "source_end", "start_line", "end_line"))
                if key not in seen:
                    seen.add(key)
                    combined["functions"].append(function)
            combined["diagnostics"].extend(payload["diagnostics"])
            combined["translation_units"].extend(payload["translation_units"])
        return combined

    def _analyze_batch(
        self, files: list[Path], compilation_database_dir: Path
    ) -> dict[str, Any]:
        if not self.is_available():
            raise FileNotFoundError(f"Clang 分析器不存在：{self.executable}")
        command = [
            str(self.executable),
            "-p",
            str(compilation_database_dir),
            *(str(path) for path in files),
        ]
        # Headers are parsed through their translation units, not as separate C++ programs.
        translation_units = [path for path in files if path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}]
        command = command[:3] + [str(path) for path in (translation_units or files)]
        try:
            process = subprocess.run(
                command, text=True, encoding="utf-8", errors="replace",
                capture_output=True, timeout=self.timeout_seconds, check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"Clang 分析超过 {self.timeout_seconds} 秒") from error
        if process.returncode != 0:
            raise RuntimeError(process.stderr.strip() or "Clang 分析器执行失败")
        payload = json.loads(process.stdout)
        if not isinstance(payload, dict) or payload.get("schema_version") not in {"1.0", "1.1"}:
            raise ValueError("Clang 分析器输出的协议版本不受支持，请重新构建原生工具")
        if not isinstance(payload.get("functions"), list) or not all(isinstance(fn, dict) for fn in payload["functions"]):
            raise ValueError("Clang 分析器未返回有效的 functions 数组")
        payload["diagnostics"] = [process.stderr.strip()] if process.stderr.strip() else []
        payload["translation_units"] = [str(path.resolve()) for path in (translation_units or files)]
        return payload
