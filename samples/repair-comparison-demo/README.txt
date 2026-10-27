多轮覆盖对照演示：zero.cpp 和 one.cpp 中的赋值故意保留。
原版 zero / one 测试正常返回失败；两项 condition.compare 组合后通过。
helpers.cpp 未修改，逐组可观察 chosen / fallback 行执行记录的变化。
每版 2—5 轮独立构建；测试组包含依赖与自动 fixture。
这是合成演示，不提供真实修复正确性真值。
