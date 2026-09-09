from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from defectguard.analysis.rules import RuleEngine, SEVERITY_RANK
from defectguard.config import load_config
from defectguard.domain import Severity
from defectguard.environment import inspect_environment
from defectguard.graphs import write_graphs
from defectguard.pipeline import AnalysisPipeline
from defectguard.reporting import write_reports
from defectguard.storage import RunStore
from defectguard.verification import IsolatedVerifier


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _safe_output(source: Path, requested: Path | None) -> Path:
    if requested is not None:
        output = requested.resolve()
    else:
        candidate = (Path.cwd() / "artifacts" / source.name).resolve()
        output = source.parent / f"{source.name}-analysis" if _inside(candidate, source) else candidate
    if _inside(output, source):
        raise ValueError("为保持原始工程只读，输出目录不能位于源码目录内部")
    return output


def _scan(args: argparse.Namespace) -> int:
    source = Path(args.source).resolve()
    config = load_config(Path(args.config).resolve() if args.config else None)
    output = _safe_output(source, Path(args.output) if args.output else None)
    formats = tuple(args.format) if args.format else config.report_formats
    report = AnalysisPipeline(config).scan(source, run_verification=args.verify)
    written = write_reports(report, output, formats)
    database = output / "runs.sqlite3"
    RunStore(database).save(report)
    print(f"扫描完成：{len(report.findings)} 条告警，{report.metrics.file_count} 个源码文件")
    print(f"解析后端：{report.parser_backend}")
    print(f"原始源码未变化：{'是' if report.source_unchanged else '否'}")
    if args.verify:
        print(f"编译测试验证：{report.verification.status}")
    for path in written:
        print(f"报告：{path}")
    print(f"运行记录：{database}")
    if args.verify and report.verification.status == "failed":
        return 1
    if args.fail_on != "none":
        threshold = SEVERITY_RANK[Severity(args.fail_on)]
        if any(SEVERITY_RANK[item.severity] >= threshold for item in report.findings):
            return 3
    return 0


def _verify(args: argparse.Namespace) -> int:
    source = Path(args.source).resolve()
    config = load_config(Path(args.config).resolve() if args.config else None)
    output = _safe_output(source, Path(args.output) if args.output else None)
    result = IsolatedVerifier(config.timeout_seconds).verify(
        source, config.build_command, config.test_command, config.exclude
    )
    output.mkdir(parents=True, exist_ok=True)
    path = output / "verification.json"
    path.write_text(json.dumps(result.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"验证状态：{result.status}")
    print(f"验证日志：{path}")
    return 0 if result.status in {"passed", "skipped"} else 1


def _doctor(args: argparse.Namespace) -> int:
    source = Path(args.source).resolve()
    config = load_config(Path(args.config).resolve() if args.config else None)
    result = inspect_environment(source, config)
    ready = (result["layer1"]["fallback_scan_ready"]
             and (config.parser_backend != "clang" or result["layer1"]["native_clang_ready"])
             and (config.model_backend != "trained" or result["layer2"]["model_preflight_ready"]))
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if ready else 2
    python = result["python"]
    print(f"Python：{python['version']} ({'可用' if python['supported'] else '版本过低'})")
    print(f"源码目录：{'可用' if result['source_exists'] else '不存在'} {source}")
    for tool in result["tools"]:
        print(f"{tool['name']}：{tool['path'] or '未找到'}")
    native = result["clang_libtooling"]
    print(f"defectguard-clang：{native['executable'] or '未找到'}")
    print(f"compile_commands.json：{native['compilation_database'] or '未找到'}")
    print(f"降级扫描：{'就绪' if result['layer1']['fallback_scan_ready'] else '不可用'}")
    print(f"Clang 主解析：{'就绪' if result['layer1']['native_clang_ready'] else '未就绪'}")
    model = result["model_preflight"]
    status_labels = {"disabled": "未启用", "blocked": "条件不满足", "preflight-passed": "轻量预检通过（尚未验证实际推理）"}
    print(f"模型：{status_labels.get(model['status'], model['status'])}")
    if model["enabled"]:
        print(f"模型解释器：{model['interpreter']}")
        for dependency in model["dependencies"]:
            print(f"{dependency['distribution']}：{dependency['version'] or '当前解释器未安装'}")
        for issue in model["issues"]:
            print(f"模型预检问题：{issue}")
    return 0 if ready else 2


def _rules(args: argparse.Namespace) -> int:
    del args
    for rule in RuleEngine.catalog():
        print(f"{rule['rule_id']}  {rule['default_severity']:<8}  {rule['title']}")
    return 0


def _export_graphs(args: argparse.Namespace) -> int:
    source = Path(args.source).resolve()
    config = load_config(Path(args.config).resolve() if args.config else None)
    output = _safe_output(source, Path(args.output) if args.output else None)
    report = AnalysisPipeline(config).scan(source)
    paths = write_graphs(report, output)
    print(f"导出 {report.program_structure.function_count} 个函数图；标签状态：未标注")
    for path in paths:
        print(f"图数据：{path}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="defectguard", description="C/C++ 软件缺陷检测与自动修复课程设计框架"
    )
    subcommands = parser.add_subparsers(dest="command", required=True)

    scan = subcommands.add_parser("scan", help="扫描源码并输出 JSON、Markdown、HTML 报告")
    scan.add_argument("source", help="待分析的 C/C++ 源码目录")
    scan.add_argument("--config", help="TOML 配置文件")
    scan.add_argument("--output", help="报告输出目录，必须位于源码目录外")
    scan.add_argument("--verify", action="store_true", help="扫描后在隔离副本中执行编译和测试")
    scan.add_argument(
        "--fail-on",
        choices=("none", "low", "medium", "high", "critical"),
        default="none",
        help="出现达到指定严重度的告警时返回退出码 3",
    )
    scan.add_argument(
        "--format",
        action="append",
        choices=("json", "markdown", "html"),
        help="可重复指定；不指定时使用配置文件中的 formats",
    )
    scan.set_defaults(handler=_scan)

    verify = subcommands.add_parser("verify", help="在临时副本中执行构建与测试")
    verify.add_argument("source", help="待验证的源码目录")
    verify.add_argument("--config", required=True, help="包含构建与测试命令的 TOML 配置")
    verify.add_argument("--output", help="验证日志输出目录，必须位于源码目录外")
    verify.set_defaults(handler=_verify)

    doctor = subcommands.add_parser("doctor", help="检查原生解析环境与当前解释器的模型依赖/关键文件")
    doctor.add_argument("source", nargs="?", default=".", help="待检查的源码目录")
    doctor.add_argument("--config", help="TOML 配置文件")
    doctor.add_argument("--json", action="store_true", help="输出 JSON")
    doctor.set_defaults(handler=_doctor)

    rules = subcommands.add_parser("rules", help="列出基础静态规则")
    rules.set_defaults(handler=_rules)

    graphs = subcommands.add_parser("export-graphs", help="导出带源码映射的 AST/CFG/DFG 函数图 JSONL")
    graphs.add_argument("source", help="待分析的 C/C++ 源码目录")
    graphs.add_argument("--config", help="TOML 配置文件")
    graphs.add_argument("--output", help="图数据目录，必须位于源码目录外")
    graphs.set_defaults(handler=_export_graphs)
    from defectguard.experiments.cli import register_commands
    register_commands(subcommands)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (OSError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 2
