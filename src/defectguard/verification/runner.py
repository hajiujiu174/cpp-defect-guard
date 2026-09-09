from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from defectguard.domain import VerificationResult, VerificationStep


class IsolatedVerifier:
    """在临时副本中执行构建与测试，绝不在原始源码目录中应用修改。"""

    def __init__(self, timeout_seconds: int = 120) -> None:
        self.timeout_seconds = timeout_seconds

    def verify(
        self,
        source_root: Path,
        build_command: tuple[str, ...],
        test_command: tuple[str, ...],
        exclude: tuple[str, ...] = (".git", ".venv", "build", "artifacts"),
    ) -> VerificationResult:
        commands = (("build", build_command), ("test", test_command))
        if not any(command for _, command in commands):
            return VerificationResult(status="skipped", steps=(), workspace_removed=True)

        steps: list[VerificationStep] = []
        workspace: Path | None = None
        with tempfile.TemporaryDirectory(prefix="defectguard-verify-") as temp_dir:
            workspace = Path(temp_dir) / "project"
            shutil.copytree(
                source_root,
                workspace,
                ignore=shutil.ignore_patterns(*exclude),
            )
            for name, command in commands:
                if not command:
                    continue
                executed_command = list(command)
                executable = Path(executed_command[0])
                workspace_executable = workspace / executable
                if not executable.is_absolute() and workspace_executable.is_file():
                    executed_command[0] = str(workspace_executable.resolve())
                started = time.perf_counter()
                try:
                    process = subprocess.run(
                        executed_command,
                        cwd=workspace,
                        text=True,
                        capture_output=True,
                        timeout=self.timeout_seconds,
                        check=False,
                    )
                    status = "passed" if process.returncode == 0 else "failed"
                    step = VerificationStep(
                        name=name,
                        command=command,
                        return_code=process.returncode,
                        duration_seconds=round(time.perf_counter() - started, 3),
                        stdout=process.stdout,
                        stderr=process.stderr,
                        status=status,
                    )
                except subprocess.TimeoutExpired as error:
                    step = VerificationStep(
                        name=name,
                        command=command,
                        return_code=None,
                        duration_seconds=round(time.perf_counter() - started, 3),
                        stdout=self._text(error.stdout),
                        stderr=self._text(error.stderr),
                        status="timeout",
                    )
                except OSError as error:
                    step = VerificationStep(
                        name=name,
                        command=command,
                        return_code=None,
                        duration_seconds=round(time.perf_counter() - started, 3),
                        stdout="",
                        stderr=str(error),
                        status="error",
                    )
                steps.append(step)
                if step.status != "passed":
                    break

        overall = "passed" if steps and all(step.status == "passed" for step in steps) else "failed"
        return VerificationResult(
            status=overall,
            steps=tuple(steps),
            workspace_removed=workspace is not None and not workspace.exists(),
        )

    @staticmethod
    def _text(value: str | bytes | None) -> str:
        if value is None:
            return ""
        if isinstance(value, bytes):
            return value.decode("utf-8", errors="replace")
        return value
