from __future__ import annotations

import importlib.metadata
import importlib.util
import json
import math
from pathlib import Path
import platform
import shutil
import sys
from typing import Any

from defectguard.config import AppConfig


def _tool(name: str) -> dict[str, Any]:
    path = shutil.which(name)
    return {"name": name, "available": path is not None, "path": path}


def _dependency(module: str, distribution: str) -> dict[str, Any]:
    """Inspect this interpreter without importing torch, transformers or their dependencies."""
    errors = []
    try:
        discoverable = importlib.util.find_spec(module) is not None
    except (ImportError, ValueError, AttributeError) as error:
        discoverable = False
        errors.append(f"无法定位模块：{error}")
    try:
        version = importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        version = None
    except (OSError, ValueError) as error:
        version = None
        errors.append(f"无法读取安装信息：{error}")
    if not discoverable:
        errors.append(f"当前解释器找不到 {module} 模块")
    if not version:
        errors.append(f"当前解释器没有 {distribution} 的版本记录")
    return {"module": module, "distribution": distribution, "version": version,
            "discoverable": discoverable, "available": bool(discoverable and version), "issues": errors}


def _file_status(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path.resolve()), "exists": False, "nonempty": False, "size_bytes": None}
    try:
        if path.is_file():
            result.update(exists=True, size_bytes=path.stat().st_size)
            result["nonempty"] = result["size_bytes"] > 0
    except OSError as error:
        result["error"] = str(error)
    return result


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"重复字段：{key}")
        result[key] = value
    return result


def _json_constant(value):
    raise ValueError(f"非有限数值：{value}")


def _checkpoint_metadata(path: Path) -> tuple[dict[str, Any], dict[str, Any] | None]:
    result: dict[str, Any] = {"path": str(path.resolve()), "basic_valid": False, "schema_version": None,
                             "model_kind": None, "mode": None, "issues": []}
    try:
        # A doctor command must not accidentally read a checkpoint-sized JSON file.
        with path.open("rb") as stream:
            raw = stream.read(4 * 1024 * 1024 + 1)
        if len(raw) > 4 * 1024 * 1024:
            raise ValueError("元数据超过轻量预检的 4 MiB 限制")
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_json_object, parse_constant=_json_constant)
        if not isinstance(value, dict):
            raise ValueError("metadata.json 必须是 JSON 对象")
        result.update(schema_version=value.get("schema_version"), model_kind=value.get("model_kind"))
        if value.get("schema_version") != "1.0" or value.get("model_kind") != "defectguard-binary-detector":
            raise ValueError("检查点类型或版本不支持")
        network = value.get("network")
        if not isinstance(network, dict) or network.get("mode") not in ("sequence", "gnn", "fusion"):
            raise ValueError("network.mode 必须是 sequence、gnn 或 fusion")
        result["mode"] = network["mode"]
        threshold = value.get("threshold")
        if type(threshold) not in (int, float) or not math.isfinite(threshold) or not 0 <= threshold <= 1:
            raise ValueError("检查点 threshold 必须是 0 到 1 的有限数值")
        result["basic_valid"] = True
        return result, value
    except (OSError, UnicodeError, ValueError, RecursionError) as error:
        result["issues"].append(f"检查点元数据预检失败：{error}")
        return result, None


def _model_preflight(source_root: Path, config: AppConfig, native_path: str | None,
                     database: str | None) -> dict[str, Any]:
    dependencies = [_dependency(module, distribution) for module, distribution in (
        ("torch", "torch"), ("transformers", "transformers"),
        ("torch_geometric", "torch-geometric"), ("safetensors", "safetensors"))]
    enabled = config.model_backend == "trained"
    native = {"parser_backend": config.parser_backend, "executable": native_path,
              "executable_exists": bool(native_path), "compilation_database": database,
              "compilation_database_exists": bool(database),
              "ready": bool(source_root.is_dir() and native_path and database and config.parser_backend != "fallback")}
    result: dict[str, Any] = {
        "status": "disabled" if config.model_backend == "disabled" else "blocked",
        "backend": config.model_backend, "enabled": enabled, "interpreter": sys.executable,
        "dependencies": dependencies, "dependencies_ready": all(item["available"] for item in dependencies),
        "checkpoint": {"configured_path": config.model_checkpoint, "path": None, "exists": False, "files": {}},
        "metadata": None,
        "encoder": {"required": None, "configured_path": config.model_encoder, "path": None,
                    "path_source": None, "files": {}, "ready": False},
        "native": native, "issues": [],
        "limitations": ["仅检查当前解释器的包发现/版本记录、关键文件和基本元数据；未导入深度学习包。",
                        "未校验完整元数据、权重哈希/张量、编码器指纹、包兼容性或执行实际推理。",
                        "原生程序和编译数据库仅检查文件存在；未验证可执行性、数据库内容或实际解析。"],
    }
    if config.model_backend == "disabled":
        return result
    issues = result["issues"]
    if not enabled:
        issues.append(f"不支持的 models.backend：{config.model_backend}")
        return result
    for item in dependencies:
        issues.extend(item["issues"])
    if not source_root.is_dir():
        issues.append(f"源码目录不存在：{source_root}")
    if config.parser_backend == "fallback":
        issues.append("训练模型需要 Clang AST/CFG；parser.backend=fallback 无法提供原生程序图")
    if not native_path:
        issues.append("找不到已配置或 PATH 中的 defectguard-clang 原生分析器")
    if not database:
        issues.append("找不到 compile_commands.json 编译数据库")
    if not config.model_checkpoint:
        issues.append("models.checkpoint 未配置")
        return result
    checkpoint = Path(config.model_checkpoint).resolve()
    result["checkpoint"].update(path=str(checkpoint), exists=checkpoint.is_dir())
    if not checkpoint.is_dir():
        issues.append(f"模型检查点目录不存在：{checkpoint}")
        return result
    for name in ("metadata.json", "model.safetensors"):
        status = _file_status(checkpoint / name)
        result["checkpoint"]["files"][name] = status
        if not status["nonempty"]:
            issues.append(f"模型检查点缺少非空文件：{status['path']}")
    metadata, value = _checkpoint_metadata(checkpoint / "metadata.json")
    result["metadata"] = metadata
    issues.extend(metadata["issues"])
    if value is not None:
        encoder = result["encoder"]
        encoder["required"] = metadata["mode"] != "gnn"
        if not encoder["required"]:
            encoder["ready"] = True
        else:
            directory = config.model_encoder or value.get("encoder_directory")
            encoder["path_source"] = "models.encoder" if config.model_encoder else "metadata.encoder_directory"
            if not isinstance(directory, str) or not directory:
                issues.append("序列/融合模型需要本地编码器目录（models.encoder 或 metadata.encoder_directory）")
            else:
                path = Path(directory).resolve()
                encoder["path"] = str(path)
                for name in ("config.json", "tokenizer_config.json", "tokenizer.json", "vocab.json", "merges.txt",
                             "pytorch_model.bin", "model.safetensors"):
                    encoder["files"][name] = _file_status(path / name)
                files = encoder["files"]
                config_ready = files["config.json"]["nonempty"]
                tokenizer_ready = files["tokenizer.json"]["nonempty"] or (
                    files["vocab.json"]["nonempty"] and files["merges.txt"]["nonempty"])
                weights_ready = files["pytorch_model.bin"]["nonempty"] or files["model.safetensors"]["nonempty"]
                encoder["ready"] = bool(config_ready and tokenizer_ready and weights_ready)
                if not config_ready:
                    issues.append(f"编码器缺少非空 config.json：{path}")
                if not tokenizer_ready:
                    issues.append(f"编码器缺少 tokenizer.json 或 vocab.json + merges.txt：{path}")
                if not weights_ready:
                    issues.append(f"编码器缺少非空 pytorch_model.bin 或 model.safetensors：{path}")
    if not issues:
        result["status"] = "preflight-passed"
    return result


def inspect_environment(source_root: Path, config: AppConfig) -> dict[str, Any]:
    source_root = source_root.resolve()
    tools = [_tool(name) for name in ("clang", "clang++", "gcc", "g++", "cmake", "ninja")]
    configured_tool = Path(config.clang_tool) if config.clang_tool else None
    native_path = (
        str(configured_tool.resolve())
        if configured_tool and configured_tool.is_file()
        else (None if configured_tool else shutil.which("defectguard-clang"))
    )
    database_candidates = []
    if config.compilation_database:
        configured_database = Path(config.compilation_database)
        database_candidates.append(
            configured_database
            if configured_database.is_absolute()
            else source_root / configured_database
        )
    database_candidates.extend(
        [source_root / "compile_commands.json", source_root / "build" / "compile_commands.json",
         source_root / "out" / "compile_commands.json"]
    )
    database = next((str(path.resolve()) for path in database_candidates if path.is_file()), None)
    build_tool = config.build_command[0] if config.build_command else None
    test_tool = config.test_command[0] if config.test_command else None

    def command_available(command: str | None) -> bool:
        if command is None:
            return False
        path = Path(command)
        return path.is_file() if path.parent != Path(".") else shutil.which(command) is not None

    preflight = _model_preflight(source_root, config, native_path, database)
    return {
        "python": {
            "version": platform.python_version(),
            "executable": sys.executable,
            "supported": sys.version_info >= (3, 11),
        },
        "source_root": str(source_root),
        "source_exists": source_root.is_dir(),
        "tools": tools,
        "clang_libtooling": {
            "executable": native_path,
            "compilation_database": database,
            "ready": bool(native_path and database),
        },
        "verification": {
            "build_command_configured": bool(config.build_command),
            "test_command_configured": bool(config.test_command),
            "build_tool_available": command_available(build_tool),
            "test_tool_available": command_available(test_tool),
        },
        "layer1": {
            "fallback_scan_ready": source_root.is_dir() and sys.version_info >= (3, 11),
            "native_clang_ready": bool(native_path and database),
        },
        "layer2": {
            "dependencies_ready": preflight["dependencies_ready"],
            "model_enabled": preflight["enabled"],
            "model_preflight_ready": preflight["status"] == "preflight-passed",
        },
        "model_preflight": preflight,
    }
