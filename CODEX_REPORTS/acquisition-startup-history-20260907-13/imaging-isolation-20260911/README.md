# 实时成像与采集保存链路隔离 — 2026-09-11 历史归档

原 4 份阶段文档（变更说明、实机操作卡、日志字段、测试报告）已合并为本摘要。

## 实施目的

当时的问题是避免实时成像处理阻塞/反压采集保存。实现形成三个边界：

```text
SocketReceiver/SourceCore ingress
save queue -> FileSaver
ImagingBypass queue -> RingBlockAssembler/ImagingSvc
```

`DataProcessor::DeliveryResult` 分离 save/display/imaging/publisher/exception 结果；
ImagingBypass 对 Disabled、QueueFull、QueueBusy、Stopping、InvalidFrame、
StaleSession、ServiceNotReady、CallbackFailed 等原因独立计数。

## 仍有价值的设计原则

- imaging queue 满或 service 不可用不能等价为 UDP ingress loss；
- save 与 imaging 接受/丢弃必须分开观测；
- stale session/invalid frame 应在成像旁路 fail closed；
- stop 时 save queue 的 drain 与 realtime imaging 的 discard 语义分开；
- 最后不足一个 Ring block 不应报告为 UDP 丢包。

这些原则后来继续被 RoundIdentity/PhysicalRound/timeout work 扩展。

## 历史验证

当时记录了 Debug/Release 构建、27/27 CTest、4000 帧 deterministic saturation test、
4-card loopback 20/50 us × 4000 triggers 等软件结果。

当时没有新的真实采集卡/FPGA/LabVIEW 环境，因此原操作卡里的 4001、20/50 us 四组合等
是阶段验证计划，不是当前协议常量或当前项目状态。

精确原文可从 2026-09-11 前后 Git history 恢复；当前状态以 A–D closeout 为准。
