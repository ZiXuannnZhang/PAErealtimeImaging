诊断口径说明 — 「count-boundary frontend-only」组的判定方式
============================================================

1. stage-7 trace 组合（既有 observe 不动）：
   超界（FrontendOnly）组在 consumeSync 的 stage-7 观测中呈现
     observe(7,1) = frontendAccepted = 1
     observe(7,2) = frontendSubmit = FrontendSubmitResult::Accepted
     observe(7,3) = publisherAccepted = 0
   的 (1,0) 组合。Pass 组健康态为 (1,1)；Drop 组在 sync 入口即返回，
   根本不产生 stage-7 记录。生产接线中 m_framePublisher 恒非空（构造于
   NetworkController::startPaimage），故健康运行里 (1,0) 组合的唯一产生者
   就是 count-boundary frontend-only 组。异常路径（exception）可能复现
   (0,0)/(1,0) 形态，但伴随 observe(7,4)，可区分。

2. FrontendPreprocessor 旁证（looplog/snapshot 可取）：
   超界组被跳过 Ring 腿时 downstreamRingAttempts 不递增而 displayUpdates
   递增——同一时段内 displayUpdates 增量 > downstreamRingAttempts 增量
   即为「前端刷新、成像腿未尝试」的直接指纹。ImagingBypass/Ring 统计
   （attempts/accepted/blocksFormed 等）对超界组零增量，不产生任何
   imaging 丢弃计数（沿「imaging 丢弃不计入 UDP 丢包」纪律，此处根本
   不尝试）。

3. 显式标记：未在 looplog/trace 增加新的显式字段（任务文档 §1.2 第 6 条
   允许「不可则不做」；上述组合已可无歧义判定，最小 diff 优先）。

已知限制/未验证项（如实记录）
------------------------------
- 实机三项（时频持续刷新 / 成像冻结不重置 / 保存与计数照常）未验证，
  留给用户；软件/单测 PASS ≠ 实机 PASS（四层证据分离）。
- C1-C3/C8/C13 的 G-系列与 FO 系列在勾选模式下共用「超界组不进 Ring 腿」
  语义；单测以 RingSink 到达数与 FramePublisher 提交数为成像冻结代理，
  未（也无法在本层）断言 ImagingSvc 重建输出像素级冻结。
- physical_round_normalizer_test 在首轮全量 CTest 出现一次 SEGFAULT
  （ctest_debug_run1_segfault_note.log），单测复跑即绿
  （ctest_debug_run1_rerun_physical_round_normalizer_green.log）；最终
  aa9097e 全量 51/51 一次通过（该测试直接通过）。ring_enhancer_bench
  本轮未复现 SegFault。与任务文档预警一致，属已知环境性间歇问题。
- MinGW-Release 为参考口径（无 preset，手工 Ninja/Release 缓存），未跑
  Release CTest（沿 discovery-earlyexit-port 先例：Release 仅构建+BuildIdentity）。
- stash@{0} 保护性备份未触碰；_pending_delete/、_task_work/、References/、
  verification/ 未触碰。
