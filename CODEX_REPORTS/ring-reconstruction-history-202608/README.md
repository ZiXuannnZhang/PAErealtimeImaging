# Ring reconstruction historical reports — 2026-08

> 历史工程资料。当前 Ring/ImagingSvc/PhysicalRound 行为以 canonical `main` source 和根目录治理文档为准。

本目录保留仍有工程追溯价值、但不应占据仓库根目录的环扫重建资料：

- `multi-radius-implementation.md`：逐通道扫描半径/配准模式方案及当时实施、A/B、实机验收记录；
- `reconstruction-core-benchmark/`：2026-08-16、commit `fd98556` 的历史 CUDA 重建核心性能基准及 raw CSV；
- `ring-pipeline-refactor/`：早期崩溃调查、新链路重构设计与需求 notes 的合并历史摘要及原始 crash/screenshot 证据。

这些文件中的旧绝对路径、旧 SHM/API 设想、旧性能数字和旧 commit 仅用于历史比较，不应直接作为当前实现规范。
