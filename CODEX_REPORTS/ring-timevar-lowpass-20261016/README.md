# Ring 旧增强移除与时变低通：执行回执

本实现候选供分析代理独立审查；未合并 main，未进行真实采集/视觉验收，真实 T_arr 仍为 UNVERIFIED。

## 身份与范围

- 任务：`origin/codex/task-docs@4687ebb61f827d100720220f0f1f5119bb7af948:TASKS/环形旧增强移除与时变低通接入_20261016-132206.md`。
- 起点：`a8c15cff203da959bd935120605e69dc32ff7145`；实现分支：`codex/ring-timevar-lowpass-20261016`。
- 实现/数值/性能验证源码：`bac25da503022ba2e70e5405a3a3631efd9f5727`，单父提交，tracked-clean。
- 最终交付 revision、local/remote 相等证明与完整目录由包内 `git-receipt.txt` / `build-manifest.txt` 记录。
  回执提交只追加证据，正式包在最终交付 revision 重新 configure/build；不把旧 BuildIdentity 改标。
- 本地 main 的独有文档/开发配置提交 `d3054d51e19570a6ef38774b8a82326738dce53b` 原样保留。
  开发工具使用指引已在开始实施前阅读，未把该提交隐式接入实现分支。

## 实现

删除旧两项补偿的控件、配置类型、controller/service 成员、JSON 发送/解析、生产算法和 CMake 引用。
删除仅覆盖旧算法的 ring_signal_enhancer_test、ring_enhancer_bench、ring_enhancer_parity_test 三项测试。
历史分支、CODEX_REPORTS 原始证据、frozen migration provenance 和旧暂存区均未改动。

新顺序是完整 Ring A-line 转 double → RingTimevarLowpass → 原 preprocessBlock 的 DBR 置零/限幅/DelayCut
→ 原 WL2 跨块对齐/几何映射 → CUDA DAS。共享 frontend clone、DisplayBuffer、raw publisher、FileSaver
及 CUDA 数值/ABI、SHM 数据布局保持。关闭时直接 bypass，连输入扫描、延拓、重采样或转换也不执行。

配置通过独立 `ring.timevarLowpass` 对象传输：enabled、fcMaxMhz、fcMinMhz、p1、p2、windowLength、
overlapRatio、fftLength、filterOrder、transition。INI 在 RingConfigDialog/Defaults 下使用对应 timevarLp 前缀。
出厂为 false、45/1 MHz、780/2000、128、0.75、auto(0)、4、smoothstep。p1/p2 是裁切前 1-based 点号，
Fs 来自当前 Ring 配置。高级确定只改父草稿；父应用/确定才提交，取消恢复最近成功应用的滤波快照。

JSON/INI 存在但非法的字段拒绝整个快照；缺字段按规格回退出厂字段。非法已存快照提示并使用完整 disabled
出厂配置，不覆盖坏记录。只有显式成功“设为默认”才清理旧 enhance 键。延迟创建窗口先保留完整默认快照，
等实际采集深度已知再校验；以后深度改变时拒绝超界应用，不钳制 p1/p2 或窗长。控件显示精度不暗改已保存 double。

服务先完成校验和计划准备，再替换配置/各通道延时。任一 A-line 失败时不提交半块 CUDA，也不部分更新 WL2
历史；无静默 fallback。新核是原创 radix-2 / Bluestein CPU FFT，没有新增第三方依赖或 MATLAB 运行依赖。
缓存窗、plan、掩膜和工作区；每条线清空信号状态。仅新增 CPU 内核启用 -O2（GCC/Clang），保留 Debug 符号，
不使用 fast-math。显式资源上限为窗/FFT 16384、完整深度 1048576、掩膜 8M doubles（64 MiB），超界拒绝。

为完整块计时在已有 snapshot JSON 添加兼容字段 process_us；不更改 SHM 布局、轮次所有权或完成判定。

## 独立数值与回归

原 MATLAB 文件只读，字节 SHA256 为 E04AA8726B53E1284DCFAEA6FE8962CA8E424171D2D48BE28829D62458BB2235。
提取参考只删除 Plot/绘图、注释/空白及未使用的 S0。Oracle 为 MATLAB 9.14.0.2206163 (R2023a)、
Signal Processing Toolbox 9.2，固定 Twister seed=20261016。输入、输出、F/T/fc/H、yTemp/tTemp 和版本/参数
在 tests/reference/fixtures；导出脚本、原始 Windows 文件 SHA 与 canonical Git blob SHA 分开记录，避免换行误判。

- 36 组配置、327 条独立输入；double maxAbs 归一化最坏约 5.03e-13，低于 1e-8；L2 同时满足规格。
  服务最终 float 与 single(reference) 对照满足 1e-5（本轮最坏 float 误差为 0）。检查包括头尾，没有排除边缘。
- 默认 N=2030/5000/7500、非 hop 整数长度、N=窗长、N=2、小/奇/偶窗、auto/256/192/129 FFT、
  linear/smoothstep、相同截止、非整数阶数、零/DC/首尾与 p1/p2 脉冲/双频/固定噪声及多列输入。
- 周期 Hann、帧中心（奇窗半采样点）、原始完整帧、共轭对称、FFT 归一化和 WOLA 已由 MATLAB 锁定。
  默认 N=5000 的 yTemp 为 4992 点；长度分支按 tTemp 线性插值到原时间轴、范围外为零。
- 原 exact baseline 的 preprocessBlock + OLD enhancer 双关，独立编译捕获 128 个输出字节指纹，逐位通过。
  生产共享 helper 覆盖 DBR/限幅/DelayCut 的全部开关组合、多个 systemDelay；auto/显式 FFT 等价，重复和交错线无状态泄漏。
- 极小截止案例（N=2030、fcMinMhz=1e-30、p1=780、p2=1000）使 MATLAB istft 以 expectedFinite 拒绝；
  生产校验在接受配置前按相同公式明确拒绝浮点相消产生的非正 fc，不另加 padding 或暗改响应。
- UI/临时 INI、父/高级取消与应用、默认保存/恢复/重启、首次真实深度、深度缩短拒绝、坏 INI 不覆盖通过。
  服务实际入口覆盖配置原子性、末线失败整块屏障、共享输入保持、跨轮 reset/stale；新增测试只在专用目标启用 friend seam。
- 实际基线 CTest 51；移除旧算法 3 项，保留 48 项，加数值/UI/服务 3 项，现为 51 项，通过 51/51。
  Python 解释器已显式指定；初次基线的 8 个临时目录写入失败在工作区内 TMP/TEMP 重跑后通过，不记成断言通过。

## 构建、依赖与复现

标准主入口为 delivery/build_mingw_debug.cmd，mingw-debug / mingw-debug-build，Qt 6.8.0、GNU 13.1.0、
CMake 3.30.5、Ninja 1.12.1，Debug + USE_ZEROMQ=ON。最终包包含完整 native bin、Qt/plugins、diagnostic-tools、
runtime、manifest、文件和依赖 SHA256、validation 和 build.log；详见包内回执。

Ring CUDA 实际来源是 _migration_pack/prebuilt_cuda，无 CUDA 核心或 ABI 改动，无重编。
cudart64_12.dll 与本机 CUDA v12.8 toolkit 字节相同。cuFFT 是已验证、原样保留的本地依赖
delivery/libs/imaging/cufft64_12.dll（NVIDIA 有效签名，cuFFT 12.1.0.31）；其 SHA 与 2026-09-22/23
以及原环形预算/交付回执一致，不把它误记为本机 CUDA v12.8 的 cufft64_11.dll。
五个关键 runtime hash 均与原已验证组合相同，验证与交付复用同一份 runtime。

复现命令、退出码、最终性能原始记录及完整文件清单在本目录证据文件和本地 artifacts/ring-timevar-lowpass-20261016。
tests/reference 下的工具可重导 MATLAB oracle、构建 exact old baseline；tests/run_timevar_service_matrix.py
可重跑内存输入的 GPU 服务矩阵。真实设备操作见 hardware-operation-card.md。

## 验收边界

本分支候选的软件/回放结果供分析代理审查；真实硬件、视觉效果和 Layer 2b T_arr 均未验收。
不把 GPU 合成回放或数值通过写成现场视觉/实时预算 PASS，不回溯证明额外 trigger 根因。

## 最终回放与性能结果

- exact旧基线服务与新服务OFF：10个块、双波长20份图像raw文件SHA256全部相同；原始文件在本地off-compare，hash清单已归档。
- ON的无显式reset跨RoundIdentity屏障、缺块后source-final完成语义与下一轮清洁启动通过；2秒timeout的圈中2.5秒暂停产生epoch_reset并完成后续20块。
- 最终微基准12单元×30个warm；主服务矩阵12单元/450块/438个warm；实际本地几何3单元/120块/117个warm全部exit=0。
  当前本地几何默认ON的service median/P95=554.082/616.821 ms；详细cold/median/P95/max、吞吐、滤波增量、设计时间/内存见performance.md及原始记录。
- 首次独立旧基线回放曾首块等待超时；清理本任务遗留进程、显式使用与主包一致的DLL/PATH后，原正式发送器重跑通过。该首次等待的根因未单独归因，失败日志保留。
- 同步模拟发送器的0.2秒timeout试验在正常约0.6秒阻塞处理回调间隙误触发了模拟idle；改用本地已保存的2秒阈值、专门暂停2.5秒后通过。
  这描述模拟发送器的时间模型，不改生产timeout owner，也不把它当真实采集链路的预算证据；原失败和重跑命令/日志均保留。

证据文件在evidence/；最终HEAD与交付包的BuildIdentity、remote相等证明及文件hash见包内git-receipt.txt、build-manifest.txt。

独立审核 P2「主窗口忽略配置拒绝仍启动」的追加修复、真实入口集成测试与新交付入口见
[start-rejection-fix/README.md](start-rejection-fix/README.md)。以上原始执行证据保留，不能作为本次修复后的新构建身份。
