# 程序图数据接口

## 生成

先运行 `scripts/run_native_demo.ps1` 完成构建与样例准备，或对已有编译数据库的项目单独执行：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
$env:PYTHONPATH = "$PWD\src"
py -3.13 -m defectguard export-graphs .\samples\vulnerable `
  --config .\config\cmake-demo.toml `
  --output .\artifacts\graphs
```

输出 `graphs.jsonl` 和 `manifest.json`。每行对应一个函数；只能从含真实 AST 的原生结果导出。降级解析结果不会被包装成 AST/DFG。

## 数据结构

| 字段 | 内容 |
|---|---|
| graph_id | 文件、函数签名、位置和源码哈希组成的稳定标识 |
| file / function / signature | 相对文件名、函数名、类型签名 |
| source / source_sha256 | 按 Clang token 范围截取的源码与 SHA-256 |
| source_slice | token-range；旧协议缺少偏移量时为 line-range-fallback |
| nodes | AST 与 CFG 节点，含层次、类型、变量标识和源码行列 |
| edges | 带类型的有向边 |
| edge_index | 两行整数数组，索引对应 nodes 中的位置 |
| edge_type | 每条边的类型编号，与 manifest 中的词表一致 |
| calls | 调用表达式的节点、直接目标名称和源码行；间接目标可能为空 |
| rule_findings | 函数范围内的规则告警，不是训练真值 |
| label / label_source | null / unlabeled，需要外部可靠标注 |
| dfg_status | 数据流覆盖范围或不可用状态 |

边类型编号固定：AST 父子边 0、CFG 后继边 1、CFG 到 AST 包含边 2、定义—使用边 3。节点 ID 在每个函数内唯一；跨函数使用 graph_id 区分，不依赖内存地址。清单包含图文件哈希、源文件指纹和没有函数图的文件列表。

`experiments/network.py` 已将 edge_index/edge_type 映射到 RGCN，并增加类型独立的反向边（编号 4 至 7）。节点种类词表只从训练集拟合；源码经冻结编码器生成序列向量。图导出与在线推理共用同一构造函数。

## 定义—使用分析

C++ 端用 Clang CFG 提取局部标量的定义、读取和未初始化声明事件。Python 端先排除不可达块，再对前驱定义集合求并集，按语句顺序执行覆盖/失效操作，迭代到固定点，最后连接到达定义与读取节点。

- 分支汇合保留多个可能定义。
- 循环保留迭代前后可能到达的定义。
- 变量标识来自声明，内外层同名变量不会合并。
- 多变量声明按 Clang CFG 拆分后的实际顺序处理。
- 参数在入口形成定义；未初始化声明不会伪造有效初值。

当前不做路径可行性、跨函数传播、指针别名、堆/数组元素和调用副作用分析。关系是保守的局部数据依赖，不能直接当作缺陷证明。

## 可复现与后续训练

重复扫描同一源码和编译配置，图文件保持一致；清单不写入随机运行 ID。导出前再次核对源码指纹，避免图与扫描结果不同步。

v0.4.0 已补充独立自建样例、来源组划分、固定种子与近似词法精确去重，并跑通 GraphCodeBERT 序列基线、RGCN 融合与评估。尚需核对许可证后接入代表性公开数据集、扩充近克隆检测和多种子统计。不得把规则告警当作真值后在同一批样例上宣称模型准确率。详见 [第二层实验说明](layer2-experiments.md)。
