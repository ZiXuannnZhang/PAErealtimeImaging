# RoundIdentity 阻塞点 3 收敛与 Final Marker 可靠性整改 — 执行报告

日期：2026-09-16
任务文档：`codex/task-docs:TASKS/RoundIdentity阻塞点3收敛与FinalMarker可靠性整改_20260916.md`

```text
BASE_SHA        = 23debab2bf64473e5500231689113e567b3ca229（integrated 分支任务基线，merge-base b4376203e7c9c8b369b6b87551f3718bd1edf424，ahead 2 / behind 0，工作树干净）
SOURCE_SHA      = 26d54b5a2b564a127b046ab213a7f6d09b8c83b9（"Close Ring final reconstruction completion semantics"）
REPORT_SHA      = d721f4c540a2a5ac0b5c1d97a90a38e6e6013e65（"Add final completion closure report and validation evidence"）
REMOTE_BRANCH   = codex/physical-round-normalizer-integrated-20260916
REMOTE_HEAD_SHA = d721f4c540a2a5ac0b5c1d97a90a38e6e6013e65（git ls-remote 核验 local==remote）
```

## 任务目标回顾

阻塞点 3 剩余闭环：ImagingSvc 把上游 `roundComplete`（只证明观察到该物理轮最后一个逻辑触发）直接透传为 `ring_snapshot_ready.round_complete`，中间块丢失时仍会放行 final PNG / presentation。次级风险：one-shot final 标记可能随首个被分类但 Ring 未启用的 card 被过滤而丢失。

## 改动文件

```text
MC_410T_MultiCard/delivery/include/RingReconCompletion.h            新增：生产/测试共用完成判定 seam
MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h  新增 isFinalLogicalTrigger 字段
MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp    边界分支置位；cached 保留稳定属性
MC_410T_MultiCard/delivery/include/DataTypes.h                       TriggerGroup.isFinalLogicalTrigger + reset
MC_410T_MultiCard/delivery/src/PaimageAcquisition/FrameConverter.cpp convert 透传稳定字段
MC_410T_MultiCard/delivery/include/RingBlockAssembler.h              final 标记接口语义改名 sourceRoundComplete
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp                PendingTrigger/回调改名（barrier 行为不变）
MC_410T_MultiCard/delivery/src/MainWindow.cpp                        Ring feed 改用 isFinalLogicalTrigger
MC_410T_MultiCard/delivery/include/ImagingController.h               submitRingBlock 参数语义改名
MC_410T_MultiCard/delivery/src/ImagingController.cpp                 ring_block_ready 改发 source_round_complete
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.h/.cpp          P0 完成语义拆分 + 诊断字段
MC_410T_MultiCard/delivery/src/RingReconCuda/ring_svc_selftest.cpp   新增 --drop-block 确定性丢块验收
MC_410T_MultiCard/delivery/tests/CMakeLists.txt                      blockers 测试链接 Normalizer 源
MC_410T_MultiCard/delivery/tests/ring_production_blockers_test.cpp   C1/C2/C3 + M2/M3 + T1
MC_410T_MultiCard/delivery/tests/paimage_core/physical_round_normalizer_test.cpp  T18（M1）
MC_410T_MultiCard/delivery/tests/paimage_core/conversion_checks.cpp  FrameConverter 透传检查
```

未触碰冻结项：RoundIdentity 定义、assembler/ImagingSvc 身份屏障、TimeoutBoundary→ring_reset 链路、CountBoundary 语义、submit_index stale cutoff、presentation 精确匹配、CUDA 数值算法、SHM ABI/二进制保存格式、FPGA/UDP 协议、START admission fence、AutoSaveRoundCoordinator 映射核心。assembler 仅做 final-marker 接口语义改名，未改 barrier core；未新增第二套 UI round counter；IPC 未扩张（`ring_block_ready` 的 `round_complete` 更名为 `source_round_complete`，`ring_snapshot_ready.round_complete` 保持同名但语义收敛为 reconstructionComplete）。

## 任务问题逐项回答

### A. sourceRoundComplete 的定义是什么？

数据面事实：该物理轮的最后一个逻辑触发已被上游观察到。链路上由 Normalizer 的稳定 terminal 属性 `isFinalLogicalTrigger` 承载，经 FrameConverter→TriggerGroup→MainWindow feed→`RingBlockAssembler` PendingTrigger 多通道 OR→BlockCallback→`ring_block_ready.source_round_complete` 进入 ImagingSvc。它只决定**生命周期关闭**（close + resetRingRecon），不代表重建完整。

### B. reconstructionComplete 的定义是什么？

```text
reconstructionComplete = sourceRoundComplete && 服务端实际消费块数 == 配置期望块数
```

由生产 seam `paimage::evaluateRingReconCompletion(sourceRoundComplete, m_ringBlockIndex, m_ringBlocksPerFrame)`（`include/RingReconCompletion.h`）在 `ImagingSvc::processRingPulse()` 内计算；精确相等判定，块数超出同样 fail-closed（不用 `>=`），并产生 `blockCountMismatch`。

### C. 哪一个字段现在控制 ring_snapshot_ready.round_complete？

只有 reconstructionComplete。`sendRingSnapshotToHost(..., completion.reconstructionComplete)`；`ring_snapshot_ready.round_complete` 的唯一业务含义即重建完成事实。UI 的 final gate（`noteSnapshot` → final PNG / presentation completion）消费且仅消费该字段。

### D. 中间 block 丢失但 final source block 到达时实际行为是什么？

以 expected=5、实际消费 B1 B2 B4 B5(final) 为例（`ring_svc_selftest --drop-block 2` 实测）：

```text
该轮每张快照 round_complete=false（含 source-final 块）
恰好一次 round_block_count_mismatch 诊断：
  measurement_session/round_generation = 该轮身份
  blocks_consumed=4, expected_blocks=5
  source_round_complete=true, reconstruction_complete=false
不触发 final PNG / presentation completion
source end 到达即 close 当前 RoundIdentity 并 resetRingRecon()
下一轮第一块从干净累积器开始（实测与干净首图逐像素差为 0），绝不等待补齐
```

### E. final logical trigger 首个到达的是 disabled card 时，enabled card 如何仍获得 terminal metadata？

Normalizer 分类缓存中新增稳定数据属性 `isFinalLogicalTrigger`：首次分类为最后逻辑触发时置 true，同一 `(measurementSession, triggerSeq)` 的 cached/迟到分类仍返回 true（one-shot `roundComplete` 仍在 cached 时强制为 false，控制副作用保持 one-shot）。MainWindow Ring feed 改从 `TriggerGroup.isFinalLogicalTrigger` 取 final 标记，因此启用卡即使后到也携带轮末标记，assembler fan-in OR 后形成 `sourceRoundComplete=true` 的完整块。M2 测试（disabled card 先分类 / enabled card 后到）与 M3（多卡 terminal OR 幂等、迟到 terminal 不能重开已关闭轮）在 `ring_production_blockers_test` 中覆盖。

### F. CountBoundary 为什么仍不会直接 reset assembler/service？

CountBoundary 保持 one-shot observer 事件：只做 save binding 提交与 presentation transition 注册（MainWindow 边界 sink 未改），`RingRoundUiState::onCountBoundary()` 仍是无副作用注释实现；assembler 的 `completeLogicalRound()` 兼容 seam 依旧只在 clean 状态生效；数据面由最后逻辑触发与下一 RoundIdentity 驱动闭环。T18 断言 observer 每轮恰好一次；M2/M3 两轮共两次；`ring_round_ui_state_test` B7 断言 CountBoundary 前后计数不变、epoch 不前进。

### G. TimeoutBoundary 行为是否保持？

保持。`resetAfterPhysicalTimeout`（残留清除 + 身份下限）、`ring_reset`→svc close/reset、`submit_index` stale cutoff 均未改动。新增 `timeoutCompletionRegressionTest`（T1）串联 normalizer timeoutBoundary + assembler owner reset + svc close/reset + UI cutoff：旧身份双侧 stale-drop、新身份 clean start 并完成。既有 T6/T8（UI）、T8/T9（normalizer）、T9/T10（assembler）全部通过。

### H. blocker 1 / blocker 2 的回归测试结果？

- Blocker 1（跨物理轮重建污染）：真实 ImagingSvc + CUDA `--identity-jump-at 2` 自检通过，新轮首图与干净首图最大像素差 0；`ring_production_blockers_test` barrierTest 通过。
- Blocker 2（snapshot metadata/SHM pixels 绑定）：`ring_production_blockers_test` copyTest（RingSnapshotCopy mock + 真实 QSharedMemory）、`ring_shm_observability_test` 通过。
- 完整 CTest 40/40 通过（含 ring_*、physical round normalizer、frame converter、save/network/session 全套）。

### I. 硬件验收状态？

```text
HARDWARE_VALIDATION_PENDING
```

本任务为软件语义整改；真实 FPGA/NIC 采集卡多轮验收未执行。

## 构建与测试证据

详细命令、工具链版本、依赖 SHA256 与日志见：

```text
CODEX_REPORTS/round-identity-final-completion-validation-20260916/
  commands-and-dependencies.txt   命令、工具链、依赖来源与 SHA256
  test-configure.txt              测试套件 configure
  test-build.txt                  测试套件构建（0 error）
  ctest-last-test.txt             ctest 40/40 通过
  main-build-final.txt            终 commit 完整 configure+build
  svc-selftest-final.txt          终 commit 二进制三场景实测
```

构建摘要：

```text
主工程：build_mingw_debug.cmd（mingw-debug preset 完整 configure+build）exit 0；
        PAimageReceiverDiagnostics.exe / ImagingSvc.exe / ring_svc_selftest.exe / ring_udp_replay.exe 生成；
        BuildIdentity PAIMAGE_GIT_SHA=26d54b5a2b564a127b046ab213a7f6d09b8c83b9，TRACKED_DIRTY=false。
测试：  tests 套件独立 Ninja 构建，ctest 40/40 通过。
服务级：ring_svc_selftest（14.dat，8 通道，K=500，5 块/轮，真实 CUDA）：
        正常两轮 exit 0（两轮 final 快照 round_complete=true，仅在精确块数）；
        --identity-jump-at 2 exit 0（max_pixel_difference=0）；
        --drop-block 2 exit 0（final 快照 round_complete=false；恰好一次
        round_block_count_mismatch 且字段完整；下一轮首图逐像素差 0 并正常完成）。
```

环境偏差记录：测试构建树置于 worktree 根 `/build/tests`（.gitignore 已忽略）。worktree 绝对路径过深时，CMake 3.30 为外部源码生成的哈希对象目录叠加后超过 Win32 MAX_PATH（260），导致 `data_processor_imaging_isolation_test` 的 depfile 无法创建；缩短构建根后全量构建通过。未改动任何源码、预设或工具链配置规避该问题。

## 完成标准对照

```text
1. ImagingSvc 不再把 sourceRoundComplete 直接作为 snapshot.round_complete   — 是（D/C）
2. snapshot.round_complete 仅在 source end + 精确期望块数时为 true          — 是（B/C，C3 不用 >=）
3. source end + 块数不匹配仍 close/reset，但不触发 final PNG/presentation  — 是（D）
4. final logical trigger 的 data-plane terminal 属性对 cached cards 稳定    — 是（T18/M2/M3）
5. disabled card first / enabled card later 不再丢 Ring final marker        — 是（M2）
6. CountBoundary observer 仍 one-shot，且不直接 reset 数据面                — 是（F）
7. TimeoutBoundary / submit_index stale cutoff 无回归                       — 是（G/T1）
8. blocker 1 重建身份屏障无回归                                             — 是（H）
9. blocker 2 SHM seq/pixels 绑定无回归                                      — 是（H）
10. mandatory 测试通过                                                      — 是（ctest 40/40 + 服务级三场景）
11. Windows 构建通过                                                        — 是
12. 硬件状态 HARDWARE_VALIDATION_PENDING                                    — 是
```

## 最终状态

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_VALIDATION_PENDING
```

## 未验证项与限制

- 真实采集卡/FPGA/NIC 的多轮连续采集验收未执行（保持 HARDWARE_VALIDATION_PENDING）。
- `--drop-block` 只丢第 0 轮非 final 中间块；“final 块本身丢失”语义由既有 C4（`droppedFinalTest`，UI 层）与 assembler partial-final 残留丢弃覆盖。
- CUDA 核心未改动，使用 `_migration_pack/prebuilt_cuda` 既有 runtime；`cufft64_12.dll` 为本机既有 CUDA 12 runtime 文件，SHA256 与上一轮验证一致。
