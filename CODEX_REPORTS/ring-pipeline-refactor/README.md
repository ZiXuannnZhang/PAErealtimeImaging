# 环形实时成像主链路重构交付报告

本目录保存任务书要求的设计、边界、验证和运行证据。源码与脚本在工作树 `D:/ChatGPT/PAERealtimeImaging/_ring_pipeline_refactor_20260912` 的 `MC_410T_MultiCard/delivery` 下。

阅读顺序：

1. `architecture-contract.md`：主链路、身份、质量、轮次和 IPC v3 契约。
2. `change-scope.md`：本次改动与明确未改动范围。
3. `saved-index-schema.md`：A/B 数据、索引提交和检查器。
4. `diagnostics-fields.md`：LoopLog、队列、成像服务和保存计数。
5. `validation-report.md`：构建、回归、自测、制品和限制。
6. `hardware-run-card.md`：真实硬件验收步骤与通过条件。

最终工作树状态、提交和证据入口见 `PROGRESS.md`。
