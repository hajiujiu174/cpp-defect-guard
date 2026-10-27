# 修复验证的可选 gcov 覆盖率

更新日期：2026-10-26。P3 第三阶段增加 Core、CLI、Qt 覆盖率采集、修复所在行的执行记录门槛及有条件的批量排序。默认关闭；原始工程仍不自动应用候选。当前证据见 [覆盖率验收](evidence/repair-coverage-20261026.json)。模板和基础门槛沿用 [修复候选](repair-candidates.md)，批次记录沿用 [批量验证](repair-batch.md)。

## 使用

CLI `repair validate` 和 `repair evaluate` 支持：

```powershell
codeguard-cli repair validate D:\demo --database D:\results\scan.sqlite3 `
  --output D:\results\runs --candidate D:\results\candidate.cgfix `
  --cxx-compiler D:\toolchain\bin\g++.exe `
  --coverage gcov --gcov-tool D:\toolchain\bin\gcov.exe
```

`--coverage off` 为默认行为。`--gcov-tool` 需要显式开启 `--coverage gcov`；工具路径须为 PATH 中的命令名或绝对路径。重复参数、未知模式和仅生成候选时传覆盖率选项会被拒绝。C/C++ 编译器和 gcov 必须兼容；本轮使用锁定工具链的 GNU GCC/gcov 16.2.0。Clang/MSVC 编译器不静默替换成 GCC，配置阶段会说明不支持。

Windows 下开启采集时，输出目录的完整路径须仅含 ASCII 字符，例如 `D:\CodeGuardRuns`；源码目录及文件可以含中文和空格。该限制用于避免本轮 GNU 运行库把 UTF-8 输出路径作为系统编码解释后写入错误目录，不静默更换输出位置。输入不满足时在编译前说明原因。

Qt 修复对话框提供“采集 gcov 覆盖率（仅 GNU 编译器）”复选框及工具路径。编译器沿用工程设置，可在那里选择 GNU 工具链。单候选详情显示工程行触达/插桩数、分支记录触达数、所选行、未计入文件及记录路径；批量表增加行触达和所选行列。开关属于本次验证，不保存为项目默认配置。

运行包提供界面和采集实现，未附带 GCC/gcov、CMake、Ninja 或工程依赖。关闭采集时的预览、导出与取消记录仍可在隔离 PATH 下使用；真正采集需要开发工具链。

## 数据从哪里来

gcov 读取 GCC 插桩产生的记录；这些计数会随执行累积，因此本流程在 CTest 前隔离旧计数。gcov 的行记录、分支记录及优化方面的解释见 [GCC 官方说明](https://gcc.gnu.org/onlinedocs/gcc/Invoking-Gcov.html)。

本次候选副本通过单独的 CMake 项目钩子添加 `--coverage -O0 -fprofile-abs-path -fprofile-update=atomic`，链接添加 `--coverage`，并检查启用的 C/C++ 语言使用 GNU 编译器。已有显式 `CMAKE_PROJECT_INCLUDE` 钩子时拒绝覆盖。项目也可能覆盖这些选项；没有有效 gcov 数据时保留失败，不能凭请求过插桩就称采集成功。

配置/构建结束、CTest 即将开始时，把构建目录内已有 `.gcda` 移到同一次工作目录的 `pre-test-profile`，保留相对路径和 `coverage-reset.txt`；不删除原始工程数据。随后只读取本次构建目录中的 `.gcno` 和 CTest 后产生的计数。测试包括正常的 CTest fixture/setup 行为；数据是整个本次测试集的合并结果，不是逐测试归因。

每个对象保留 `coverage/object-N.log` 和原始 `object-N.json`，另存 `version.log`、`profiles.json` 和 `summary.json`。采集使用 gcov JSON 格式 2，不依赖其文本模式重新打开源文件；利用已有 SQLite JSON 支持读取结构化计数，没有新增 Python 运行依赖。gcov 出错、输出被截断、没有插桩工程行、工具缺失、超时或取消都有明确状态。遍历最多十万个条目、512 个对象；档案副本上限 512 MiB，单对象捕获上限 32 MiB，总标准输出上限 128 MiB，超过即报告失败。

本轮 Windows GNU 运行库对中文对象名可能产生确定的编码别名。采集器先寻找原名，再核对 UTF-8 字节按系统代码页解释的精确别名；两者不一致且同时存在时拒绝歧义。每对 `.gcno/.gcda` 复制到采集目录内的 ASCII 对象名，再交给 gcov；原档案保留，映射写入 `profiles.json`，不按相似名称或文件顺序猜测。中文/空格源文件及目录已另作实际编译、采集、静态对照和原文件哈希验收。

## 统计口径与门槛

- 仅统计原始 C/C++ 清单内、候选副本中的插桩行。按工程相对文件名和行号合并多个对象中的行，只要任一记录有执行计数就标为触达。
- 注释、空行及 gcov 标为不可执行的行不进入分母。没有有效插桩行的工程文件列入 `unmeasured_files`，不把它们假定为已覆盖或零覆盖。
- `branch_records` 按对象及 gcov 实际输出的记录逐条计数，包括可能重复的头文件实例。未请求无条件分支，不把这个数称为去重后的全工程源分支覆盖率。
- 所选行有执行记录，不表示这一行全部表达式、全部分支或补丁语义被测试充分验证；多个语句在同一行时尤其如此。丢失运行计数只能表述为“未记录执行”。

不开启采集时保留 `coverage.status: not_measured`。开启后，原有编译/测试/同配置静态复查门槛仍须通过，并额外要求采集状态 `collected`、所选行 `recorded`。其他情况不能通过：采集失败对应 `coverage_failed`，没有所选行执行记录对应 `coverage_target_uncovered`。已有行为测试失败等原因优先保留，同时仍可查看已采集的覆盖率。

未完整解析的静态复查不再把空告警集合显示成目标已消失；无法完整对比时，新增/重新显现告警数保持未知。覆盖率成功也不能掩盖解析失败。

## 与只读静态分析的关系

原始编译数据库和实际构建参数保留。用于前后 AST 对照的另一个数据库仅去除本流程生成的三项参数：`--coverage`、`-fprofile-abs-path`、`-fprofile-update=atomic`，并记录 `coverage_commands_normalized`。`-O0`、宏定义、包含路径等仍保留。其他 profiling、插件或有副作用选项继续被原来的只读检查拒绝；专项测试确认 `-fprofile-use=external` 不被移除，也不被放行。

原始对照仍须在相同规范化参数下复现所选问题。此过程是 Clang 对既有构建命令的静态建模，不是 GCC/Clang 所有预定义宏及运行行为完全一致的证明。

## 批量排序

2026-10-27 新增默认关闭的逐测试组重跑模式，单独保存依赖/fixture 组的计数和目标行记录，并检查行并集与原整套测试一致，见 [逐测试组覆盖](repair-test-coverage.md)。下述排序仍使用整套 gcov 的既有证据次序，逐组记录不作为修复概率。

排序升级为 `evidence-lexicographic-v2`。原有门槛、测试、构建、完整静态对照、目标规则及新增告警优先级保留。在修改字节比较之前，只有**全部候选**均成功采集、gcov 版本相同、插桩工程文件/行号集合及行分母一致时，才比较所选行是否有记录及工程触达行数。

否则整个批次忽略覆盖率排序项，不把未知、不同分母或不同版本混在一起比较。相同范围的判断使用明确的范围指纹及行数，指纹用于意外变化识别，不是数字签名。分支记录不参与排序。批次 JSON 提供 `test_coverage`（`not_measured` / `collected` / `incomplete`）和 `coverage_comparable`，每个候选保存自己的完整覆盖率记录。

覆盖率不是正确修复概率。`correctness` 继续为 `not_assessed`；没有新增行/分支阈值，也没有据此自动选择或应用补丁。

## 可复现示例

```powershell
python scripts/accept_repair_candidates.py --batch --coverage `
  --cli build/p2-2-flow-20261026-analysis/codeguard-cli.exe `
  --core-cli build/p2-2-flow-20261026-core/codeguard-cli.exe `
  --toolchain build/p0-windows/toolchain-4 --output artifacts/coverage-reproduction-NEW
```

本轮独立 CLI 示例的记录：

| 候选 | 工程行触达 | 分支记录触达 | 所选行 | 原有验证结果 |
|---|---|---|---|---|
| 条件比较 | 5/7 | 4/8 | 已记录执行 | 门槛通过 |
| 有意赋值加括号 | 5/7 | 2/8 | 已记录执行 | 行为测试失败 |
| 有界格式化 | 5/7 | 2/8 | 未记录执行 | 原条件缺陷导致行为测试失败 |

另外复制一个仅测试 `main(){return 0;}`、从不调用待修函数的合成工程。相同候选在未采集覆盖率时通过既有静态/测试门槛，开启后因所选行没有执行记录而被拒绝。两个工程的原始文件 SHA-256 均前后一致。无 Clang 的 Core 可以采集覆盖率，但仍因缺少静态复查而不能通过修复验证门槛。

测试还覆盖构建后执行产生的旧计数隔离、未编译头文件列示、不同插桩范围/工具版本不比较、缺失 gcov、非 GNU 编译器、已有钩子、覆盖率阶段取消、只读参数检查及 Qt 控件操作。完整当前回归与三个固定真实工程的结果以本轮证据和 JUnit 为准，不借用前轮计数。

本轮先完成 Core 65 项中 64 通过/1 跳过、Clang/Qt 149 项中 148 通过/1 跳过，跳过项为符号链接权限；三个固定真实库 59 个有效 TU、44 个上游 CTest 及线程/缓存一致性通过。随后为中文文件修正采集器的 JSON/计数路径映射，Clang/Qt 相关 14 项、Core 相关 12 项复测全部通过；最终编译告警清理后另有计数隔离与参数检查 2 项通过。全量和真实工程记录属于该编码修正前的构建，末次改动的直接证据为关联复测、中文用例及交付运行包验收。

2026-10-27 后续已实现显式补丁集和逐目标行记录检查，见 [组合补丁](repair-set.md)。逐测试组采集已在后续实现，见 [专项说明](repair-test-coverage.md)。仍未实现原始/修改版本的多次覆盖率增量对照、MC/DC/路径覆盖、真实修复正确性真值评测或模型部署。本机增量构建及运行包检查不替代独立远端 CI 或其他实体电脑验收，源码副本仍不是操作系统安全沙箱。
