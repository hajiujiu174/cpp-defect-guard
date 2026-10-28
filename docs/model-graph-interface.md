# 无标签程序图研究推理接口

日期：2026-10-28。本轮补充 `infer-graphs`，从现有无标签程序图导出包调用已有检查点。接口只输出研究预测与覆盖记录，不训练、不下载模型、不改写原工程；默认 C++ Core/CLI/Qt 和已交付运行包保持原来的确定性流程。

## 使用

在项目根目录使用已有模型环境：

```powershell
$env:PYTHONPATH = "$PWD\src"
$env:PYTHONUTF8 = "1"
$env:HF_HUB_OFFLINE = "1"
$env:TRANSFORMERS_OFFLINE = "1"
.\.venv-ml\Scripts\python.exe -B -m defectguard infer-graphs `
  --graph-bundle artifacts/layer2/demo-20260906/graphs `
  --checkpoint artifacts/layer2/demo-20260906/fusion `
  --encoder artifacts/models/graphcodebert-base `
  --output artifacts/new-graph-inference
```

输出目录必须尚不存在，且位于图包、检查点和编码器目录之外。输出包含 `predictions.json` 与最后写入的 `manifest.json`。参数注册和输入格式检查不导入 Torch；实际推理使用已有可选模型依赖。基础 Python 环境可以显示帮助和拒绝无效图包，不自动安装依赖。模型加载继续使用本地文件和既有权重/形状/有限值/编码器指纹门槛。

## 输入与输出约定

输入仍为 [程序图导出协议](program-graphs.md) 1.0：目录内的 `graphs.jsonl` 和 `manifest.json`。新入口核对图文件原始字节 SHA256、函数数量、Clang 后端、固定边类型词表、源码指纹格式、未覆盖文件清单及 DFG 范围说明。逐函数必须提供完整协议字段、有效节点/边、源码哈希、可重算且唯一的 graph_id。缺失或未来版本、降级解析、重复函数位置、错配身份、监督标签或训练样例身份均报错。规则提醒仍只是附带信息，不作为标签或模型特征。

结果格式为 `CodeGuardGraphInference 1`。每条预测绑定 graph_id、函数源码 SHA256 和行范围；保留分数、排名行及行分数。阈值固定沿用检查点，当前命令没有按预测结果调阈值的选项。判定为 `suspected` 或 `below_threshold`；低于阈值不能解释为没有缺陷。置信度明确为 `uncalibrated`，正确性仍 `not_assessed`。

结果清单绑定图包两文件、检查点元数据与权重以及预测文件的大小和 SHA256；推理后再次核对输入，并核对编码器当前文件组合指纹。预测数量、身份、有限分数与行范围和编码覆盖不一致时不生成成功结果。检查是前后内容核对，不声称对其他进程的并发文件改写提供原子隔离。

覆盖范围明确记录为 `exported_functions_only`，仓库覆盖为 `not_assessed`。保留未生成函数图的文件、token-range/line-range-fallback 计数和 DFG 状态；不把“导出列表为空”解释为整个仓库已覆盖。序列模型逐函数记录原始/保留 token 数、是否截断；纯图模式标为 `not_used`。序列截断不表示图分支也被截断，图与 DFG 的范围按各自声明解释。

## 本机验收

- 141 项非训练 Python 检查全部通过、无跳过，包含新增 14 项接口检查和真实原生解析检查。排除会建立训练检查点的 `test_ml.py`；sweep 单测使用伪训练器，未执行新的模型训练。
- 原有 24 个自建函数图用旧 fusion 检查点实际离线推理；分数、行排名、行分数与既有 train/validation/test 记录逐项相等。这里只证明接口预测等价，不把全部旧划分混合后作为独立准确率评测。
- 新建的 96 条更新语句长函数经 Clang 实际导出 294 个 AST 节点、token-range 源码切片；实际编码 786 token，保留 256，完整报告一项截断。该函数仅用于覆盖行为检查，没有缺陷真值标签。首轮测试配置把编译数据库目录传给 Python 解析器而失败；改为具体 JSON 文件后通过，生产代码未因此调整，失败说明保留。
- 篡改专用图文件的真实命令以退出码 2 拒绝，未创建输出目录；旧图包、检查点、原始划分和长函数输入摘要核对保持。
- 已准备 Windows CI 的无模型依赖接口单测步骤；本轮尚未提交/推送或运行新独立 CI。上一轮 `92a94e9` 六作业结果不覆盖这些新 Python 改动。

机器记录见 [接口验收证据](evidence/model-graph-interface-20261028.json)，原始日志、预测和长函数图保存在 `artifacts/p3-model-graph-interface-20261028`。本轮没有增加模型权重、依赖安装或新的 C++ 产品集成。独立数据与标签审计、置信度校准、不依赖开发工作区的独立部署以及后续产品接入仍待完成。
