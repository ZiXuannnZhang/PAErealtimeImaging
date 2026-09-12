# 改动边界

## 已改动

- `RingTypes.h`、`RoundTracker.*`、`RingPipeline.*`：统一身份、质量、轮次和环形入口。
- `RingBlockAssembler.*`、`PacketAssemblyBuffer.h`、`DataTypes.h`、`SourceCore.*`、`FrameConverter.cpp`：固定位置槽、完整性和尾块语义。
- `ImagingSharedMemory.h`、`ImagingController.*`、`ImagingSvc.*`、`ImagingBypass.*`：IPC v3、双槽、服务 generation/config、旁路和观察字段。
- `DataProcessor.cpp`、`MainWindow.*`：把身份/质量沿既有保存、显示、发布边界传递。
- `FileSaver.*`、`OutputWorkers.*`：有序保存、A/B 与 sidecar commit、停止/启动排空和计数。
- `PaimageAcquisition/LoopLog.*`、`TraceBundle.cpp`、`startup-looplog-schema.md`：诊断预算、保留、冻结、导出和失败分类。
- 测试、`ring_svc_selftest`、`build_refactor.ps1`、`test_refactor.ps1`、`package_refactor.ps1`、`check_saved_index.py`：自动化验证和交付检查。

## 保留边界

- 未删除旧 IPC v2 结构；v2 兼容代码保留，环形新链路明确使用 v3。
- 未替换旧接收器、`paimage_core`、线性显示/保存/发布或 CUDA 重建实现。
- 未把硬件采集、FPGA、LabVIEW、示波器或现场网络状态伪装成已验证。
- 未在本分支执行 merge、push、reset、clean，也未修改根目录原有用户改动。

## 兼容与拒绝策略

- 旧调用可继续走 legacy `RingBlockAssembler` callback；新调用使用带身份和质量的 `RingBlock`。
- 只要 packet coverage、长度、session、generation、config 或 valid bit 不满足成像条件，就拒绝成像消费并记录原因。
- 保存/显示/发布不因单个块不适合成像而停止；保存通过 sidecar 表达质量和缺口。
