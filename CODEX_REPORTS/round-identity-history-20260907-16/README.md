# Ring / RoundIdentity / PhysicalRound history — 2026-09-07 to 09-16

> 历史归档。最终 A–D 接受语义见 `../session-abcd-closeout-20260918/`。

这一组材料记录从 RingBlockAssembler/SHM 安全加固，到 RoundIdentity hard barrier、
PhysicalRoundNormalizer 和 completion/timeout closure 的演进。

## 最终演化方向

- Ring block ownership 不再只靠固定块数推断；
- `RoundIdentity=(measurementSession, roundGeneration)` 成为跨 Ring/UI/service 的边界；
- old identity / stale snapshot fail closed；
- source completion 与 reconstruction completion 区分；
- PhysicalRoundNormalizer 统一 distinct physical trigger、startup filter、CountBoundary/TimeoutBoundary；
- count-boundary save binding / timeout transition 与目录 rollover 绑定；
- 后续 Session A–D 增加 configurable filter、disable mode realtime cap、active timeout、
  capture-before-reset 等最终行为。

## 归档内容

- RingBlockAssembler 安全加固；
- Ring SHM 链路观测；
- PhysicalRoundNormalizer 多轮 remediation reports；
- RoundIdentity production blocker / final completion closure；
- 对应 CTest/build/CUDA barrier validation logs。

这些报告中的中间字段、旧 owner、旧“尚待下一轮修复”结论已经被最终 source 和 Session A–D 覆盖；
保留它们只是为了审计设计演进。
