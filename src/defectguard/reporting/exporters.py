from __future__ import annotations

from html import escape
import json
from pathlib import Path

from defectguard.domain import AnalysisReport


def _markdown(report: AnalysisReport) -> str:
    lines = [
        "# C/C++ 软件缺陷分析报告",
        "",
        f"- 运行编号：`{report.run_id}`",
        f"- 源码目录：`{report.source_root}`",
        f"- 源码指纹：`{report.source_fingerprint}`",
        f"- 解析后端：`{report.parser_backend}`",
        f"- 构建系统：{', '.join(report.project.build_systems) or '未识别'}",
        f"- 原始源码未变化：{'是' if report.source_unchanged else '否'}",
        f"- 模型状态：{report.model_status}",
        f"- 修复状态：{report.repair_status}",
        f"- 编译测试验证：{report.verification.status}",
        "",
        "## 汇总",
        "",
        "| 文件数 | 代码行 | 注释率 | 重复行率 | 函数数 | 平均函数复杂度 | 最大嵌套 | 依赖数 | 告警数 | 每千行告警 |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        (
            f"| {report.metrics.file_count} | {report.metrics.code_lines} | "
            f"{report.metrics.comment_ratio:.2%} | {report.metrics.duplicate_line_ratio:.2%} | "
            f"{report.metrics.function_count} | {report.metrics.average_function_complexity} | "
            f"{report.metrics.max_brace_depth} | {report.metrics.include_dependency_count} | "
            f"{len(report.findings)} | "
            f"{report.metrics.warning_density_per_kloc} |"
        ),
        "",
        "## 程序结构",
        "",
        f"- 表示方式：`{report.program_structure.representation}`",
        f"- 函数数：{report.program_structure.function_count}",
        f"- AST 节点数：{report.program_structure.ast_node_count}",
        f"- CFG 节点数：{report.program_structure.cfg_node_count}",
        f"- CFG 边数：{report.program_structure.cfg_edge_count}",
        f"- DFG 状态：{report.program_structure.dfg_status}",
        f"- DFG 定义—使用边数：{report.program_structure.dfg_edge_count}",
        "",
        "## 告警",
        "",
    ]
    if not report.findings:
        lines.append("未发现已启用检测器能够识别的问题。该结论不代表源码不存在缺陷。")
    else:
        lines.extend(
            [
                "| 规则 | 严重度 | 位置 | 缺陷类型 | 说明 | 置信度 |",
                "|---|---|---|---|---|---:|",
            ]
        )
        for finding in report.findings:
            message = finding.message.replace("|", "\\|")
            location = f"{finding.location.file}:{finding.location.line}:{finding.location.column}"
            lines.append(
                f"| {finding.rule_id} | {finding.severity.value} | `{location}` | "
                f"{finding.defect_type} | {message} | {finding.confidence:.2f} |"
            )
        lines.extend(["", "## 证据与建议", ""])
        for index, finding in enumerate(report.findings, 1):
            evidence = finding.evidence.replace("`", "'")
            lines.extend(
                [
                    f"### {index} {finding.rule_id} {finding.location.file}:{finding.location.line}",
                    "",
                    f"证据：`{evidence}`",
                    "",
                    f"检测器：`{finding.detector}`",
                    "",
                    f"建议：{finding.suggestion}",
                    "",
                ]
            )
    lines.extend(
        [
            "## 文件指标",
            "",
            "| 文件 | 代码行 | 注释行 | 函数数 | 圈复杂度 | 最大嵌套 | 重复行率 | 依赖数 |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for item in report.metrics.files:
        lines.append(
            f"| `{item.file}` | {item.code_lines} | {item.comment_lines} | "
            f"{item.function_count} | {item.cyclomatic_complexity} | "
            f"{item.max_brace_depth} | {item.duplicate_line_ratio:.2%} | {item.include_count} |"
        )
    lines.extend(["", "## 编译与测试验证", ""])
    if not report.verification.steps:
        lines.append(f"验证状态：{report.verification.status}。未执行命令。")
    else:
        lines.extend(
            [
                "| 步骤 | 状态 | 返回码 | 耗时秒 | 命令 |",
                "|---|---|---:|---:|---|",
            ]
        )
        for step in report.verification.steps:
            command = " ".join(step.command).replace("|", "\\|")
            lines.append(
                f"| {step.name} | {step.status} | {step.return_code if step.return_code is not None else '-'} | "
                f"{step.duration_seconds} | `{command}` |"
            )
            if step.stdout:
                lines.extend(["", f"{step.name} 标准输出：", "", "```text", step.stdout.rstrip(), "```", ""])
            if step.stderr:
                lines.extend(["", f"{step.name} 错误输出：", "", "```text", step.stderr.rstrip(), "```", ""])
    if report.diagnostics:
        lines.extend(["## 诊断信息", ""])
        lines.extend(f"- {item}" for item in report.diagnostics)
        lines.append("")
    return "\n".join(lines)


def _html(report: AnalysisReport) -> str:
    finding_rows = "".join(
        "<tr>"
        f"<td>{escape(item.rule_id)}</td>"
        f"<td class='sev-{escape(item.severity.value)}'>{escape(item.severity.value)}</td>"
        f"<td><code>{escape(item.location.file)}:{item.location.line}:{item.location.column}</code></td>"
        f"<td>{escape(item.defect_type)}</td>"
        f"<td>{escape(item.message)}</td>"
        f"<td>{item.confidence:.2f}</td>"
        "</tr>"
        for item in report.findings
    )
    if not finding_rows:
        finding_rows = "<tr><td colspan='6'>未发现已启用检测器能够识别的问题，该结论不代表源码不存在缺陷。</td></tr>"
    detail_blocks = "".join(
        "<article>"
        f"<h3>{index}. {escape(item.rule_id)} {escape(item.location.file)}:{item.location.line}</h3>"
        f"<pre>{escape(item.evidence)}</pre>"
        f"<p>检测器：<code>{escape(item.detector)}</code></p>"
        f"<p><strong>建议：</strong>{escape(item.suggestion)}</p>"
        "</article>"
        for index, item in enumerate(report.findings, 1)
    )
    file_rows = "".join(
        "<tr>"
        f"<td><code>{escape(item.file)}</code></td>"
        f"<td>{item.code_lines}</td><td>{item.comment_lines}</td>"
        f"<td>{item.function_count}</td><td>{item.cyclomatic_complexity}</td>"
        f"<td>{item.max_brace_depth}</td><td>{item.duplicate_line_ratio:.2%}</td>"
        f"<td>{item.include_count}</td></tr>"
        for item in report.metrics.files
    )
    verification_rows = "".join(
        "<tr>"
        f"<td>{escape(step.name)}</td><td>{escape(step.status)}</td>"
        f"<td>{step.return_code if step.return_code is not None else '-'}</td>"
        f"<td>{step.duration_seconds}</td><td><code>{escape(' '.join(step.command))}</code></td>"
        "</tr>"
        for step in report.verification.steps
    ) or f"<tr><td colspan='5'>未执行命令，状态：{escape(report.verification.status)}</td></tr>"
    verification_logs = "".join(
        "<details>"
        f"<summary>{escape(step.name)} 输出</summary>"
        f"<h4>标准输出</h4><pre>{escape(step.stdout)}</pre>"
        f"<h4>错误输出</h4><pre>{escape(step.stderr)}</pre>"
        "</details>"
        for step in report.verification.steps
        if step.stdout or step.stderr
    )
    diagnostics = "".join(f"<li>{escape(item)}</li>" for item in report.diagnostics)
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>C/C++ 软件缺陷分析报告</title>
<style>
body {{ font-family: "Segoe UI", "Microsoft YaHei", sans-serif; margin: 0; color: #172033; background: #f4f7fb; }}
main {{ max-width: 1100px; margin: 32px auto; padding: 32px; background: white; border-radius: 12px; box-shadow: 0 8px 28px #1d35571a; }}
h1, h2, h3 {{ color: #153b64; }}
.meta {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 8px 20px; }}
.metrics {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 12px; margin: 20px 0; }}
.metric {{ border: 1px solid #dbe4ee; border-radius: 8px; padding: 14px; }}
.metric strong {{ display: block; font-size: 1.5rem; color: #153b64; }}
table {{ width: 100%; border-collapse: collapse; font-size: 0.94rem; }}
th {{ background: #245681; color: white; text-align: left; }}
th, td {{ border: 1px solid #d9d9d9; padding: 9px; vertical-align: top; }}
tr:nth-child(even) td {{ background: #eef4f9; }}
code, pre {{ font-family: Consolas, monospace; }}
pre {{ background: #f5f7fa; padding: 12px; overflow-x: auto; border-radius: 6px; }}
.sev-critical, .sev-high {{ color: #a31d1d; font-weight: 700; }}
.sev-medium {{ color: #9a5a00; font-weight: 700; }}
article {{ margin-top: 26px; }}
</style>
</head>
<body><main>
<h1>C/C++ 软件缺陷分析报告</h1>
<div class="meta">
<div><strong>运行编号</strong><br>{escape(report.run_id)}</div>
<div><strong>解析后端</strong><br>{escape(report.parser_backend)}</div>
<div><strong>构建系统</strong><br>{escape(', '.join(report.project.build_systems) or '未识别')}</div>
<div><strong>源码保护</strong><br>{'未变化' if report.source_unchanged else '检测到变化'}</div>
<div><strong>验证状态</strong><br>{escape(report.verification.status)}</div>
<div><strong>模型状态</strong><br>{escape(report.model_status)}</div>
<div><strong>修复状态</strong><br>{escape(report.repair_status)}</div>
</div>
<div class="metrics">
<div class="metric"><span>文件数</span><strong>{report.metrics.file_count}</strong></div>
<div class="metric"><span>代码行</span><strong>{report.metrics.code_lines}</strong></div>
<div class="metric"><span>函数数</span><strong>{report.metrics.function_count}</strong></div>
<div class="metric"><span>平均函数复杂度</span><strong>{report.metrics.average_function_complexity}</strong></div>
<div class="metric"><span>注释率</span><strong>{report.metrics.comment_ratio:.1%}</strong></div>
<div class="metric"><span>重复行率</span><strong>{report.metrics.duplicate_line_ratio:.1%}</strong></div>
<div class="metric"><span>告警数</span><strong>{len(report.findings)}</strong></div>
<div class="metric"><span>每千行告警</span><strong>{report.metrics.warning_density_per_kloc}</strong></div>
</div>
<h2>程序结构</h2>
<p>表示方式：<code>{escape(report.program_structure.representation)}</code>；函数 {report.program_structure.function_count} 个；AST 节点 {report.program_structure.ast_node_count} 个；CFG 节点 {report.program_structure.cfg_node_count} 个；CFG 边 {report.program_structure.cfg_edge_count} 条；DFG 定义—使用边 {report.program_structure.dfg_edge_count} 条；DFG 状态：{escape(report.program_structure.dfg_status)}。</p>
<h2>告警列表</h2>
<table><thead><tr><th>规则</th><th>严重度</th><th>位置</th><th>类型</th><th>说明</th><th>置信度</th></tr></thead>
<tbody>{finding_rows}</tbody></table>
<h2>证据与建议</h2>{detail_blocks}
<h2>文件指标</h2>
<table><thead><tr><th>文件</th><th>代码行</th><th>注释行</th><th>函数数</th><th>圈复杂度</th><th>最大嵌套</th><th>重复行率</th><th>依赖数</th></tr></thead>
<tbody>{file_rows}</tbody></table>
<h2>编译与测试验证</h2>
<table><thead><tr><th>步骤</th><th>状态</th><th>返回码</th><th>耗时秒</th><th>命令</th></tr></thead>
<tbody>{verification_rows}</tbody></table>{verification_logs}
<h2>诊断信息</h2><ul>{diagnostics or '<li>无</li>'}</ul>
</main></body></html>"""


def write_reports(
    report: AnalysisReport, output_dir: Path, formats: tuple[str, ...]
) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    normalized = {item.lower() for item in formats}
    if "json" in normalized:
        path = output_dir / "report.json"
        path.write_text(
            json.dumps(report.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8"
        )
        written.append(path)
    if "markdown" in normalized or "md" in normalized:
        path = output_dir / "report.md"
        path.write_text(_markdown(report), encoding="utf-8")
        written.append(path)
    if "html" in normalized:
        path = output_dir / "report.html"
        path.write_text(_html(report), encoding="utf-8")
        written.append(path)
    return written
