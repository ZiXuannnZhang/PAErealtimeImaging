# 启动段 START 竞争与入口证据验证

## 结论

已完成验证增强实现并提交到独立分支。实现保持生产路径不变；新增的 `ControlSocket` seam 只在 `PAIMAGE_SOCKET_TEST_SEAM` 测试编译定义下生效，没有修复或改写产品根因。

真实 WinSock loopback 集成测试已通过 60 个 session（3 个场景×20 次）：4 张逻辑卡、真实 `ControlSocket::sendto()`、真实 `SocketReceiver` 接收线程、`SourceCore` 和 `HostOutput` 回调均参与验证。测试没有 socket 错误、emulator 发送错误、trace queue drop、死锁或超时。

最新产物目录：

`MC_410T_MultiCard/delivery/build/tests_release/start-race-validation-artifacts/run-94252100707`

应用 trace 经增强分析器复核为 `application_start_gate_drop_observed`：60 个 session、240 次 START、3840 个 raw ingress、3840 个 exact stage2 join、0 个缺 join；其中 40 个 session 观察到 START span 内的 `Disabled`，20 个 session 为 accepted-only。

硬件/系统抓包未接入，本次硬件结论为 `not_verified`；上述结论仅来自完整的应用 trace。

## 实现内容

- `ControlSocket` 增加测试专用的逐次 `sendto()` 回调；测试在每次真实 START 发送后等待对应卡的 raw/stage2 处理，再继续下一张卡，避免把跨 session 的延迟处理误算为通过。
- 新增 `paimage_start_race_test`：覆盖 whole-trigger gated、prefix gated partial/`TriggerSwitch`、normal-after-start 三个场景；检查 START 顺序、raw ingress、`Disabled`、accepted 数据、完整/部分帧、socket/trace 错误及 trace 产物。
- `startup_diagnostics_analyze.py` 解析 stage 6 `START` 发送和全部 stage 2 决策，按 `(session, correlation, card)` 精确关联；新增每 session/card 的发送时间、成功状态、首个 raw、延迟、decision、span、unmatched join 和失败字段，并新增顶层 `startAdmissionEvidence` 及 evidence matrix 项 `application_start_admission`。
- 新增 analyzer synthetic fixtures，覆盖 positive gate drop、accepted-only、queueDropped/traceIncomplete、跨 session 的 trigger/packet 重号、control send failure。

## 验证结果

工具版本：CMake 3.30.5、Ninja 1.12.1、MinGW g++ 13.1.0、Qt 6.8.0、Python 3.12.14。

| 验证项 | 实际结果 |
| --- | --- |
| `py_compile` + synthetic analyzer fixtures | 通过；输出 `PASS startup diagnostics START admission fixtures...` |
| `ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release -R '^paimage_start_race$' --output-on-failure` | 通过；3 场景均 `passed=true`，A `640/640 Disabled`、`acceptedPre=0`，B `240/240 Disabled` 且 `80` partial + `80` complete，C `160` complete |
| `ctest --test-dir MC_410T_MultiCard/delivery/build/paimage_core --output-on-failure` | 10/11 通过；`paimage_discovery_socket_checks` 因 PID 1528 的 `PAimageReceiverDiagnostics` 已占用 UDP 8000 而失败（8000–8004 均被占用），未终止该进程 |
| `ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release --output-on-failure` | 14/32 通过。新增 race、analyzer fixture 及底层 checks 通过；17 个既有 Qt 测试在整套 CTest 顺序执行时返回 `0xc0000135`，单独复跑其中的 Qt 测试可通过；另有上述 UDP 8000 占用失败 |
| 生产全量 build | 失败于既有外部依赖 `MC_410T_MultiCard/delivery/libs/imaging/cufft64_12.dll` 缺失 |
| 涉及目标 `MC410T_Receiver` | 源码编译和链接阶段完成，但 post-build 复制同一缺失的 `cufft64_12.dll` 失败，不能报告为完整 target 通过 |

分析器最新汇总：`startSendCount=240`、`startSendFailureCount=0`、`rawIngressCount=3840`、`matchedDirectDecisionCount=3840`、`missingStage2JoinCount=0`、`disabledIngressCount=880`、`acceptedIngressCount=2960`、`traceIncomplete=false`。

## Git 交付

- 基线：`origin/main == f326056ee99e5f9c135635d7e97fe2027ee50fbc`
- 分支：`codex/start-race-validation-20260912-205615`
- 实现提交：`7a74d3666a7453776fd2e5692334a5fd4977a499`
- 本报告随后作为独立提交加入同一分支；最终本地/远端 SHA 以推送后的交付回执为准。
- 未 merge、未 rebase、未 force push。
