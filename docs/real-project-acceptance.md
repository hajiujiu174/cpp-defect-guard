# P0-2：真实 C/C++ 工程验收

2026-09-11。本轮仅验证 Windows x64 的三个固定库工程；不将这些结果外推到大型应用、全部构建配置或其他平台。

## 工程和配置

版本、提交、许可文件 SHA-256、编译器、CMake 定义和编译命令选择策略保存在 `config/real-projects.lock.json`。克隆后核对提交和许可原文；不修改上游受 Git 跟踪的文件。

| 工程 | 固定提交 | 许可 | 构建配置 |
|---|---|---|---|
| [cJSON v1.7.19](https://github.com/DaveGamble/cJSON/tree/c859b25da02955fef659d658b8f324b5cde87be3) | `c859b25` | MIT | GCC 16.2、Debug、静态库、Utils 和测试开启、CMake 最低策略 3.5、关闭上游自定义警告集合 |
| [TinyXML-2 10.0.0](https://github.com/leethomason/tinyxml2/tree/321ea883b7190d4e85cae5512a12e5eaa8f8731f) | `321ea88` | Zlib | Clang 22.1.8、Debug、静态库、测试开启，副本保留 `resources/out` |
| [fmt 11.1.4](https://github.com/fmtlib/fmt/tree/123913715afeb8a437e6388b4473fcc4753e1c9a) | `1239137` | MIT，含许可文件中的可选例外 | Clang 22.1.8、Debug、静态库、测试开启、文档和模块关闭 |

三个工程均由 CodeGuard 的 Clang 22 分析器解析。cJSON 使用 GCC 生成的编译参数，分析器仍按 Clang 的内建宏和语义工作，不保证 GCC 专用语义完全等价。嵌入的 Unity、GoogleTest 等测试依赖保持上游源码和许可原状；产物含各工程顶层许可，重新分发源码时还需保留其中的第三方许可。

## 验收方法与复现

先按照 [Windows CI](windows-ci.md) 还原 79 个固定依赖包并构建当前 Analysis 版本。验收脚本额外需要 Windows 上的 Python 3.10 或更新版本，仅使用标准库，不是核心程序的运行依赖。

```powershell
python scripts/accept_real_projects.py `
  --cli build/windows-ci-analysis/codeguard-cli.exe `
  --toolchain build/locked-toolchain `
  --output artifacts/real-projects-new
```

输出必须是新目录。可选 `--projects cjson tinyxml2 fmt` 选择工程；已有对应 Git 克隆时可用 `--source-cache <父目录>`，仍核对锁定提交和许可。默认从固定官方仓库拉取指定提交，不跟随分支最新版本。GitHub Actions 的手动运行勾选 `real_projects` 可在独立 runner 重放本流程；普通 push 保留原来的完整产品回归和运行包验收，不重复下载外部验收工程。

每个工程依次执行：生成编译数据库 → 保存原始扫描 → 明确选择每文件一个编译配置 → 分别用 1/4 个请求线程扫描 → 比较符号、度量、关系、覆盖、TU 诊断和告警 → 调用 CodeGuard 在源码副本中构建并运行 CTest → 查询持久化构建结果 → 检查所有原始受跟踪文件的 SHA-256。TinyXML-2 仅有 3 个源文件，实际工作线程会缩减为 3。

`summary.json` 保存编译、测试、覆盖和资源数据；`analysis.json` 与 `raw-analysis.json` 保存逐 TU 诊断和告警；数据库、各阶段命令、日志及 CTest XML 原样保留。运行时间按墙钟计量，峰值工作集每 20 ms 查询一次；扫描内存包含该进程的分析线程，构建父进程内存不代表编译器子进程总内存。各线程数只运行一轮、未清空缓存，不据此宣传稳定加速比。

## 已发现并处理的障碍

1. cJSON 的最低 CMake 版本为 3.0，锁定的 CMake 4.4 不再接受旧兼容策略。新增 CLI `--cmake-define KEY=VALUE`，向独立配置进程传递 `CMAKE_POLICY_VERSION_MINIMUM=3.5` 等定义，保留上游文件原状。
2. TinyXML-2 测试需要 `resources/out`，旧副本规则按目录名忽略所有 `out`，导致测试写文件后崩溃。新增 CLI `--copy-include resources/out` 精确保留所需目录。仍拒绝绝对路径、点路径、越界、符号链接和 `.git`；父级被忽略时也需显式保留父级。这是 CLI 的最小修复，Qt 配置管理留待 P1-1。
3. cJSON 自带 Unity 在 Windows Clang 下禁用弱定义，而 CMake 仅为 MSVC 附加 `unity_setup.c`，导致链接缺少 `setUp/tearDown`。锁定配置改用已有 GCC 编译测试，不修改第三方测试、不跳过失败测试。
4. fmt 有 5 个文件被多个目标以不同参数编译，原始扫描如实报告 `ambiguous_command`。验收配置优先选择生产库及公共测试目标，其余按稳定字典序选择一个命令；所有被排除的变体记录在 `selected/selection.json`。产品不会自动猜选配置。

新增行为回归 `codeguard.build_configuration` 覆盖带空格的多个定义、默认忽略目录中的必需资源、无效复制路径和非定义参数拒绝；本地完整 Analysis 回归 66 项中 65 通过、1 符号链接权限跳过。

## 当前实测与边界

本地三个工程的有效编译配置均已通过分析、构建和测试，1 线程与多线程结果一致。cJSON 的 22 个 CTest、TinyXML-2 的 1 个 CTest、fmt 的 21 个 CTest 均通过，无失败或跳过。CTest 数量是测试程序/注册项数量，不是程序内部断言数量。

| 工程 | C/C++ 清单文件 / 物理行 | 有命令并成功的 TU | 无命令的源文件 | 覆盖清单文件 | 告警 |
|---|---:|---:|---:|---:|---:|
| cJSON | 99 / 22,367 | 27 / 27 | 49 | 33 / 99 | 26 条 CG001 |
| TinyXML-2 | 4 / 8,266 | 2 / 2 | 1 | 3 / 4 | 0 |
| fmt | 70 / 68,025 | 30 / 30 | 16 | 52 / 70 | 0 |

清单包含被扫描的上游测试代码和部分嵌入依赖，不是净业务代码行数；条件编译未进入的代码也不能视为已解析。三个仓库整体分析状态仍为 `partial`：未参与当前构建的示例、辅助程序、特性探针等保留 `missing_command`，没有伪装成仓库 100% 覆盖。fmt 模块及其他目标变体未被本轮穷举；头文件统计只说明成功 TU 访问到了文件，不说明所有模板实例均已展开。

缺失命令的范围包括 cJSON 的 47 个 Unity 示例/自测源文件、1 个 fuzzing 入口和 1 个仅其他编译器使用的测试钩子；TinyXML-2 的 `contrib/html5-printer.cpp`；fmt 的 fuzzing、模块、CUDA、特性探针和独立安装/导出测试目录。精确文件名及原因保留在各项目 `analysis.json`。本轮真实工程通过 CLI 执行；Qt 控件仍由产品回归覆盖，未将其视为三个真实项目的逐页人工 UI 验收。

## 逐条告警复核

全部 26 条告警的位置及判断依据保存在 `config/real-project-review.json`，复现脚本核对提交和完整告警位置集合，新增、消失或规则改变会使验收失败并要求重新审查。

26 条均为真实的 `sprintf/strcpy/strcat` 调用，符合 CG001 的“无界 API 提醒”契约。其中 17 条在 cJSON/Utils 实现，9 条在测试；未确认任何一条为缓冲区溢出。多数位置已有按字面量大小、字符串长度或索引位数预留的容量，规则尚不能利用这些上下文降低提示量。因此可以说“26/26 与该 API 模式相符”，不能说漏洞检测精确率为 100%。

复核也发现部分 Utils 分配后未在本地判空，数值格式化的事后长度检查不能独立证明写入前安全。这些是后续数据流/故障注入复核线索，不把当前 CG001 提醒等同于已证明的路径缺陷。CG002—CG005 本轮没有产生可作统计的真实阳性样本；TinyXML-2 和 fmt 无告警不等于无缺陷。本轮没有为全部源代码建立缺陷真值，因此不报告全项目 Recall/F1。

## 最终远端验收记录

代码提交 `a0073e1a7e0476d4dbff599f66c5c02fc90c6325` 的 [运行 34579099292](https://github.com/hajiujiu174/cpp-defect-guard/actions/runs/34579099292) 已全部成功。Windows Server 2022 runner 从官方仓库按固定 SHA 获取三个工程，原始工作区无未提交变更；三个工程的计数、告警集合和覆盖边界均与上表一致。构建记录中的 Git 提交分别匹配版本锁，1/4 请求线程的结果完全一致，所有原始受跟踪文件均未修改。

| 工程 | 单线程扫描墙钟 s / 采样峰值 MiB | 多线程扫描墙钟 s / 采样峰值 MiB | 副本配置 / 编译 / CTest s | 通过的 CTest |
|---|---:|---:|---:|---:|
| cJSON | 2.084 / 38.71 | 1.145 / 52.88 | 0.822 / 4.131 / 0.220 | 22 / 22 |
| TinyXML-2 | 1.185 / 68.14 | 1.063 / 68.88 | 0.825 / 1.262 / 0.127 | 1 / 1 |
| fmt | 74.697 / 490.76 | 36.778 / 722.13 | 1.241 / 63.193 / 0.549 | 21 / 21 |

多线程请求为 4，TinyXML-2 实际工作线程为 3。表中扫描墙钟包括导入、分析和保存数据库；分析阶段内部计时另存 JSON。内存为单个扫描进程的采样峰值工作集，不是进程树合计，也不是静态库或运行包大小。

同次运行的产品回归为 Core 44 项中 43 通过、1 权限跳过；Clang/Qt 66 项中 65 通过、1 同类跳过。第二台全新 runner 的 CLI、Clang、Qt、内建头文件和中文字体运行包检查全部通过。

长期保留的紧凑原始数据见 [验收证据 JSON](evidence/real-projects-20260911.json)，包含环境与输入哈希、精确未编译文件名单、原始/选定 TU 计数、测试结果和资源读数。完整下载保存在本地 `artifacts/ci-run-34579099292`；GitHub 的 `windows-real-projects` 产物包含原始 SQLite、命令、日志、许可和 CTest XML，保留 14 天。第三方源码和二进制不提交本仓库。

P0-2 按上述范围完成，下一项为 P1-1 工程导入与配置管理。优先处理本轮暴露的编译配置选择、被忽略资源目录的可见配置，以及 CLI 选项到 Qt 的持久化接入；规则的容量上下文与数据流能力仍按后续优先级推进。
