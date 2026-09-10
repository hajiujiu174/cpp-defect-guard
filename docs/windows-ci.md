# Windows CI 与可复现构建

2026-09-10：按用户最新要求，当前只交付 Windows x64，Linux/macOS 等平台的新增构建与验收暂缓。既有 POSIX 代码和 WSL 测试记录保留。

## 构建输入

`config/windows-toolchain.lock.json` 固定 79 个 MSYS2 MINGW64 原生依赖包的版本、官方归档 URL、字节数和 SHA-256，约 364 MiB 压缩数据。根依赖是 Clang、Clang Tools Extra、LLVM、GCC、CMake、Ninja、Qt 6 Base、SQLite、GoogleTest 和 Binutils；递归包含本次依赖闭包。Clang 的 CMake 导出配置引用 Tools Extra 的静态库，因此即使业务不直接调用这些工具，也必须一同还原。

| 组件 | 锁定包版本 |
|---|---|
| Clang / LLVM | 22.1.8-2 |
| GCC / Binutils | 16.2.0-3 / 2.47-3 |
| Qt Base | 6.11.2-2 |
| SQLite | 3.53.4-1 |
| CMake / Ninja | 4.4.2-2 / 1.13.2-1 |
| GoogleTest | 1.17.0-2 |

还原脚本只将校验通过的包内 `mingw64` 前缀解压到新目录，不更新本机 MSYS2，不运行安装钩子、不修改系统 PATH。下载失败或校验不符时终止，不回退到最新版。归档来自 [MSYS2 官方包服务器](https://repo.msys2.org/mingw/mingw64/)，包管理与归档机制见 [官方说明](https://www.msys2.org/docs/package-management/)。官方若移除旧归档，需要维护者审查更新锁文件或保留已校验的包缓存，不能保证归档永久在线。

这里的“可复现”指固定源代码与原生工具链输入、相同的构建测试操作和可核对记录，不承诺二进制逐字节一致。Windows runner 镜像、PowerShell、Git、系统 tar、7-Zip 和操作系统仍是宿主前置条件，CI 保存其可获取版本信息；构建路径、时间戳等也可能影响二进制字节。

运行包另通过 `config/windows-font.lock.json` 固定 Noto Sans CJK SC 字体及原始 OFL 1.1 许可的来源提交、字节数和 SHA-256。打包时校验下载或缓存内容，放入 `resources/fonts`，Qt 仅在应用内加载，无需安装系统字体。这样可在缺少中文系统字体的 Windows Server 上正常显示中文；源码开发启动仍可使用系统字体。

## 本地从干净克隆开始

需要 Windows x64、PowerShell 7、Git、系统 tar，以及 PATH 中支持 Zstandard 的 `7z.exe`（7-Zip 24.01 或更新版本）。还原时先由 7-Zip 解压压缩流，再由 tar 提取原生前缀，避免旧版 Windows Server tar 的解压子进程兼容问题；单个中间 tar 在提取完成后清除。以下命令在仓库根目录用 PowerShell 7 执行；目标工具链和构建目录必须尚未存在。

```powershell
git clone https://github.com/hajiujiu174/cpp-defect-guard.git
cd cpp-defect-guard
# 私有仓库需要本人的 GitHub 读取权限。
./scripts/restore_windows_toolchain.ps1 -Destination ./build/locked-toolchain
./scripts/ci_windows.ps1 -ToolchainRoot ./build/locked-toolchain -Mode Core
./scripts/ci_windows.ps1 -ToolchainRoot ./build/locked-toolchain -Mode Analysis -Package
```

有本地包缓存时可为还原脚本提供 `-PackageCache`，所有命中仍校验 SHA-256。它拒绝覆盖已有工具链；中断后的目录不视为成功环境，应使用新的目标目录。重复构建可使用新的工作副本，或显式提供新的 `-BuildDirectory` 和 `-OutputDirectory`，避免将旧 CMake 缓存或旧 EXE 算作成功。

构建脚本临时限定 PATH 为该工具链、Git 和 Windows 系统目录，清理常见编译器、包发现和 Qt 环境覆盖，显式指定编译器及依赖前缀，结束时恢复进程环境。Core 用 Debug、完整 Clang/Qt 用 Release，均强制找到 GoogleTest，防止 CI 悄悄漏跑合同测试。当前完整适配器要求 LLVM/Clang 22，未宣称支持 MSVC ABI。

## 自动流程和产物

`.github/workflows/windows-ci.yml` 在 push、pull request 和手动触发时运行，只申请仓库读取权限，外部 Actions 固定到提交 SHA。相同分支的新运行会取消旧运行。

1. 在 `windows-2022` 上还原锁定工具链；缓存键包含整个锁文件的哈希。
2. 从空构建目录依次构建 Core Debug 和 Clang/Qt Release，运行完整 CTest，保存 JUnit XML。
3. 完整版通过后收集 Qt、SQLite、Clang 运行依赖及内建头文件；执行清理 PATH 后的 CLI/Clang/Qt 自动检查，打包 ZIP 并生成 SHA-256。
4. 第二个全新 Windows runner 下载、校验和解压 ZIP，不还原开发工具链，再执行运行包检查。

运行包检查包含程序启动、真实 Clang 分析、包内头文件来源、Qt 后台操作/取消、中文字体加载及代表性汉字字形检查和窗口截图。缺失包内字体会使验收失败，不能仅凭 GUI 启动成功判定中文显示正常。真实工程的构建测试仍需要用户自己的 CMake、CTest、编译器、Ninja 和 Git，不随运行包提供。这里不发布 GitHub Release，仅提供本次 CI 产物。

- `windows-ci-diagnostics`：配置、编译、测试日志，环境信息、CTest XML 和界面截图；失败时也尝试上传。
- `CodeGuard-Windows-x64`：通过检查的运行 ZIP 和 SHA-256。
- `windows-clean-runtime-qa`：第二台 runner 的运行检查结果、日志与截图。

产物保留 14 天，可在仓库 Actions 对应运行页面下载。构建目录为 `build/windows-ci-core`、`build/windows-ci-analysis`；本地产物在 `artifacts/windows-ci`。还原工具链失败时原因记录在 Actions 步骤日志。

## 依赖升级

在单独维护环境中升级并测试整套 MSYS2 依赖，再执行 `scripts/update_windows_lock.ps1 -MsysRoot <该环境根目录>` 生成候选锁文件。生成器读取安装元数据和缓存归档，只记录必需依赖闭包；更新锁文件后必须重新还原、完整构建、测试及运行包检查，不可仅修改版本字符串。包缓存不提交 Git，锁文件必须提交。

## 本轮执行记录

本地已使用从缓存归档解压的新工具链、清理后的环境变量和空构建目录验证：Core Debug 43 项中 42 通过、1 符号链接权限跳过；Clang/Qt Release 65 项中 64 通过、1 同类跳过，完整运行包检查通过。配置初次发现 Clang 导出目标缺少 Tools Extra 库，已将该包纳入最终锁文件；Core 不依赖此包，首次 Core 记录使用补入前的锁文件。

还原脚本另通过真实小包下载/解压、错误校验值拒绝、已有目标目录保护、异常来源及文件名拒绝测试。79 个锁定包的官方 URL 已检查可访问。本地记录在 `artifacts/windows-ci`、`artifacts/windows-ci-2`；这些目录不进入 Git。

GitHub Actions 的实际结果在远端运行后另行记录；本地通过不替代远端验收。跨平台工作不属于本轮完成条件。
