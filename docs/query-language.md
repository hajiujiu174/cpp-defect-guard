# CodeGuard 自研查询语言

更新日期：2026-09-09。对应方案书第 5 章及 Level 3 查询模块。

v0.3.0 增加 `issues` 与 `builds`，覆盖规则与构建测试记录。字段和完整工程闭环见 [Level 3 验收](level3-acceptance.md)。下文保留最初四类实体的语法说明与查询阶段验收；最新整体测试结果以 Level 3 文档为准。

2026-09-13 P1-2 增加 `suppressed_issues`（`issues` 的全部字段加字符串 `reason`）及 `rule_diagnostics`（字符串 `message`）。`issues` 仅含活动问题，已抑制问题的证据、理由及未应用诊断可独立查询；详见 [规则引擎](rule-engine.md)。规则诊断也可在仅文件清单的快照中存在，解析状态不因抑制改变。

用户查询由纯 C++ 的 Lexer、递归下降 Parser、Query AST、语义分析、执行计划和执行器处理。输入只用于查询当前 ScanResult 快照，不交给 SQLite 执行。CLI 从只读数据库连接恢复快照，Qt 查询当前已载入的快照，两端使用同一个 `execute_query` 接口。

## 使用方法

先运行 `scripts/run_codeguard.ps1 -Analysis -RunTests` 生成样例编译数据库与扫描数据库，再执行：

```powershell
.\build\codeguard-analysis\codeguard-cli.exe query .\samples\codeguard-relations `
  --database .\build\codeguard-analysis\tests\codeguard\cli-analysis.sqlite3 `
  --query "SELECT name, file, complexity FROM functions WHERE complexity >= 2 ORDER BY complexity DESC LIMIT 20;"
```

启动 `scripts/run_codeguard.ps1 -Analysis -Gui`，导入工程或打开分析数据库后切换“查询”页。输入查询并点击“执行只读查询”；点击“查看执行计划”展开流水线。双击包含 `file` 的结果定位文件，同时投影 `line` 可跳转对应行。

无 Clang 版可查询 `files` 和 `builds`。例如，先用 `core-debug` 的 CLI 扫描工程，再对该扫描数据库执行 `SELECT file, lines FROM files ORDER BY lines DESC LIMIT 20`。函数、符号、关系及规则问题需要已请求的 Clang 分析快照。

## 语法

```text
query       := SELECT ('*' | field (',' field)*) FROM table
               [WHERE or_expr]
               [ORDER BY field [ASC | DESC] (',' field [ASC | DESC])*]
               [LIMIT nonnegative_integer] [';'] EOF
or_expr     := and_expr (OR and_expr)*
and_expr    := primary (AND primary)*
primary     := '(' or_expr ')' | field comparison literal
comparison  := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
literal     := signed_integer | single_quoted_string
```

关键字、表名和字段名不区分 ASCII 大小写；字符串值区分大小写，按 UTF-8 字节序比较，不提供本地化排序。字符串中的单引号写为两个单引号，例如 `'中文/O''Brien.h'`。整数采用有符号 64 位范围，禁止溢出和数字/字符串隐式转换。布尔字段用 0/1 表示。

AND 优先于 OR，括号可改变顺序。ORDER BY 支持未出现在 SELECT 中的字段；数字按数值排序。多键相同时保留输入快照中的行序。LIMIT 在排序后执行，0 返回空结果但仍校验完整查询；省略 LIMIT 返回所有匹配行。

不支持 JOIN、子查询、聚合、LIKE、NULL、浮点数字面量、注释、写入语句或多条语句。错误包含行号与字节列号；中文内容后的列号不是 Unicode 字符列号。

## 逻辑表与字段

| 表 | 字符串字段 | 整数字段 | 含义 |
|---|---|---|---|
| files | file, language, hash | lines, size, mtime | 当前清单；lines 为物理行数，size 为字节数，mtime 沿用存储层值 |
| functions | name, file, symbol_id | line, complexity, lines, parameters | 存在函数度量的符号；complexity 为 -1 表示 CFG 不可用 |
| symbols | name, file, kind, symbol_id | line, column, definition, external | 当前已保存符号，含定义/声明与外部标志 |
| edges | kind, source, target, file | line, column | call/include 边；调用端点是 USR，包含端点是文件标识 |

`functions` 是符号与函数度量合成的逻辑视图，不声称包含所有函数声明。各表只对应当前扫描批次，不跨历史扫描。分析不完整时仍返回已保存数据，同时报告 `partial` 等原始状态；未执行 Clang 时查询分析表报错，避免用空结果冒充没有函数或关系。

## 核心接口与实现

- `lex_query` 返回 Token 序列和源码字节位置。
- `parse_query` 返回公开的 `QueryAst`，包含投影、表、表达式节点、排序键及 LIMIT；表达式树以节点索引保存。
- 内部语义分析器按固定 Schema 解析字段索引并检查字面量类型，即使输入表为空也拒绝非法字段。
- `execute_query` 完成语法/语义检查、快照行生成、短路布尔过滤、稳定排序、限制与投影，返回列名、带类型值、扫描批次、分析状态和计划。

例如方案书中的查询会形成 `Snapshot → Scan(functions) → Filter → StableSort(complexity DESC) → Limit(20) → Project(name, file, complexity)`。计划包含 AST 节点数量；完整树可通过 `QueryAst` 查看。CLI 的标准输出为制表符表格，字符串里的制表符、换行、回车及反斜杠会转义；计划和行数统计写入标准错误流。正常返回 0，参数/查询错误返回 1，不完整快照返回 2。

## 边界与后续工作

查询输入最多 64 KiB、4096 Token、256 表达式节点，括号深度最多 64。字段使用白名单；输入文本不拼接为数据库语句，查询无需迁移 SQLite Schema。

当前在内存生成逻辑行并执行，未下推数据库索引。设快照行数 N、表达式节点数 E、匹配行数 M、排序键数 K，过滤约 O(N×E)，排序约 O(M log M×K)，另有行生成和投影成本。GUI 当前同步执行，显示最多 2000 条返回行并明确提示截断，CLI 可获取全部结果；该显示上限不限制 Core 的计算和内存。大规模查询异步化、分页与查询性能基准仍待完成。

## 验收

新增六组核心测试，覆盖方案书查询、AST、括号和优先级、数值及多键排序、稳定排序、字符串转义、整数边界、语法/语义错误、输入资源限制和解析覆盖状态。

CLI 集成在真实样例分析数据库上查询函数、验证非法类型被拒绝，并对查询前后数据库及源码做 SHA-256 比较。Qt offscreen 回归使用实际按钮和表格信号执行查询、检查返回行与错误清空、验证双击定位，生成常规与紧凑窗口截图用于检查。验收不包含人工走完文件对话框，也不代表 Linux 或整个 Level 3 已完成。

2026-09-09 本机验证结果：

- `core-debug` 配置与编译成功；28 项 CTest 中 27 项通过，1 项符号链接权限测试跳过。
- `analysis-release` 配置与编译成功；45 项 CTest 中 44 项通过，1 项符号链接权限测试跳过，包含新增六组查询测试和原有规模/取消回归。
- 查询页布局调整后，重新编译 GUI 并重跑样例配置、分析 GUI、128 TU 完整窗口和轻量 GUI，共 4 项全部通过。已打开检查 1480×860 与 1080×680 查询页截图，确认输入、状态和结果行可读。
- 独立 Core CLI 对 `samples/fixed` 执行文件查询，得到 `main.cpp / 25`；未启用 Clang 时函数表查询按预期返回 1。数据库和样例源码的查询前后 SHA-256 一致，演示数据库位于 `artifacts/codeguard-query-20260909/inventory.sqlite3`。

本次未运行模型训练或旧 Python 全套测试，因为没有修改这些模块；未进行 Linux 编译验证。
