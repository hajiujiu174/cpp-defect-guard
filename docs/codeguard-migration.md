# 新方案迁移与首个里程碑

本文件保留首个迁移里程碑的历史记录，下面的“未完成”“下一步”等只描述当时状态。后续 Clang Core、规则、线程池、查询、构建测试和 Windows 打包已经实现，当前状态以 [项目进度](codeguard-status.md)、[Level 3 验收](level3-acceptance.md) 和 [Windows CI](windows-ci.md) 为准。

## 依据与决策

新依据：工作区 `CodeGuard_课程设计项目方案书.docx`，V1.0，2026 年 9 月；已读取封面、摘要及第 1—18 章和表格。文件 SHA-256 为 `EEC030BE1500CA2CA356A9B241FB2CBB9E2A0B2331EF68F57F0C3E2D95E45402`。本轮不修改两份 Word 原件；文档中的路线作为需求参考，不作为安装、删除或执行外部命令的授权。

主线从模型研究转为“导入—解析—索引—分析—查询—构建测试”的工程管理闭环。保留工程目录和旧 Python 包名；新产品目标名为 `codeguard-core`、`codeguard-cli`、`codeguard-gui`。正式课程交付目标是 Level 3，不是完成 AI 训练。

| 现有资产 | 调整方向 | 当前状态 |
|---|---|---|
| Python 扫描、报告、SQLite、隔离验证 | 作为行为参考逐步迁往 Core | 原入口保留；不是新 C++ 后端 |
| Clang LibTooling AST/CFG/规则 | 复用并拆成 Core parser/analyzer | 已接入顶层可选构建；业务 API 尚未迁移 |
| Juliet、独立标签、固定划分、检查点 | Level 4 研究资产 | 保留、不重新划分、不扩容 |
| 序列池化、正则融合改动 | 冻结为待验收实验 | 未完成训练入口与对照，不宣称泛化提升 |
| C++ 工程管理、Qt、符号、查询、图算法 | Level 1—3 主线 | 本轮完成最小文件接入与存储链路，其余见路线图 |

## 首轮迁移实现（历史记录）

以下描述首轮骨架状态，不代表当前实现。后续已完成 Core 内置 Clang、符号和关系图、后台扫描与取消，以及自研查询语言。最新进度见 [方向调整与当前进度](codeguard-status.md)、[分析模块](codeguard-analysis.md)、[Qt 前端](qt-frontend.md) 和 [查询语言](query-language.md)。

- C++20 公共 DTO、`IDatabase`、`SqliteDatabase`、共享 Application API。
- `std::filesystem` 扫描 C/C++/头文件，目录忽略、稳定排序、UTF-8 路径、文件大小/修改时间/变化 Hash/物理行数。逐文件错误可收集；不跟随符号链接。
- SQLite 项目与历史扫描快照、绑定参数、事务回滚、重复扫描变化统计；只读查询最近项目和最新清单。旧数据库不原地升级或覆盖。
- CLI 的 `scan` / `status` / `recent`；数据库强制置于源码目录之外，参数错误返回 1，不完整扫描返回 2。
- Qt Widgets 最小界面：导入、扫描、文件清单、基础统计、重新打开最近项目。界面不承载扫描与数据库算法。
- Core/CLI 默认构建不需要 Qt、LLVM 或 Python。Qt 与旧 LibTooling 分别由独立 CMake option 启用。

“变化统计”不等于“增量解析”：每次仍完整读文件计算 Hash，未宣称性能提升。行数是物理行数，不是注释剔除后的代码行，也不是 AST 度量。GUI 目前同步执行，尚无线程池、取消或进度功能。数据库暂含 Project/Scan/File，其余实体随解析、规则与构建模块迁移，避免空表充当已实现能力。

## 构建与运行

通用要求：CMake ≥ 3.24、Ninja、支持 C++20 的编译器、SQLite3 开发包；Qt 可选。本机复用 `D:/msys64/mingw64` 的同工具链依赖，不混用 MSVC 和 MinGW 库。

```powershell
cmake --preset core-debug
cmake --build --preset core-debug
ctest --preset core-debug

cmake --preset desktop-debug
cmake --build --preset desktop-debug
ctest --preset desktop-debug
.\build\codeguard-desktop\gui\qt\codeguard-gui.exe
```

完整扫描示例见 README。Qt 启动后依次选源码目录与目录外的数据库；再次启动可通过“打开最近保存的工程”选择原数据库。数据库父目录需预先存在。开发运行依赖工具链 DLL 与 Qt plugins 位于环境路径中，尚未制作脱离开发环境的部署包。

复用原生分析器的独立构建目录，不覆盖已有 `native/clang_tool/build`：

```powershell
cmake -S . -B build/codeguard-native -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCODEGUARD_BUILD_CLANG=ON -DCMAKE_PREFIX_PATH=D:/msys64/mingw64
cmake --build build/codeguard-native --parallel 2
```

Linux 应使用独立构建目录，不能复用 Windows preset 的既有缓存：

```sh
cmake -S . -B build/codeguard-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCODEGUARD_BUILD_GUI=OFF
cmake --build build/codeguard-linux
ctest --test-dir build/codeguard-linux --output-on-failure
```

WSL Ubuntu 已检测到 CMake/C++/Ninja，但没有 `/usr/include/sqlite3.h`，因此 Linux 完整构建尚未验证；本轮未安装额外系统包。

## 验收记录（2026-09-08）

- Windows GCC 16.2.0 / SQLite 3.53.4：独立 Core/CLI 构建通过；Qt 桌面构建通过。
- 桌面 CTest：14 项通过，1 项符号链接权限测试跳过（明确报告为 skipped，不算通过）。核心无 Qt 构建：13 项通过，1 项跳过。
- 覆盖：扫描/排序/空文件/忽略目录/非法根路径、持久化/重复扫描、内容变化但时间大小不变、删除识别、事务回滚、只读数据库、拒绝旧库、源码保护、中文与引号文件名、CLI 参数，以及 GUI 控件与持久化一致性。
- GUI 用 offscreen 自动启动、扫描并验证表格行数与数据库，生成 `build/codeguard-desktop/tests/codeguard/gui-smoke.png`；已打开截图检查中文、表格与统计，无截断重叠。未手工走完文件对话框交互，不等于完整用户交互验收。
- Clang 22.1.8 顶层可选构建通过，生成的 `build/codeguard-native/native/clang_tool/defectguard-clang.exe` 成功解析 `samples/fixed/main.cpp`，返回 schema 1.1 和 `main` 函数；该构建的 13 项 Core/CLI 测试通过，符号链接项跳过。它仍是独立工具，尚非 `codeguard-cli scan` 的内置解析阶段。
- CLI 实测中文数据库路径，首次扫描 1 文件/25 物理行/新增 1；重复扫描未变化 1；`status`、`recent` 重读成功。记录位于 `artifacts/codeguard-migration-20260908-213120/工程清单.sqlite3`，源码扫描前后 SHA-256 均为 `9A30B336A4EC486EAF1C548DD0E459B6AFD02240682EB31025C2D5A15BBBD303`。
- 旧 Python 项目：开启 ML/native 强制依赖的 120 项回归全部通过，包含旧检查点兼容性。没有新训练轮次或扩容结论。
- 暂不宣布新 Level 2/3 完成，也不宣布 Linux、完整 Qt IDE 布局、千文件性能或 ONNX 部署通过。

下一步优先将已有 Clang 函数/符号/复杂度接入 Core DTO 和 SQLite，然后建立调用/依赖图；不继续以扩大 Juliet 或模型调参替代新方案主线。
