# CodeGuard：跨平台 C/C++ 软件质量分析与工程管理系统

2026-09-10 交付范围调整：当前仅面向 Windows x64，跨平台开发与新增验收暂缓。Windows CI、锁定工具链、干净构建及运行包检查见 [Windows 构建说明](docs/windows-ci.md)。

当前以 `CodeGuard_课程设计项目方案书.docx`（V1.0，2026 年 9 月）为主方案，正式交付目标为 **Level 3 工程分析闭环**。架构调整为 C++20 Core 静态库、原生 CLI、可选 Qt 6 前端；AI 训练、CFG/DFG 和自动修复归入 Level 4，不再作为基础交付的前置条件。保留原目录名 `cpp-defect-guard`，避免破坏配置和历史产物路径。

## Level 3 规则与工程闭环

v0.3.0 已补齐五类 Clang AST 规则、多 TU 线程池、单写线程批量事务、Windows/POSIX ProcessRunner、源码副本内 CMake/CTest、只读 Git 信息及构建历史。CLI/Qt 共用 Core，SQLite schema 3 保存 Issue 和 BuildTest 记录，查询语言新增 `issues` / `builds`。Qt 已提供问题定位、线程设置、后台构建测试、停止、历史与 Git 日志。

```powershell
.\scripts\run_codeguard.ps1 -Analysis -RunTests
.\scripts\run_codeguard.ps1 -Analysis -Gui
```

使用方法、规则边界、跨平台结果、线程实验与打包见 [Level 3 验收说明](docs/level3-acceptance.md)。测试成功不等于不存在缺陷，示例 `samples/level3-demo` 有意保留五类潜在问题，运行测试只覆盖安全路径。

## 当前主线：工程分析与自研查询语言

已按方案书第 5 章补充纯 C++ 查询模块：Lexer → Parser → Query AST → 字段/类型检查 → 执行计划 → 快照查询。CLI 和 Qt 共用同一实现，可对 `files`、`functions`、`symbols`、`edges` 过滤、排序和投影。支持括号、AND/OR 优先级、多字段排序与 LIMIT；查询不修改数据库。Qt 新增“查询”页，可查看执行计划、错误位置并双击结果定位源码。

```powershell
# 先按下方运行分析版测试，生成样例分析数据库
.\build\codeguard-analysis\codeguard-cli.exe query .\samples\codeguard-relations `
  --database .\build\codeguard-analysis\tests\codeguard\cli-analysis.sqlite3 `
  --query "SELECT name, file, complexity FROM functions WHERE complexity >= 2 ORDER BY complexity DESC LIMIT 20;"
```

查询语法、字段、边界和验收见 [查询语言说明](docs/query-language.md)。最新范围见 [方向调整与当前进度](docs/codeguard-status.md)。规则引擎、并行分析与构建测试管理已经接入；后续重点为 Windows 交付与真实工程验收，Linux Qt/Clang 验收暂缓。

## Clang 符号、复杂度与项目关系图

已新增可选的 **Core 内置 LibTooling 分析**：读取实际 `compile_commands.json`，提取函数/类/变量/字段符号、Clang USR、定义位置、CFG 圈复杂度、直接调用边和预处理器 include 边；支持前缀符号检索、复杂度排行、SCC/递归与循环依赖分析。SQLite 保存分析和覆盖诊断，CLI/Qt 共用纯 C++ DTO。Core 不依赖 Python、Qt 或模型权重，旧深层规则与 CFG/DFG 导出仍保留原入口。

运行新的分析版（本机 LLVM/Clang 开发包和 Qt 已配置）：

```powershell
.\scripts\run_codeguard.ps1 -Analysis -RunTests
# 测试已生成此演示编译数据库；GUI 仍需点击“导入工程并扫描”，选择 samples/codeguard-relations
.\scripts\run_codeguard.ps1 -Analysis -Gui -CompileCommands .\build\codeguard-analysis\tests\codeguard\relations-build
```

CLI 分析、符号检索、复杂度与图命令见 [Clang 接入使用与验收](docs/codeguard-analysis.md)。`analysis-release` 包含 Qt；无 Qt 环境可手动配置 `-DCODEGUARD_ENABLE_ANALYSIS=ON -DCODEGUARD_BUILD_GUI=OFF`。

## Qt 简单前端

已提供三栏工作台：左侧工程树，中间带行号/基础 C++ 高亮的只读源码，右侧可筛选的文件、符号、复杂度、关系、循环与诊断结果。点击文件或结果可定位源码；支持重新扫描和从已有数据库恢复最近工程。

扫描已接入后台任务、阶段进度与取消：扫描时可浏览/筛选旧结果，阻止重复启动；取消在安全边界响应，事务回滚且保留旧快照，越过提交边界后不再接受取消。Clang 现在由多线程池分析各 TU；单个正在执行的 TU 仍在结束后响应取消。

```powershell
.\scripts\run_codeguard.ps1 -Analysis -Gui
```

也可自动载入样例（先执行上面的 `-Analysis -RunTests` 生成样例编译数据库）：

```powershell
.\scripts\run_codeguard.ps1 -Analysis -Gui -Project .\samples\codeguard-relations -Database .\artifacts\codeguard-frontend.sqlite3 -CompileCommands .\build\codeguard-analysis\tests\codeguard\relations-build
```

操作说明和当前边界见 [Qt 前端](docs/qt-frontend.md)。

已补充 [Qt 规模与锁竞争验收](docs/qt-scale-acceptance.md)：2,048 文件清单、128 TU / 4,096 函数和完整窗口回归；修正 SQLite 被占用时取消需等待完整锁超时的问题。

以下轻量模式只做文件扫描、变化统计和持久化，不要求 LLVM：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_codeguard.ps1 -RunTests
# 启动 Qt 初版（本机 Qt 6 已可编译）
.\scripts\run_codeguard.ps1 -Gui -RunTests
```

命令行扫描（数据库父目录需已存在，且必须在被扫描源码目录之外）：

```powershell
.\build\codeguard-core\codeguard-cli.exe scan .\samples\fixed --database .\artifacts\codeguard.sqlite3
.\build\codeguard-core\codeguard-cli.exe status .\samples\fixed --database .\artifacts\codeguard.sqlite3
.\build\codeguard-core\codeguard-cli.exe recent --database .\artifacts\codeguard.sqlite3
```

新旧功能映射、边界、跨平台构建和验收见 [新方案迁移说明](docs/codeguard-migration.md)、[架构](docs/architecture.md)、[四级路线图](docs/roadmap.md)。旧 Python `defectguard` 命令与历史实验仍保留兼容；**下面的“三层次”内容是旧路线的使用记录，不代表当前开发优先级**。上一轮池化/正则融合改动保留为未完成实验，不宣称已有泛化提升。

## 旧路线归档：可靠性与多种子验证

复用本机已有数据集，运行三种模型 × 三个随机种子的实验及完整测试：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_model_stability.ps1 -RunTests
```

每次创建新的输出目录，不重新划分数据。`summary.md` 给出均值与样本标准差，`cases.json` 保留逐函数误报、漏报和各次概率；`-Resume -OutputDirectory <原目录>` 在校验计划、权重及预测后复用完整检查点，不覆盖损坏或不完整的运行。

本机 2026-09-07 已完成 9 次训练；融合模型测试 F1 为 0.582 ± 0.031。测试仍只有 10 个函数、2 个来源组，标准差不是置信区间，不能据此证明融合更优。当前功能、预检命令和验收记录见 [第二层可靠性说明](docs/layer2-reliability.md)。

## Juliet 公开语料首批扩容

已接入受固定 SHA-256 校验的 NIST Juliet C/C++ 1.3.1 导入器。导入器只读取 ZIP，拒绝不安全路径和大小写冲突；它只生成 CMake `OBJECT` 目标进行编译，绝不链接或执行带缺陷用例。生成语料会移除 `CWE`、`good`、`bad` 等显式答案线索，原始文件字节、来源、哈希和标签约定另行保存。

首次 120 个用例对的已验收产物位于 `artifacts/layer2/juliet-20260908-batch120`，详细范围、命令与限制见 [Juliet 导入与首批验收](docs/juliet-import.md)。原始压缩包的官方来源为 [NIST Juliet C/C++ 1.3.1](https://samate.nist.gov/SARD/test-suites/116)。

后续复核发现首批命名清洗遗漏、图特征丢失常量差异和只有负例的定位监督。相关修正、同批分类对照及扩容条件见 [Juliet 清洗与语义特征修正](docs/juliet-correction.md)。新模型使用 `semantic-v2`；旧检查点按原特征版本加载。

## 第二层一键实验

本机已准备 `.venv-ml` 和固定修订的 GraphCodeBERT 权重，直接执行：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_ml_demo.ps1 -Ablations -RunTests
```

每次在 `artifacts/layer2/demo-时间戳` 创建新的实验目录，不覆盖已有检查点。流程包含 24 个自建函数的编译、原生构图、独立标签校验、三组模型训练、去 CFG/DFG 消融、检查点重载评估和模型扫描。`comparison.md` 汇总测试集指标，`model-scan/report.html` 展示规则与模型告警。

新机器准备环境（不影响系统 Python）：

```powershell
.\scripts\setup_ml.ps1 -DownloadEncoder
```

模型环境使用 Python 3.12 或更高版本；基础规则扫描仍不依赖深度学习包。编码器仅从本地加载，扫描不上传源码。数据标注、评估口径、手动命令和当前边界见 [第二层使用与验收说明](docs/layer2-experiments.md)。

## 第一层现有能力

- 递归接入 C/C++ 源码目录，并排除构建目录、第三方代码和缓存目录。
- 自动识别 CMake、Make、Meson、Visual Studio 工程与 `compile_commands.json`。
- 降级解析器提取函数边界并生成可检查的简化 CFG。
- Clang LibTooling 原生工具已在本机完成构建和接入，可输出函数 AST 元数据、圈复杂度和 Clang CFG。
- 提取带类型、变量标识和源码位置的 AST 节点，以及直接调用目标；支持中文和空格路径。
- DG001/DG002/DG004 使用 AST 检测库函数调用、常量数组越界和直接条件赋值，并保留规则启停与严重度过滤。
- 函数内局部标量定义—使用分析支持分支汇合、循环、变量遮蔽和多变量声明；可导出带索引的函数图 JSONL。
- 内置七类规则：危险函数、字面量数组越界、动态内存泄漏、条件赋值、空指针解引用、未初始化局部变量和文件资源未关闭。
- 扫描前屏蔽注释和字符串，减少把示例文字误判为真实代码。
- 计算代码规模、注释率、重复行率、函数复杂度、最大嵌套深度、依赖数量和告警密度。
- 输出结构化 JSON、Markdown、单文件 HTML 报告以及 SQLite 运行记录。
- 在临时副本中执行构建与测试，记录返回码、耗时和完整日志，完成后删除副本。
- 提供 `doctor` 环境诊断、规则目录和按严重度返回非零退出码的 CI 门禁。

未编译原生工具时，报告会明确显示 `fallback-regex`。此时函数边界和 CFG 属于词法近似，不能替代 Clang 语义分析，也不能证明“未报告即无缺陷”。

## 一键运行原生分析与图导出

本机 LLVM/Clang 与 CMake 已配置。推荐入口：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_native_demo.ps1 -RunTests
```

脚本构建原生分析器、生成两个样例的编译数据库、执行原生扫描和隔离 CTest，并导出函数图。报告位于 `artifacts/native/vulnerable` 与 `artifacts/native/fixed`，每个目录的 `graphs` 子目录包含 `graphs.jsonl` 和 `manifest.json`。

图数据的训练标签保持 `null`，不会把规则输出当成真实标签。DFG 当前只覆盖函数内局部标量；数据格式、算法与边界见 [程序图说明](docs/program-graphs.md)。

## 运行基础演示

要求 Python 3.11 或更高版本。本机已经存在 MinGW g++ 时，在 PowerShell 中执行：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_demo.ps1
```

脚本会扫描缺陷样例，并在隔离副本中执行 GCC 编译和测试。结果位于 `artifacts/layer1`：

- `report.json`：版本化机器可读结果，包含项目、程序结构、指标、告警和验证日志。
- `report.md`：适合实验记录和版本管理。
- `report.html`：适合浏览器演示。
- `runs.sqlite3`：保存运行摘要、告警、检测器、置信度和验证状态。

只运行静态扫描、不执行编译测试：

```powershell
.\scripts\run_demo.ps1 -SkipVerify
```

对比缺陷版与修复版：

```powershell
.\scripts\run_comparison.ps1
```

对比结果写入 `artifacts/comparison`。缺陷版应检出 DG001 至 DG007，修复版应为 0 条已启用规则告警；两边都必须通过隔离编译与测试。

## 手动运行

```powershell
$env:PYTHONPATH = "$PWD\src"
python -m defectguard scan .\samples\vulnerable `
  --config .\config\gcc-demo.toml `
  --output .\artifacts\layer1 `
  --verify
```

出现高危或严重告警时让命令返回退出码 3：

```powershell
python -m defectguard scan .\samples\vulnerable `
  --config .\config\default.toml `
  --output .\artifacts\quality-gate `
  --fail-on high
```

环境检查与规则目录：

```powershell
python -m defectguard doctor .\samples\vulnerable --config .\config\gcc-demo.toml
python -m defectguard rules
```

## 运行测试

```powershell
.\scripts\run_tests.ps1
```

测试覆盖七类规则、注释与字符串屏蔽、已释放资源反例、简化 CFG、质量指标、三种报告、SQLite 字段和隔离验证，并确认验证过程不会修改原始源码。

## 其他验证方式

`config/cmake-demo.toml` 提供本机 MSYS2 Clang、CMake 与 CTest 配置；隔离副本中先配置工程，再编译并运行测试：

```powershell
$env:PYTHONPATH = "$PWD\src"
py -3.13 -m defectguard verify .\samples\vulnerable `
  --config .\config\cmake-demo.toml `
  --output .\artifacts\verification
```

工具缺失、命令失败或超时都会写入验证结果，不会被静默忽略。

## 目录结构

```text
cpp-defect-guard/
├─ config/                    扫描、解析、模型、修复和验证配置
├─ docs/                      架构、路线图和各阶段验收记录
├─ native/clang_tool/         Clang LibTooling 原生解析器
├─ samples/
│  ├─ vulnerable/             覆盖七类规则的缺陷样例
│  ├─ fixed/                  对应修复版本
│  └─ ml_corpus/              24 个独立标注的受控函数样例
├─ scripts/                   Windows 演示与测试脚本
├─ src/defectguard/
│  ├─ analysis/               源码屏蔽、静态规则与质量指标
│  ├─ models/                 禁用后端与已训练检查点扫描适配
│  ├─ experiments/            数据划分、编码、训练、评估与实验汇总
│  ├─ parser/                 降级解析器与 Clang 适配器
│  ├─ repair/                 模板、掩码、扩散修复接口
│  ├─ reporting/              JSON、Markdown、HTML 报告
│  ├─ storage/                SQLite 运行记录与兼容迁移
│  ├─ verification/           隔离构建与测试
│  ├─ cli.py                  扫描、验证、构图与模型实验命令
│  ├─ config.py               TOML 配置加载与校验
│  ├─ domain.py               统一领域数据结构
│  └─ pipeline.py             端到端编排与源码指纹检查
└─ tests/                     自动化测试
```

## 三层次对应关系

| 层次 | 当前框架 | 后续工作 |
|---|---|---|
| 第一层 基础交付 | 工程识别、Clang AST/CFG、三类 AST 规则与四类词法规则、质量指标、报告、SQLite、隔离编译测试和环境诊断 | 剩余四类规则的语义迁移、独立标注评测集 |
| 第二层 智能增强 | 原生函数图、受控独立标签、来源划分、GraphCodeBERT 序列/RGCN/融合、分类与 Top-k 评估、消融、模型扫描、完整性校验、多种子汇总 | 代表性公开数据、完整微调与图引导注意力、缺陷类别、扩大评测及区间估计、规则特征消融 |
| 第三层 研究拓展 | PatchCandidate 接口、隔离验证器、构建与测试日志 | 模板修复、候选应用与排序、掩码填充或离散扩散原型 |

详细设计见 `docs/architecture.md`，任务安排见 `docs/roadmap.md`，验收状态见 `docs/layer1-acceptance.md`、`docs/layer2-experiments.md` 和 `docs/layer2-reliability.md`。

## Clang LibTooling 主解析入口

`native/clang_tool` 已实现函数名称、返回类型、参数数量、起止位置、圈复杂度和 Clang CFG 的 JSON 输出。它需要本机 LLVM/Clang 开发包和 CMake。只有原生工具与 `compile_commands.json` 同时可用时，`parser.backend = "auto"` 才会启用 Clang；`parser.backend = "clang"` 条件不足时会直接报错，不会静默降级。

本机已接入 LLVM/Clang 22.1.8 与 CMake 4.4.3，构建产物为 `native/clang_tool/build/defectguard-clang.exe`。完整的重新构建、样例编译数据库生成和原生运行命令见 [原生工具说明](native/clang_tool/README.md)。`config/cmake-demo.toml` 使用严格的 Clang 后端；DG001/DG002/DG004 的检测器标记为 `clang-ast`，其余四条规则暂保留词法实现。原生解析成功时，函数度量使用 Clang 的函数与 CFG 结果。

## 安全边界

- 扫描只读取源码，报告目录必须位于源码目录之外。
- `scan --verify` 的构建和测试只在临时副本内运行；演示脚本另在自带样例的 build 目录生成编译数据库和构建产物，不修改样例源码。
- 模型默认禁用；显式启用后若依赖、检查点或原生解析条件缺失则报错，不静默假装运行成功。
- 模型输出是未经概率校准的二分类疑似告警，不能证明缺陷存在或不存在。
- 报告记录扫描前后的源码指纹比较结果。
- 当前版本不生成或应用补丁。
- 后续候选补丁必须先输出统一差异，再在隔离副本中应用和验证。
