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

## Frozen package files

`迁移包SHA256.txt` 对本目录若干文件的**原始路径与内容**做了哈希，包括：

```text
迁移包说明.md
HANDOFF.md
migration scripts
realtime_imaging_migration.bundle
prebuilt_cuda/*
```

因此这些文件故意保持原位置/原内容，不把它们移动到 historical 子目录，也不把旧 handoff
“更新成今天的状态”；否则会破坏冻结 package manifest。

本目录根新增的这个 `README.md` 是外层说明，不属于原 migration-package hash contract。

历史 `HANDOFF.md` / `迁移包说明.md` 中的 old branch、old build path、
`MC410T_Receiver.exe`、200 MHz、未提交状态等都不是当前项目事实。

当前状态只看仓库根：

```text
PROJECT_STATUS.md
REPOSITORY_BASELINE.md
BUILD_STANDARD.md
```
