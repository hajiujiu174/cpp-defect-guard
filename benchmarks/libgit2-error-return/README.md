# libgit2 错误返回值优先级修复基准

该基准使用上游已合并的 [PR #7426](https://github.com/libgit2/libgit2/pull/7426)，提交和父提交固定在 `manifest.json`。`upstream-reference.json` 保留从 GitHub 读取的提交差异及合并信息。许可证以原仓库 `COPYING` 为准。

`regression.c` 是本项目编写的公共 API 回归程序，并非上游原有测试。它创建独立临时 Git 仓库，以非法 diff 选项版本和损坏的索引触发错误路径；不访问真实用户仓库。三个检查要求返回负错误码，另有一个合法空索引对照。缺陷版三个错误路径返回 `1`，上游修复后四项均通过。

参考修改涉及四个位置、三个函数。`git_merge` 中第一次 `git_repository_index` 调用前已有 weakptr 索引读取，本基准没有独立触发该位置失败，不能把四个参考位置都算作行为已复现。

复现需要已构建的 Windows Clang 版 CLI、GNU 工具链、Git、CMake/Ninja/CTest，以及 Python 3.11+（仅标准库）。不安装软件包。示例在项目根目录执行：

```powershell
git clone --filter=blob:none --no-checkout https://github.com/libgit2/libgit2.git build/libgit2-reference
python scripts/accept_real_repair.py `
  --cli build/p2-2-flow-20261026-analysis/codeguard-cli.exe `
  --repository build/libgit2-reference --toolchain build/p0-windows/toolchain-4 `
  --proposal-helper build/p2-2-flow-20261026-analysis/tests/codeguard/codeguard-real-proposal-helper.exe `
  --output build/real-repair-run
```

辅助程序使用已有 Core 多问题候选接口，一次重新解析整个选定范围，生成所有备选；CLI 负责组合与验证。没有辅助程序时也可逐项调用 CLI，但会重复全范围解析。准备会保存两个原始源码归档、来源和 SHA-256、实际配置、所有编译/测试日志。输出必须使用新的、较短的 ASCII 路径，以免 Windows 原生编译工具超出路径限制。

离线证据包的 `archives/` 提供两个原始 Git ZIP。可用 `--archives PATH` 代替 `--repository PATH`，脚本核对固定 SHA-256 和 ZIP 注释中的提交号。回归仓库放在输出目录下的专用短目录，并按构建目录摘要隔开；源码副本的更深路径不用于存放 Git 测试数据。

固定配置仅构建 Windows 静态库与本项目回归程序，关闭网络、SSH、上游测试、示例及模糊测试。未选择的源码文件逐项记录为排除项；保留全部实际构建单元进行静态验证。该范围不能称为整个 libgit2 仓库或所有平台验收。

测试通过、消除 CG003、与参考结构匹配分别记录。参考比较保留类型、运算符、转换和声明名称，去掉括号 AST 包装及位置/临时标识，再比较三个相关函数；原始 AST 仍保留。它是当前构建配置下的结构证据，不是任意程序的语义等价判定器。

产品候选的 `correctness` 继续为 `not_assessed`。该单项目、单修复提交的小基准不能用于宣称通用正确修复率、Recall/F1 或模型泛化效果。结果及本轮限制见 [真实修复评测说明](../../docs/real-repair-benchmark.md)。
