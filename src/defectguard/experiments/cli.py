"""命令注册不加载 PyTorch；纯规则用户无需安装模型依赖。"""
from __future__ import annotations

import json
from pathlib import Path

from .data import prepare_dataset


def _run(args):
    if args.command == "import-juliet":
        from .juliet import import_juliet
        result = import_juliet(Path(args.archive), Path(args.output), max_cases=args.max_cases,
                               cwes=tuple(args.cwes), flows=tuple(args.flows), seed=args.seed,
                               selection=Path(args.selection) if args.selection else None)
    elif args.command == "prepare-dataset":
        result = prepare_dataset(Path(args.graphs), Path(args.labels), Path(args.output), args.seed)
    elif args.command == "sweep-models":
        from .sweep import run_sweep
        try:
            result = run_sweep(Path(args.dataset), Path(args.output), modes=tuple(args.modes), seeds=tuple(args.seeds),
                               encoder=Path(args.encoder) if args.encoder else None, epochs=args.epochs,
                               max_length=args.max_length, cache=Path(args.cache) if args.cache else None, resume=args.resume)
            result = {"status": result["status"], "run_count": result["run_count"],
                      "test_sample_count": result["test_sample_count"],
                      "summary": str((Path(args.output) / "summary.md").resolve()),
                      "cases": str((Path(args.output) / "cases.json").resolve()),
                      "f1": {mode: values["classification"]["f1"] for mode, values in result["models"].items()}}
        except ImportError as error:
            raise ValueError("多种子训练需要 .venv-ml 模型环境") from error
    else:
        try:
            from .runner import train_experiment, evaluate_checkpoint
            from .encoder import download_encoder
        except ImportError as error:
            raise ValueError("模型依赖未安装，请使用 .venv-ml 环境或运行 scripts/setup_ml.ps1") from error
        encoder = Path(args.encoder) if getattr(args, "encoder", None) else None
        if args.command == "download-encoder":
            result = {"encoder_directory": str(download_encoder(Path(args.output)))}
        elif args.command == "train-model":
            excluded = tuple(sorted({edge for name in args.without for edge in {"cfg": (1, 2), "dfg": (3,)}[name]}))
            result = train_experiment(Path(args.dataset), Path(args.output), mode=args.mode, encoder=encoder,
                                      epochs=args.epochs, seed=args.seed, learning_rate=args.learning_rate,
                                      max_length=args.max_length, excluded_edges=excluded,
                                      cache=Path(args.cache) if args.cache else None,
                                      feature_version=args.feature_version, localization_weight=args.localization_weight)
        else:
            result = evaluate_checkpoint(Path(args.dataset), Path(args.checkpoint), Path(args.output), encoder)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def register_commands(subcommands):
    from .juliet import DEFAULT_CWES, DEFAULT_FLOWS
    juliet = subcommands.add_parser("import-juliet", help="导入固定 NIST Juliet 1.3.1 首批 C 基础变体，保留标签和来源")
    juliet.add_argument("--archive", required=True, help="已下载的官方 ZIP；导入前校验固定 SHA-256")
    juliet.add_argument("--output", required=True, help="尚不存在且与下载目录独立的输出目录")
    juliet.add_argument("--max-cases", type=int, default=1500, help="最大用例对数量，默认 1500（去重前最多 3000 函数）")
    juliet.add_argument("--cwes", type=int, nargs="+", default=list(DEFAULT_CWES))
    juliet.add_argument("--flows", type=int, nargs="+", default=list(DEFAULT_FLOWS))
    juliet.add_argument("--seed", type=int, default=42)
    juliet.add_argument("--selection", help="上一批 provenance.jsonl，固定同一组用例作清洗对照")
    prepare = subcommands.add_parser("prepare-dataset", help="合并独立标签、去重并按来源组固定划分")
    prepare.add_argument("--graphs", required=True)
    prepare.add_argument("--labels", required=True)
    prepare.add_argument("--output", required=True)
    prepare.add_argument("--seed", type=int, default=42)
    download = subcommands.add_parser("download-encoder", help="下载固定修订的官方 GraphCodeBERT 权重")
    download.add_argument("--output", required=True)
    train = subcommands.add_parser("train-model", help="训练并评估序列基线、RGCN 或融合模型")
    train.add_argument("--dataset", required=True)
    train.add_argument("--output", required=True, help="新的空目录，不覆盖已有实验")
    train.add_argument("--mode", choices=("sequence", "gnn", "fusion"), default="fusion")
    train.add_argument("--encoder", help="本地 GraphCodeBERT 目录；gnn 模式不需要")
    train.add_argument("--epochs", type=int, default=40)
    train.add_argument("--seed", type=int, default=42)
    train.add_argument("--learning-rate", type=float, default=0.003)
    train.add_argument("--max-length", type=int, default=256)
    train.add_argument("--without", choices=("cfg", "dfg"), action="append", default=[])
    train.add_argument("--cache", help="冻结序列特征缓存目录，按源码和编码器哈希复用")
    train.add_argument("--feature-version", choices=("legacy-v1", "semantic-v2"), default="semantic-v2")
    train.add_argument("--localization-weight", type=float, help="定位损失权重，0 为纯分类；无正例行监督时自动为 0")
    evaluate = subcommands.add_parser("evaluate-model", help="重新加载检查点评估其原始测试划分")
    evaluate.add_argument("--dataset", required=True)
    evaluate.add_argument("--checkpoint", required=True)
    evaluate.add_argument("--encoder", help="搬移检查点时可指定同指纹编码器")
    evaluate.add_argument("--output", required=True)
    sweep = subcommands.add_parser("sweep-models", help="固定数据划分的多种子实验、汇总及错误样例复核")
    sweep.add_argument("--dataset", required=True)
    sweep.add_argument("--output", required=True)
    sweep.add_argument("--modes", choices=("sequence", "gnn", "fusion"), nargs="+", default=["sequence", "gnn", "fusion"])
    sweep.add_argument("--seeds", type=int, nargs="+", default=[42, 43, 44])
    sweep.add_argument("--encoder")
    sweep.add_argument("--epochs", type=int, default=40)
    sweep.add_argument("--max-length", type=int, default=256)
    sweep.add_argument("--cache")
    sweep.add_argument("--resume", action="store_true", help="核验并复用同一计划中已经完成的实验；不删除不完整检查点")
    for command in (juliet, prepare, download, train, evaluate, sweep):
        command.set_defaults(handler=_run)
