# Layer 2b（ring_udp_replay 全链 + 敏感性）——未执行，原因与复跑命令

状态：**NOT EXECUTED**（无人值守会话内无法安全执行；不属于"测过 PASS"，不得记为任何形式的链路 PASS）。

## 阻塞原因（技术链）

1. 全 UDP 链的接收端是 GUI 主程序 `PAimageReceiverDiagnostics.exe`（project MC410T_Receiver）：
   UDP ingress（SocketReceiver→SourceCore→HostOutput→RingBlockAssembler→ImagingController）
   全部寄宿在该 Qt Widgets 进程内，无无头替代宿主。
2. 该进程的量测启动前置要求 PAimage CONFIG 收齐全部目标卡 60-byte CONFIG ACK：
   `NetworkControllerPaimage::pollPaimage()` 仅在 `m_paimage->configured()` 为真时进入
   `ConfigPhase::Confirmed`，否则 `measurementStartFailed("PAimage CONFIG failed")`，
   数据链路不会启动（`src/PaimageAcquisition/NetworkControllerPaimage.cpp:508-527`）。
3. `ring_udp_replay` 只向 127.0.0.1:8001-8004 发送数据包（包头 4 字节 seq/trigger），
   不模拟 CONFIG/START 控制面，也不能替真卡产生 CONFIG ACK。
4. ring configure（网格/块大小/网格参数与 `ring.enhance`）由 GUI 的 RingConfigDialog 经
   ImagingController 下发；无人值守会话无法可靠驱动该 GUI 完成多配置矩阵
   （且 branch-both 全链单元需要分支 GUI 的增强开关界面）。
5. 按任务文档 §9/派发边界，修改 `ring_udp_replay.cpp`（src/ 生产测试工具）不在
   本次允许改动清单内，故未实现 CONFIG-ACK 模拟来"绕过"阻塞。

## 影响面与替代（已执行）

- §8 判据的 `T_arr` 以任务文档 §6 预注册的默认节拍 40 Hz 由块周期公式给出（设计节拍），
  并在 `criteria_s8_substitution.txt` 中逐条代入；**正式判定待 Layer 2b 实测确认**。
- 敏感性矩阵以 Layer 2a 同构驱动（SHM+ZMQ，绕过 UDP ingress）替代执行了
  块大小(100/200)与网格(10/20µm)两轴 × {main, branch-both}（mingw-release 口径），
  明确标注为**替代口径**：不含 SocketReceiver/SourceCore/HostOutput 与真实 UDP 节拍抖动。

## 用户在场时的复跑命令（每命令一行；两侧构建产物已存在）

预备（同机，先启动 GUI 并完成环形配置与启动）：

```text
# 1) 启动接收端 GUI（main 口径用 main 构建；branch-both 口径用分支构建）
D:\ChatGPT\PAERealtimeImaging\_task_work\main-baseline-c95917f\MC_410T_MultiCard\delivery\build\mingw_debug\bin\PAimageReceiverDiagnostics.exe
#    （或分支：...\_task_work\ring-enhance-branch-20260926181824\MC_410T_MultiCard\delivery\build\mingw_debug\bin\PAimageReceiverDiagnostics.exe）
# 2) GUI 内：环形扫描参数设定 -> 网格 0.01mm、每通道每块 200、alinesPerFrame=8000、8 通道；
#    分支 GUI 另设：重建增强 = 双极补偿 + 低频补偿（both）或全关（off/main 口径无此界面）
# 3) GUI 内：启动成像（PAimage CONFIG/START 需要真卡或等效控制面应答）
```

回放与采集：

```text
# 全链回放（≥2 圈；K=500/通道/波长 → 每圈 1000 触发；2 圈 = 2000）
ring_udp_replay.exe --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat --id 14 --bits 32 --rate 40 --triggers 2000
# 敏感性：--block 由 GUI 下发（100/200；400 非法，见 README 偏差 3），网格 0.01/0.02mm 各一组
# 采集：诊断日志导出（RingSHMObs avg_process_us/avg_queue_us/avg_copy_us、逐块耗时、
#       block_copy_rejected、整圈 wall）+ 逐块帧落盘，与 Layer 2a 同口径汇总
```

若未来允许改动测试工具：为 `ring_udp_replay` 增加最小 CONFIG-ACK 应答模拟（控制端口协议），
即可在无真卡环境跑通全链；该改动超出本次授权范围，未实施。
