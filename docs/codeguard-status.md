# CodeGuard 方向调整与当前进度

更新日期：2026-09-09。依据工作区中的《CodeGuard 课程设计项目方案书》V1.0。

项目主线为“基于静态程序分析的跨平台 C/C++ 软件质量分析与工程管理系统”。验收目标采用方案书的 Level 1—4 分级，以 Level 3 为课程交付目标。Python 模型实验保留为 Level 4 研究资产，核心程序不以模型训练作为运行条件。

## 架构与模块

```text
cpp-defect-guard/
├─ core/                 纯 C++20 核心库
│  ├─ include/codeguard/ 公共 DTO 和业务接口
│  ├─ project/           文件扫描与变化统计
│  ├─ parser/            可选 Clang LibTooling
│  ├─ graph/             遍历、SCC、环与拓扑排序
│  ├─ query/             词法、语法、语义分析与快照执行
│  └─ database/          SQLite 扫描与分析持久化
├─ cli/                  codeguard-cli 原生命令行
├─ gui/qt/               Qt 工作台、查询页与后台扫描
├─ tests/codeguard/      C++ / CLI / Qt 回归
├─ samples/              工程分析样例与历史实验数据
├─ src/defectguard/      保留的 Python 原型和模型实验
└─ docs/                 当前验收、路线图与历史记录
```

## 与新方案的对应关系

| 级别 | 当前已有 | 仍需完成 |
|---|---|---|
| Level 1 骨架 | Core/CLI/Qt、目录扫描、项目快照、最近项目、SQLite 事务 | 更完整的配置管理与部署打包 |
| Level 2 分析 | Core Clang、USR 符号与定义、CFG 复杂度、直接调用/include 图、图算法、覆盖诊断、源码定位、五类 AST 规则 | 引用/宏索引、交互图画布、更完整度量及更广规则评测 |
| Level 3 工程闭环 | 自研查询语言、CLI/Qt 查询、多 TU 线程池、单写线程、有界队列、进度/取消、Windows/POSIX 进程、CMake/CTest/Git、BuildTest/Issue 持久化、并发实验入口 | 完整 Linux Clang/Qt 环境验收、2—3 个外部真实工程验证；具体本机验收见 Level 3 说明 |
| Level 4 研究 | 保留旧 AST/CFG/DFG、Juliet 数据、模型实验和检查点 | 可选 ONNX C++ 部署或自动修复对照，不能宣称已完成 |

## 查询阶段调整（上一轮记录）

1. 新增 `core/query` 与公共查询 DTO，实现四类实体的只读查询；对空表也进行字段和类型检查。
2. CLI 新增 `query` 命令，从只读 SQLite 连接恢复快照后执行；输出表格，执行计划与统计输出到标准错误流。
3. Qt 新增查询页，复用 Core 执行器，支持错误反馈、执行计划和源码定位；新快照载入时清空旧查询结果。
4. 新增语法、优先级、排序、输入边界、覆盖状态测试；扩展数据库查询和 Qt 真实控件信号回归。
5. README、路线图和迁移记录统一到新方向，明确历史状态，避免把旧三层模型路线当成当前课程排期。

原方案书、旧实验数据与模型文件保持原状。最新一轮补齐了规则、线程池与构建测试，详情见 [Level 3 验收](level3-acceptance.md)；不将其等同于整份 16 周计划所有扩展项已完成。

## 后续实施顺序

规则、Issue 存储、多 TU 分析池、集中写入、ProcessRunner 及构建/测试/Git 业务服务已经实现。后续重点是扩大真实工程覆盖、补齐 Linux 全功能环境、完善增量分析与引用索引。AI 新训练和自动修复在工程主线稳定后推进。

本次查询验收与运行方式见 [查询语言说明](query-language.md)。
