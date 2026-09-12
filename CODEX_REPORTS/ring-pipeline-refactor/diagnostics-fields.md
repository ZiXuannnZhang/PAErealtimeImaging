# 诊断字段与观察面

## LoopLog

`looplog-summary.json` 区分以下字段：

- `recordsIssued`、`recordsWritten`、`writerUnwritten`
- `queueDropped`、`queuePeak`、`queueAllocatedBytes`
- `retentionEvicted`、`freezeRejected`、`exportTruncated`
- `segmentBudgetBytes`、`segmentDurationNs`、`retainDurationNs`、`totalBudgetBytes`
- `budgetExhausted`、`ioWriteFailed`、`writeFailed`、`loopLogIncomplete`

预算耗尽、保留淘汰、冻结窗口不足、导出截断、IO 失败和接收队列丢弃不再合并成一个模糊的 dropped 字段。只要存在覆盖或持久性限定，`loopLogIncomplete=true`。

## 保存与输出队列

保存暴露 `acceptedCount`、`writtenCount`、`savedCount`、队列深度、批次和 write error。`OutputWorkers` 的 bounded save command queue 返回 enqueue 结果，Stop/Start hook 只在先前 accepted frame 排空后生效，不清空已接受数据。

## ImagingSvc / RingSHMObs

服务观察输出包含 session、epoch、submitted、notifications、consumed、mismatch、ready_zero、duplicate、gap，以及 queue/copy/process 时间均值。生产者另报 submitted、slot_busy、last_seq 和 last_interval_us。v3 自测中这些字段用于证明双槽没有覆盖、通知和消费数量一致。

## 错误归因

`BusySlots`、`InvalidPayload`、`VersionMismatch`、`StaleGeneration`、quality invalid、packet coverage incomplete 和 write failure 分开保留；排障时可以从身份、质量、IPC、CUDA 入口、保存提交和诊断持久化分别定位。
