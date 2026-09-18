# _migration_pack — frozen migration/provenance archive

本目录是 2026-09-05 工作区迁移阶段留下的**冻结迁移包与依赖来源档案**，不是当前日常同步流程。

当前 canonical workflow：

```text
GitHub origin/main
 -> git fetch
 -> local main --ff-only
 -> BUILD_STANDARD build
```

不要再用旧 migration bundle/reset 脚本替代正常 Git 同步。

## 当前保留价值

本目录继续保留是因为它包含：

- 历史 git bundle；
- retained/prebuilt CUDA runtime/source provenance；
- 迁移包 SHA256；
- 当时的构建/验证上下文；
- 之后 Session D 用于追溯 `testdata/14.dat`、CUDA runtime 等来源的历史证据。

## 文件角色

- `迁移包SHA256.txt` — 当时 package integrity；
- `prebuilt_cuda/` — retained CUDA artifacts/provenance；
- migration scripts — 只用于复现 2026-09-05 migration 场景；
- `historical/HANDOFF_20260905.md` — 当时工作区 handoff；
- `historical/迁移包说明_20260905.md` — 当时迁移操作手册。

历史文档中的 old branch、old build path、`MC410T_Receiver.exe`、200 MHz、未提交状态等都不是当前
项目事实。当前状态只看仓库根 `PROJECT_STATUS.md` / `REPOSITORY_BASELINE.md` /
`BUILD_STANDARD.md`。
