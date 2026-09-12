# 环形实时成像主链路重构进度

## 任务身份

- 任务书：`CODEX_REPORTS/环形实时成像主链路重构_无人值守执行任务书_20260912.md`
- 基线提交：`9d0fd7a84b3725a3cfa6e1aefb9be2559add0cda`
- 实现分支：`codex/ring-pipeline-refactor-20260912`
- 实现工作树：`D:/ChatGPT/PAERealtimeImaging/_ring_pipeline_refactor_20260912`
- 实现提交：`c217d4d`、`b33b200`，最终文档/工具提交见最终 `git log`

## 总状态

状态：已完成本地实现、验证和可交付打包；硬件验收保持待执行。

已完成：

- 从固定 clean 基线建立独立 worktree，未修改根目录用户脏工作树。
- 建立 `FrameIdentity`、`FrameQuality`、`RoundTracker`、`RingPipeline`，环形下游只由一条主链路接收。
- 重写位置归属与缺口语义：位置不压缩，保留 `validBits`、缺失原因、尾块和轮次关闭原因。
- 建立 IPC v3 双槽 Free/Ready 协议，带 session/service/config generation、序号、固定布局、边界检查和 ready 通知。
- 保留旧接收器、PAimage/core、线性链路和 CUDA 入口；成像无效输入只旁路成像，不阻断保存/显示/发布。
- 保存 A/B 数据与 `index.jsonl`、`manifest.json` 绑定提交；补充可读检查器和 accepted/written 计数。
- 补充 LoopLog 的段预算、保留/冻结/导出/IO/队列计数和 `loopLogIncomplete` 语义。
- 提供无人值守构建、测试、打包脚本，并完成 30 项回归和 `ring_svc_selftest`。

## 证据索引

- 架构契约：`architecture-contract.md`
- 改动边界：`change-scope.md`
- 保存索引契约：`saved-index-schema.md`
- 诊断字段：`diagnostics-fields.md`
- 验证报告：`validation-report.md`
- 硬件运行卡：`hardware-run-card.md`
- 全量回归摘要：`test-summary.json`
- v3 自测日志：`ring-svc-selftest-v3.log`
- 构建/测试原始日志：本目录下 `stage-*`、`test-*.log`

## 未宣称事项

当前没有真实采集卡、FPGA、示波器、LabVIEW 或现场网络验收证据。本交付只宣称源码、模拟输入、自测、构建和本地回归已通过；硬件运行须按 `hardware-run-card.md` 执行后再签字。
