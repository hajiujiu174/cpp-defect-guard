# 逐测试组覆盖演示

两个条件赋值故意使分类测试失败。组合两个“改为相等比较”候选后，`zero [sample]` 只执行 zero.cpp 的修复行，`one.sample` 只执行 one.cpp 的修复行；`idle` 不执行这两行。`one.sample` 通过 CTest fixture 自动带上 ready 和 cleanup，记录中明确列出整组测试，不能把辅助测试执行归因于单个目标测试。

在 Qt 勾选 gcov 采集和逐测试组重跑，或使用 `repair validate-set ... --coverage gcov-tests --gcov-tool PATH/gcov.exe`。需要 GNU 编译器、CTest 3.29+ 和 ASCII 输出目录。原始工程不自动写回；该合成示例不用于计算真实修复正确率。
