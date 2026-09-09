# Clang 符号、复杂度、调用图与依赖图

## 当前实现

分析在 `codeguard-core` 内直接调用 LibTooling，不启动 Python、不执行编译数据库里的编译器命令，也不改变旧 `defectguard-clang` JSON 协议。Clang 由编译时匹配的 resource-dir 提供 builtin headers。

| 结果 | 提取方式 | 边界 |
|---|---|---|
| 函数/方法、类/结构体/联合体、变量/参数/字段 | AST + Clang USR | 定义优先；未实现宏索引和完整引用索引 |
| 函数长度、参数数、圈复杂度 | 源位置 + CFG | 圈复杂度为 `1 + Σ max(0, succ_size−1)`，与旧工具一致；CFG 失败记 -1 |
| 直接调用图 | 函数体 CallExpr / 显式构造调用 | 静态直接目标；不承诺完整虚分派、函数指针或隐式析构图 |
| include 图 | PPCallbacks 实际解析的包含指令 | 保留被 include guard 跳过的包含边；不猜测未激活条件分支 |
| 图结构 | 迭代 DFS / Kosaraju SCC、BFS、Kahn 拓扑 | 支持自环、非连通分量和深链；SCC 成员不是有序环路径 |

外部直接调用目标保留外部符号与位置。include 边仅从清单内文件出发，指向清单内相对路径或外部已解析绝对路径；不递归分析整个系统头文件依赖树。节点数包含孤立符号/文件，边数按端点对去重；数据库中的边仍保留调用/include 位置。拓扑序按 **起点在终点之前** 输出；对 include 图而言，若要依赖优先顺序需反转，且它不能直接替代完整构建系统。

同一头文件定义在多个 TU 中出现时，符号按 USR 去重，位置优先定义并确定性排序，复杂度保留最大值，边为成功 TU 的并集。这不是单一运行时配置下的完整程序图。内部或无链接符号额外包含项目相对文件路径，避免不同目录中同名文件的静态函数被合并。

lambda 函数体不归到外层函数；尚未提供独立 lambda 调用建模。全局初始化中的调用不在函数调用图范围。`indirect_calls` 记录无法解析直接目标的调用（包括被跳过的隐式构造目标），不凭函数名字连线。系统/第三方源码不因被调用而全量纳入项目分析。

## 编译数据库与失败处理

- 显式传入目录或 `compile_commands.json`；匹配规范化源文件路径，不猜默认编译参数。
- 保留宏、include 路径和语言标准；多条配置对应同一 TU 时标记 `ambiguous_command`，需提供单配置数据库。
- 缺少条目记 `missing_command`，语法失败记 `parse_failed`。只合并成功 TU 的 AST，保存失败诊断；单个失败不阻断其余 TU。
- `complete` 指清单中所有源文件 TU 成功，不代表每个头文件都被引用；`covered_files` / `uncovered` 独立展示。
- 插件、response file、额外编译器配置、PCH/module 和部分写文件选项被拒绝；LibTooling 采用 syntax-only 并去除依赖文件输出。这不是执行不可信源码的安全沙箱，不能用于保证恶意编译器输入不会触发 Clang 缺陷。
- 源码默认只读。分析前后重新核对清单/Hash/时间/大小，若变化则不保存不一致快照。没有实现 AST 增量缓存和多线程分析。
- CLI 扫描正常返回 0，输入/环境错误返回 1，不完整扫描或部分/失败分析返回 2。查询部分分析也显示状态并返回 2，不把部分结果伪装为完整结果。

## 构建与演示

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\run_codeguard.ps1 -Analysis -RunTests
```

等价于 `cmake --preset analysis-release`、`cmake --build --preset analysis-release` 和 `ctest --preset analysis-release`。当前 PPCallbacks 适配器锁定 LLVM/Clang **22.x** 开发包，不假设跨大版本 API 兼容。本机 Release 构建已验证；MinGW 的静态 Clang Debug 链接曾报 `IMAGE_REL_AMD64_REL32`，尚未解决，因此分析预设明确采用 Release。未安装额外链接器或修改系统配置。基础 `core-debug` / `desktop-debug` 仍是无 Clang 的轻量模式。

测试会为 `samples/codeguard-relations` 生成 `build/codeguard-analysis/tests/codeguard/relations-build/compile_commands.json`，该样例只配置/编译，不执行递归函数。手动运行：

```powershell
$cli = '.\build\codeguard-analysis\codeguard-cli.exe'
$source = '.\samples\codeguard-relations'
$db = '.\artifacts\codeguard-relations.sqlite3'
$commands = '.\build\codeguard-analysis\tests\codeguard\relations-build'
& $cli scan $source --database $db --compile-commands $commands
& $cli symbols $source --database $db --prefix overloaded
& $cli metrics $source --database $db
& $cli graph $source --database $db --kind call
& $cli graph $source --database $db --kind include
```

Qt：

```powershell
.\scripts\run_codeguard.ps1 -Analysis -Gui -CompileCommands .\build\codeguard-analysis\tests\codeguard\relations-build
```

点击“导入工程并扫描”，选择 `samples/codeguard-relations`，再选源码目录外的数据库。可查看文件、符号、复杂度排行、关系边、循环分量、解析覆盖与诊断。已增加三栏工程树/只读源码/结果区、基础高亮、行号、筛选与点击定位，以及后台扫描、阶段进度、取消和安全关闭，见 [Qt 前端](qt-frontend.md)。尚无图形布局或多 TU 线程池。

## 数据与兼容

Schema 2 新增 `analysis / translation_unit / symbol / function_metric / graph_edge / coverage`；所有结果与扫描批次关联，参数化写入、事务回滚。首次以可写连接打开已知 schema 1 时只做新增表的事务升级，旧快照不改；只读打开 v1 不升级。旧 Python runs 数据库仍拒绝误用。新二进制可以只读查询新旧 CodeGuard 清单；旧 v1 二进制不支持打开已升级 v2 的数据库。

USR 是扫描内键，数据库用 `(scan_id, usr)` 隔离批次。当前前缀检索在 DTO 上执行，未宣称百万符号交互性能。所有模型数据、标签和检查点均未修改。

## 后台接入补充验收（2026-09-09）

Core 的 `ScanContext` / `ScanControl` 使用标准 C++ 回调与原子状态，不依赖 Qt；Clang 每个任务使用独立物理文件系统，源文件相对路径按编译命令工作目录解析。GUI 只在主线程更新控件；SQLite 取消会回滚数据事务，提交与取消有原子边界。具体行为和初次初始化空库的边界见 [Qt 前端](qt-frontend.md)。

分析 Release：33 项通过、1 项权限跳过；轻量桌面版：23 项通过、1 项跳过；纯 Core：21 项通过、1 项跳过。新增完整窗口后台交互/取消/安全关闭、取消后重扫、部分完成状态、新旧数据库回滚与并发提交边界测试。旧 Python/模型链路本轮未改动、未重跑。

后续新增规模与锁竞争验收：分析版 38 项、轻量桌面版 26 项、纯 Core 21 项通过，各 1 项权限跳过。修正数据库占用时的取消延迟，并测试 2,048 文件清单、128 TU / 4,096 函数与完整窗口，见 [Qt 规模与锁竞争验收](qt-scale-acceptance.md)。这仍不是多线程吞吐或第三方真实工程基准。

## 前轮验收记录（2026-09-08，保留原始结果）

- Clang 22.1.8 / SQLite 3.53.4 / Qt：分析 Release 构建成功，25 项 CTest 通过，1 项 Windows 符号链接权限测试明确跳过。
- 测试覆盖实际编译参数、中文路径、共享头文件去重、重载、不同目录同名文件静态函数、函数/类/变量、CFG 条件复杂度、跨文件互递归、include guard 循环、孤立头文件、函数指针、缺失/歧义/不安全命令、单 TU 语法失败保留部分结果、v1 迁移和重读，以及 12,001 节点深链。
- CLI 端到端完成扫描、数据库重读、前缀检索接口/指标/两类图查询，源码 SHA-256 保持不变；GUI offscreen 验证分析表格与数据库一致，已打开截图检查，无文字重叠或表格截断。不等于完整交互或大工程响应性验收。
- 演示结果：6 文件，5 个文件被 TU 覆盖，24 符号，9 个函数度量；调用图 9 节点/6 边/1 个循环 SCC，include 图 6 节点/5 边/1 个循环 SCC；`alpha` 复杂度 3、`beta` 复杂度 2；1 个函数指针调用未强行解析，`uncovered.h` 明确未覆盖。
- 演示工程的两个 OBJECT 源文件均通过 Clang 实际编译，未链接或运行递归函数。
- 无 Clang 的 `core-debug`：15 项通过，1 项符号链接权限跳过，保持轻量部署边界。
- 旧 Python / 原生 / 模型链路开启 ML/native 强制依赖后，120 项回归全部通过，旧检查点与旧分析器仍兼容。
- 现有原生分析器源码验收：使用 `build/codeguard-native/compile_commands.json` 分析 `native/clang_tool`，1 文件/345 物理行，提取 349 个符号（含外部调用目标）、15 个函数度量，状态 complete；16 个未解析直接目标的调用明确计数。数据库位于 `artifacts/codeguard-analysis-20260908-220138/native-project.sqlite3`，源码前后 SHA-256 均为 `730AEE92D49D5AF13CCC0576BA65C3265A4AB799A6EB3996691340C751B92AC4`。
- 该实测暴露重复路径规范化的开销，已增加每 TU 路径缓存。修正后单次耗时约 5.87 秒；不是受控性能基准，未据此声称并行或千文件性能。修正前的慢扫描已停止，没有产生伪造的成功快照。
- 自动验收数据库与文本输出：`build/codeguard-analysis/tests/codeguard/cli-analysis.sqlite3*`；Qt 截图：`build/codeguard-analysis/tests/codeguard/gui-analysis/gui-smoke.png`。

Linux 全量编译、多个外部开源项目、性能基准、线程池、深层静态规则和 Level 3 自研查询语言尚未验收。
