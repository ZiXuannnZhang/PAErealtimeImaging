# 真实硬件运行卡

本卡只在具备真实采集卡、目标网卡、目标 FPGA 配置、CUDA 运行时和现场保存盘后执行。当前报告不代替这一步。

## 前置

1. 固定本次硬件配置：卡数、card/channel 映射、wavelengths、4001 physical / 4000 effective、40 Hz、30 us、7500 samples、50 positions/block。
2. 保存 `build-manifest.json`、配置快照、驱动版本、网卡 MTU/队列和目标磁盘空间。
3. 启动接收器、`ImagingSvc` 和主程序；确认服务 generation/config version 与 producer 一致。

## 运行

至少完成一轮稳定采集、一次跨 16-bit 回绕、一次人为/自然缺口、一次 stop/start 和一次异常/恢复。全程保存主链路日志、`looplog-summary.json`、`RingSHMObs`、A/B data、index、manifest 和系统事件。

## 通过条件

- physical/effective 计数符合配置，4001 -> 4000 的首触发偏置有身份记录。
- 无未解释的 session/epoch/config/generation mismatch；任何缺口均在固定 position、valid bit、sidecar 和 round close reason 中可追溯。
- `notifications == consumed`（允许明确的 fallback 计数），无 slot overwrite、duplicate、unexpected gap；队列和 process latency 在项目门限内。
- A/B 每个已提交 batch 都有完整 data + commit，`check_saved_index.py` 返回 0；`writtenCount` 与 commit 行一致。
- stop 后 worker 排空，manifest closed，LoopLog 的 incomplete/IO/budget 字段与现场事实一致。

## 回收物

把现场日志、配置、构建清单、索引检查 JSON、性能摘要和人工签字放入独立验收目录；不得覆盖本地模拟自测证据。
