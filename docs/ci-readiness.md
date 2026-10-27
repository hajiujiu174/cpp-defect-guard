# 当前 P2/P3 的独立 CI 验收准备

日期：2026-10-27—28。最初审阅快照尚未提交或推送。2026-10-28 用户已授权在新验收分支提交、推送及运行完整 Windows 验收；本页准备检查仍不代表独立 CI 已通过。

## 本轮改动

`.github/workflows/windows-ci.yml` 保持 push、pull request、手动触发时的完整 Core/Clang/Qt CTest、打包和全新 runner 运行包检查；另把参考 AST 的 4 项及命令取证的 6 项 Python 测试纳入普通 CI。Python 使用 runner 已有环境，不安装模型依赖。

手动触发新增 `real_repairs`，默认关闭。开启后，构建作业上传同一次构建产生的提案 helper；另一个 Windows runner 下载同次已测试运行包和 helper，还原相同锁文件的工具链，拉取基准清单中的两个固定 libgit2 提交。Git 证书后端按单条命令选择 OpenSSL，保留证书校验，不改全局 Git 配置。上游拉取步骤最多 5 分钟；修复重放步骤最多 150 分钟，整个作业最多 180 分钟。

本机 CLI 凭据已失效，连接器支持 Git 对象及分支写入但不提供 workflow_dispatch 接口。因此该次已授权分支 `codex/p2-p3-independent-ci-20261027` 的 push 也启用两个完整验收开关；此特例只匹配该分支的完整 ref，普通分支仍按原流程。

重放调用既有 `accept_real_repair.py`，从新扫描和新提案开始，不使用 `--reuse-scan`、`--reuse-proposals`、`--resume-prepared` 或准备后即退出选项。原生缺陷/参考对照、修复组合、意图负对照、完整 C/C++ 输入内容、每 TU 覆盖、静态问题变化及参考函数 AST 门槛保持，正确性仍单独标为 `not_assessed`。具体案例限制见[真实修复评测](real-repair-benchmark.md)。

`windows-real-repair` 产物在失败时也尝试保存命令日志、准备与验收 JSON、扫描数据库、JUnit、编译数据库及上游许可。上传步骤不能保证 runner 被强制终止后的产物仍完整；源码、工具链、原生构建目录和权重不整包上传。原有三个真实工程由另一个手动开关 `real_projects` 控制；建议独立验收时同时勾选这两个开关。

## 本地检查及边界

本轮检查 YAML 结构、所有 shell 步骤的 PowerShell 语法、固定 Actions 提交号、开关与作业依赖，并实际运行上述 10 项 Python 测试。命令执行现将 stdout/stderr 直接写入独立原始日志，在启动前后和结束时保存状态、PID、退出码及耗时，异常退出与超时保留诊断；既有 `.log` 和 stdout 返回值保持。超时仍按原脚本语义清理直接子进程，不将本 helper 声称为完整 Windows 子进程树管理器。CI shell 另保存 Python 退出码。

预演使用第十阶段运行包和对应 helper，核对 ZIP 后解压，从新 bare 对象库获取两个固定提交并执行 `--prepare-only` 原生准备；此模式不执行新的静态修复重放。一次预演在参考版构建中途退出，没有留下原始构建输出，原因尚未确定；该失败记录保留，随后补强以上取证并重新准备。机器记录见 `docs/evidence/ci-readiness-20261027.json`，原始记录在 `artifacts/p3-ci-readiness-20261027`。

本机第一次网络拉取在 Schannel 凭据接口处失败，OpenSSL 网络尝试超过本轮 5 分钟观察预算后只停止了该预演的 Git 子进程。两次记录均保留。本机后续准备改用已有、提交号已核对的上游对象；不能据此宣称公网拉取或 GitHub runner 已验证成功。

第十阶段已测试的 120 个源码及构建输入 SHA-256 本轮均未变化。该阶段完整回归为 Core 104 通过/1 权限跳过、Clang/Qt 235 通过/1 同类跳过；不将本轮语法或准备检查写成新一次完整回归。详见[验证输入记录](validation-inputs.md)。

## 提交与审阅

本轮另准备完整源码审阅快照、逐文件 SHA-256、精确拟提交清单、已跟踪文件补丁和排除清单，并从快照新目录检查 Core 与 Clang/Qt 的 CMake 配置生成。配置生成只检查输入可解析、目标源码可找到，不等于重新编译或独立运行。

快照包含所有本次待验收的已完成 P2/P3 源码、样例、测试和对应文档，排除构建产物、运行包、虚拟环境、权重、下载源库及失败路径测试目录。Git 分支、HEAD 和暂存区摘要在准备前后核对，提交/推送仍遵循项目 AGENTS.md 的用户授权要求。用户已授权在新分支提交清单内文件、推送现有 origin 并运行全部 Windows 验收，main 保持；本机初始提交为 c99548b。为使用已连接的仓库写入能力，后续按同一 Git 文件树上传并记录实际远端提交及 CI 运行号。


## 首轮独立运行与 GUI 修正

运行 [40551264918](https://github.com/hajiujiu174/cpp-defect-guard/actions/runs/40551264918)，产品提交 a88b6b1：Core 105 项中 104 通过、1 符号链接权限跳过；Clang/Qt 236 项中 233 通过、1 同类跳过，level3_gui 和 analysis_gui 分别在 60/30 秒超时，后续作业未完成。原始日志和 SHA/CRC 校验通过的诊断 ZIP 保存在 artifacts/p3-independent-ci-20261028，不将此运行算作成功。

两个超时均在图显示后。画布在 resizeEvent 内调用 fitInView，而自动滚动条可以反过来改变视口尺寸；[Qt 官方文档](https://doc.qt.io/qt-6/qgraphicsview.html#fitInView) 明确说明此递归调整风险。修正为自动适应时固定滚动条策略，手动缩放时恢复按需滚动，保留缩放上限和拖动范围；原有检查时限及门槛不变。新增图窗口连续调整、节点可见、手动缩放保持、平移及恢复适应检查。本机 11 项关联检查通过；新增独立窗口检查在修正前本机也通过，因此不声称已在本机复现远端超时，修正是否解决远端问题仍须第二轮独立运行证明。
