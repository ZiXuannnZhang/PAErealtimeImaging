# Acquisition / startup history — 2026-09-07 to 09-13

> 历史归档。当前 production architecture 见
> `MC_410T_MultiCard/delivery/README.md`，当前项目状态见根 `PROJECT_STATUS.md`。

这一阶段完成了从旧接收思路向 PAimage production ingress 的迁移、启动段观测增强、
system capture 诊断，以及 START admission 软件 fence 的定位/修复。

## 最终被 canonical source 吸收的要点

- production ingress owner 转为 SocketReceiver -> SourceCore -> HostOutput；
- CONFIG ACK / ready / START 控制证据分层；
- stage-1/stage-2/control trace 与 looplog/system-capture 观测；
- START admission per-card fence/hold-release/fail-closed；
- software regression 与 candidate delivery 证据。

当前 canonical source 已包含 START admission 软件修复；用户后续 A–D 实机测试当前范围通过。
但历史 startup-loss 的唯一硬件根因、以及额外 startup trigger 的精确 FPGA/LabVIEW 来源仍未证明。

## 归档内容

本目录保留当时的：

- P0 core boundary / StartFence 迭代说明；
- PAimage behavior map / migration task；
- receiver regression / port evidence；
- diagnostic enhancement / operation cards；
- system-capture diagnostics；
- START-admission implementation report；
- candidate/final delivery receipts。

这些文件不应再作为“main 是否已包含修复”“还在等哪次实机”的当前状态入口。

## Imaging isolation

`imaging-isolation-20260911/README.md` 合并了原 4 份“实时成像与采集保存链路隔离”
变更/操作/日志/测试文档。核心设计仍存在于 current source，但历史测试数字与当时的
硬件 PENDING 状态只作追溯。
