# CodeGuard 系统架构说明

## 当前架构（新方案 V1.0）

2026-09-13：当前存储版本为 schema 4，增加工程配置表及分析/构建配置溯源。`core/project/configuration.cpp` 提供纯 C++ 配置校验、编码、发现与恢复；Qt 设置页及 CLI 复用该模型。下文 schema 2/3 说明保留迁移顺序，具体新流程见 [配置管理](project-configuration.md)。

```text
cli/                         gui/qt/（仅 Qt Widgets 表示层）
  └──────────────┬──────────────┘
      core/include/codeguard/application.hpp
             Application API / 标准 C++ DTO
                   │
         core/project/scanner.cpp
                   │
         core/parser/clang_analysis.cpp（可选 LibTooling）
              ├─ 符号 / CFG 函数度量 / TU 覆盖诊断
              └─ 直接调用 / include → core/graph（BFS / DFS / SCC / 拓扑）
                   │
         IDatabase → SqliteDatabase
                   │
         独立的 CodeGuard inventory 数据库

CLI 只读恢复快照 / Qt 当前快照 → core/query/query.cpp
     Lexer → Parser → Query AST → Semantic Plan → Executor → QueryResult

native/clang_tool/  → 已有独立 LibTooling 可执行工具，顶层 CMake 可选构建
src/defectguard/    → 原 Python 原型，兼容保留；尚未迁入新 Core
```

`codeguard-core` 是 C++20 静态库，基础依赖 STL 与 SQLite C API；`CODEGUARD_ENABLE_ANALYSIS=ON` 时直接链接 Clang/LLVM。无 Qt 类型或 Python 运行时依赖。`codeguard-cli` 和 Qt 界面共用 `import_project`；数据库路径必须位于源码树之外。扫描器不跟随符号链接、不执行源码或构建脚本；按相对路径稳定排序。单文件/目录错误进入 DTO 的 diagnostics，不中断其他目录；文件清单不完整时不替换最后一个完整快照。

SQLite schema 2 在 `project → scan → file` 上新增 `analysis / translation_unit / symbol / function_metric / graph_edge / coverage`。已知 schema 1 可事务式增量升级，历史文件快照不变；只读打开 schema 1 不做升级。SQL 参数均绑定；每次扫描与分析一起事务提交，失败回滚。独立 `application_id` 拒绝旧 Python runs 数据库及未知版本。CLI 查询命令使用只读连接。

当前 schema 3 进一步新增 `issue / build_run / build_step`，保存规则证据与构建测试全过程；analysis 保存实际线程数和分析耗时。schema 2→3 为附加事务迁移。多 TU 线程池各自产生 DTO，协调线程确定性合并后，把完整快照交给单一数据库写线程提交；不进行多个 SQLite 连接的并发写入。工程管理经 `core/testing/engineering.cpp → ProcessRunner` 执行副本内 CMake/CTest，并只读获取 Git 信息，详见 [Level 3](level3-acceptance.md)。

分析器逐 TU 隔离 Clang 上下文，成功单元才合并符号/指标/边；失败单元丢弃不可靠 AST，诊断单独入库。因此“文件清单完整、部分 TU 解析失败”可保存 `partial` 快照，而“文件枚举或分析期间源码变化”不会保存。`complete` 仅说明所有清单中的源文件 TU 成功，不表示孤立头文件也被分析。符号身份采用 USR，内部/无链接符号补充文件路径，声明合并优先定义；图保留定位信息并按端点去重计算结构。详细语义与限制见 [Clang 接入说明](codeguard-analysis.md)。

文件变化依据 `hash + size + mtime`，其中 hash 为带版本名的 FNV-1a 64 位变化检测值，不承担安全校验；历史模型数据的 SHA-256 校验不变。目前仍读取所有文件，只统计新增/变化/未变/删除，**未实现 AST 结果复用或增量解析提速**。GUI 用一个后台任务协调多 TU 线程池，构建测试使用独立后台控制器，两类任务在一个窗口内互斥。

已落地 `core/parser`、`core/graph`、`core/query` 与符号/函数度量 DTO。自研查询按固定 Schema 对快照内存执行，不把用户文本交给 SQLite；CLI/GUI 共用接口，详细语法与边界见 [查询语言](query-language.md)。前缀检索为内存过滤排序；SQLite 有名称索引，但还没有独立高性能引用索引模块。规则、线程池、进程与构建测试已经补齐，后续集中于引用索引、增量复用和更广泛验证。

`core/analyzer` 提供规则目录，Clang 适配器内执行五类 AST 检查；`core/include/codeguard/thread_pool.hpp` 提供有界池，`core/process` 提供平台进程实现，`core/testing` 编排构建测试与 Git，`gui/qt/build_task` 负责后台界面控制。尚待扩展的内容包括引用查询、增量结果复用及更深的数据流规则。当前阶段及验收限制见 [方向与进度](codeguard-status.md)。首轮调整见 [历史迁移说明](codeguard-migration.md)。以下保留原 Python 架构记录，供后续迁移比对。

---

# 旧 Python 原型架构（归档）

## 1 设计目标

框架优先保证第一层可以独立运行和验收，同时让第二、三层通过接口替换接入。各模块共享统一数据结构，解析失败、构建失败、测试失败和后端未启用都必须形成显式状态或日志。

## 2 数据流

```text
C/C++ 源码目录
      │
      ▼
项目接入与只读文件发现
      │
      ├──────────────► Clang LibTooling 原生工具
      │                    │
      │                    └─ AST/CFG、变量读写事件与三类 AST 规则
      ▼
原生或降级结果统一为 ParsedFile
      │
      ├─► 静态规则 ──────────────┐
      ├─► 质量指标               │
      └─► 已训练模型后端         │
                                  ▼
                            统一 Finding
                                  │
             ┌────────────────────┴───────────────────┐
             ▼                                        ▼
      JSON Markdown HTML                         SQLite 运行记录
             │
             ▼
       修复候选接口
             │
             ▼
       临时副本隔离验证
       语法 编译 单测 回归
```

## 3 模块边界

### 3.1 项目接入与配置

`config.py` 只负责读取 TOML 并形成不可变配置。CLI 负责解析用户参数和检查输出路径，禁止把报告写入被分析源码目录。源码发现排除构建目录、依赖目录和缓存目录。

### 3.2 程序表示

`FallbackProjectParser` 提供文件读取、函数边界、返回类型、参数数量、源位置和近似 CFG。`ClangToolClient` 是主路线适配器；原生工具输出类型化 AST、源码范围、圈复杂度、Clang CFG 与有序读写事件。`analysis/dataflow.py` 使用固定点到达定义分析构建函数内局部标量 DFG，`graphs.py` 导出带源码切片及节点索引的图数据。具体算法与边界见 `program-graphs.md`。

统一 `ParsedFile` 隔离上层分析与底层解析实现。Clang 结果完整校验后替换对应文件的函数结构，质量度量使用同一份函数数据；原生解析失败时显式报错或记录降级原因。未参与翻译单元的头文件保留词法分析，并列出覆盖范围。

### 3.3 分析检测

`RuleEngine` 以规则编号选择启停，并按最低严重度过滤，统一输出 `Finding`。DG001/DG002/DG004 在原生覆盖范围内采用 AST 结果，跳过对应词法规则以避免重复告警；其余四类资源、空指针与初始化规则仍为词法启发式。原生告警的 `detector` 为 `clang-ast`，词法告警为 `rule`。

质量指标单独计算，包括规模、注释率、重复行率、函数复杂度、嵌套深度、依赖数量和告警密度，不把指标直接等同为缺陷。第二阶段的模型后端同样输出 `Finding`，用 `detector` 和 `confidence` 与规则结果区分。

`experiments/data.py` 合并外部标签、检查源码/图结构、按来源组划分并去重；`encoder.py` 使用冻结 GraphCodeBERT 做序列均值池化；`network.py` 使用两层 RGCN 编码 AST/CFG/局部 DFG，拼接序列向量进行二分类，并以 AST 节点行聚合得分提供候选定位。序列基线不提供行定位。`runner.py` 仅用训练集拟合节点类型词表、权重和定位损失权重，验证集选择轮次，独立测试集评估。

`models/trained.py` 加载 safetensors 检查点并核对编码器指纹，通过 `graphs.function_record` 复用训练导出时的图构造。模型告警使用 `ML001` 和 `model-sequence` / `model-gnn` / `model-fusion`，默认中等严重度，同样遵守严重度过滤。默认不输入标签、路径、组 ID 或规则告警特征；缺失原生覆盖的文件不会伪造模型图，覆盖数及截断数进入状态。

### 3.4 修复与验证

`PatchGenerator` 接口接收告警和程序表示，输出 `PatchCandidate`，但当前禁用自动生成，避免未经验证的文本修改被误认为正确修复。

`IsolatedVerifier` 复制原始工程后执行构建和测试命令。每一步记录命令、返回码、耗时、标准输出、错误输出和状态；任一步失败即停止后续步骤。后续补丁应用也必须插入复制之后、构建之前。

### 3.5 报告与存储

报告 JSON 使用版本化模式，Markdown 适合实验记录，HTML 适合演示。三种报告均包含程序结构、文件指标、告警和编译测试结果。SQLite 保存完整报告、可查询告警、检测器、置信度、源码保护与验证状态，并对旧数据库自动补列。

## 4 关键接口约定

| 接口 | 输入 | 输出 | 约束 |
|---|---|---|---|
| ProjectParser | 源码根目录 | ParsedFile 列表 | 失败必须进入 diagnostics |
| Rule | 单个 ParsedFile | Finding 列表 | 必须包含规则编号、位置、证据和建议 |
| DetectorBackend | ParsedFile 列表 | Finding 列表 | 必须报告后端和置信度 |
| PatchGenerator | Finding 与 ParsedFile | PatchCandidate 列表 | 只生成差异，不直接改原工程 |
| IsolatedVerifier | 源码、构建命令、测试命令 | VerificationResult | 只在临时副本执行 |

## 5 启用 Clang 主解析的顺序

1. 安装并固定 LLVM/Clang 开发包与 CMake 版本。
2. 编译 `native/clang_tool`，确认 `defectguard-clang` 可执行文件可运行。
3. 为目标工程生成并验证 `compile_commands.json`。
4. 在配置中指定原生工具与编译数据库，先用 `doctor` 检查，再执行扫描。
5. 对照降级结构与 Clang 结构，补充宏、模板、头文件和解析失败用例。
6. 使用 `export-graphs` 导出 AST/CFG/局部 DFG，为后续 GraphCodeBERT/GNN 和 Top-k 定位准备数据。图保持未标注，不把规则输出当作训练真值。
