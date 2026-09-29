新契约断言清单 — paimage_host_output_test.cpp（FO 块 + G1/G2 修订）
====================================================================

计数口径说明
------------
「publisher 提交数」采用双口径同时断言：
1. FramePublisher 提交计数（publisherFrames）——真实 FramePublisher 实例
   （configure(true,1,16)、独立 QThread）经 framePublished 信号
   （Qt::DirectConnection，原子计数）统计 m_framePublisher->submit 的下游产出；
   任务文档字面口径。
2. 前端 Ring 腿到达数（ringArrivals）——FrontendPreprocessor 的 RingSink
   到达计数，即生产成像主链（MainWindow::ringFeedSink→ImagingBypass）的
   直接触发代理。原因见 README.md「§0 事实 2 偏差说明」：FramePublisher 是
   默认关闭的 ZeroMQ 支路，成像冻结的软件证明必须落在 Ring 腿上。
两者在实现中同受 imagingSubmit 门控（跳过 submit 的同时打 frontendDisplayOnly、
跳过 Ring 腿），故数值一致（N 时冻结）。
「displayUpdates」= FrontendPreprocessor::snapshot().displayUpdates（时域/频域
信号窗口刷新的直接计数）。

FO 块（勾选模式 + 不勾选模式恒等，N=4）
----------------------------------------
FO1 Drop 不变性（启动控制触发）：
  send(100)（首个 distinct identity → OperationalStartupControl）后
  立即断言 displayUpdates==0 && ringArrivals==0 && publisherFrames==0 &&
  savedCount==0（Drop 判定在入队前同步发生，零计数为稳定事实）。
FO2 N 个设计逻辑触发（101..104）：
  until(displayUpdates==4 && ringArrivals==4 && publisherFrames==4 &&
  savedCount==4)——前 N 个触发前端、双 publisher、保存各收满 N 次。
FO3 超界组（105..106，k=2）：
  until(displayUpdates==6 && savedCount==6)——displayUpdates 继续递增
  （时频持续刷新）且保存照常；
  ringArrivals==4（前端 Ring 腿停在 N，成像冻结）；
  publisherFrames==4（FramePublisher submit 停在 N）；
  ringFinals==0（超界组无 synthetic final）。
FO4 Drop 不变性（过期轮次帧）：
  pollPhysicalRoundTimeout(4000000000LL) 强制物理空闲超时后，
  output.sync(104, 旧 identity) → displayUpdates==6 && ringArrivals==4 &&
  publisherFrames==4（迟到旧轮组零到达）。
FO5 闸门在新轮次重开：
  轮次重置重新武装启动过滤（与既有 C8 准入语义一致）——send(200)（控制，
  Drop）后 send(201)（新轮首个逻辑触发，Pass）→ until(displayUpdates==7 &&
  ringArrivals==5 && publisherFrames==5 && savedCount==7)。
FO6 不勾选模式恒等（disableCountBoundary=false，同输入）：
  until(displayUpdates==5 && ringArrivals==5 && publisherFrames==5 &&
  savedCount==5)——五个组全部 Pass（Nth 触发关闭该轮；下一代首个 identity
  被「未改动」的准入语义重新按启动控制收口，与改动前逐位一致）；
  ringFinals==1（Nth 触发恰一次 CountBoundary final）；
  normalizerSnapshot().roundGeneration==1（代际照常推进）。

G1/G2 既有断言修订（同文件，超界组前端到达口径）
------------------------------------------------
G1（阈值 5，12 发）：
  旧：until(frontends==5) "G1 only the designed five logical triggers reach the frontend"
  新：until(frontends==12) "G1 every trigger keeps reaching the frontend signal path
      beyond the count boundary"
      + imagingArrivals==5 && frontendOnlyArrivals==7
        "G1 imaging arrivals stop at the designed five; the rest arrive frontend-only"
  （frontendOnlyArrivals 经 sink 内 group->frontendDisplayOnly 标记区分，
   同时锚定标记在 frontend sink 处可见。）
G2（live 阈值更新 5→20，再 4 发）：
  旧：until(frontends==9) "G2 the gate follows a live threshold update"
  新：until(frontends==16) "G2 the frontend signal path continues across the live
      threshold update"
      + until(imagingArrivals==9) "G2 imaging arrivals follow the live threshold update"
  （旧断言的判别内核——到达数随 live 阈值更新跟随——完整保留，落点移到成像侧：
   12..15 触发在新阈值下恢复 Pass，imagingArrivals 5→9。）

既有断言逐字保留核对
--------------------
golden（无 normalizer→Pass）、T11、T12/T1+T7、T13/T3、C1-C3/C8/C13 四迭代、
Session-A policy seam、G3、H4/H5(S1/S2)、frontend 全分辨率块：零改动
（C1-C3 的 indices.size()==8「每卡仅四个逻辑下标到达 Ring」在勾选模式下
依然成立——超界组不进 Ring 腿正是本任务语义）。count_boundary_save_binding_test、
round_policy_settings_test、network_diagnostics_test、frontend_preprocessor_test、
physical_round_normalizer_test：零改动。
