# Juliet 数据导入与首批扩容验收

后续复核更正：首批“答案词复查”仅检查了独立词或前缀，漏过 18 个函数中的 `dataGoodBuffer` / `dataBadBuffer`。v2 清洗与同批对照见 [修正说明](juliet-correction.md)。下文首批数字是历史记录，不能据其清洗检查认定完全消除了标签线索。

## 范围

本项目新增 `import-juliet` 命令，用于导入 [NIST Juliet C/C++ 1.3.1](https://samate.nist.gov/SARD/test-suites/116) 的受控子集。导入器锁定官方归档 SHA-256：

```text
331b288f17ea95076d76e7bcc1d0e307f9e78241b16e744340213c8e2986919a
```

首个配置文件只接纳 C、局部控制流、无参数 `void` 叶函数的正负样例对；默认候选 CWE 为 121、122、124、126、127、369、401、415、416、457、476，流变体为 01–07、15–18。C++、跨过程控制流、包含函数定义的前置代码以及无法安全隔离的样例会被拒绝并记录原因。该范围是为了让本机原生 LibTooling 和训练管线可重复运行，不表示完整覆盖 Juliet。

标签来自 Juliet 的 `bad`/`good` 构造约定，绝不使用本项目静态规则的结果作为真值。当前不导入可信的缺陷行标签，因而只报告二分类指标，行定位指标的分母为 0。

## 安全与泄漏控制

- 仅读取 ZIP；在任何写入前验证固定 SHA-256，并拒绝绝对路径、`..`、反斜杠、盘符、大小写冲突、符号链接、加密成员和超限成员。
- 输出目录必须尚不存在，且必须与归档下载目录独立；导入器不执行归档中的脚本、构建系统或测试。
- 每个已接纳样例都保存原始字节、归档路径、原始 SHA-256、行映射与生成源哈希。生成源将函数改名为 `process`，并移除 `CWE`、`good`、`bad`、`POTENTIAL FLAW`、`FIX` 等显式答案线索。
- 同一 Label Definition 文件以及保守的规范化模板哈希会合并为来源组；数据切分按整个组隔离。精确重复源码只保留一个代表样本，同时保留去重追溯关系。
- 生成的 `CMakeLists.txt` 仅定义 `OBJECT` 库。验证只编译，绝不链接或运行带未定义行为的样例。

## 可复现导入

先从 NIST 下载官方 ZIP 到独立下载目录。以本机第一批为例：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
$env:PYTHONPATH = "$PWD\src"

py -3.13 -m defectguard import-juliet `
  --archive .\artifacts\datasets\juliet-1.3.1\juliet-1.3.1.zip `
  --output .\artifacts\layer2\juliet-新的批次 `
  --max-cases 120 --seed 42

cmake -S .\artifacts\layer2\juliet-新的批次\corpus `
  -B .\artifacts\layer2\juliet-新的批次\corpus\build `
  -G "MinGW Makefiles" -DCMAKE_C_COMPILER=D:\msys64\mingw64\bin\clang.exe
cmake --build .\artifacts\layer2\juliet-新的批次\corpus\build `
  --target juliet_cases --parallel 4
```

然后为该批次建立严格 Clang 配置，`parser.backend` 设为 `clang`，`clang_tool` 指向原生分析器的绝对路径，`compilation_database` 设为相对于被扫描 `corpus` 的 `build/compile_commands.json`。执行：

```powershell
py -3.13 -m defectguard export-graphs .\artifacts\layer2\juliet-新的批次\corpus `
  --config .\artifacts\layer2\juliet-新的批次\juliet-clang.toml `
  --output .\artifacts\layer2\juliet-新的批次\graphs
py -3.13 -m defectguard prepare-dataset `
  --graphs .\artifacts\layer2\juliet-新的批次\graphs\graphs.jsonl `
  --labels .\artifacts\layer2\juliet-新的批次\labels.jsonl `
  --output .\artifacts\layer2\juliet-新的批次\dataset --seed 42
```

模型训练应在 `.venv-ml` 中显式执行，并写入新的空目录；不要用测试集选择模型、轮数或随机种子。

## 首批 2026-09-08 验收

产物：`artifacts/layer2/juliet-20260908-batch120`；机器可读结论：`acceptance.json`。

| 项目 | 结果 |
|---|---:|
| 官方归档校验 | 通过，SHA-256 与锁定值一致 |
| 适格候选 / 拒绝 | 4,180 / 18 |
| 已选用例对 / 导入函数 | 120 / 240（正负各 120） |
| 只编译 CMake OBJECT 目标 | 通过，Clang 22.1.8 |
| 原生 LibTooling 图 | 240 个函数，`clang-libtooling` 后端 |
| 去重后数据集 / 来源组 | 239 / 29 |
| 固定切分（训练/验证/测试） | 145 / 40 / 54，三部分均正负平衡 |
| 答案词复查 | 240 个生成函数均未出现 `CWE`、`good` 或 `bad` |
| 融合检查点重载 | 54 个测试预测逐项一致 |

本次仅进行了管线冒烟训练：GNN 5 轮、sequence 3 轮、fusion 3 轮。fusion 的测试 F1 为 0.667，但该模型将测试集全预测为正例；另外两种模型 F1 为 0。它们只能证明训练、保存和重载路径可用，不能作为公开基准结果或模型优劣结论。

## 边界

Juliet 是构造性基准，可能保留未被完全消除的模板特征，不能替代真实工程评测。模板哈希只是保守的近克隆分组启发式，不是语义等价证明。归档中没有被该导入器收集到的根许可证文件；使用、再分发或发表前，应再次核对 NIST 页面和归档附带的适用条款。
