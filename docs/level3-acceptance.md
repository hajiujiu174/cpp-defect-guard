# CodeGuard Level 3 工程闭环

本轮依据课程方案书补齐规则引擎、并行分析、构建测试与 Git 管理。当前运行链路为：导入工程 → Clang 分析与五类规则 → 合并结果 → 单写线程提交 SQLite → 查询与定位 → 在源码副本中配置、构建和测试 → 保存日志及扫描版本关联。

## 使用入口

```powershell
# 构建并测试完整 Windows 版本
.\scripts\run_codeguard.ps1 -Analysis -RunTests

# 启动桌面工作台
.\scripts\run_codeguard.ps1 -Analysis -Gui

# 演示工程已由测试生成编译数据库
.\build\codeguard-analysis\codeguard-cli.exe scan .\samples\level3-demo `
  --database .\artifacts\level3.sqlite3 `
  --compile-commands .\build\codeguard-analysis\tests\codeguard\level3-demo-build --threads 4

# 查看告警及证据
.\build\codeguard-analysis\codeguard-cli.exe issues .\samples\level3-demo `
  --database .\artifacts\level3.sqlite3

# 副本构建与测试，包含配置、编译及 CTest 三个阶段
.\build\codeguard-analysis\codeguard-cli.exe build .\samples\level3-demo `
  --database .\artifacts\level3.sqlite3 --output .\artifacts\level3-builds --jobs 4 --timeout 120

# 查看历史与查询当前扫描的测试结果
.\build\codeguard-analysis\codeguard-cli.exe builds .\samples\level3-demo --database .\artifacts\level3.sqlite3
.\build\codeguard-analysis\codeguard-cli.exe query .\samples\level3-demo `
  --database .\artifacts\level3.sqlite3 --query "SELECT stage, status, tests_total, tests_failed FROM builds;"
```

数据库父目录须存在，数据库与构建输出目录必须位于源码目录之外。`scan` 支持 `--threads 0..64`，0 根据硬件并发数自动选择、最多 8 个，并按实际 TU 数量缩减。CLI 的 Ctrl+C 可取消扫描或构建；扫描取消不提交新快照，构建取消仍保存已经产生的日志。

Qt 顶部配置分析线程数；“问题”页可筛选和点击定位，完整证据及建议在表格中；“构建测试”页输入输出目录后点击“构建并测试副本”。可通过“设置”指定目标、C++ 编译器、并行数与阶段超时，“历史”载入最近 50 次运行。阶段结果与日志分开显示，Git 页显示所选构建记录对应的分支、提交、状态、日志与差异。源码树、扫描记录和构建记录始终保留关联。

## 五类规则

| 规则 | 严重度 | 实际检测范围 | 排除与限制 |
|---|---|---|---|
| CG001 | warning | 已解析到库函数的 gets/strcpy/strcat/sprintf 调用 | 危险 API 提醒，不直接断言已经溢出 |
| CG002 | error | 定长数组加编译期常量下标，越过合法边界或为负 | 支持 `&array[N]` 合法尾后地址；不推断动态下标区间 |
| CG003 | warning | if/while/do/for 条件直接使用未加额外括号的赋值 | 额外括号或显式比较表示有意赋值，不报此规则 |
| CG004 | error | 返回自动局部变量地址或局部数组退化指针 | 排除 static 与引用变量；不追踪复杂别名 |
| CG005 | error | 对可证明是空指针常量的表达式进行解引用或箭头访问 | 不进行局部变量赋值传播或跨过程路径推断 |

规则读取 Clang AST，避开 sizeof/noexcept/typeid 等当前不分析的求值上下文。每条 Issue 保存规则、级别、文件、行列、消息、源码证据、建议、函数标识和检测器。源片段最多 500 字节，宏无法形成可靠连续源码区间时证据可能为空；位置采用展开位置。

失败 TU 不输出其不可靠告警，分析状态及覆盖诊断同时保留。共享头文件中的相同规则位置与函数按稳定键去重。无告警不能证明无缺陷。示例工程特意保留五个潜在问题，但运行测试只覆盖安全路径，因此“测试通过且有告警”是预期结果。

## 并行与单写线程

`ThreadPool` 使用标准 C++ 线程、互斥锁和条件变量，具有有界任务队列；分析队列容量为工作线程数的两倍。每个 TU 拥有独立 Collector、CompilerInstance、诊断器和物理 VFS，不共享 Clang AST 或 SQLite 连接。

任务返回普通 DTO，通过 future 传播异常。协调线程按文件清单顺序合并符号、函数度量、关系边和规则结果；相同复杂度时维持稳定输入次序。进度回调串行化，避免回调接收者发生竞争。取消在队列任务入口和 TU 边界检查，已运行的单个 TU 不强制中断；所有已接受任务结束后再释放线程池。

合并、源码一致性检查完成后，将完整快照作为一个批次送入单写线程的有界队列。写线程独占 SQLite 可写连接，一次事务提交所有文件、分析、规则与覆盖结果。当前采用“并行分析、合并后批量提交”，不是逐文件流式落库；保留结果 DTO 的内存成本，并通过原有提交栅栏保证取消与 COMMIT 只会有一方成功。

数据库升级为 schema 3，以附加迁移保留 schema 1/2 的历史记录。新增 issue、build_run、build_step，analysis 增加 workers 和 elapsed_ms。只读打开旧版本不迁移；写入时使用事务升级。

## 进程与构建测试

`ProcessRunner` 接受可执行程序及参数数组，不经过 shell。Windows 使用 CreateProcessW、明确的句柄继承列表和 Job Object；POSIX 使用 fork/exec、独立进程组及非阻塞管道。两端分开捕获 stdout/stderr，提供退出码、耗时、启动失败、超时和取消状态。

输出每个流保留前 4 MiB，超出仍持续排空管道并标记截断，避免死锁或无限占用内存。Windows 取消/超时结束 Job 内子进程树；Linux 结束该进程组。主动脱离进程组的程序不在 Linux 进程组保证之内，源码副本也不等于操作系统沙箱。

构建前必须存在扫描快照，并检查当前 C/C++ 文件内容与快照一致。项目复制到新建唯一目录，跳过符号链接、build/out/artifacts、.git 和既有第三方/虚拟环境忽略目录。复制上限为 100000 文件或 2 GiB；需要这些被忽略内容的工程应调整工程布局，不会悄悄回退到原目录执行。

CMake 负责配置和编译，CTest 以 `--no-tests=error` 运行；没有测试不能算通过。每阶段应用超时，前序失败则后续阶段标记 skipped。成功/失败/超时/取消均保留日志；Git 读取失败单独记录，不导致非 Git 工程无法构建。C/C++ 源文件在运行结束后再次与快照比较；该核对不覆盖所有非源码资源文件。

产物包括源码副本、build 目录、各阶段日志、summary.txt、git.txt；CTest 正常生成时另有 tests.xml。数据库保存扫描批次、构建目标、Git 提交、阶段命令、状态、日志、退出码、耗时及测试计数。日志截断或未产生可识别的 CTest 摘要时，计数字段为 -1，表示未知而非零。跳过测试单独计数，实际通过数应按 total - failed - skipped 计算。

扫描与构建在同一 Qt 窗口互斥，过程均在后台。停止按钮结束进程并保存日志；窗口关闭会等待任务安全结束。构建结束的提交边界之后不再接受取消。

## 查询扩展

`issues` 字段：rule_id、severity、file、line、column、message、evidence、suggestion、symbol_id、detector。

`builds` 每行对应一个构建阶段，字段：run_id、scan_id、stage、status、exit_code、duration_ms、tests_total、tests_failed、tests_skipped、target。该逻辑表展示当前扫描对应的、最近 50 次项目构建中可找到的记录；跨扫描历史通过 `builds` 命令或 GUI“历史”读取。只读扫描快照加载历史摘要，点击特定运行时才加载其完整日志，避免查询文件清单时读取全部日志。

## 测试与交付边界

自动测试覆盖规则阳性/阴性边界、证据与数据库往返、并行一致性/取消、线程池异常、参数中的空格/引号/反斜杠/中文、双流大量输出、启动失败、进程超时/取消和子进程清理。工程测试覆盖编译成功/失败、测试失败、无测试、配置超时、取消、陈旧快照拒绝、原目录保护、持久化和查询；Git 用独立临时仓库验证分支、提交、差异及索引未改写。

Qt 自动验收通过实际控件信号走完规则定位、构建、测试、历史、查询、取消与延迟关闭，并对普通和紧凑窗口截图做人工图像检查。GoogleTest 合同测试在本机 Windows 开发包存在时启用，核心 CTest 用例独立于 GoogleTest。

Linux Core/CLI 与 POSIX 进程实现通过本机 WSL 构建验证后，测试结果记录在本文件末尾。Linux Clang 22/Qt 开发包未配置，因此 Linux 全功能 GUI/Clang 验收仍需在相应环境执行；不以 Linux Core 测试替代它。当前课程主线的三项功能已实现，但尚未将 2—3 个许可明确的大型外部工程验证、完整跨平台桌面部署或深层规则精度评测算作完成。

## 复现实验与打包

`scripts/benchmark_codeguard.ps1` 创建独立的 128 TU / 4096 函数受控工程，默认分别以 1/2/4/8 线程重复三次，保存原始数据库、日志、耗时、采样 CPU/峰值工作集、加速比和效率。未清空系统文件缓存，数据不能外推到真实项目或不同机器。

`scripts/package_codeguard.ps1` 生成独立 Windows 运行目录，收集 Qt 与其他动态依赖、Clang 内建头文件、Core 静态库/头文件、样例和说明。使用 `objdump` 检查传递依赖。运行包不包含完整 CMake/Ninja/C/C++ 开发工具链，真实工程仍需自己的编译参数及头文件。

## 本机最终验证记录 2026 年 9 月 9 日

| 配置 | 结果 | 范围 |
|---|---|---|
| Windows analysis-release | 65 项中 64 通过、1 跳过 | Clang 22.1.8、Qt、规则、并行、进程、工程管理、查询、CLI 与完整窗口 |
| Windows core-debug | 43 项中 42 通过、1 跳过 | GCC 16.2.0，无 Qt/Clang 的 Core/CLI；含 GoogleTest 合同测试 |
| WSL Ubuntu Core | 42 项全部通过 | GCC 15.2.0、CMake 4.2.3，POSIX 进程、构建测试、SQLite 与查询 |

Windows 跳过的是符号链接权限测试，未计入通过数；WSL 该项通过。Linux 使用单独目录中的 SQLite 3.53.4 公共头文件与系统 SQLite 3.46.1 运行库，测试覆盖实际使用的 C API；未安装新系统包。WSL 的 GoogleTest 自动发现曾误选 Windows 包，已禁用该跨平台错误候选。常规 Linux 环境应使用本机匹配的 SQLite 开发包，不能直接复用 Windows CMake 缓存或 MinGW 依赖路径。

独立规则样例测得六处应报位置（数组正/负越界各一处，另四类各一处），反例包含显式赋值括号、合法数组下标、尾后地址、sizeof、静态存储和引用参数。16 TU 并行用例验证告警顺序及数量一致、取消不追加扫描；不将这些构造样例折算为真实工程 Precision/Recall。

128 TU / 4096 函数并发实验在 Intel Core i7-14650HX、16 核/24 逻辑处理器上完成，每组重复三次：

| 分析线程 | 平均分析耗时 ms | TU/s | 相对单线程加速比 | 并行效率 |
|---|---:|---:|---:|---:|
| 1 | 20908.67 | 6.12 | 1.000 | 1.000 |
| 2 | 12203.67 | 10.49 | 1.713 | 0.857 |
| 4 | 7629.00 | 16.78 | 2.741 | 0.685 |
| 8 | 5052.33 | 25.33 | 4.138 | 0.517 |

原始结果位于 `artifacts/level3-benchmark-20260909`，包含 measurements.csv、summary.csv、环境信息、12 个数据库和对应日志。CPU 时间与峰值工作集按 20 ms 采样，属于采样值；耗时表采用分析阶段计时，完整命令耗时另列于原始 CSV。此实验使用受控本地代码，未清空文件缓存，不能把 4.14 倍加速作为所有工程的保证。

逐行比较这 12 个数据库的符号、函数度量、关系边、分析覆盖和规则结果，排除扫描批次编号后内容完全一致；核验摘要见同目录 `result-equivalence.json`。

Windows 独立运行包已在 PATH 仅包含包目录和 Windows System32、移除 Qt 开发环境变量的子进程中通过 CLI 启动、Clang 分析和 Qt 窗口自动验收。include 关系确认实际使用包内 `resources/clang/include/stddef.h`；界面截图已检查。验收记录位于 `artifacts/codeguard-package-qa-20260909-2/result.json`。这是本机隔离环境变量验收，尚未替代另一台全新 Windows 机器的部署测试；构建真实工程所需的 CMake、CTest、Ninja、编译器及 Git 不随运行包提供。
