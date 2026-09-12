# P1-1 工程导入与配置管理

更新日期：2026-09-13。本项面向 Windows，Core/CLI 使用纯 C++20，Qt 仅负责展示与操作。工程配置与扫描快照保存在源码目录外的 CodeGuard SQLite 数据库中，原方案书与源码不被配置编辑器改写。

## Qt 导入流程

1. 启动 Analysis 版，点击“导入工程并扫描”，选择工程根目录和源码目录外的分析数据库。
2. 自动查找根目录及向下三层中的 `compile_commands.json`。唯一候选直接使用；多个候选要求选择；没有候选时保存文件清单，并提示下一步。
3. 点击“工程设置”。可编辑 C/C++ 编译器、CMake/CTest/Git 命令、生成器、构建类型、目标、并行数、超时、CMake 定义、复制范围和五条规则的开关。保存后重新扫描使分析设置生效。
4. CMake 工程缺少参数时，点击“生成参数并分析”。程序核对已有快照，在源码副本中配置 CMake，将副本源码路径映射回原工程，并保留生成头文件所在的构建目录；配置成功后自动扫描。这个操作不代表编译或测试已通过。
5. 在“构建测试”页点击“构建并测试副本”。按同一工程的设置依次运行配置、构建及 CTest；查看阶段日志、测试数和历史记录。
6. 关闭并重新启动，默认恢复上次工程、配置和已保存快照，不自动扫描或构建。顶部提示当前展示的是历史快照。

设置按规范化工程根目录分别保存，同一数据库可保存多个工程，切换工程会载入对应设置。会话只记录上次数据库与工程路径，采用 `QStandardPaths::AppConfigLocation/session.ini` 文件，不写 Windows 注册表。直接启动 GUI 时可用 `--session-file <绝对路径>` 指定独立会话；`--project`/`--database` 显式启动参数优先。会话、源码或数据库丢失时显示具体错误，可以重新导入。

历史结果上方显示本快照停用的规则及显式命令选择数量。之后编辑设置不会修改旧快照；没有告警仍不等于没有缺陷。

## CLI 流程

以下示例在仓库根目录运行，先按 Windows 构建说明生成 Analysis 版本；`artifacts` 已存在。CMake、Ninja、编译器及 CTest 须可在当前 PATH 中找到，或在配置中填写工具的绝对路径。

```powershell
$cli = '.\build\codeguard-analysis\codeguard-cli.exe'
$project = '.\samples\level3-demo'
$database = '.\artifacts\configured-demo.sqlite3'

# 查看候选；不会执行工程的构建脚本
& $cli discover $project

# 保存一次；工具命令名或绝对路径，不能使用依赖当前目录的工具相对路径
& $cli config $project --database $database `
  --compile-commands auto --c-compiler clang --cxx-compiler clang++ `
  --output .\artifacts\configured-builds --jobs 4 --threads 4 `
  --build-type Debug --generator Ninja --timeout 120

# 未扫描时先导入清单，再在副本中配置 CMake；保存生成的参数路径
& $cli prepare $project --database $database
& $cli scan $project --database $database
& $cli build $project --database $database

# 新 CLI 进程加载同一工程配置与历史
& $cli config $project --database $database
& $cli status $project --database $database
& $cli builds $project --database $database
```

`config` 不带修改参数时只显示配置；其他命令加载已有配置，再应用本次命令行覆盖。`scan`/`build` 的临时覆盖不保存为工程默认值；`prepare` 成功后保存参数路径及配置覆盖。退出码：0 成功，2 分析部分覆盖或构建未通过，1 参数/配置/数据库错误，130 取消。`prepare` 的成功状态为 `configured`；只有完整构建测试通过才是 `passed`。

| 参数 | 含义 |
|---|---|
| `--compile-commands auto / none / PATH` | 自动发现 / 仅清单 / 显式编译数据库文件或目录 |
| `--cmake-define KEY[:TYPE]=VALUE` | 可重复；含空格的值作为完整参数传递，无 Shell 拼接 |
| `--copy-include resources/out` | 可重复；补充默认忽略目录中的必需资源 |
| `--copy-exclude experiments` | 可重复；从扫描清单和构建副本中同时排除项目相对目录 |
| `--disable-rule CG004` | 可重复；停用指定规则，快照记录停用项 |
| `--clear-list definitions/includes/excludes/rules/commands` | 显式清空对应列表；支持重复 |
| `--select-command <完整 SHA256>` | 为该命令所属文件选择本次分析的编译配置 |

指定某个列表参数时，第一次出现会替换该类已存列表，后续同名参数追加。未指定的列表保留。规则全部启用可用 `--clear-list rules`。编译器、类型与导出数据库开关采用专用字段，拒绝通过 `--cmake-define` 重复覆盖对应 `CMAKE_*` 字段。

## 多个编译配置

```powershell
& $cli commands $project --database $database
# 从上述输出复制完整指纹；每行同时展示源文件和完整 argv
& $cli config $project --database $database --select-command '<完整指纹>'
& $cli scan $project --database $database
```

Qt 的“编译命令选择”页先载入当前数据库的命令，再为歧义文件选择条目。原始数据库不被过滤或改写。选择绑定到工作目录、规范化源文件路径、输出和完整参数的 SHA256；再次扫描发现旧条目不存在时要求重新选择，不自动切换目标。删除文件的残留选择可在此页清除。

没有选择的多命令文件继续报告 `ambiguous_command`；没有精确条目的文件报告 `missing_command`。显式选择后，诊断仍记录未分析的其他配置数量。目标字段仅控制构建目标，不自动推断一组兼容的多文件宏配置；不同 TU 的符号/图仍是成功 TU 的合并视图。单个配置全部解析成功，不代表其他配置或整个仓库均已覆盖。

## 复制与依赖边界

- 默认忽略目录保持原行为；补充复制目录只改变副本的资源范围，不把默认忽略的第三方 C/C++ 文件纳入分析。显式排除同时影响扫描与副本，优先于补充；更改后需要重新扫描以通过快照一致性检查。
- 复制仅允许工程内的相对目录，拒绝 `..`、`.`、`.git`、符号链接与越界路径。外部依赖不自动搬运；可用 CMake 定义填写绝对依赖路径，依赖必须在本机可访问。依赖内容变化目前不会自动使旧结果失效，重新分析由用户启动。
- 支持 CMake 配置阶段在构建目录生成的头文件。该构建目录与映射后的编译数据库必须保留。若配置阶段在副本源码树生成或修改 C/C++ 文件，自动参数导入失败，日志给出原因，原源码保持不变。
- 构建阶段才生成的源文件/头文件、非 CMake 构建系统、PCH/module/response file，以及需要原目录布局的相对外部依赖，需要先用原构建系统准备匹配数据库；既有分析器不支持的参数仍会拒绝，不承诺所有工程都可自动导入。
- 生成只执行 CMake 配置，但项目脚本可执行程序或下载依赖。源码副本不是操作系统沙箱。每次创建独立运行目录；失败、超时、取消均保留日志，源码变化则要求重扫。

发现过程最多检查 2048 个目录，不跟随符号链接；超过限制或发生权限错误时提示手动选择文件。完整历史加载和候选发现目前在 GUI 线程中执行，大工程的异步优化仍属于 P2-1。

## 存储与验收

本地真实工程补充检查使用 P0-2 相同的固定版本：TinyXML-2 已通过 `config → prepare → scan → build/test`，两个有效 TU 成功、一个无命令文件保留未覆盖，CTest 1/1 通过；fmt 直接使用原始 50 条编译命令，经原生配置显式选择 5 个歧义文件，30 个有效 TU 成功、16 个无命令文件保留未覆盖。fmt 的符号、度量、关系、覆盖和告警与 P0-2 选定配置完全一致；两个工程全部原始受跟踪文件 SHA-256 均未变化。紧凑记录见 [证据 JSON](evidence/project-configuration-20260913.json)，完整命令/日志/数据库保存在 `artifacts/p1-1-real-config-2`。

SQLite schema 4 增加 `project_configuration`，并在 `analysis`、`build_run` 中保存配置；构建记录额外保留生成的数据库路径。schema 1/2/3 可读，可写打开时增量迁移；只读打开不升级。未知数据库版本或损坏/未知版本的工程配置报错，不覆盖原配置。升级后的数据库无法由旧二进制打开，需保留原件备份才能回退。

新增回归覆盖工程隔离/重载、配置转义和损坏保护、schema 3 迁移、0/1/多数据库发现、宏变体显式选择及失效、规则启停记录、生成头文件路径映射、复制范围、缺失工具、源码树生成拒绝、独立 CLI 流程、Qt 控件选择及两个独立 GUI 进程的导入/恢复。窗口测试使用真实 Qt 控件与后台任务，offscreen 截图已经人工检查，不等同于所有实体显示器/DPI 的验收。

最终代码提交 `6ab64d1ce793092c7dec9e74fa173221d61c63be` 的 [Windows CI 34710000310](https://github.com/hajiujiu174/cpp-defect-guard/actions/runs/34710000310) 全部成功：Core 48 项为 47 通过、1 跳过；Clang/Qt 75 项为 74 通过、1 跳过，跳过项均为 Windows 符号链接权限测试。三个固定真实工程的 44 个上游 CTest 全部通过，59 个有效 TU 均解析成功，1/4 请求线程结果一致；原始源码未修改，26 条 CG001 与已复核位置一致。三个仓库整体仍为部分覆盖。

第二台全新 Windows runner 的 CLI、Clang、Qt、内建头文件、隔离 PATH 和包内中文字体检查全部通过。原始构建 runner 未预装完整中文字体，未打包程序的部分截图有缺字；运行包的字体检查及截图正常。完整下载保存在 `artifacts/ci-run-34710000310`，紧凑数据随上述证据 JSON 长期保留；GitHub Actions 原始产物保留 14 天。

下载的 Windows 运行包已校验 SHA-256，并在本机复跑打包后 GUI 的设置、生成参数、分析、构建测试和独立进程重启恢复。配置页正常/紧凑窗口的包内中文字体与布局均已检查，记录位于 `artifacts/p1-1-packaged-settings-qa`；包文件为 `artifacts/ci-run-34710000310/package/CodeGuard-Windows-x64.zip`。这个额外检查使用外部锁定工具链执行 CMake/CTest，运行包本身不包含工程编译工具链。

P1-1 按上述范围完成。后续优先级为 P1-2：规则引擎模块化与可信度，不在本项提前实现规则抑制、级别重配或 AST 缓存。
