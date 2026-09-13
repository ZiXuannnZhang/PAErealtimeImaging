# 本地开发历史补充与云端同步说明

日期：2026-09-13。仓库：ZiXuannnZhang/PAErealtimeImaging。范围仅为用户删除整理后仍存在的 workspace-cleanup-20260913 内容，不追踪或恢复用户已删除的独立文件。

## 网页端先读

本次是历史补充，不是功能合并。正式 main 保持 432cafcd49cbc410241928db86da3a47784e142c；START 实机候选保持 6313540f72544c0f68820c4815903abaa0b8c1e1。没有把旧本地实现或 ring 分支混入 main，也没有改变任何验收结论。

- `backup/local-workspace-source-20260913@cab86a1933d659f8d3e68ecadaee6d20b472ec80`：可直接浏览的历史源码快照，以远端 master@219188a 为父提交，重建自原 tracked stash 并补入仍存在的 18 个未跟踪源码文件。它是归档重建，不是原 62 次历史的重写版本，也不应直接合并 main。
- `codex/local-history-supplement-20260913`：以 main@432cafc 为父提交，提供本说明、祖先关系矩阵、恢复清单，以及去除第三方工具副本后的本地独有报告与分析脚本。
- Release 标签 `archive/local-development-20260913`：完整 Git 历史 bundle 与 11 个去重证据 ZIP；属于历史归档，不是可交付软件版本。
- 原已删除的 `codex/local-docs-sync-20260913` 分支名没有恢复；其 commit c3cc9fa 保存在完整历史 bundle 中，文档作为历史资料保留。

## 祖先关系结论

| 本地历史 | 与 main@432cafc 的关系 | 云端已有或新增位置 |
|---|---|---|
| 原 master@9342af1，62 次提交 | 无共同祖先 | 原历史保存在 Release bundle；其源码与远端 master@219188a 完全相同，除 67 个 testdata 文件和 cufft64_12.dll |
| tracked stash@d5523a6 | 基于原 master，同样与 main 无共同祖先 | 原 stash 保存在 bundle；可读重建快照为 backup/local-workspace-source-20260913 |
| docs@c3cc9fa | 从 f326056 分叉，本地侧 1 次、main 侧 11 次提交 | bundle 和历史文档副本；不恢复旧分支名 |
| ring@d861b4e | 从 9d0fd7a 分叉，本地侧 6 次、main 侧 18 次提交 | 已有 origin/codex/ring-pipeline-refactor-20260912 |
| diagnostics/startup@c24e915 | main 和 START 候选的祖先 | 已有同名 diagnostics/startup 分支，main 包含其提交 |
| start-race@8431476 | 从 f326056 分叉，本地侧 4 次、main 侧 11 次提交 | 已有 origin/codex/start-race-validation-20260912-205615 |
| 旧 main@f326056 | main 和 START 候选的祖先 | 已在远端历史内 |

完整的逐远端分支 SHA、merge-base、双方独有提交数见 `ancestry-matrix.json`。两个互不相关的根历史不能因源码相似而宣称拥有 Git 祖先关系；新 backup 快照的父关系是本次明确建立的归档关系。

## 资料含义与归置

现存 27,659 个文件，总计 10,777,167,269 字节。内容分为历史源码、现场诊断原始数据、分析报告/脚本、工具与运行库、旧整理回执。

这些不是当前构建交付 staging。正式归置为：

- 云端 Git backup 分支：历史源码。
- 本补充分支 `docs/history/local-development-20260913/`：可阅读的历史报告和分析脚本。历史报告中的状态只代表原报告日期。
- 云端归档 Release：大型原始诊断数据、二进制、第三方工具副本和完整历史。
- 本地 `workspace-records/20260913-local-cloud-sync/`：小型同步说明、清单、哈希、远端回执；不长期保存原始数据或构建产物。

原 `artifacts/workspace-cleanup-20260913` 和本次 `.local-sync/20260913` 上传临时目录，仅在对应云端内容核验完成后移除。用户当前 main、START 工作区及其 build、runtime、testdata 不属于这次删除范围。

## 归档恢复

`recovery-manifest.json` 为本次现存文件的恢复索引：

- `existing-git`：按 git_blob 从仓库对象库取回；remote_path 仅为索引参考，Git 对象 SHA 是身份。
- `release`：下载 asset 所指 ZIP，解压 member（objects/<SHA256>），恢复至 path，并核对 SHA-256。相同内容只上传一次。
- `consolidated-history`：六个旧 bundle 的引用目标已汇总到 local-history-complete.bundle；不承诺重建旧 bundle 容器的字节排列，保留的是全部原 Git 对象和历史身份。

下载 Release 的 local-history-complete.bundle 后可执行 `git bundle verify` 和 `git clone --mirror <bundle> <独立目录>`。原 master SHA 为 9342af13a9a7d7e632d0aba3d9e0d3726e275468，stash SHA 为 d5523a69d1853c7489bbe9a1fe322a32e242c452，临时 docs SHA 为 c3cc9faa7b9c28aaf0dc1c72d4056a38815b6c46。原始完整 bundle 也含其历史版本的数据和依赖；这不同于恢复用户删除的工作目录文件。

## 验证与边界

本轮检查 Git 祖先关系、现存文件内容哈希、bundle 完整性、源码快照的大对象排除和云端资产大小/SHA-256。没有重新编译历史源码或运行实机测试；历史快照可能包含旧目录调整和相互配套的文件，应独立审查后提取所需差异。

根工作区 `_migration_pack/HANDOFF.md` 的用户标题修改保持原样。本地最终仍只保留 main 和指定 START 分支；云端增加的 backup 和说明分支仅供追溯与审查。

实际上传回执见 `remote-assets.json`，清理完成回执见 `cleanup-receipt.json`（完成后生成）。
