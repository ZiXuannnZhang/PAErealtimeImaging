# RoundIdentity 三个生产级阻塞点修复与验证

日期：2026-09-16。结论：三个阻塞点已在独立源码副本修复；Windows 主程序完整构建、五项回归测试和真实 ImagingSvc/CUDA 跨轮自检通过。

## 源码隔离与依据

- 原源码：`D:/ChatGPT/PAERealtimeImaging/_worktrees/physical-round-normalizer-20260914-043019`。
- 原源码 HEAD：`b4376203e7c9c8b369b6b87551f3718bd1edf424`。
- 修复副本：`D:/ChatGPT/PAERealtimeImaging/_worktrees/physical-round-blockers-fix-20260916`。
- 按原 worktree 的 `git ls-files` 复制受版本管理的文件，没有复制旧 build/cache、交付目录或 `.git` 指针。在副本建立独立 Git 仓库和复制基线，便于审查完整差异。
- 依据工作区《物理轮次 RoundIdentity 闭环三个生产级阻塞点根因调查审核报告.md》，特别是第 5 节修改建议；保留 RoundIdentity 数据面、Normalizer 分类语义、submit_index stale cutoff 和 presentation 精确匹配。
- 本任务以用户指定源码及“复制后修改”为准，不采用 main 上较早的 START 候选作为构建对象。

## 修复内容

### 1. 重建累积器的轮次屏障

`RingReconRoundState.h` 管理服务端 active identity 与 closed floor。`ImagingSvc::processRingPulse` 在追加任何新轮重建数据之前清空双波长 CUDA accumulator、block index 和每通道 WL2 的跨块缓存、角度及半径。较旧身份、已完成/超时关闭轮的迟到同轮 block 均拒绝，不能重新打开该轮。新测量 session 初始化重新建立屏障。

跨轮事件经现有 `sendRingObservation` 输出 `round_barrier_reset`、`round_stale_drop`，附旧/新身份及累计 transitions/stale_drops。

去掉 `m_ringBlockIndex % m_ringBlocksPerFrame` 业务重置。收到源端 final block 时，先发布该轮快照，再关闭身份并清空累积器。配置块数只用于 `round_block_count_mismatch` 诊断。

### 2. 快照元数据与像素原子关联

服务端在写入双波长像素的同一次 SHM 锁内递增并保存 `frame_seq`，通知直接使用保存的序号，不再解锁后重新读取。

Controller 使用 `RingSnapshotCopy.h`：一次 `lock` 内读取 header、比较通知序号与 `frame_seq`、执行 memcpy，随后解锁。不匹配、锁失败或无数据均拒绝投递，释放已预留的显示缓冲，并输出通知序号、实际序号及拒绝原因。停止期间的丢帧路径同样释放缓冲。latest-wins 行为保留。

额外为服务端原始 block 读取补上 `block_ready` / `block_seq` 同锁校验；不匹配的通知不清除新 block 的 ready 标记，记录 `block_copy_rejected`。SHM 布局和版本不变。

### 3. 完成事实从 Normalizer 透传至 UI

```text
TriggerGroup.roundComplete
  -> MainWindow feed -> RingBlockAssembler PendingTrigger
  -> BlockCallback(round, roundComplete)
  -> ring_block_ready.round_complete
  -> ImagingSvc snapshot
  -> ring_snapshot_ready.round_complete
  -> Controller request / worker / signal
  -> RingRoundUiState::noteSnapshot(roundComplete)
  -> exact presentation lookup / PNG gate
```

PendingTrigger 对各通道携带的一次性标记取 OR，避免首个通道携带的 true 被后续通道的 false 覆盖。最后触发组形成整块才会发布 final block；若只形成 partial block，则清除残留、关闭该轮，不生成“完整快照”，迟到数据也不能补齐。

UI 不再用快照数量取模。缺失或非布尔 `round_complete` 按 false 处理；计数仅作诊断。既有 snapshot admission 与 RoundIdentity 精确匹配仍在完成操作之前执行。

## 验证结果

| 验证 | 结果 |
|---|---|
| 标准 `build_mingw_debug.cmd` configure + build | PASS，主程序、ImagingSvc、自检、UDP replay 及运行依赖生成 |
| ring_production_blockers_test | PASS |
| ring_block_assembler_test | PASS |
| ring_shm_observability_test | PASS |
| ring_round_ui_state_test | PASS |
| ring_round_identity_test | PASS |
| 真实 ImagingSvc + CUDA，8 通道、10 blocks、主动轮次跳变 | PASS，双波长新轮首图相对干净首图最大像素差 0 |
| git diff --check | PASS |

新增测试覆盖：同轮连续累积、新轮隔离、旧轮/旧 session 拒绝、重复 final、超时身份下限、generation gap、一次性标记多通道合并、partial final 残留丢弃、末帧丢失后新轮首张不误判完成。

快照复制测试使用可注入 SHM mock 和真实 QSharedMemory；在 mock unlock 时立即更新 header/pixels，确认复制所得仍是匹配旧序号的原像素；同时验证 mismatch 不复制、下一条匹配通知恢复、锁失败、空 data 和锁释放。

CUDA 自检新增 `--identity-jump-at 2`，在旧轮仅处理两个块后直接切换身份、复用首块输入，没有发送 ring_reset，也没有旧轮 final 标记。比较新轮与干净首图的全部双波长像素，最大差为 0，服务统计 notifications=10、consumed=10、mismatch=0。

首次运行 Qt 单测因独立测试目录缺少运行 DLL 返回 `0xc0000135`；部署本次构建的 Qt6Core / MinGW runtime 后，5/5 全部通过，非测试断言失败。

### 复现

在副本 `MC_410T_MultiCard/delivery` 运行标准 `build_mingw_debug.cmd`。测试目录为 `build/round_fix_tests`，使用 Qt 6.8.0 / MinGW 13.1 / Ninja / Debug 构建；具体 configure 命令与版本、依赖散列另存验证目录。

```powershell
ctest --test-dir build/round_fix_tests -R 'ring_(production_blockers|block_assembler|round_identity|round_ui_state|shm_observability)_test' --output-on-failure
```

在 `build/mingw_debug/bin` 执行：

```powershell
./ring_svc_selftest.exe --data D:/ChatGPT/PAERealtimeImaging/testdata/14.dat --svc ./ImagingSvc.exe --id 14 --grid-mm 0.5 --block 200 --channels 255 --identity-jump-at 2 --out ../../round_barrier_gpu
```

## 产品语义与验证边界

- 末帧快照丢失意味着该轮不生成最终 PNG、不触发该轮完成 presentation；不会由下一轮首张补偿成“完整”。
- partial final、一次性 final 标记在上游丢失也保持不完成。源标记是唯一完成依据，svc 的块数不另立业务状态机。
- 未启用审核报告中可选的 superseded transition 消费策略；仍保留精确匹配和最多 128 条的现有有界 pending 管理，允许真正迟到的旧轮 final 完成自己的映射。
- Windows 本机 CUDA 验证使用 0.5 mm 网格和既有 14.dat，未替代真实采集卡/FPGA/NIC 的生产参数持续采集验收。
- CUDA 核心未改动，使用副本中原有 `_migration_pack/prebuilt_cuda/bin`；`cufft64_12.dll` 从指定原源码的本机构建依赖复制，SHA256 为 `2480D8AB849D7E9A375275F6C0278B8764C14AC0C1A3BDACAF256AE4A93C5590`。

原源码目录未修改，未合并或推送远端。
