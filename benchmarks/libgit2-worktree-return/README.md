# libgit2 worktree 返回值基准

三项测试版本机完整重放通过：修复 3 项通过，意图负对照 2 项失败被拒绝；219 个 C/C++ TU 的原版/修改版范围一致，21→19 问题且无新增，两处错误路径均独立复现，完整目标函数 AST 及源码/副本完整性门槛通过。26 项 Python 检查通过，6 单候选、2 组合及差异共 16 项原件核对通过。正确性仍 not_assessed、gcov 未测，远端六作业 CI 已授权但尚未运行。

固定父提交 c16683f5fb3100442a370bb15c2965dc75311043，参考修复 c0a4ef05f4d4a57eb06a15df8bcd4ace26aec1f5。两处修改分别对应回调错误和 worktree 查找错误。完整上游差异见 upstream-reference.json；GitHub zipball 用 archive-sha256.json 的固定摘要和根前缀核对，不以 ZIP comment 伪装 Git 归档。Git 来源模式另行核对实际提交与 Git 归档 comment。

三个 CodeGuard 自编公开 API 回归：head-lock 检查专用 HEAD.lock 的 -14 返回；worktree-read-lock 检查专用 commondir 共享读取冲突的 -1 返回，且根 HEAD 已更新；control 检查正常返回 0 和 HEAD 目标。不以 git_error_last 是否为空判断成功。句柄关闭，不改 ACL，不操作用户仓库。Windows 文件共享语义是本案例配置前提。

入口 scripts/accept_real_repair.py --case libgit2-worktree-return，配合 --cli、--archives 或 --repository、--toolchain、--output；使用全新短 ASCII 输出路径。测试并非上游完整测试集；选定配置和 219 TU（含回归）不等于全仓库/所有配置覆盖。源码提取保留 COPYING 和 AUTHORS，许可 GPL-2.0-only WITH libgit2-linking-exception。旧两项测试版和未触发目标错误的补充尝试均保留，未冒称远端验收成功。
