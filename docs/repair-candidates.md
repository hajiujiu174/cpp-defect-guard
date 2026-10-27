# 确定性修复候选与副本验证

2026-10-27 第七阶段新增 `condition.assignment_result`：先保存返回值再比较，支持逻辑条件中的合适局部整数赋值。Core/CLI/Qt 继续共用原始对照、全部目标、无新增告警、构建测试及副本完整性门槛；真实参考、行为回归和当前限制见 [真实修复评测](real-repair-benchmark.md)。以下首阶段三个模板和计数保留其原范围。

更新日期：2026-10-26。P3 首阶段已接入 Core、CLI 与 Qt：重新解析 AST 生成候选、只读差异预览、导出及源码副本内验证。没有训练或部署新模型，也没有自动写回原始工程的入口。完整本机验收索引见 [机器可读证据](evidence/repair-candidates-20261026.json)。后续独立 Windows CI 尚未执行。

## 支持范围与候选影响

后续验证可选择采集 gcov 数据并核对所选行是否有执行记录，见 [覆盖率与验证门槛](repair-coverage.md)。未开启时仍保留未测量状态；覆盖率不等于正确修复。

后续批量验证阶段已增加“验证全部并排序”、CLI `repair evaluate` 及完整批次记录，见 [批量验证与证据排序](repair-batch.md)。下文三种模板及单候选门槛继续适用。

| 规则与模板 | 源码条件 | 候选影响 |
|---|---|---|
| CG003 / `condition.compare` | 条件中的直接内建 `=` 运算，运算符源位置可确定 | 将 `=` 改为 `==`，停止赋值并改变分支行为；比较意图仍需人工确认 |
| CG003 / `condition.intent` | 可取得完整源码范围的条件赋值表达式 | 为赋值加括号，保留赋值行为并明确意图；告警消失不能证明行为缺陷得到修复 |
| CG001 / `printf.bound` | `sprintf` 的目的地是直接局部定长普通 `char` 数组 | 改为 `snprintf` 并传入 `sizeof(array)`；可能截断，须检查返回值、错误处理和函数声明 |

宏展开范围不猜测；指针不推断容量；静态数组、volatile、宽字符及未知目的表达式不套用容量模板。复合赋值只可能生成意图候选。每个 V1 候选为单文件、单处修改，不自动补头文件。2026-10-27 新增显式集合组合多个 V1 候选，见 [组合补丁](repair-set.md)。

生成前重新核对保存的工程配置、C/C++ 文件清单及内容、有效 TU、命令指纹，并强制重新解析 AST。编辑范围临时保存在内存中，不从旧数据库或 TU 缓存恢复。保存扫描后源码、配置或分析命令变化时需要重新扫描，不重用旧范围。

## Qt 操作

1. 打开工程，选择有效编译数据库并完成扫描。
2. 在活动问题列表选中问题，右键选择“生成修复候选”。
3. 在下拉列表选择候选，审阅影响说明及只读 unified diff。
4. 将输出目录设在原工程外，可先“导出候选”，再“在副本中验证”。编译器等选项沿用工程设置。
5. 分别查看编译、测试计数、同配置原始副本复现、静态复查、新增/重新显现告警及源码一致性。取消或安全关闭会保留已产生的记录。

生成与验证在后台执行。扫描、构建或历史读取期间主窗口入口不可用；首次扫描结束后入口可正常使用。普通及紧凑对话框布局、真实控件操作、取消/重试/关闭、主窗口入口及隔离 PATH 下的包内预览均纳入本机检查。

## CLI 操作

先使用 `prepare` 或工程已有的 `compile_commands.json` 完成扫描；下面的路径为示例，应替换为实际工程及活动告警行号。

```powershell
codeguard-cli scan D:\demo --database D:\results\scan.sqlite3 --compile-commands D:\configure
codeguard-cli repair propose D:\demo --database D:\results\scan.sqlite3 `
  --output D:\results\candidates --rule CG003 --file main.cpp --line 5
codeguard-cli repair validate D:\demo --database D:\results\scan.sqlite3 `
  --output D:\results\runs --candidate D:\results\candidates\CANDIDATE\candidate.cgfix `
  --cxx-compiler D:\toolchain\bin\g++.exe --jobs 2 --timeout 60
```

同一行存在多个位置时传 `--column`。验证还支持 `--c-compiler`、`--target` 和重复的 `--cmake-define KEY=VALUE`。数值、重复项、空参数及不适用选项会被拒绝。生成成功/门槛通过返回 0，无适用模板或验证未通过返回 2，取消返回 130。验证结果以 JSON 输出，`correctness` 始终为 `not_assessed`。

每次导出建立新目录，包含 `candidate.cgfix`、`candidate.diff`、原文件/候选文件文本及显式 `not_run` 的 `validation.json`。验证另建目录，记录运行中和最终状态、差异、构建日志及原始/候选分析 SQLite；数据库中的 BuildRun 关联生成候选时的扫描批次。

## 验证门槛

验证首先检查候选与当前工程、最新扫描、配置及原始字节一致，仅在复制出的源码树中应用修改。构建前再保留一份未修改源码，供同配置对照。候选通过须同时满足：

- 副本配置、编译及 CTest 成功；测试总数大于零，失败与跳过均为零，计数未知不通过。
- 使用构建生成的同一编译数据库重定位分析原始对照和候选；原始对照完整解析并复现所选问题，候选完整解析且编辑范围内目标规则不再检出。
- 没有新增或因文件指纹变化而重新显现的其他告警；编辑引起的位置偏移按单处修改映射。
- 构建后候选文件仍与拟议字节完全一致；最终原始 C/C++ 文件清单和内容仍与保存扫描一致。

因此，改变 CMake 选项使原告警从编译范围消失、测试失败、全部禁用/跳过、构建阶段重写候选、缺少 Clang、取消、解析不完整或旧扫描都不能成为通过证据。构建/测试成功、目标规则消失、全部门槛通过及正确修复分别记录。

此流程沿用工程已有的 CMake/CTest 源码副本机制。构建脚本仍在本机运行；副本不是操作系统安全沙箱。门槛核对 C/C++ 清单和内容，未完整固定所有资源、外部依赖和工具链行为。多配置/部分 TU 覆盖不等于整仓库所有路径均已验证。正确性和测试充分性仍需人工评估。

## 可复现演示与结果

本轮 Windows 完整回归：Core Debug 62 项中 61 通过、1 跳过；Clang/Qt Release 134 项中 133 通过、1 跳过。跳过项均为符号链接权限测试。本阶段在既有锁定工具链构建目录增量构建，没有将其写成新的干净克隆或远端 CI 验收。三个固定真实库的 59 个有效 TU、44 个上游测试、线程/缓存一致性及源文件保持检查全部通过，仓库未编译文件仍保留部分覆盖状态。

`samples/repair-demo` 包含一个故意错误的零值分类函数，以及一个未被行为测试调用的格式化函数。原版编译成功、行为测试预期失败。独立 CLI 验收脚本复制该样例，生成并逐个验证三种候选：

| 候选 | 编译 | 行为测试 | 目标规则 | 最终门槛 |
|---|---|---|---|---|
| `condition.compare` | 通过 | 1 通过 | 不再检出，CG001 保留 | 通过 |
| `condition.intent` | 通过 | 1 失败 | 不再检出，CG001 保留 | 拒绝 |
| `printf.bound` | 通过 | 1 失败，原 CG003 行为未改变 | 不再检出，CG003 保留 | 拒绝 |

三种候选均在同配置原始副本复现目标，且原样例和工作副本全部原始文件 SHA-256 前后一致。无 Clang 的 Core CLI 对比较候选可编译并测试通过，但输出 `analysis_unavailable`、门槛不通过。上述集合为合成演示，不能据此估算真实工程正确修复率或 Top-k 成功率。

```powershell
python scripts/accept_repair_candidates.py `
  --cli build/p2-2-flow-20261026-analysis/codeguard-cli.exe `
  --core-cli build/p2-2-flow-20261026-core/codeguard-cli.exe `
  --toolchain build/p0-windows/toolchain-4 --output artifacts/repair-reproduction-NEW
```

专项回归还覆盖 C11/C++、宏、指针、复合赋值、BOM/CRLF/Unicode/空格路径/无末尾换行、实际 Git 副本应用后的字节比较、损坏与越界候选、陈旧扫描、缺声明编译失败、抑制失效、配置变化导致原问题无法复现、复制文件被生成器重写、未执行测试及取消记录。

## 格式与交付限制

候选保存完整原始和修改后字节，单文件最多 2 MiB，AST 编辑范围最多 32 KiB，编码候选最多 16 MiB。路径必须属于保存扫描的 C/C++ 清单；拒绝越界、符号链接逃逸和源码树内输出。FNV 内容 ID 用于检测意外变化，不是数字签名；只对可信本地候选执行验证。

Windows 包含 CLI/Qt、运行依赖、内建头文件、示例及本说明。隔离 PATH 检查覆盖候选预览与导出；实际副本编译/CTest 仍需另行提供 CMake、Ninja、编译器及工程依赖。独立远端 CI、其他实体电脑和模型修复研究不属于本轮已验收范围。
