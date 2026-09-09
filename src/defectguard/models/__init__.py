from defectguard.models.base import DetectorBackend, DisabledDetectorBackend


def create_detector(config):
    if config.model_backend == "disabled":
        return DisabledDetectorBackend()
    if config.model_backend != "trained":
        raise ValueError("models.backend 必须为 disabled 或 trained")
    if not config.model_checkpoint:
        raise ValueError("启用 trained 模型需要 models.checkpoint")
    from pathlib import Path
    try:
        from .trained import TrainedDetectorBackend
    except ImportError as error:
        raise ValueError("模型依赖未安装，请使用 .venv-ml 环境或运行 scripts/setup_ml.ps1") from error
    return TrainedDetectorBackend(Path(config.model_checkpoint), config.model_threshold,
                                  Path(config.model_encoder) if config.model_encoder else None)

__all__ = ["DetectorBackend", "DisabledDetectorBackend"]
