# 架构契约

## 主链路

数据路径收敛为：

`MultiPortReceiver / PacketAssemblyBuffer -> DataProcessor -> RingPipeline -> RingBlockAssembler -> ImagingController -> ImagingSvc -> CUDA`

`RingPipeline` 是环形语义入口，负责把物理触发号、扩展触发号、session、epoch、card、channel、position 和配置上下文交给唯一的环形装配器。旧接收器、`paimage_core`、线性显示/保存/发布路径和 CUDA 后端保留在原边界内；环形重构不替换这些既有入口。

## 身份与质量

每个触发组携带 `FrameIdentity`：`sessionId`、`epoch`、`wireTrigger`、`expandedTrigger`、`cardId`、`channelId`、`position`、`configVersion`。`FrameQuality` 分开记录 `qualityUnknown`、`assemblyComplete`、`packetCoverageComplete`、`expectedBytes`、`actualBytes`、缺口/尾部原因和 wavelength 来源。

轮次状态由 `RoundTracker` 管理：支持 16-bit wire trigger 扩展、正常回绕、小乱序、idle/suspected reset 和 oracle 确认；无法确认的新周期使用新的 epoch，不能把旧周期帧混入新周期。

## 4001 -> 4000 和位置语义

默认采集配置保留物理 4001 triggers、4000 effective triggers、40 Hz、30 us trigger、7500 samples/trigger 和 50 positions/block。物理首触发的偏置由配置/身份层表达，不在后处理静默删除。

位置槽位固定按 `position` 归属，缺失位置保留槽位和 `validBits`，不得通过压缩有效帧改变后续位置。超出当前 block 的触发进入尾块/下一块；`finishRound` 和超时关闭都输出可审计的 `RoundCloseReason`。

每个成像块携带显式 wavelengths；缺失时只能产生带 `assumedWavelengths` 标记的受控 fallback。成像消费前检查 sample length、valid bits、session/generation/config；无效输入计入旁路并继续保存、显示和发布。

## IPC v3

`ImagingSharedMemory.h` 定义固定头、固定 slot header 和 checked layout。协议版本为 3，slot 数为 2，slot 状态为 `Free -> Ready -> Free`。生产者只获取 Free slot，完整复制后发布 Ready；Busy 时返回计数，不覆盖 Ready 数据。slot 元数据包含 session、epoch、block sequence、config/service generation、position count、valid bits、raw/angle/channel/wavelength 元数据和 payload offsets/lengths。

服务端只消费匹配通知或按序扫描的 Ready slot，并在 CUDA 处理前释放 slot；通知丢失时使用有界轮询兜底。generation/config mismatch、旧 sequence、越界 offset、长度不符均被拒绝并留下观察字段。

## 保存与诊断

保存采用 A/B 数据文件 + JSONL index + manifest。每个 data row 绑定 session/card/wire/expanded trigger、样本数、A/B offset/length、quality；A/B 写完且 index commit 成功后才递增 `writtenCount/savedCount`。`acceptedCount > writtenCount` 表示仍有缓冲尾部，不等于数据已落盘。

LoopLog 使用有界预分配队列，接收路径不做 JSON/file I/O。段按 1 s 或 8 MiB 轮换，默认保留 5 s、最多 8 个冻结窗口、总预算 256 MiB；`queueDropped`、`retentionEvicted`、`freezeRejected`、`exportTruncated`、`budgetExhausted`、`ioWriteFailed` 和 `writerUnwritten` 分开记录。
