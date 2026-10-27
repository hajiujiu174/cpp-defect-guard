# 显式补丁集与同一副本验证

更新日期：2026-10-27。P3 第四阶段。本功能把用户明确选择的多个候选放进同一份源码副本，保留各候选的来源、组合差异和逐目标结果。它不会枚举所有组合，也不会自动应用到原工程。

## 使用

先使用 `repair propose` 为同一次扫描的不同问题导出候选，再组合选中的 V1 文件：

```powershell
codeguard-cli repair compose PROJECT --database scan.sqlite3 --output OUT `
  --candidate FIRST/candidate.cgfix --candidate SECOND/candidate.cgfix
codeguard-cli repair validate-set PROJECT --database scan.sqlite3 --output RUNS `
  --set EXPORTED/repair.cgset --cxx-compiler PATH/g++.exe --jobs 2 --timeout 60
```

`FIRST`、`SECOND` 和 `EXPORTED` 用实际导出路径替换。候选文件名以 `repair propose` 输出为准。`compose` 只校验保存的来源与组合结构；验证时才重新检查当前源码、活动问题、扫描及配置。源码已变化时须重新扫描并生成候选。

Qt 问题列表支持按 Ctrl/Shift 选择 2—16 个问题，右键“组合选中问题的候选 / 同一副本验证”。后台生成候选池后，明确勾选 2—16 个候选，每个问题只选一个方案；初始不勾选任何候选。没有适用模板的问题不会生成候选行，实际处理范围以勾选行和导出的 `targets` 为准。可预览只读差异、导出组合、验证、取消及关闭；验证中的关闭先请求取消，等待记录完成。

可选 `--coverage gcov --gcov-tool PATH/gcov.exe`，或勾选 Qt 中的采集选项。当前要求 GNU 工具链和 Windows ASCII 输出目录，详细限制沿用 [覆盖率说明](repair-coverage.md)。每个目标均须在修改后的行上有执行记录；同一物理行上的多个表达式共用行记录，不能据此证明每个表达式都执行。

## 组合约束与导出

- 所有组件必须来自相同工程、扫描、配置和分析器版本。同一问题的两个备选、重复组件、重叠源码范围及不一致的原始字节均被拒绝。相邻修改可以组合。
- 所有修改按原始字节坐标定位，再倒序应用；后面的目标位置按累计长度变化重新计算。同文件和跨文件均支持，保留 BOM、CRLF、中文/空格文件名及末尾无换行。
- 上限为 16 个组件、每个组合后文件 2 MiB、组件载荷合计 32 MiB、集合文件 64 MiB。Qt 差异最多显示 512000 字节，超出会提示；导出差异完整。
- `repair.cgset` 采用 `CodeGuardRepairSet 1`，内嵌原 V1 候选。规范化顺序使反序输入产生相同集合 ID。FNV 内容指纹用于变化识别，不是数字签名。
- 输出使用原工程之外的新目录，保留 `candidate.diff`、逐文件修改前后字节、`repair.cgset`、`validation.json` 和说明。验证另存构建日志、原始对照及修改后的静态数据库。原单候选 V1 仍可读取；`repair evaluate` 继续逐个独立验证。

## 验证门槛

所有修改一起进入一份副本，构建和 CTest 只运行一套。除原有单候选门槛，还逐项目标保留原/新行列、原始告警是否复现、修改后是否未再检出及行执行记录。集合必须同时满足全部目标，不以第一项的结果代表整组。

原始对照使用相同构建配置，必须完整分析且复现每个目标。修改后的完整分析必须未再检出每个目标，没有新增或重新显现的告警；新位置按多处修改反向映射，无法可靠映射时保守计为新告警。构建成功、存在且全部通过的测试、原始已扫描 C/C++ 文件内容一致也都是必要条件。

本轮还补强了单候选与组合共用的检查：副本中的**全部已扫描 C/C++ 文件**须与“原始扫描加拟议修改”一致，成功时记录 `proposed_copy_verified=true`。此前只核对修改文件可能遗漏 CMake 对未选 `main.cpp` 的改写；已保存先失败后修复通过的回归记录。检查在构建/测试之后、分析前后及最终返回前执行，原工程另行核对。

此检查不锁定 CMake 脚本、资源、外部依赖或系统环境，也不是操作系统沙箱。源码指纹校验不代替密码学完整性保护。`gates_passed` 只表示本次配置下的验证门槛通过，`correctness` 始终为 `not_assessed`；没有真实修复正确性真值或正确修复率。

## 复现与证据

```powershell
python scripts/accept_repair_set.py --coverage `
  --cli PACKAGE/codeguard-cli.exe `
  --core-cli build/p2-2-flow-20261026-core/codeguard-cli.exe `
  --toolchain build/p0-windows/toolchain-4 --output artifacts/set-reproduction-NEW
```

`samples/repair-set-demo` 是刻意设计的两个错误条件：任一比较修改单独验证时仍有行为测试失败，两个比较修改组合后通过；一项比较加一项保留赋值虽然消除两个告警，仍被行为测试拒绝。示例结论限于该合成测试。

最终运行包的独立 CLI 演示还验证了反序组合导出相同内容、Core 缺少 Clang 时即使测试通过也保留 `analysis_unavailable`，以及混合格式化/条件模板的同一行位置映射。混合用例保留格式化参数内的一条既有 CG002，修改后仍为一条既有告警，新增计数为零；行为测试只执行其安全路径，这项检查用于验证位置归因。

关联回归 16 项全部通过，覆盖同文件/多文件、字节与位置映射、重叠/过期/配置拒绝、原始对照逐目标核验、抑制失效、未选文件改写、覆盖率未执行目标、取消留档、CLI、Qt 控件及主窗口入口。

本轮最终构建的检查结果：

| 检查 | 结果 |
|---|---|
| Windows Core Debug 完整回归 | 65 通过、1 因符号链接权限跳过，零失败 |
| Windows Clang/Qt Release 完整回归 | 164 通过、1 因符号链接权限跳过，零失败 |
| cJSON / TinyXML-2 / fmt | 59 个选定有效 TU 全部解析；44 个上游测试通过；线程/缓存结果一致，受检源码未变 |
| 最终包 CLI 演示 | 单项失败、组合通过、行为反例拒绝、Core 分析缺失、混合模板位置映射、原始文件 SHA-256 一致 |
| 运行包隔离 PATH | CLI、Clang 内建头文件、中文字体、Qt 组合预览/导出/取消及两个目标留档通过 |

构建使用已有锁定工具链目录增量完成；没有把本机增量构建写成干净克隆或独立 CI。真实库仍只覆盖选定编译配置，未编译文件保留部分覆盖语义，没有完整缺陷真值。普通/紧凑窗口截图已查看，紧凑窗口通过滚动区查看后续结果。

完整回归、最终运行包和 CLI 演示证据见 [本轮记录](evidence/repair-set-20261027.json)。Windows 运行包为 `artifacts/p3-set-20261026/CodeGuard-Windows-x64-P3-Set.zip`，旁边保留 `.zip.sha256`。本机验收不等于独立远端 CI 或另一台实体电脑验收，当前改动尚未提交或推送。
