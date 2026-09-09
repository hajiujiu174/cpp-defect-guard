# Clang LibTooling 原生解析器

该工具是程序表示层的主解析入口。当前读取编译数据库，输出函数签名、准确源码范围、带类型的 AST、Clang CFG 与变量读写事件，并在 AST 上执行 DG001/DG002/DG004。Python 端从 CFG 事件计算函数内局部标量 DFG，导出用于后续模型实验的函数图。

## 构建前提

- CMake 3.20 或更高版本
- LLVM 与 Clang 开发包，且二者版本一致
- 可被 CMake 找到的 `LLVMConfig.cmake` 和 `ClangConfig.cmake`

## 本机配置与构建

当前 Windows 环境使用 MSYS2 的 LLVM/Clang 22.1.8（`D:\msys64\mingw64`）和 CMake 4.4.3。LLVM 与 Clang 的开发库、头文件及 CMake 包来自同一个 MSYS2 安装目录，使用 `MinGW Makefiles` 生成器。

重新打开 PowerShell 后，在项目根目录执行以下命令。路径从已安装的 LLVM 工具读取：

```powershell
cd D:\Work\WORKDONE\cpp-defect-guard
$llvmPrefix = (llvm-config --prefix).Trim()
$llvmCmakeDir = (llvm-config --cmakedir).Trim()

cmake -S .\native\clang_tool -B .\native\clang_tool\build `
  -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang `
  -DCMAKE_CXX_COMPILER=clang++ `
  "-DCMAKE_PREFIX_PATH=$llvmPrefix" `
  "-DLLVM_DIR=$llvmCmakeDir" `
  "-DClang_DIR=$llvmPrefix/lib/cmake/clang"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

cmake --build .\native\clang_tool\build --parallel 2
if ($LASTEXITCODE -ne 0) { throw "原生分析器编译失败" }
```

产物为 `native/clang_tool/build/defectguard-clang.exe`。该目录及 MSYS2 的 `bin` 目录已加入本机 PATH；也可以直接使用可执行文件的完整路径。

`CMakeLists.txt` 同时启用 C/C++，供 LLVM 的 C 依赖探测使用；LLVM 导出的宏定义先按独立参数拆分，再传给当前目标，避免多个 `-D` 被误合并成一个宏值。

## 运行

目标工程需要先生成 `compile_commands.json`。CMake 构建原生工具时会自动定位匹配版本的 Clang 内置头文件，样例编译参数不再需要硬编码资源目录：

```powershell
foreach ($sample in @("vulnerable", "fixed")) {
  cmake -S ".\samples\$sample" -B ".\samples\$sample\build" `
    -G "MinGW Makefiles" `
    -DCMAKE_CXX_COMPILER=clang++ `
    "-DCMAKE_CXX_FLAGS=" `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  if ($LASTEXITCODE -ne 0) { throw "样例配置失败：$sample" }
  cmake --build ".\samples\$sample\build" --parallel 2
  if ($LASTEXITCODE -ne 0) { throw "样例编译失败：$sample" }
  ctest --test-dir ".\samples\$sample\build" --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "样例测试失败：$sample" }
}

.\native\clang_tool\build\defectguard-clang.exe `
  -p .\samples\fixed\build .\samples\fixed\main.cpp
```

接入 Python 主流程并启用隔离编译测试：

```powershell
$env:PYTHONPATH = "$PWD\src"
py -3.13 -m defectguard scan .\samples\vulnerable `
  --config .\config\cmake-demo.toml --verify `
  --output .\artifacts\clang-config-vulnerable-verify
```

`config/cmake-demo.toml` 使用严格的 `clang` 后端；原生工具或编译数据库不可用时会报错。升级或移动 LLVM 后重新配置并构建原生工具，以更新资源目录。通用的 `config/default.toml` 仍保留自动选择解析器的行为。

原生 JSON 协议版本为 `1.1`。每个函数包含 `ast`、`cfg`、`findings`、`dataflow_status` 及源码字节范围，顶层 `native_rules` 声明已实现的 AST 规则。诊断输出写入标准错误，JSON 写入标准输出；Python 适配器保存诊断、检查协议并处理超时。

`llvm::InitLLVM` 统一 Windows 命令行参数编码；共享头文件函数按源位置和签名去重。未参与翻译单元的头文件不会伪造 Clang 图，Python 主流程会保留其词法分析并注明范围。
