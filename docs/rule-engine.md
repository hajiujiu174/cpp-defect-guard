# P1-2 规则引擎模块化与可信度

更新日期：2026-09-13。当前范围为 Windows C++ 主线的五条 Clang AST 规则，支持按工程启停、设置级别及带理由的精确位置抑制。CLI、Qt 和持久化使用同一 Core 配置，解析覆盖和规则问题分别展示。

## 使用方法

Qt 的“工程设置 → 分析与规则”可逐条选择启用状态和默认/error/warning/info 级别。在“问题”列表选中一行，右键选择“抑制此问题（填写理由）”；保存后重新扫描，问题进入“已抑制”页，保留级别、位置、证据和理由。“工程设置 → 抑制管理”可编辑理由或移除条目，重新扫描后应用。保存失败会显示错误。

CLI 示例使用已有工程配置和分析快照；将路径及行列替换为实际 `issues` 输出：

```powershell
$cli = '.\build\codeguard-analysis\codeguard-cli.exe'
$project = '.\samples\level3-demo'
$database = '.\artifacts\configured-demo.sqlite3'
& $cli config $project --database $database --rule-severity CG001=info --rule-severity CG002=warning
& $cli scan $project --database $database
& $cli issues $project --database $database --severity warning
& $cli suppress $project --database $database --rule CG002 --file main.cpp --line 12 --column 8 --reason '已复核：演示用缺陷，保留课程反例'
& $cli scan $project --database $database
& $cli suppressed $project --database $database
& $cli query $project --database $database --query 'SELECT rule_id, file, line, reason FROM suppressed_issues'
& $cli query $project --database $database --query 'SELECT message FROM rule_diagnostics'
# 恢复默认级别并清空所有抑制；可在 Qt 中只移除某一项
& $cli config $project --database $database --clear-list severities --clear-list suppressions
& $cli scan $project --database $database
```

重复 `--rule-severity` 的首次出现替换已保存的级别映射，后续追加不同 ID；未指定时保留已有映射。`scan` 上的设置只作用于该扫描，`config` 保存工程默认值。`--disable-rule` 和 `--clear-list rules` 延续既有规则启停方式。历史扫描不会随配置编辑而重写。

抑制只能从保存快照中的活动问题生成，必须给出规则、项目相对文件、正数行列以及非空白理由（最多 2000 UTF-8 字节）。每条绑定整份源文件内容指纹；源码变化会使抑制失效，重新产生的活动问题可见。文件删除/排除、规则停用、未命中和解析覆盖不足等未应用情况写入“规则配置诊断”，不会无声丢弃设置。

抑制的键是规则 + 文件 + 行列，作用于该位置的各模板实例；内容指纹采用现有 FNV-1a 64 位变化检测值，并非安全校验。目前不绑定编译参数、工具链或外部头文件内容，改变这些上下文后仍需主动复核原有抑制。抑制不代表修复，也不会将 `failed`/`partial` 改为 `complete`。

## 实现与数据

`core/analyzer/clang_rules` 内的五个独立模块分别实现 CG001—CG005。每个 TU 创建独立 Engine，只注册启用的规则，停用规则不执行检查。共享遍历器处理 lambda 捕获、局部声明、地址上下文和已确定的 `if constexpr` 分支；Clang 适配器负责位置、宏展开证据和快照合并。公共 DTO 不暴露 Clang 或 Qt 类型。

Core 的 `apply_rule_policy` 在新分析结果上执行一次：应用级别，将匹配条目转入 `suppressed_issues`，记录未应用诊断，再与扫描一起事务保存。修改策略通过重新扫描生效，不对旧快照重复调用策略处理。宏无法形成连续源码证据时尝试展开区间，仍失败则显示 AST 节点类型占位，不伪造源码。

SQLite schema 5 在 schema 4 上附加 `suppressed_issue` 和 `rule_diagnostic`。只读兼容 schema 1—5，可写打开旧版本时事务迁移；原扫描保留。工程配置写入 `CodeGuardProjectConfig 2`，继续读取版本 1，新增规则级别映射和抑制列表。未知版本、重复键、损坏字段仍报错，不覆盖旧配置。旧程序不能打开升级后的数据库。

`issues` 查询只含活动问题；`suppressed_issues` 包含相同字段以及 `reason`，`rule_diagnostics` 提供 `message`。Qt 同时显示活动数、已抑制数和策略诊断数，解析状态及 TU 诊断继续独立保留。仅导入文件清单或解析失败时，空问题表不能解释为无缺陷。

## 评测与验收

固定集合见 [样例说明](../samples/rule-evaluation/README.md)。每条规则独立解析 9 个样例，涵盖宏、模板、反例和已知漏报；回归验证启停、证据、基线以及解析成功。按样例目标位置统计，不按模板实例数量累计。

| 规则 | 样例 | TP | FP | TN | FN | 解析失败 |
|---|---:|---:|---:|---:|---:|---:|
| CG001 | 9 | 3 | 0 | 5 | 1 | 0 |
| CG002 | 9 | 4 | 0 | 4 | 1 | 0 |
| CG003 | 9 | 4 | 0 | 5 | 0 | 0 |
| CG004 | 9 | 4 | 0 | 4 | 1 | 0 |
| CG005 | 9 | 5 | 0 | 3 | 1 | 0 |
| 总计 | 45 | 20 | 0 | 21 | 4 | 0 |

这些数值仅描述已标注合成集合。CG001 是 API 风险提醒；CG003 是语法意图提示。规则尚未实现别名、局部值传播、跨过程路径或动态数组区间推断。`typeid` 当前统一跳过，多态表达式实际求值的情况也不覆盖。模板不同实例的同位置问题仍按函数标识保存；本集合没有验证全部实例、所有预处理配置或所有求值上下文。

本机 Clang/Qt 全量回归 85 项：84 通过、1 跳过；Core 干净构建 50 项：49 通过、1 跳过，均跳过 Windows 符号链接权限项。新增测试覆盖配置版本 1 读取、schema 4→5 迁移、理由与证据往返、每条规则三个级别的保存/恢复、真实 CLI 流程、全部规则关闭后的覆盖、真实解析失败及 Qt 控件流程。普通/紧凑抑制页和问题导航截图已检查；自动 offscreen 控件测试不代表所有实体桌面环境。

最终远端 CI、真实工程和运行包结果将在本页及 [证据 JSON](evidence/rule-engine-20260913.json) 中登记，以被测提交为准。P1-2 完成后按优先级进入 P1-3 报告与版本对比。
