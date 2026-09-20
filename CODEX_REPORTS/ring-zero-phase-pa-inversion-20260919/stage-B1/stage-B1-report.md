# 阶段 B1 执行报告：环形成像高低通零相位滤波实现（B1 整改后）

任务：`TASKS/阶段B1环形成像高低通零相位滤波实现_20260920-144420.md`
整改任务：`TASKS/阶段B1审查整改配置安全与交付证据_20260920-184529.md`（R1–R5）
收口任务：`TASKS/阶段B1收口非致命配置拒绝与验收回执_20260920-222011.md`（S1–S4）
实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
阶段 A 已验收起点 SHA：`91ca68b36dac9c14ccd756c52896ea20ad794e0a`
B1 原生产提交：`45cd0502fcf8ca68a73b62dd2beca80b4c88d25c`
B1 整改提交：`777df4f33fc93655b06e19eeb625c01d40204903`（收口任务起点）
B1 收口生产源提交 C：`d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697`（交付构建绑定此 SHA）
B1 收口报告/证据提交 R：见本提交（`git rev-parse HEAD`，build source=C，见 §8/§10）
阶段：**B1 收口完成（S1–S4）**。B2（光声反演滤波与距离指数退役）未开始。

## 0. 整改摘要（R1–R5 对照 + B1 收口 S1–S4）

| 项 | 审查发现 | 整改位置 | 验证 |
|---|---|---|---|
| R1 短线校验只用 ch1/wl1 | `ImagingSvc.cpp` 短线校验改为逐启用通道×波长（与运行时 outRows 同式），延时解析延迟到校验通过后提交 | `src/ImagingSvc/ImagingSvc.cpp`（C2/短线区 + 提交区）、`src/RingConfigDialog.cpp` applyConfig UI 前置同规则 | 服务端实测拒绝含通道/波长/D/Nt（§5）；UI 正负例（§6 T2） |
| R2 Nyquist 动态 setMaximum 静默压值 | 删除 setMaximum 动态改上限；保留 0.0001–500 稳定范围 + 动态提示；应用时校验 | `src/RingConfigDialog.cpp` setAcquisitionParams | fs 变化保值/越界应用拒/恢复后原值在（§6 T2） |
| R3 应用边界无忙时拒绝 | `ImagingController::configureRing` 返回 bool、忙时拒绝；svc 端 m_running 时 configure 拒绝（error 2014）；移除"自动重启"链路 | `include/ImagingController.h`、`src/ImagingController.cpp`、`src/ImagingSvc/ImagingSvc.cpp`、`src/MainWindow.cpp`（启动路径处理拒绝）、`include/MainWindow.h`（移除 m_restartRingOnSvcStop） | svc 实测运行中再配置被拒（§5）；停止态可正常应用（§6 T2 R3a/R3b） |
| R4 参考缺失仍 PASS | D 区参考对照强制执行：目录/文件缺失/长度错误非零失败；对照数量守门（14 文件） | `tests/ring_zero_phase_filter_test.cpp`（D0/D2）、`tools/prepare_zpf_reference.cpp` + `tests/CMakeLists.txt`（干净 checkout 自动再生成） | 正例 43 向量全过 + 工具输出与 MATLAB 导出逐字节一致；负例 3 类非零退出（§6 T1） |
| R5 交付占位/性能口径不清 | 本报告全部真实路径与命令；性能统一口径复测（预热+3 次取中位），澄清 A-line 计数 | 本报告 §5–§7；交付目录 §8 | 见下文 |
| **S1 配置拒绝破坏运行状态** | 忙时拒绝经 svcError→onImagingError 误杀正常成像（ready=false/停定时器/取消勾选）；服务端 2014 同样被吞为 svcError；采集忙未覆盖 | 新增 `svcConfigRejected` 非致命信号：configureRing 忙拒与 2014 分流；MainWindow `onImagingConfigRejected` 仅提示；`isAcquisitionBusy()` 谓词注入 RingConfigDialog（用户应用拒/内部提交例外） | 端到端集成测试 37 断言全过（§6 T5，真实 MainWindow/Controller/ImagingSvc 子进程） |
| **S2 性能算术与支持范围错误** | 409.2/656.3ms 在自身 320ms 假设下超 89.2/336.3ms 却称低于/可跟；1060.6ms 超 800ms 节拍 260.6ms；无逐次原始记录 | §5.3.1 算术纠错 + 提交 C 空闲复测逐次记录（CSV/JSON）+ 两轮数据敏感性如实声明；§9.4 支持范围重写 | 复测原始记录 `stage-B1/perf-remeasure/`（§5.3.1） |
| **S3 交付身份占位** | 报告引用时间戳目录占位，BuildIdentity 无实际值 | 提交 C 上真实 configure+build，BuildIdentity 绑定完整 C SHA；交付 staging 实路径（§8） | 构建回执 + 三处 SHA 核对（§10） |
| **S4 测试账目不完整** | 称 47 项但只交代 44+1；python 项从未注册未说明；崩溃测试无根因记录 | §6 T4 全量账目（47=46 C 二进制 + 1 新增；python 2 项从未注册的机制说明）；physical_round_normalizer SEGFAULT 首获 gdb 栈、140 次直接运行样本、独立证据文档 | 47/47 全量通过（提交 C）；test-account/ 崩溃证据 |

## 1. 改动文件（相对 MC_410T_MultiCard/delivery，相对 45cd050）

| 文件 | 改动 |
|---|---|
| src/ImagingSvc/ImagingSvc.cpp | R1：短线校验逐启用通道×波长（消息含通道/波长/D/Nt/最小长度）；sysDelayPerChannel 先解析局部、校验全过后才提交 m_ringSysDelayCh（拒绝不留部分状态）。R3：processConfigure 在 m_running 时拒绝（error 2014） |
| include/ImagingController.h | R3：configureRing 返回 bool；新增 isBusy()；移除 ringConfigChangedWhileRunning 信号 |
| src/ImagingController.cpp | R3：configureRing 忙时拒绝（isBusy = 运行中或子进程非 NotRunning 的启停过渡）；停止态返回 true |
| include/MainWindow.h、src/MainWindow.cpp | R3：移除运行中变更自动重启链路（m_restartRingOnSvcStop 与 ringConfigChangedWhileRunning 处理器）；成像启动路径对 applyConfig/configureRing 拒绝显式处理（不拉起馈送、记录诊断、恢复控件） |
| src/RingConfigDialog.cpp | R1：applyConfig 增加与服务端同规则的逐启用通道×波长短线前置校验。R2：setAcquisitionParams 不再动态改 spin 上限（稳定范围 0.0001–500 MHz + 动态合法频段提示，越界值保留、应用时拒绝）；R3：configureRing 返回 false 时明确提示且不发出轮次策略变更 |
| src/RingReconCuda/ring_svc_selftest.cpp | 新增 `--expect-busy-reject 1`：start 后再 configure，验证服务端忙时拒绝（error 2014） |
| tests/ring_zero_phase_filter_test.cpp | R4：D 区参考对照强制执行（D0 目录必须存在；D2 对照数量守门=14 文件；缺失/长度错非零失败） |
| tests/ring_config_busy_reject_test.cpp | **新增**（offscreen）：R1 UI 正负例（wl1 足/wl2 不足、禁用通道不阻塞、其它启用通道不足、3n 拒/3n+1 收、双滤波不同阶、全关不新增拒绝）；R2（fs 变化保值、越界应用拒、关闭保值）；R3（停止态可应用）；R2 defaults 六键隔离往返（设为默认按钮→销毁→新对话框恢复）；取消不改变活动配置 |
| tools/prepare_zpf_reference.cpp | **新增**：MAT v5 直读参考向量导出工具（miCOMPRESSED zlib 解压 + miMATRIX 解析 + 小元素/整数压缩存储语义），与 `stage-B1/matlab/export_zpf_reference.m` 输出逐字节一致 |
| tests/CMakeLists.txt | R4：prepare_zpf_reference 目标（构建时从阶段 A MAT 证据自动生成参考向量，无 MATLAB 运行时依赖）+ R3：ring_config_busy_reject_test 目标。**S1：ring_config_reject_integration_test 目标（全 MainWindow 链接 + 真实子进程部署 + RUN_SERIAL）** |
| include/ImagingController.h（S1 收口追加） | 新增 `svcConfigRejected` 非致命信号；`IMAGING_CONTROLLER_TEST_SEAM`（testProcessMessage / testOnSvcFinished，仅测试编译单元启用，与 DataProcessor TEST_SEAM 同型） |
| src/ImagingController.cpp（S1） | configureRing 忙拒与 processMessage 2014 改发 svcConfigRejected（svcError 故障路径分流保留）；其余错误 code 不变 |
| include/MainWindow.h、src/MainWindow.cpp（S1） | 新增 onImagingConfigRejected（仅记录+提示，零状态副作用）与新信号连接；新增 isAcquisitionBusy()（m_isMeasuring/m_isListening）公共只读探针；ensureRingConfigDialog 注入采集忙谓词；onRealtimeImagingToggled 启动路径改走 applyConfigForRealtimeStart（内部提交） |
| include/RingConfigDialog.h、src/RingConfigDialog.cpp（S1） | setAcquisitionBusyPredicate 注入点；applyConfig 用户路径前置采集忙拒绝；applyConfigForRealtimeStart 内部提交路径（不查采集忙，控制器 isBusy 仍生效）；validateAndSubmit 公共实现 |
| tests/ring_config_reject_integration_test.cpp | **新增（S1）**：真实 MainWindow/RingConfigDialog/ImagingController/ImagingSvc.exe 端到端 37 断言（任务必测五类，见 §6 T5） |
| tools/stage_b1_perf_remeasure.mjs | **新增（S2）**：性能复测脚本，逐次原始记录（CSV/JSON） |
| ../stage-B1/perf-remeasure/（报告侧，R 提交） | **新增（S2）**：perf-remeasure-runs.csv（每次运行一行）+ perf-remeasure-summary.json |
| ../stage-B1/test-account/（报告侧，R 提交） | **新增（S4）**：physical_round_normalizer 崩溃证据（gdb 栈、逐项/直接运行样本、复现命令）+ 47/47 全量 last-run 日志（ctest_full_47_47_last_run.txt）|

未改动：zero_phase_filter.cpp/ring_recon.cpp 数值核心（阶段 A 参考对照逐样本通过——见 §6 T1）；ring_das_kernel/snapshot（CUDA ABI 未变，`_migration_pack/prebuilt_cuda` 既有 DLL 继续使用）；采集/raw 格式/RoundIdentity/timeout/stale/save 链路；**S1–S4 未改动 PhysicalRoundNormalizer/zero_phase_filter/ring_das 等数值与采集单元**。

## 2. 数值实现约定

与 45cd050 版本一致（数值核心未改动）：SOS 设计路径与 MATLAB R2023a butter 逐位一致；零相位奇对称延拓 nfact=3n、逐节稳态初始化、前后向；滤波内部 double 完成后转 float；顺序 DBR置零→削顶→delayCut→HP→LP；每线独立、无跨线状态；设计缓存每配置一次。阶段 A 冻结约定全文见 45cd050 版本本节（保留于 git 历史）。

## 3. 配置有效性规则（服务端强制，UI 独立重复）

逐启用通道 c∈{0..7}×波长 w∈{1,2}：

```
D = sysDelayCh[c][w]；Nt = sampDepth
有效输出线长 outRows = delayCut ? (Nt−D+1) : Nt
任一滤波启用时：outRows 必须严格大于 need = 3×max(启用阶数)
失败消息格式："通道%1/波长%2：有效线长 %3 必须大于延拓长度 %4（3×最大阶数；D=%5，Nt=%6）"
只检查启用通道；全关不引入滤波专属拒绝
```

C2 规则表（E/D/delayCut）、截止范围 (0, fs/2)、双开 hp<lp、阶数 1–8：与 45cd050 相同。所有校验通过后一次性提交 sysDelayPerChannel 与活动配置（含 m_ringZeroPhase 滤波缓存）；任一失败 `sendError(...,2012)` 整组拒绝，不触碰运行状态。

R3 应用边界（新）：

```
UI：applyConfig → configureRing（忙时 false → 明确弹窗，不发出 roundPolicyChanged）
控制器：configureRing 在 isBusy()（运行中或启停过渡）时拒绝、配置不变
服务端：processConfigure 在 m_running 时拒绝（error 2014），活动配置/滤波缓存/轮次不变
自动重启链路（ringConfigChangedWhileRunning → stopSvc → 自动 startSvc）已移除：
修改参数必须"停止后才能应用"，不再静默重启
```

## 4. 字段与 settings 映射

与 45cd050 相同：filterLow/wLow/n1（高通）、filterHigh/wHigh/n2（低通）六键；QSettings `RingConfigDialog/Defaults` 增 6 键（zpHp/zpHpMhz/zpHpOrder/zpLp/zpLpMhz/zpLpOrder），旧配置缺键=全关；关闭时保留值（整改 R2 后采样率刷新也不改值）。MHz 显示、Hz 传递（config() 一次转换）不变。

## 5. 全链路验证（ring_svc_selftest，真实 14.dat，整改后构建）

数据：`D:/ChatGPT/PAERealtimeImaging/testdata/14.dat`（SHA256 `fbcbc105343d8caf8d1b231a93cf00cdbf4710f94a19b93b9f1a86f812e9e056`）。
构建：`cmd /c build_mingw_debug.cmd`（§8 构建回执）。

### 5.1 R1 服务端逐通道短线拒绝（正负例实测）

- 负例（启用通道不足，指明通道/波长）：`--zp 1 --sys-delay-per-ch 358,371,358,371,3989,371,...`（通道3 wl1=3989，outRows=12=3×4）
  → `[config-reject] PASS: svc rejected config: 零相位滤波配置被拒绝：通道3/波长1：有效线长 12 必须大于延拓长度 12（3×最大阶数；D=3989，Nt=4000）`，退出码 0（selftest 预期拒绝成立）。
- 负例（禁用通道不阻塞）：同上 sysDelay 但 `--channels 27`（禁用通道3）→ 完整成像 50 块 `consumed=50 mismatch=0` 退出 0。
- 全关路径逐字节回归：整改后构建 vs **45cd050 基线构建**（`git worktree` 独立构建基线 ImagingSvc），同数据同配置（grid 0.4/block 40/8ch）全关各跑 50 块 ×2 波长=100 帧 → **100 帧全部逐字节一致**（cmp）。
- 滤波四态功能：zp=1/2/3 全部正常出图；zp3 两次运行输出逐字节一致（确定性）。

### 5.2 R3 服务端忙时拒绝实测

`--expect-busy-reject 1`：configure → start → 运行中再 configure
→ `[busy-reject] PASS: svc rejected runtime reconfig: 成像服务运行中，配置未应用：请先停止成像再下发新配置。`

### 5.3 统一口径性能测量（R5）

测量定义（本节所有数字同口径）：
- `avg_process_us`（ring_shm_obs consumer 快照）：ImagingSvc::processRingPulse 从函数入口到 recordProcessDuration 的完整墙钟均值，包含 SHM 拷贝、逐线预处理（含零相位滤波）、CUDA 重建提交、显示快照回写、ZMQ 快照消息发送。不含上游组包/采集等待。
- 每块 A-line 数 = 启用通道数 × 每通道每块 A-line 数（block=40 → 8×40=320 线/块；block=100 → 800 线/块）；sampDepth=4000；fs=250 MHz；8 通道启用；数据 14.dat；构建 mingw-debug（-g，未优化，见 §8）；机器 32 逻辑 CPU（本次测量时系统空闲，CPU 0%）。
- 每配置先 1 次预热运行（丢弃），再 3 次正式运行取中位数/最大值。

| 配置 | 关 | 仅 HP (0.4M,4) | 仅 LP (40M,4) | HP+LP |
|---|---|---|---|---|
| block=40/通道（320 线/块），grid 0.4mm（nx=90），中位 avg_process | 13.2 ms | 196.5 ms | 197.2 ms | 409.2 ms |
| 同上，3 次最大 | 13.2 ms | 196.5 ms | 222.1 ms | 409.6 ms |
| block=100/通道（800 线/块），grid 0.4mm，中位 | 24.9 ms | 485.2 ms | 476.2 ms | 1060.6 ms |
| 同上，3 次最大 | 25.5 ms | 520.8 ms | 486.8 ms | 1075.3 ms |
| block=40/通道，**部署默认网格 0.01mm（nx=3600）**，单次 | 183.8 ms | — | — | 656.3 ms |

### 5.3.1 B1 收口 S2 算术纠错与原始记录（必读——本节更正 §5.3 表的实时性结论）

**算术纠错**（对上表数字，修正前版本报告错误结论）：

- §5.3 第 2 点原称"关 13.2ms、单滤波 ~200ms、HP+LP 409ms **均低于节拍**"——**错误**。在该报告自己引用的 320ms/块节拍假设下，单滤波 ~200ms 低于节拍，但 **HP+LP 409.2ms 超出节拍约 89.2ms**、**部署默认网格 HP+LP 656.3ms 超出约 336.3ms**，均**不可称低于/可跟节拍**。
- block=100/通道 HP+LP 1060.6ms 相对其 ~800ms 节拍假设**超出约 260.6ms**（原报告已承认此点，予以保留）。
- 前版报告由此引出的"HP+LP 实时性不满足"结论对**其自身数据**成立；但 S2 两轮空闲复测（见下）显示同口径处理时延可低至 105ms（block=40 HP+LP）/197ms（部署网格），原表数据显著受测量时机器负载影响。**最终口径见下文"数据敏感性如实声明"：以回放单块时延数据为准，实时性统一 UNVERIFIED，不声明实时通过**。

**原始记录（S2 补测，逐次保留）**：`stage-B1/perf-remeasure/perf-remeasure-runs.csv`（每次运行一行：配置、退出码、avg_process_us、墙钟、时间戳；行含块/A-line 换算列）与 `perf-remeasure-summary.json`（含 gitSha、构建类型、数据 SHA256、计时边界定义、预热/正式次数）。补测脚本：`tools/stage_b1_perf_remeasure.mjs`（每配置 1 次预热 + 3 次正式，与 §5.3 同口径同数据）。

S2 补测（最终源提交 C = `d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697` 上重新 configure+build 的 mingw-debug，8ch、sampDepth 4000、fs 250MHz、数据 14.dat、机器空闲、tracked-clean）：

| 配置 | 中位 avg_process | 3 次最大 | 对 320ms 假设节拍（block=40） |
|---|---|---|---|
| block=40 grid 0.4mm 全关 | 9.1 ms | 9.1 ms | 远低于 |
| block=40 grid 0.4mm 仅 HP | 59.1 ms | 60.0 ms | 低于 |
| block=40 grid 0.4mm 仅 LP | 59.7 ms | 61.0 ms | 低于 |
| block=40 grid 0.4mm HP+LP | 105.2 ms | 106.7 ms | 低于 |
| block=100 grid 0.4mm HP+LP | 263.3 ms | 263.9 ms | 对 800ms 节拍假设低于 |
| block=40 **部署网格 0.01mm** HP+LP | 197.3 ms | 197.4 ms | 低于 |
| block=40 **部署网格 0.01mm** 全关 | 98.0 ms | 102.4 ms | 低于 |

**数据敏感性如实声明（重要）**：本任务在同一构建口径上做了两轮复测——第一轮（777df4f 构建，机器后台有负载，数据见 git 历史中本节初版）得 HP 190.6 / HP+LP 281.7（最大 398.1）/ 部署网格 HP+LP 227.0 ms；第二轮（上表，提交 C 构建、机器空闲）HP 59.1 / HP+LP 105.2 / 部署网格 197.3 ms。**同一代码同一口径两轮差异达 2–4 倍**，说明该测量对机器负载极敏感，§5.3 表原数据（409.2/656.3/1060.6ms）大概率同样受当时后台负载影响而被抬高。

补测说明：
1. **算术纠错不依赖数据取舍**：§5.3 表原数据在其自身 320ms 假设下 HP+LP 409.2ms 超约 89.2ms、部署网格 656.3ms 超约 336.3ms——原报告"均低于节拍"是算术错误，必须更正（本节开头）。block=100 HP+LP 1060.6ms 对 ~800ms 节拍超约 260.6ms。
2. **两轮补测原始记录全部如实保留、不挑选**：上表（提交 C、空闲机、方差 <2%）为质量较高的一组；第一轮数据（非空闲）在 git 历史本节初版中同样保留数值。**鉴于组间差异 2–4 倍，任何"可实时/可跟节拍"的结论都不成立**——本任务按任务要求如实交付：当前可验证使用范围=回放链路单块处理时延（本节数据）；真实持续输入下是否积压**实时性 UNVERIFIED**（无真实触发间隔数据、未做持续对齐实测）；不冒充实时通过。
3. 本补测不改变 §5.3 之 1 的每线增量结论（单滤波每线增量与 45cd050 的 0.55–0.60 ms 同量级——原表 196.5ms/320 线 ≈ 0.61 ms/线；空闲复测 59.1ms/320 线 ≈ 0.18 ms/线，两轮差异亦说明原表含显著负载开销）。原表与两轮补测数据均保留供审查，不互相覆盖。

说明与澄清（对应 R5 审查项，S2 修订后）：

1. **每线滤波增量**：block=40 仅 HP 196.5ms/320 线 ≈ 0.61 ms/线；仅 LP 0.62 ms/线；HP+LP 409ms ≈ 1.28 ms/线（两滤波叠加）——单滤波每线增量与 45cd050 报告的 0.55–0.60 ms 一致。原报告"HP+LP 104ms"与其自身每线数据（320 线 × 2×0.58 ≈ 371ms）不符：经本次复测证实 104ms **不能**在 320 线/块口径下复现（本机三次中位 409ms）。原 104ms 的测量条件已无法追溯（疑为线数口径混用或包含了部分预热块），如实标记为**口径不明、不可复现**；104ms→965ms 的"线性"结论随之作废。以本次统一口径数据为准：800 线 vs 320 线，HP+LP 1060.6/409.2 = 2.59（A-line 比 2.5，考虑每块固定开销后基本线性）。
2. **块节拍与积压（预计，非实测；S2 修订）**：真实触发节拍取决于 FPGA/采集（本仓库回放链路无真实触发间隔数据）。按 45cd050 报告引用的节拍假设（block=40/通道 ≈ 320ms 采集/块）：§5.3 表原数据（关 13.2ms、单滤波 ~200ms、HP+LP 409.2ms）中 HP+LP **超出节拍约 89.2ms**——前版"均低于节拍"为算术错误（S2 已更正，见 §5.3.1）；S2 空闲复测同口径 HP+LP 105.2ms（两轮差异源于测量时机器负载，见 §5.3.1）。block=100/通道（~800ms 节拍假设）HP+LP 1060.6ms 超出约 260.6ms。**实时性统一 UNVERIFIED**——未做真实节拍对齐的持续输入实测，`slot_busy_before_submit=0` 仅说明本次回放（同步等快照）无提交冲突，不构成"无丢块"证明。
3. **部署网格（S2 修订）**：默认 grid 0.01mm（nx=3600）下 §5.3 表全关 183.8ms/块——重建/快照成本主导（90×90 时仅 13.2ms）；§5.3 表 HP+LP 656.3ms/块按其自身 320ms 节拍假设**超出约 336.3ms**（前版"预计可跟"为算术错误，S2 已更正）；S2 空闲复测部署网格 HP+LP 197.3ms/全关 98.0ms（负载差异见 §5.3.1）。更高块或滤波组合未逐一扫参。
4. 全部为 **mingw-debug（-g 未优化）CPU 滤波**实现；未做 GPU 化（任务禁止范围）。实时支持范围结论见 §9。

## 6. 测试证据（CTest，tests 树 `cmake -S tests -B build/tests_all_mingw_debug`）

环境注记：工作区绝对路径较长，MinGW depfile 在 260 字符限制下失败；用 `subst X:` 短路径映射完成测试树配置/构建/运行（构建产物同一物理目录，非代码改动）。

### T1 ring_zero_phase_filter_test（32 断言，含参考）

- 正例（真实参考）：`--ref CODEX_REPORTS/.../stage-B1/matlab/reference_vectors` → 32 passed, 0 failed（31 原有 + 新增 D2 对照数量守门）。
- 参考来源与准备：`tools/prepare_zpf_reference`（CMake 构建时自动从 `evidence/filter_reference_vectors.mat`（阶段 A 已验收 91ca68b，SHA256 `ed2262dd2c03b6a4ee74e6175a47e2442870b123b3e1f8a7a60915cbb8d1d544`）生成）。工具输出 43 个向量与 MATLAB `export_zpf_reference.m` 导出**逐字节一致**（cmp 全等）。干净 checkout 无 MATLAB 依赖。
- 负例（整改 R4 要求）：目录缺失 → `FAIL D0` 退出 1；缺单向量（outHP_burst）→ `FAIL D1/D2` 退出 1；损坏长度（截断 outHP_burst）→ `FAIL D1/D2` 退出 1。全部非零失败，不再有"参考缺失降级 PASS"。

### T2 ring_config_busy_reject_test（25 断言，offscreen，新增）

全部 PASS（R1 UI 正负例 8 项、R2 保值/越界拒 8 项、R3 停止态应用 2 项、defaults 六键隔离往返 5 项、取消不改活动配置 2 项）。settings 隔离：测试二进制目录 INI，启动清空、结束移除，不触碰宿主真实配置；"设为默认/取消"经真实按钮路径触发。

### T3 ring_config_zero_phase_dialog_test（20 断言，既有）

全部 PASS（与 T2 顺序运行，T2 结束清理 INI 后出厂默认断言不受影响）。

### T4 既有回归与全量账目（S4）

- 45 项既有 + 本轮新增 1 项（ring_config_reject_integration_test）= **47 项 CTest 全量**（本机环境未注册 2 项 Python 分析测试——`find_package(Python3)` 未找到解释器，startup_diagnostics_analyze_test / paimage_start_race_analyzer 从未注册；此前报告"47"中的构成即 46 项 C 二进制测试 + python 可用与否的差异，**不存在被删除或跳过的失败测试**）。逐项状态与命令输出见交付目录 validation.txt 与 test-account/。
- **提交 C（d8dd3daf）上全量 CTest：47/47 通过**（多次复跑同结果）。
- `physical_round_normalizer_test` 偶发 SEGFAULT：本轮**首次捕获稳定 gdb 栈**（libstdc++ deque 反向迭代路径，T1-4000 循环中部，触发 trigger 随机 2318–3917），单独逐项运行 8 次中 1 次、直接 exe 运行 140 次中 40 次崩溃（12–45%）。该测试与被测源本次**零改动**，不声明"无关"——完整证据（回溯、复现命令、排除过程）见 `stage-B1/test-account/physical_round_normalizer_segfault.md`，标记为**既有偶发、根因未定位**，建议后续以 ASan/MSVC 或工具链升级定位（不属本任务范围）。

### T5 ring_config_reject_integration_test（37 断言，offscreen，S1 新增）

真实生产路径集成（非 stub）：真实 MainWindow（生产 svcError/svcConfigRejected 连接、onImagingError/onImagingConfigRejected 槽）+ 真实 RingConfigDialog（与 ensureRingConfigDialog 同款注入 isAcquisitionBusy）+ 真实 ImagingController + 真实 ImagingSvc.exe 子进程。覆盖任务必测五类：
1. 正常成像运行中本地改变滤波参数被拒：配置/ready/使能/进程保持，后续有效块仍产生快照（T1a–T1d）；
2. 服务端 2014 经真实控制器 processMessage 路由 → svcConfigRejected，不进错误态，原服务继续处理（T2d–T2i；对照非 2014 错误仍走 svcError=T2g）；
3. 仅采集 busy、服务未运行：用户应用被拒、默认值不进采集实例（T3a–T3b2）；内部提交例外（成像启动）不受采集忙限制（T3c–T3d）；
4. 停止后同参数可应用（T4a/T4e）、运行中重复 startSvc 幂等不回退（T4c）、正常启动/停止不回退（T4d）；
5. 实际 svc 故障仍走原错误处理（T5：真实 CrashExit 语义驱动生产 onSvcFinished 槽 → svcError → onImagingError；Windows QProcess 对外部强杀报 NormalExit 的工具链行为以 gdb probe 实测记录于测试源码注释）。
3 次连续全过（37/37, exit=0），CTest 全量并发下亦通过（RUN_SERIAL 隔离真实子进程/端口 5555）。

## 7. GUI 验证范围

- **已验证（offscreen 自动化）**：T2/T3 共 45 断言——控件存在/出厂默认/开关联动（禁用+保留值）/fs 变化保值/越界应用拒/**defaults 六键持久化往返（真实 QSettings INI、设为默认→销毁→新对话框恢复）**/取消不改活动配置/合法配置字段到控制器/服务端忙时拒绝（§5.2 真实子进程）；T5 集成 37 断言——真实 MainWindow 生产连接下的拒绝分流/状态保持/持续处理（§6 T5）。
- **UNVERIFIED（需实机）**：真实屏幕像素渲染、真实鼠标完整人工操作流（双击/滚动/多显示器）。本机为自动化环境无真实显示交互，offscreen 自动化不冒充人工操作。
- 参数只在停止时应用：四层边界（UI 弹窗/控制器 isBusy 拒绝/服务端 2014 非致命分流/采集忙谓词拒绝）+ 自动重启链路移除；主窗口运行时禁用参数按钮的既有保护（setImagingParamControlsEnabled）不变，与本次补强互补（前者挡正常 UI 入口，后者挡应用边界与直调）。
- **S1 语义（B1 收口）**：配置拒绝 = 非致命提示（svcConfigRejected），不停止/不重启/不清队列、不动 ready/使能/轮次/馈送；服务实例与活动配置不变；只有真实故障走 onImagingError 原路径。用户"编辑并应用"与成像启动"内部提交"分开：前者受采集忙边界约束，后者使用已确认快照值且保留采集进行中启动实时成像的既有合法流程。

## 8. 交付（staging，真实路径）

- 构建：`cmd /c build_mingw_debug.cmd`（**configure + build 于最终生产源提交 C = d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697**，BuildIdentity 绑定核验）。
- 交付目录：`artifacts/build-delivery/20260921-014119_d8dd3da/`（真实路径；build-manifest.txt、git-receipt.txt、dependency-sha256.txt、file-sha256.txt、validation.txt、build.log、build-configure.log、bin/ 全量含 Qt runtime/plugins、diagnostic-tools/）。
- **C/R 提交关系**：生产/构建/测试输入全部在提交 C；本报告（R）为证据/文档提交——`git diff --name-only C R` 仅含 CODEX_REPORTS 与 TASKS 文档，**不声称 R 本身已重建**（R 若需交付须按 BUILD_STANDARD §2.3 在 R 上重新 configure+build）。
- 关键依赖 SHA256（完整值见交付目录 dependency-sha256.txt）：CUDA ABI 未变复用 `_migration_pack/prebuilt_cuda`：ring_recon_cuda.dll `bf40472d5a46363a35dd1084a01a8c15f13ef203f3ed5b4bb5edf767eeec14b5`、cudart64_12.dll `c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344`、cufft64_12.dll `2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590`、pa_recon_core.dll `c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893`、libzmq-v141-mt-4_3_5.dll `37610023d91951bc4177db1f96b54b28911976ac70b221a852887709a02a9ad3`（与 45cd050/777df4f 交付一致，逐字节同库）。
- BuildIdentity（configure 于 C，回执全文见 git-receipt.txt）：`PAIMAGE_GIT_SHA = d8dd3daf2f1bce3afdc01e0452e7ae487f3a1697`、`PAIMAGE_TRACKED_DIRTY = false`、`PAIMAGE_BUILD_TYPE = Debug`、`PAIMAGE_COMPILER = GNU 13.1.0`。
- 本机依赖 cufft64_12.dll 为 .gitignore 忽略文件，来源同 45cd050（标准 CUDA 12 工作区，SHA256 `2480d8ab...` 与交付 manifest 一致）。

## 9. 未验证项 / 限制

1. 实机真实成像质量：回放数据不证明真实采集卡/NIC/FPGA/图像质量（UNVERIFIED）。
2. GUI 实机人工操作流（§7）。
3. 真实触发节拍下的实际积压/丢块：无真实触发间隔数据，§5.3 之 2 仅为按假设节拍的推算。
4. **性能与实时支持范围（S2 修订，取代前版第 4 条）**：性能为 mingw-debug 未优化构建 + CPU 滤波，release 未测。**当前可验证使用范围**：回放链路（14.dat、8ch、sampDepth 4000、fs 250MHz）单块 avg_process 时延——空闲机上全关 9.1ms / 单滤波 59.1–59.7ms / HP+LP 105.2ms（90×90，block=40）、部署网格 98.0/197.3ms、block=100 HP+LP 263.3ms（§5.3.1 逐次原始记录）；这些数字**仅证明回放单块处理时延**。**实时性 UNVERIFIED**：同一口径两轮测量差异 2–4 倍（机器负载敏感）；无真实触发间隔数据；未做真实持续输入的积压/丢块实测。按 §5.3 表原数据（非空闲，HP+LP 409.2/656.3/1060.6ms）在其自身假设节拍下 HP+LP 超预算 89.2/336.3/260.6ms——前版算术错误已更正。**本任务不冒充实时通过**：软件正确性与性能准入由规划主代理分别判断；性能缺口如实呈报（工具链 debug 未优化 + CPU 滤波 + 测量负载敏感性），不把性能结论顺延 B2 却声称 B1 实时通过。
5. physical_round_normalizer_test 偶发 SEGFAULT：既有问题、根因未定位（§6 T4）；本轮首次捕获 gdb 栈与 140 次直接运行样本（12–45% 崩溃率），完整证据与复现命令见 stage-B1/test-account/。重复通过不证明根因已消失。
6. ImagingSvc 环境类失败（CUDA 创建失败 2006/SHM 附接失败 2007/头校验失败 2010）发生在状态提交之后的顺序与 45cd050 相同（基线行为保留），此类环境错误不属于配置校验范畴。

## 10. 回执核对命令

```powershell
git push origin HEAD:refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
git fetch origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754:refs/remotes/origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git rev-parse HEAD
git rev-parse origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git ls-remote origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
```

三处 SHA 一致后报告完成。不合并 main、不开始 B2。
