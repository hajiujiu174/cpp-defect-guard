# 逐测试组覆盖归因

更新日期：2026-10-27。P3 第五阶段。已有整套 CTest 的 gcov 采集继续保留；新增模式在同一构建副本中逐组重跑，隔离每组计数，保存每个修复目标行的执行记录。默认关闭，不自动应用候选，也不根据覆盖记录宣称修复正确。

## 使用

```powershell
codeguard-cli repair validate-set PROJECT --database scan.sqlite3 --output OUT `
  --set EXPORTED/repair.cgset --cxx-compiler PATH/g++.exe `
  --coverage gcov-tests --gcov-tool PATH/gcov.exe --coverage-test-limit 64
```

`repair validate` 和 `repair evaluate` 同样接受这些选项。原 `--coverage gcov` 只采集整套测试，`off` 保持未测量。`--coverage-test-limit` 只适用于 `gcov-tests`，默认 64，范围 1—256；声明测试数超过上限时完整保留已发现条目为 `not_run`，不悄悄截取前 N 项。

Qt 单候选、批量候选和组合对话框都有“逐测试组重跑”及上限控件；先开启 gcov，再启用重跑。详情列出请求的测试、该组选择的测试、各目标行记录及整套测试与重跑并集的一致性。切换候选可查看自己的记录；原始代码查看和差异保持只读。

需要 GNU 编译器、匹配的 gcov、CTest 3.29+ 和 Windows ASCII 输出目录。其他插桩及计数格式限制沿用 [覆盖率说明](repair-coverage.md)。CTest 的精确名称文件选择及自动 fixture 行为依据 [CTest 官方文档](https://cmake.org/cmake/help/latest/manual/ctest.1.html#run-tests)；使用 JSON v1 获取测试目录，命令使用参数数组，不拼接 Shell 或测试名称正则表达式。

## 一组包含什么

每个请求测试带上它的传递 `DEPENDS`，再由 CTest 加入所需 fixture 的 setup/cleanup。fixture 自身的依赖也纳入闭包，直到选择范围稳定。每组以 `--parallel 1` 运行，保留 CTest 的环境、工作目录和测试属性。

结果明确标为**测试组**。当一组包含依赖或 fixture 时，行记录只能归因于整组，不能断言来自请求测试本身；`selected_tests` 给出真实选择范围。单行内多个表达式仍共用行记录，不代表表达式、路径或 MC/DC 覆盖。测试名称可以含中文、空格、正则元字符和 XML 转义字符；换行、空名称或重复名称明确拒绝。

## 采集与门槛

1. 先按既有流程构建并运行整套测试，保存原 gcov JSON 及已复制的原始 profile。
2. 每组开始前，将副本构建目录的旧 `.gcda` 移入该组独立备份，再运行选择的测试。不会删除原始工程文件。
3. 每组独立调用 gcov 并保留计数/profile、JSON、命令日志、JUnit、选择文件及测试目录 JSON。核对 JUnit 测试名称集合与计划一致；禁用、跳过和未运行分别保存，不能因进程返回 0 就当作测试全部执行。
4. 每组完成后，将 CTest 目录与首次发现结果核对，并核对 gcov 版本及插桩文件/行集合。把成功组的行记录取并集，与原整套测试逐行比较；不对按对象统计的分支记录做不可靠的去重或归因。
5. 原有构建、测试、原始告警复现、完整静态复查、目标消失、新增告警及源码检查继续有效。开启新模式后还要求重跑完整且行并集一致；否则即使测试都通过也不能通过新增门槛。

重跑沿用同一构建工作区和测试资源状态，计数被隔离，环境没有为每组重新创建。依赖外部服务、文件或测试顺序的工程可能重跑失败或出现覆盖并集差异，这些都作为实际证据保留，不自动重试成“通过”。失败组的覆盖只用于诊断，不计入成功归因并集；后续组仍可继续采集。取消时保留已完成、当前及尚未运行项。

上限为 256 个声明测试、每次元数据输出 8 MiB；原有每次 gcov 512 个对象、512 MiB profile、128 MiB JSON 上限保留。逐组累计证据超过 1 GiB 后停止后续组并报告失败；检查在一组收尾执行，实际占用可能多出最后一组的记录。每个 CTest/gcov 进程仍受超时和取消控制。

## 记录位置

主 `validation.json` 新增 `test_coverage` 对象，包含状态、CTest 版本、配置上限、已发现条目数、行并集是否一致和完整组记录。每组含 `tests_total`、`tests_failed`、`tests_skipped`、`tests_disabled`、`tests_not_run`、`selected_tests`、独立覆盖及目标行状态。

`test-coverage/summary.json` 保存整个采集结果，`t0`、`t1` 等子目录保存各组材料；用序号命名目录，避免把测试名称当作路径。主构建运行及扫描编号保持原关联，额外重跑不创建新的扫描，也不覆盖原整套测试的 JUnit 文件。批次顶层既有 `test_coverage` 字符串仍表示整套 gcov 的采集状态；新逐组证据位于各候选验证对象内。

## 复现

```powershell
python scripts/accept_repair_set.py --per-test `
  --cli PACKAGE/codeguard-cli.exe `
  --core-cli build/p2-2-flow-20261026-core/codeguard-cli.exe `
  --toolchain build/p0-windows/toolchain-4 --output artifacts/per-test-reproduction-NEW
```

`samples/repair-test-demo` 提供五个声明测试及两个故意的条件问题。两个比较候选组合后，zero 组只执行 zero.cpp 的目标行，one 组只执行 one.cpp 的目标行并带上 ready/cleanup，idle 不执行两处目标行。该示例用于检查归因，不是完整修复真值。

最终功能关联 13 项，加既有构建成功检查，共 14 项通过。覆盖计数污染、名称精确选择、fixture 依赖闭包、失败/禁用、源码或测试目录改写、行并集漂移、上限、取消和长测试名；JUnit 使用有界顺序扫描，压力用例的名称包含 2048 个 `&` 和中文字符。早期 12 项关联结果另行保留。

| 验证范围 | 实际结果 |
|---|---|
| Core Debug 完整回归 | 65 通过，1 因符号链接权限跳过 |
| 最终产品 Clang/Qt 完整运行 | 178 项：176 通过、1 权限跳过、1 局部数据流缓存超时 |
| 缓存专项诊断复测 | 三轮 64 TU、命中数及结果一致性均通过，107.53 秒 |
| 最终运行包 | 五测试 CLI 归因、行为反例、无 Clang 的 Core 边界、混合模板及原文件 SHA-256 一致；隔离 PATH/Qt/字体检查通过 |

全量超时现场只有串行基线入库、43/64 个冷缓存记录，尚未进入暖缓存，最长路径 202 字符。原 120 秒外层预算没有覆盖当时三轮任务；仅将 `codeguard.local_flow_cache` 的集成用例预算调为 300 秒，加入阶段日志及失败现场保留。复测基线完成于 44.984 秒、冷缓存完成于 89.541 秒、暖缓存完成于 105.953 秒，全部 64 个案例及一致性断言保留，应用进程超时未调整。

完整运行和该单项复测合计，178 项均有通过或权限跳过记录（177 通过、1 跳过）；这是两个运行的覆盖审计，**不是单次全绿**。另一次早期全量运行中的两个耗时异常构建失败，在相同二进制下立即复测通过，原始根因未确认，失败与复测记录均保留。三个固定真实库本轮未重新运行，前阶段记录不作为本轮验收。

构建使用已有锁定工具链增量目录。全部阶段、源文件与二进制 SHA-256 及原始 JUnit 见 [证据汇总](evidence/repair-test-coverage-20261027.json)。运行包为 `artifacts/p3-test-coverage-20261027/CodeGuard-Windows-x64-P3-Test-Coverage.zip`，旁边保留 `.zip.sha256`。当前尚未提交/推送，独立远端 CI 和另一台实体电脑验收未执行，正确性保持未评估。
