# M3 阶段 B：真实采集组包链路

## 目标

把环形重建接到与线性实例相同的实时数据源：采集卡 UDP → DataProcessor
（按触发组包、int16/Q0.15 差分相位 → float32 kHz 频率）→ 环形组包器
（跨通道按 triggerSeq 对齐）→ 每通道每块 A-line 数（默认 200）组块 →
环形重建实时显示。

## 新增组件：RingBlockAssembler

- 输入：每个物理通道（0..7）每触发一根 A-line（float32，sampDepth 点，
  频率数据）。
- 逻辑：
  - 按 `triggerSeq` 暂存各通道触发行，所有**启用**通道同一触发到齐后
    按 trigger-major 顺序组块（与 ImagingSvc 期望布局一致）；
  - 每通道累计 `alinesPerChannelPerBlock` 根后回调提交
    （raw + 每根角度 + 通道号 + 块序号）；
  - 角度：通道 s 扇区起点 = `sectorStartDeg + s*sectorWidthDeg`，
    wl1 行 k = 起点 + k*step，wl2 原始行 k = 起点 + (k+1)*step
    （跨块对齐后与 wl1 同角）；
  - 未完成触发最多缓冲 8 个，超出丢最旧，避免单通道丢触发导致链路停摆。

## 接入点

- `MainWindow::feedRingImagingPulse()`：环形模式下由 5ms 成像定时器轮询各卡
  `DisplayBuffer` 全分辨率频率快照，按物理通道逐触发送入组包器；
  组满一块后回调 `ImagingController::submitRingBlock` → 共享内存 → ImagingSvc。
- 环形服务由“环形扫描控制台”启动（svcReady 自动开启主窗口馈送定时器，
  停止时复位组包器）。
- 控制台新增“真实采集（UDP）”开关：勾选后不再走模拟文件喂块，
  只启动重建服务等待主窗口馈送。

## 数据格式与精度

- 组包数据为双波长 A-line 交替续接的一维数组（未切分、未对齐），
  切分与 wl2 对齐均在 ImagingSvc 预处理阶段完成。
- 精度可选 int16 / single：由采集协议 `AcqConfig.bitsPerChannel`（16/32）
  决定，DataProcessor 统一转换为 float32 kHz 频率后进入组包器；
  前端“数据精度”控件用于标注当前协议，不影响重建侧数据格式。
- 通道勾选：只取勾选通道的数据参与组包；未勾选通道即使有底噪数据也会被忽略。

## 验证

- `ring_svc_selftest` 已改为经 RingBlockAssembler 喂入（模拟每通道每触发
  UDP 到达顺序），14.dat 8 通道 5 块全链路通过，单块约 75 ms。
- 组装器输出与直接组块结果一致：wl1 相关系数 1.0，wl2 ≈ 0.997
  （差异来自组块边界 wl2 对齐方式，属预期）。
- 与 1 通道整圈参考一致：wl1 corr=1.0。

## 同机 UDP 回放整链路联调

工具：`ring_udp_replay.exe`（把已解调的 .dat 按真实 UDP 协议回放到本机
8001~8004，载荷为 B/A 交织 int16/int32，触发按扇区映射）。

接收端回放模式：`MC410T_Receiver.exe --udp-replay`（跳过设备扫描，以
127.0.0.1 为 4 张虚拟卡），并在环形控制台勾选“真实采集（UDP）”。

回放步骤：
1. 启动上位机（--udp-replay）→ 打开环形控制台 → 勾选真实采集 → 启动环形重建；
2. 主窗口点“开始监听”；
3. 运行：`ring_udp_replay --data D:\zzx\data\20260519\14.dat --id 14
   --bits 32 --rate 40`（40Hz 为真实 A-line 速率，回放约 25 秒）。

验证结果：4 卡各 1000 触发、0 丢失、5 块全部出帧；重建图像与直接组包参考
wl1 corr=0.999998、wl2 corr=0.999996（差异仅为 int32 量化）。

注意：回放速率需 ≤ 轮询能力（约 200 触发/秒）；突发发送时显示缓冲只保留
最新触发会跳过中间触发，属测试节奏问题，真实采集 40Hz 无此限制。

## 联调中发现并修复的问题

**环形共享内存偏移重复计入 64 字节头**：`ringAnglesOffset/ringChannelsOffset/
ringFramesOffset` 原以“数据区起点(h+1)”为基准却又加了一次 header 偏移，
导致角度/通道/帧区整体错位 64 字节。因读写双方错误一致，selftest 内部自洽
未暴露；外部读取与共享内存实际布局不符。已修复为相对 h+1 的偏移，总大小
单独加回 header；selftest 回归验证图像逐位一致（无回归）。

## 待真机验证项

- 实际 UDP 触发序号的对齐与丢触发行为；
- F64（每通道 8 光纤）模式暂未支持，当前按单光纤/通道处理；
- 卡数量与通道勾选的对应关系（m_nCards 与 8 通道位图）。
