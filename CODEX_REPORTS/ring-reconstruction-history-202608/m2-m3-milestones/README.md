# Ring reconstruction M2/M3 milestone archive

> 历史摘要。原 M2/M3 文档已完成使命，不再作为 current delivery docs。

## M2

M2 完成早期 CUDA DAS kernel、CPU/CUDA 数值对照和可视化验收。

历史结论包括：

- CUDA 与 CPU reference 在非奇异区满足约 1e-3 量级以内的一致性；
- 早期 360/720 grid 下 CUDA 显著快于 CPU；
- 当时使用 11.dat / 14.dat 作为开发 reference；
- CUDA 使用 nvcc + MSVC host compiler。

这些数字对应早期代码和较小 grid，不是当前 performance SLA。后续 4000×4000 benchmark 已单独保存
在 `../reconstruction-core-benchmark/`。

## M3

M3 逐步加入：

- ImagingSvc ring mode + SHM/ZMQ；
- RingBlockAssembler；
- per-A-line angle；
- 8-channel sector model；
- UDP loopback / simulator；
- ring_svc_selftest；
- 前端 ring configuration。

早期 M3 文档把 `MultiPortReceiver/DataProcessor` 当作真实生产 ingress，并使用
`MC410T_Receiver.exe`、阶段 A/B/C/D、“待真实采集”等表述。后续 production 已迁移到
SocketReceiver/SourceCore/HostOutput，且 RoundIdentity/PhysicalRound/A–D 工作重新定义了 round、
timeout、save binding 与 completion。

因此原 8 份 M2/M3 plan/acceptance 文档已从 current product docs 删除；需要逐字历史细节时从 Git history 恢复。

## 仍有效的历史输入

- 多通道 ring reconstruction 需要显式 channel/angle geometry；
- 线性与环扫共用“采集数据 -> preprocessing -> reconstruction”的总体分层，但 geometry/round ownership 不同；
- CUDA ABI 变更需要同步重建 producer/consumer；
- loopback/selftest 是软件证据，不能代替 real FPGA/NIC acceptance。
