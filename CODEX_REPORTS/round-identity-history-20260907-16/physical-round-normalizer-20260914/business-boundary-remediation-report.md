# 物理轮次业务边界统一整改 — 执行报告

## 结论

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS
WINDOWS_BUILD_PASS
HARDWARE_REVALIDATION_PENDING
```

已完成追加任务 `TASKS/物理轮次业务边界统一整改追加_20260915-130400.md`：`PhysicalRoundNormalizer` 已产生的 round semantics 已扩展为生产链统一业务边界——`roundGeneration` 成为 FileSaver 的 authoritative 文件边界（数据面判定，写组之前），`TimeoutBoundary` 立即清零 per-round UI 帧/块计数并刷新状态，`CountBoundary` 保留实机已验证的帧末驱动清零；超时后迟到的旧轮重建快照在 UI 准入层被 epoch 拒绝，不再推进新轮计数、不再污染已清空图像。

```text
不得表述为：实机问题已经解决。本整改只完成软件侧统一业务边界，
真实 FPGA/NIC 现场行为仍为 HARDWARE_REVALIDATION_PENDING。
```

## 任务身份与回执

```text
ADDENDUM_TASK         TASKS/物理轮次业务边界统一整改追加_20260915-130400.md
IMPLEMENTATION_BRANCH codex/physical-round-normalizer-20260914-043019
CONTINUE_FROM_SHA     576e0c9f0e7bc30321a57be0f562a372fcb28ea2
SOURCE_FIX_SHA        ec08d729f792811763671d6cee6a60263cd5f9a7
RECEIPT_SHA           00c347c6f413a32fb79e23cbc57a4502216cbe6a
LOCAL_HEAD_EQUALS_REMOTE_HEAD  verified after final push
```

源码从任务书指定的 continuation SHA 继续，没有 reset、rebase 或 force push。
`git rev-parse origin/codex/physical-round-normalizer-20260914-043019` 在开工前已核对
等于 `576e0c9f...`（CONTINUATION_BASELINE 通过）。

## 实机失败在自动化 fixture 中的复现

任务书 §1.2 的实机失败行为（`control + M logical, M < N → idle > timeoutResetSec → next control`）：

- 成像窗口能清空并重新成像 —— 既有 timeout remediation 已覆盖；
- **前端"输出帧计数"未清零** —— 本整改由 `RingRoundUiState::onTimeoutBoundary()` 立即清零并 `updateRingImagingStatus()` 刷新；
- **保存数据继续写入上一轮文件** —— 本整改由 FileSaver 数据面 `roundGeneration` rollover 修复。

复现与修复证据：`filesaver_round_boundary_test` T1（manual partial timeout 强制封口，
旧文件 3 条不补齐、新文件独立）、`ring_round_ui_state_test` T6（TimeoutBoundary 后
frameCount/blockCount 归零）。

## FileSaver roundGeneration rollover 设计

数据面权威：判定在**写当前 TriggerGroup 之前**完成，不依赖 observer 与 saver queue
的跨线程先后关系。`TriggerGroup::roundGeneration`（经 `FrameConverter` 从
normalizer classification 拷贝，既有机制）进入 FileSaver 的保存边界判定：

```text
haveCurrentPhysicalRound=false 时：adopt 首个 LogicalScan 组的 roundGeneration
group.roundGeneration != current 时：flush → close → fileSequence++ →
                                      currentFileTriggers=0 → adopt 新 generation
                                      → 记录 low-frequency rollover 诊断
```

- 仅 `normalizationApplied && physicalDecision==LogicalScan` 参与轮次判定；
  未打标路径（旧测试、旧数据）行为完全不变。
- 容量边界与轮次边界取先发生者：`triggersPerFile` 是文件容量上限，
  `roundGeneration` 是更强的物理轮次边界；**一个文件绝不跨 generation**
  （T3a1/T3a2/T3b 证明 perFile<N、perFile>N 及 timeout partial 均成立）。
- 手动保存：同目录、`fileSequence` 安全推进，绝不覆盖上一轮文件；
  上一轮 partial 文件按实际条数封口（T1：3 条不补到 5）。
- 自动保存：`sessionGen` 决定目录（既有方案A/C 语义不变），`roundGeneration`
  决定目录内物理轮次文件边界；gen 改变同时重置 physical-round 状态，
  新 measurement 不继承旧轮次（T5a/T5b）。
- 生命周期：`startSaving` / `stopSaving` / `suspendForSourceRestart` /
  `resumeAfterSourceRestart` / sessionGen 改变 全部重置 physical-round 状态（T10）。

## saver backlog / race 处理

保存边界完全由每个 TriggerGroup 自带的数据面元数据决定。即使 boundary 已发生，
旧轮组仍在队列中排队，FIFO 消费顺序保证旧 generation 完整写入旧文件后，
首个新 generation 组才触发 rollover。证据：T4 双形态——

- `filesaver_round_boundary_test` T4t：FileSaver `run()` 真实消费线程，
  旧轮 5 组全部入队后新轮 5 组再入队（boundary 发生时旧轮全在队列里），
  最终旧文件仅含旧轮、新文件仅含新轮；
- T4f：同交错序列确定性 FIFO drain，证明判定只依赖元数据顺序。

未使用 `requestCloseSavers()` 控制面时序作为修复手段（任务书 §2.3）。

## UI 帧/块计数与 CountBoundary 时序保留

新增可测试窄职责帮手 `paimage::RingRoundUiState`（`include/RingRoundUiState.h`，
内部自锁，UI 线程/成像 worker/测试三方可调），MainWindow 中
`m_imagingFrameCount`/`m_ringBlockCounter` 两个分散成员由它取代（单一事实来源）：

- **TimeoutBoundary**：`onTimeoutBoundary()` 立即 `frameCount=0, blockCount=0`
  （上一轮已长期 idle，stale 队列/重建状态已被清除，立即清零安全），
  并刷新 Ring imaging status。累计网络/诊断统计不清除。T6 证据。
- **CountBoundary**：不在事件当场清零——最后一个 logical trigger 被分类时
  最终重建帧可能尚未返回；帧末驱动清零保留（`noteSnapshot(blocksPerFrame)`
  达到整数倍时 `resetFrameCount()`），迟到的旧轮最终帧仍被准入、计数、
  随后归零。T7 证据。
- **帧末检测口径修正**：由 `seq % bpf`（svc 全局 shm `frame_seq`，`ring_reset`
  不归零，TimeoutBoundary 后永久失准）改为**本轮已准入快照计数**对 bpf 取模，
  与 ImagingSvc 侧 `m_ringBlockIndex` 清零口径严格对齐；TimeoutBoundary 前
  与旧行为逐点等价（count-complete 轮每轮恰 bpf 块），TimeoutBoundary 后
  恢复正确对齐。圈末 PNG 保存与 `advanceAutoSession()` 触发条件同步改用该口径。

## timeout stale reconstruction frame 处理

- 现有机制已排除大部分：boundary handler 先 `ImagingBypass::clear(StaleSession)`，
  `RingBlockAssembler::resetAfterPhysicalTimeout()` 清空半块，
  `sendRingReset` 使 ImagingSvc `resetRingRecon()` 丢弃未完成的累积帧。
- 剩余缺口（已完成帧的 snapshot 通知在 UI 复位前发出、在复位后投递）
  由最小 UI 准入 epoch 关闭：`RingRoundUiState::armStaleCutoff()` 在
  `ring_reset` 交给 ImagingSvc 之后（网络线程）以提交总数武装截止；
  svc 的 shm `frame_seq` 与提交流 FIFO 一一对应，`seq <= cutoff` 的快照即
  复位前完成帧，准入层直接丢弃（释放缓冲、不计数、不重绘、记诊断
  `physical_round_stale_frame_dropped`）。只影响 Ring 帧 UI 准入，
  未改 ImagingSvc/CUDA 数值重建。T8 证据。

有界性说明：截止精确性覆盖正常 UI 消费时延；若 UI 线程停滞超过一个完整帧周期
（约 20 s 量级，此时 UI 本身已不响应），复位前完成帧的 seq 可能被新轮同序号
提交覆盖而产生理论误准入——纯显示层、下一帧即自愈，不影响保存数据
（保存边界由数据面 `roundGeneration` 决定，与快照投递无关）。

## 可观测性（新增，低频）

- `physical_round_boundary`（UI 记录）：`measurementSession`、`roundGeneration`、
  `boundaryKind=count|timeout`、`uiFrameCountBefore/After`、
  `ringBlockCounterBefore/After`、`autoSessionGenBefore/After`（timeout）/
  `autoSessionGen`（count）、`frameReset=deferred_to_final_frame`（count）、
  `saveRoundRollover=data_plane_roundGeneration`（timeout）。
- `save.file_rollover`（FileSaver `fileRolled` 信号 → NetworkController
  `fileSaverRollover` → UI 桥接诊断）：`card`、`boundary=capacity|physical_round`、
  `oldRoundGeneration`、`newRoundGeneration`、`oldFileSequence`、`newFileSequence`、
  `oldFileTriggerCount`、`saveMode=manual|auto`——可区分容量翻滚与物理轮次强制翻滚。
- 每 trigger 不打日志；仅边界/翻滚时记录。

## 改动文件

```text
MC_410T_MultiCard/delivery/include/RingRoundUiState.h              (新增)
MC_410T_MultiCard/delivery/src/RingRoundUiState.cpp                (新增)
MC_410T_MultiCard/delivery/include/FileSaver.h                     (round rollover 状态/信号/查询)
MC_410T_MultiCard/delivery/src/FileSaver.cpp                       (数据面 rollover + 生命周期重置 + 诊断)
MC_410T_MultiCard/delivery/include/MainWindow.h                    (m_roundUi 取代两个计数成员)
MC_410T_MultiCard/delivery/src/MainWindow.cpp                      (准入/帧末/边界重置/诊断/信号桥接)
MC_410T_MultiCard/delivery/include/NetworkController.h             (fileSaverRollover 信号)
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp (信号桥接 1 行)
MC_410T_MultiCard/delivery/CMakeLists.txt                          (新增源文件)
MC_410T_MultiCard/delivery/tests/CMakeLists.txt                    (注册 2 个新测试)
MC_410T_MultiCard/delivery/tests/filesaver_round_boundary_test.cpp (新增 T1-T5/T9/T10)
MC_410T_MultiCard/delivery/tests/ring_round_ui_state_test.cpp      (新增 T6-T8)
MC_410T_MultiCard/delivery/tests/paimage_host_output_test.cpp      (旧断言更新，见下)
```

既有测试基线更新（ intentional，需网页端重点审查）：该文件 "timeout T1/T7" 场景
原断言 `Card1_ChA_timeout_000.dat == 5 触发` 编码的是整改前行为（partial timeout
轮与下一轮回共享同一文件）——正是本任务要修复的实机问题。新断言为
`_000.dat == 2 触发（旧 partial 封口）` + `_001.dat == 3 触发（新轮次新文件）`
+ `physicalRoundRolloverCount()==1` + rollover 元数据。场景内事件序、Ring
identity、savedCount==5 等其余断言全部未变。

## 测试命令与结果

工具链：Qt 6.8.0 mingw_64 / MinGW 13.1 / Ninja 1.12.1 / CMake 3.30.5。
测试构建（worktree 内既有约定）：

```powershell
cmake -G Ninja -S tests -B build/paimage_tests8 -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6 -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe
ninja -C build/paimage_tests8
$env:QT_QPA_PLATFORM='offscreen'
ctest --test-dir build/paimage_tests8 -N
# 逐项执行（规避 Windows CTest 进程复用 loader 问题）
ctest --test-dir build/paimage_tests8 -R '^<test-name>$' --output-on-failure
```

T1–T10 证据（`filesaver_round_boundary_test` + `ring_round_ui_state_test`，
全部断言文件实际内容与 rollover 事件，不只 savedCount）：

```text
T1  manual partial timeout：000={A,B,C} 001={D..H}，1 次 physical_round rollover
    （oldGen0/newGen1/oldTrig3/manual），无第三文件，control 不入文件
T2  两轮 count-complete：000/001 各 5 条，恰 1 次 round rollover
T3a perFile=2<N=5：单轮容量文件 000{2}/001{2}/002{1}（2 次 capacity）；
    partial+timeout+下轮：每文件单一 generation，partial 封口 1 条不补 5
T3b perFile=8>N=5：整轮单文件，partial 仍强制新文件，仅 1 次 round rollover
T4  saver backlog：真实 run() 线程 + 确定性 FIFO drain 双形态，新旧轮文件零交叉
T5a 自动保存 session 推进：gen1 目录 3 条、gen2 目录 5 条，无跨目录混写
T5b 固定 session 内 roundGeneration 独立成界：同目录 000/001/002 三个轮次
T6  TimeoutBoundary 后 frameCount=0、blockCount=0、epoch+1，新轮从 0/0 重新计数
T7  CountBoundary 不当场清零；迟到最终帧准入→计数→帧末归零；下一边界按全局面计数
T8  timeout 后 stale 快照（seq<=cutoff）拒绝/计数丢弃；新轮快照准入、第 5 帧帧末归零
T9  count-complete 后 idle：0 次 TimeoutBoundary、无空文件、总计 1 次 rollover
T10 startSaving/stopSaving/suspend/resume 不继承旧 round 状态；同 gen 重开不 rollover
```

两个新测试结果：`filesaver_round_boundary_test: ALL PASS`、
`ring_round_ui_state_test: ALL PASS`。

## 完整回归

36/36 通过。任务书 §12 要求的全部测试及其余注册测试逐项按名执行：

```text
physical_round_normalizer_test paimage_host_output_test paimage_network_test
ring_block_assembler_test measurement_session_transaction_test
session_boundary_test session_boundary_receiver_test data_processor_batch_test
paimage_start_race paimage_start_fence_regression paimage_start_overflow
paimage_core_checks paimage_output_checks paimage_trace_checks
paimage_worker_checks paimage_protocol_checks paimage_conversion_checks
paimage_looplog_checks filesaver_round_boundary_test ring_round_ui_state_test
（其余：imaging_bypass_test data_processor_imaging_isolation_test
  diagnostic_recorder_test ring_shm_observability_test diagnostic_dialog_test
  diagnostic_time_window_test process_scheduling_test network_ingress_observability_test
  network_snapshot_test network_diagnostics_test card_status_formatting_test
  paimage_trace_bundle_test paimage_discovery_checks paimage_discovery_socket_checks
  paimage_control_checks paimage_socket_short）
```

过程说明（不影响结论）：首轮 27 项中 6 项失败——5 项为环境性端口占用
（用户当日 15:25 启动的 `PAimage_PhysicalRoundTimeout_576e0c9_lightweight`
实机实例挂起并占用 8000-8004/5555，经用户确认后已强制结束），
1 项为上述 intentional 旧断言（编码整改前行为）。清理端口并更新该断言后
6/6 复跑通过；另有 9 项因首轮列表解析漏列随后补跑 9/9 通过。
回归日志位于 `build/paimage_tests8/regression-*.log`。

## Windows 构建证据

```text
Build target branch : codex/physical-round-normalizer-20260914-043019
Build target SHA    : ec08d729f792811763671d6cee6a60263cd5f9a7
Tracked tree clean  : yes（构建前 git status 仅任务文件，提交后干净）
Configure preset    : mingw-debug
Build preset        : mingw-debug-build
Build script        : MC_410T_MultiCard/delivery/build_mingw_debug.cmd
Toolchain           : Qt 6.8.0 mingw_64, MinGW g++ 13.1.0, CMake 3.30.5, Ninja 1.12.1
Key outputs         : build/mingw_debug/bin/PAimageReceiverDiagnostics.exe
                      build/mingw_debug/bin/ImagingSvc.exe
                      build/mingw_debug/bin/ring_svc_selftest.exe
                      build/mingw_debug/bin/ring_udp_replay.exe
windeployqt         : 已执行（Qt plugins/styles 随包）
Ring CUDA source    : _migration_pack/prebuilt_cuda（本地无 build/ring_recon_cuda，按规范回退）
```

最终交付 commit 上重新 configure + build（BUILD_STANDARD §2.3），
BuildIdentity 对应最终 SHA。交付包与依赖 SHA-256 见
`artifacts/build-delivery/<timestamp>_ec08d72/`（bin/ 完整复制 + manifest + 回执）。

## 已知限制

- HARDWARE_REVALIDATION_PENDING：真实 FPGA/NIC 现场、多轮 START/STOP、
  目标负载与 Pktmon/WPR 证据闭环未执行；不得表述为实机问题已解决。
- stale 快照 epoch 的覆盖边界见上文"有界性说明"（仅显示层、理论窗口
  要求 UI 停滞超过一个帧周期）。
- 线性成像模式的帧计数沿用同一 per-round 计数器（与整改前行为一致，
  无线性轮次语义变更）。
- 本机 Python 不在 PATH，3 个 Python 分析器测试（startup_diagnostics_analyze_test、
  paimage_start_race_analyzer 等）在 ctest 清单中不存在；paimage_start_race
  C++ 本体与 analyzer 依赖的 artifacts 目录生成均已通过，analyzer 脚本
  可在有 Python 的环境补跑。

## 未验证项汇总

```text
HARDWARE_REVALIDATION_PENDING
Python analyzer scripts not executed (interpreter absent on this machine)
```
