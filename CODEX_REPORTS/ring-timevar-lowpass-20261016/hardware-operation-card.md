# 实机/视觉验收操作卡（尚未执行）

使用最终 delivery 的完整 bin，不单独替换 DLL。候选 SHA、BuildIdentity、preset、runtime SHA256
从同包 build-manifest.txt、git-receipt.txt、dependency-sha256.txt 读取并附入现场记录。

1. 保持同一采集配置、场景、前端高/低通与成像几何。先关闭新滤波采集一轮，再启用出厂参数
   45/1 MHz、780/2000，逐通道、逐波长对比图像。记录全部设置、图像与原始诊断日志。
2. 修改四项基础参数及高级窗/重叠/FFT/阶数/transition，再 OFF↔ON 对比。p1/p2 是裁切前
   完整 A-line 的 1-based 采样点；不要按 systemDelay 减去或重编号。
3. 分别核查高级取消/确定、父取消/应用/确定；“设为默认”后重启，核对高级五项、auto/显式
   FFT 和 enabled 一起恢复。缩短采集深度使 p2/窗超界时应明确拒绝，不自动钳制。
4. 对比 OFF/ON 的前端时域与频域显示、raw publisher 和保存数据，确认开关只改变 Ring 图像。
5. 验证双波长、多个物理通道、下一轮、新 session、timeout 后重启及服务重启；旧轮不得污染新轮。
   超过设计 cap 后信号显示继续刷新，Ring 保持冻结，保存/计数沿原规则工作。
6. 如需评估现场实时性，保存真实完整块到达周期 T_arr、队列积压和服务实际 process_us。
   与包内 OFF/ON 性能记录分开报告，不把回放吞吐当真实链路到达预算。

由用户判定视觉效果是否接受。软件数值、合成回放与 GPU 构建通过均不代替这项决定。
本次不证明额外 trigger 根因，也不扩大历史硬件验收；Layer 2b T_arr 仍待真实测量。
