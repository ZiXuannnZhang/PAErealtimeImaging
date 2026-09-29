# 前端刷新闸门解耦（计数边界后时频持续刷新，实时成像保持冻结）— 证据

任务: `codex/task-docs:TASKS/前端刷新闸门解耦_计数边界后时频持续刷新_20260929-023810.md`（2026-09-29 修订 1）
分支: `codex/count-boundary-frontend-only-20260929-024946`（自 `69af29528b91f0b606e3771827df91cedc0b9993` 线性分叉）
实现 commit: `aa9097ef126ab3ea4a2a1ce2c5786ecaba7cd184`（父 = 69af295）

| 文件 | 内容 |
|---|---|
| git-receipt.txt | 分支/提交链、分叉点核对、原分支不动核对、push 核对 |
| BuildIdentity.txt | MinGW-Debug（正式）+ MinGW-Release（参考）双口径 BuildIdentity @aa9097e tracked-clean |
| ctest_final_debug_51of51_aa9097e.log | aa9097e 上最终全量 CTest：51/51 一次通过 |
| ctest_debug_run1_segfault_note.log | 实现期间首轮全量 CTest：50/51，physical_round_normalizer_test SEGFAULT 现场 |
| ctest_debug_run1_rerun_physical_round_normalizer_green.log | 上项单测复跑即绿（任务文档预警的已知环境性间歇问题） |
| new_contract_assertions.md | 新契约断言清单（FO1–FO6，displayUpdates/双 publisher 计数口径）+ G1/G2 既有断言修订清单 + 逐字保留核对 |
| counterproof_README.md / counterproof_red.log / counterproof_green_restored.log | 判别性反证：FrontendOnly 临时改回整组丢弃 → FO3 转红（exit=3）→ 还原复绿（exit=0） |
| diagnostics.md | 诊断口径（stage-7 (1,0) 组合判定 + stage 旁证）+ 已知限制 |
| impl_commit_aa9097e.patch | 实现 commit 完整补丁（11 文件，+257/−19） |
| branch_tip_log.txt | 分支 tip 提交链快照 |

## 验收对照（任务文档「验收」节）

1. 新契约断言：FO1–FO6 全绿（勾选模式 N+N..k：displayUpdates 继续递增且
   FramePublisher 提交数与前端 Ring 腿到达数停在 N；Drop 两条件零到达；
   不勾选模式同输入恒等 + 恰一次 CountBoundary）✓
2. 判别性反证：FO3 转红 → 还原复绿，全程留痕 ✓
3. 既有断言修订：G1/G2 两条（超界组前端零到达 → 前端全到达 + 成像到达数
   口径），逐条清单见 new_contract_assertions.md；其余既有断言逐字保留 ✓
4. 全量回归：最终 51/51 绿（目标集与分叉点基线一致，未新增目标）；
   physical_round_normalizer_test 首轮 SEGFAULT 复跑绿、最终轮直接绿，
   如实记录；ring_enhancer_bench 本轮未复现 SegFault ✓
5. 构建：MinGW-Debug（正式，exit 0）+ MinGW-Release（参考，exit 0），
   BuildIdentity 双口径 @aa9097e tracked-clean ✓
6. 证据：本目录 ✓
7. 提交：实现 commit aa9097e（信息含任务文档名与「仅勾选模式行为变化、
   不勾选模式逐位不变」声明，含 docs/环形重建增强.md 第 7 节）；push 后
   local HEAD == remote HEAD、原环形分支远端 tip == 69af295 的实测值见
   git-receipt.txt push 核对段 ✓
8. 实机验收：**未执行，留给用户**（时频持续刷新 / 成像冻结不重置 /
   保存与计数照常）。软件/单测 PASS ≠ 实机 PASS ✓

## §0 事实 2 偏差说明（实现落点与任务文档的差异，必须明示）

任务文档 §0 事实 2 将 `m_framePublisher->submit` 标注为「成像路径
（ImagingBypass/Ring）」。在分叉点 `69af295` 实测：

- `m_framePublisher` 是 FramePublisher（多卡汇聚 + ZeroMQ PUSH，
  `tcp://127.0.0.1:5556`），仅在 `config.enablePublisher` 为 true 时注入
  DataProcessor（`AcqConfig.h:60 enablePublisher=false` 出厂默认），其输出
  在仓库内零消费者；
- 生产成像主链是 `FrontendPreprocessor::dispatch` 的 Ring 腿：
  `setRingSink(m_ringFeedSink)` → `MainWindow::ringFeedSink` →
  `ImagingBypass::tryPush`（全仓唯一 tryPush 调用方）→ RingBlockAssembler →
  ImagingController → ImagingSvc；
- 时域/频域显示（DisplayBuffer::update/updateFullRes）与上述 Ring 腿共用
  `m_frontendSubmitSink` 入口、在 stage worker 内同 clone 分发，二者之间
  在 deliverAssembled 层不可分叉。

因此若按字面只跳过 `m_framePublisher->submit`，超界组会经 frontend stage
持续进入 ImagingBypass（RingBlockAssembler 对每轮触发数无上限，角度索引
按帧长取模回绕），实时成像将持续更新——直接违背需求裁定第 1 条与实机
验收项，也使既有断言 C1-C3 的 `indices.size()==8`（超界组 Ring 零到达）
转红，与 §1.1「跳过成像发布器（实时成像保持冻结）」自相矛盾。

落点处理（其余与 §1.2 完全一致）：

- §1.2 第 1–5 条照做：SyncFrame.beyondCountBoundary → OutputQueues/
  OutputWorkers 透传 → consumeSync 传 `deliverAssembled` 新尾参
  （语义要求传 `!beyondCountBoundary`：Pass 组 imagingSubmit=true）→
  `imagingSubmit==false` 时跳过 `m_framePublisher->submit`
  （publisherAccepted 保持 false）；
- 增补最小必要机制：`deliverAssembled` 在 frontend submit 前给组打
  `TriggerGroup::frontendDisplayOnly`（DataTypes.h 新增默认 false 字段，
  reset() 同步），`FrontendPreprocessor::dispatch` 对该组只刷新
  DisplayBuffer、跳过 Ring 腿（downstreamRingAttempts 不计）。
  FrontendPreprocessor 与 DataTypes.h 均不在任务文档 §1.3 禁改清单内；
  PhysicalRoundNormalizer / 保存 / FileSaver / AutoSave / 统计 / Ring 重建 /
  RingSignalEnhancer / SHM / RingConfigDialog / UI 零改动；
- 该偏差使单测层「成像冻结」证明落在真实的成像发布器（Ring 腿）上，
  FramePublisher 提交计数按任务文档字面口径同时保留（两者同受
  imagingSubmit 门控，数值一致）。

## 返回段（push 后实测核对，同时载于执行代理返回报告）

见 git-receipt.txt「push 时点核对」。
