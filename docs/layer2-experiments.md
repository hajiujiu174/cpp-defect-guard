# 第二层模型实验与验收

## 当前交付范围

v0.4.0 在原生 AST/CFG/局部 DFG 的基础上实现“外部独立标签—来源组划分—训练—评估—检查点—原生扫描”的可运行闭环。当前实现三种二分类模型，不生成或应用修复补丁。

v0.5.0 在上述闭环上补充完整性校验、环境预检、覆盖诊断和多种子汇总，详见 [第二层可靠性与稳定性验收](layer2-reliability.md)。下方 2026-09-06 的记录作为历史单种子基线保留。

| 模式 | 输入与学习方式 | 定位能力 |
|---|---|---|
| sequence | 冻结 GraphCodeBERT，注意力掩码均值池化，训练分类头 | 无；报告位置仅是函数入口 |
| gnn | 节点类型及结构数值特征，两层 RGCN，图均值池化 | AST 节点按源码行聚合的候选排名 |
| fusion | 拼接序列投影与图向量，联合训练分类头、GNN、定位头 | 同 GNN；不是缺陷位置证明 |

这里没有复现原始 GraphCodeBERT 的图引导注意力，也没有对编码器做全参数微调。它是固定序列编码与独立程序图网络的教学基线。默认模型特征不包含路径、标签、group_id、样例 ID、规则告警；节点种类词表仅在训练集拟合，未见类型映射到 0。

## 环境与来源

本机实际验证环境：Windows、Python 3.12.14、CPU PyTorch 2.14.0+cpu、Transformers 5.16.1、PyTorch Geometric 2.8.0.post1、safetensors 0.8.0；原生解析继续使用 LLVM/Clang 22.1.8 和 CMake 4.4.3。模型依赖在项目 `.venv-ml`，基础扫描的 Python 不受影响。

准备环境：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
.\scripts\setup_ml.ps1 -DownloadEncoder
```

脚本优先寻找已有 Python 3.12，也可用 `-Python` 指定解释器完整路径；不会安装系统 Python 或修改系统 PATH。CPU PyTorch 来自官方 wheel 源，其余依赖按 `requirements-ml.txt` 固定。依赖验证后需要联网下载一次编码器，后续训练和扫描均使用本地文件。

编码器来源为 [Microsoft GraphCodeBERT 权重仓库](https://huggingface.co/microsoft/graphcodebert-base)，固定修订 `2b0488a7bb0eefc7041f1bb2cad1ab26b0da269d`。本次下载的 `pytorch_model.bin` SHA-256 为 `fc542850abf74be2df516bcdedfc2dcdb9bd02c8098a6d5f4d63da73cbcb9e71`，与本地下载元数据的 LFS 标识一致。实际模型元数据另保存权重和 tokenizer 文件的组合指纹。

加载设置 `local_files_only=True`、`trust_remote_code=False`，禁用未使用的 pooling 层。预训练语言模型头不参与本任务，加载时出现 `lm_head.* UNEXPECTED` 是移除任务头的预期提示；不应出现编码器主干缺失权重。上游说明参考 [CodeBERT/GraphCodeBERT 项目](https://github.com/microsoft/CodeBERT/tree/master/GraphCodeBERT)、[RGCNConv 接口](https://pytorch-geometric.readthedocs.io/en/latest/generated/torch_geometric.nn.conv.RGCNConv.html) 和 [PyG 安装说明](https://pytorch-geometric.readthedocs.io/en/latest/install/installation.html)。重新分发权重或接入公开数据集前，应单独核对对应条款。

## 数据与划分

自建输入位于 `samples/ml_corpus`：24 个函数、12 对缺陷/修正版本、7 个来源/模板组。每个正例独立标注主要缺陷行，负例不含缺陷行。标签为项目自建并经代码逻辑核对，不来自静态规则，也未经第三方独立专家审定；未导入公开基准。

`labels.jsonl` 每行至少提供：

```json
{"sample_id":"case01-a","group_id":"bounds","file":"case01/a.cpp","function":"process","label":1,"label_source":"self-authored-reviewed","defect_lines":[3]}
```

- `file` 是相对于被分析工程的路径，行号是原始文件绝对行号，不是函数内偏移。
- 重载函数须提供 `signature`，确保一个图对应一个标签。未匹配、歧义、重复 ID、图索引越界、源码哈希不一致均报错。
- 正例允许缺少行标签，但此类样例不参与定位训练和定位指标分母；负例不能包含缺陷行。
- `group_id` 应代表原工程、同源模板、修复对等隔离单元。不同名称不能证明来源独立，仍需来源审计。
- 划分前去除忽略注释/空白后的近似词法精确重复；保留字符串与常见多字符运算符差异。含预处理标记或续行的函数保留精确源码哈希，防止错误合并。该实现不是完备 C++ 预处理器，也不解决语义近克隆。
- 同一源码出现在多个组时，先合并相关组再保留一个代表样例；冲突二分类标签直接报错。`duplicate_ids`、`source_groups` 保存去重关系。
- 固定种子 42，在保持整组隔离的前提下尝试覆盖正负类；比例按组取整，不保证样例数精确为 60/20/20。
- 清单记录数据集哈希、标签哈希、组归属、样例归属及类别数量；加载时再次检查集合重叠、来源组和重复源码跨集合泄漏。

本次实际划分为训练 12、验证 2、测试 10 个函数，各集合正负均衡。数组边界与空指针组全部留在测试集；相关修复版本不会混入训练集。验证集只有一组两个函数，轮次选择容易受偶然性影响。

语料使用 CMake OBJECT 目标，只编译、不链接或执行含未定义行为的正例；编译通过不代表它们没有缺陷。图、标签、权重和指标分别保存，不改写输入源码。

## 一键运行及产物

```powershell
.\scripts\run_ml_demo.ps1 -Ablations -RunTests
```

可用 `-OutputDirectory` 指定尚不存在的目录，`-Epochs` 指定训练轮数。默认 40 轮；每次创建新的实验目录，保留旧检查点。

```text
artifacts/layer2/demo-时间戳/
├─ graphs/                    原生图与图清单
├─ dataset/                   已标注 dataset.jsonl 与 splits.json
├─ sequence/                  序列基线
├─ gnn/                       纯图基线
├─ fusion/                    融合模型
├─ fusion-no-cfg/              可选：去 CFG 节点、后继边和包含边
├─ fusion-no-dfg/              可选：去定义—使用边
├─ reloaded-test/             融合模型从磁盘重载后的测试预测与指标
├─ model-scan/                JSON、Markdown、HTML 与 runs.sqlite3
├─ comparison.json / .md      同一测试划分上的实验对比
├─ scan-model.toml            可复用的模型扫描配置
└─ acceptance.json            重载、在线/离线预测、报告/存储及源码保护核对
```

每个模型目录含 `model.safetensors`、`metadata.json`、`history.json`、`splits.json`、`metrics.json` 和各集合预测文件。元数据保存种子、超参数、环境版本、节点词表、消融设置、最佳轮次、阈值、编码器/数据/划分/权重指纹。

编码器固定不训练；其特征缓存按源码、编码器指纹、序列长度和池化版本索引。跨组复用同一冻结编码器不等于拟合测试数据，标签和测试指标不参与编码；切勿把训练后的编码器当作预先固定的公共特征跨划分复用。

## 手动命令

设置解释器与项目模块路径：

```powershell
$env:PYTHONPATH = "$PWD\src"
$env:PYTHONIOENCODING = "utf-8"
$mlPython = "$PWD\.venv-ml\Scripts\python.exe"
```

已有原生图和独立标签时准备数据集：

```powershell
& $mlPython -m defectguard prepare-dataset `
  --graphs .\artifacts\graphs\graphs.jsonl `
  --labels .\samples\ml_corpus\labels.jsonl `
  --output .\artifacts\my-dataset --seed 42
```

训练新实验：

```powershell
& $mlPython -m defectguard train-model `
  --dataset .\artifacts\my-dataset --output .\artifacts\my-fusion `
  --mode fusion --encoder .\artifacts\models\graphcodebert-base `
  --epochs 40 --seed 42 --max-length 256
```

`--mode` 可取 sequence/gnn/fusion，`--without cfg` 与 `--without dfg` 可重复使用。没有实现规则特征输入，所以本版本不声称完成“去规则特征”消融。

重载检查点复评：

```powershell
& $mlPython -m defectguard evaluate-model `
  --dataset .\artifacts\my-dataset --checkpoint .\artifacts\my-fusion `
  --output .\artifacts\my-reevaluation
```

复评要求原始数据集及相同划分，不静默替换测试集。迁移权重位置后可用 `--encoder` 指向同指纹的编码器。

本次已完成的融合模型可直接扫描：

```powershell
& $mlPython -m defectguard scan .\samples\ml_corpus `
  --config .\artifacts\layer2\demo-20260906\scan-model.toml `
  --output .\artifacts\my-model-scan
```

通用 TOML 使用 `[models] backend="trained"`，`checkpoint` 指向模型目录，`encoder` 可选覆盖；这两种路径相对于配置文件。`threshold` 省略时使用检查点阈值。基础配置继续保持模型禁用。手动指定原生工具时需确保其动态库可解析，工程须有有效编译数据库。

## 训练与指标口径

CPU、2 个线程、固定随机种子、确定性算子；优化器为 AdamW，默认学习率 0.003，权重衰减 0.01，梯度范数上限 5。分类损失为二元交叉熵；图模型加入 0.25 倍定位损失，定位正例权重仅由训练集已知行计算并上限截断至 20。

按验证集分类 BCE 最小的轮次保存权重，不按测试结果选轮次或阈值。阈值默认固定 0.5。输出 Precision、Recall、F1、Accuracy、TP/TN/FP/FN；无预测正例时 Precision/F1 记为 0，不能据此认为任务成功。

定位先将同一行的 AST 节点 logit 取均值，再按得分降序排名；同分按行号升序。Top-1/3/5 的分母是**全部具有行标签的测试正例**，包括分类漏报。端到端 Top-k 额外要求函数分类为正例。无行真值或无定位能力时为 null，不能用函数起始行冒充定位命中。

## 本机验收记录：2026-09-06

实际运行：`scripts/run_ml_demo.ps1 -OutputDirectory artifacts/layer2/demo-20260906 -Epochs 40 -Ablations -RunTests`。48 项自动化测试全部通过，无跳过（同时强制原生及 ML 环境）；覆盖数据泄漏防护、指标口径、模型三种模式、CFG 消融、训练/重载、相同种子参数复现、模型告警接入、原有规则/图/报告/隔离验证。

| 实验 | 测试 Precision | Recall | F1 | Accuracy | Top-1 | 端到端 Top-1 |
|---|---:|---:|---:|---:|---:|---:|
| sequence | 0.60 | 0.60 | 0.60 | 0.60 | 不支持 | 不支持 |
| gnn | 0.50 | 0.40 | 0.4444 | 0.50 | 0.60 | 0.40 |
| fusion | 0.60 | 0.60 | 0.60 | 0.60 | 0.60 | 0.60 |
| fusion-no-cfg | 0.00 | 0.00 | 0.00 | 0.50 | 0.60 | 0.00 |
| fusion-no-dfg | 0.50 | 0.40 | 0.4444 | 0.50 | 0.60 | 0.40 |

融合模型最优轮次 7，测试混淆矩阵 TP=3、TN=3、FP=2、FN=2；重新从磁盘加载后，全部测试概率、行排名和指标与训练结束产物逐项相同。在线原生扫描 24 个函数，模型输出 8 条 ML001，另有 4 条 AST 规则和 1 条词法规则；在线概率与离线预测逐项相同，三种报告保留模型标记，SQLite 分类计数一致，源码指纹未变化，无跳过文件及序列截断。扫描全语料仅验证接口，不能替代独立测试集指标。

## 尚未完成与限制

- 当前 24 个函数只用于验证流程；历史单种子及新增三种子结果均不能证明融合更优，也不能用高 Top-k 掩盖分类漏报。小函数的 Top-5 很容易覆盖大部分语句。
- 纯 GNN 不编码标识符或字面量值，无法完整区分仅常量变化的等构程序；融合所用序列特征仍须在代表性 C/C++ 数据上验证。
- 缺少代表性公开数据、专家复核、近克隆审计、可靠区间估计、概率校准和缺陷类别头；初步多种子统计已在 v0.5.0 完成。
- 编码器默认最多 256 token（可设 8–512）；超过限制截断并显式计数。尚未做长函数分块；图节点超过 20000 时明确报错，不静默截断。
- AST/CFG/DFG 仍受第一层语义范围限制：不做跨函数、指针别名、堆/数组元素与调用副作用建模。
- 模型疑似告警统一为中等严重度；置信度未经校准，不是安全证明。模型默认禁用，缺少依赖/检查点/原生解析时明确报错。
- 第三层模板修复、候选应用/排序、掩码或扩散修复仍待实现，保持原始工程不被自动改写。
