# 阶段 B1 执行报告：环形成像高低通零相位滤波实现（B1 整改后）

任务：`TASKS/阶段B1环形成像高低通零相位滤波实现_20260920-144420.md`
整改任务：`TASKS/阶段B1审查整改配置安全与交付证据_20260920-184529.md`（R1–R5）
实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
阶段 A 已验收起点 SHA：`91ca68b36dac9c14ccd756c52896ea20ad794e0a`
B1 原生产提交：`45cd0502fcf8ca68a73b62dd2beca80b4c88d25c`
B1 整改最终 SHA：见本提交（`git rev-parse HEAD`，本文档随最终提交入库）
阶段：**B1 整改完成（R1–R5）**。B2（光声反演滤波与距离指数退役）未开始。

## 0. 整改摘要（R1–R5 对照）

| 项 | 审查发现 | 整改位置 | 验证 |
|---|---|---|---|
| R1 短线校验只用 ch1/wl1 | `ImagingSvc.cpp` 短线校验改为逐启用通道×波长（与运行时 outRows 同式），延时解析延迟到校验通过后提交 | `src/ImagingSvc/ImagingSvc.cpp`（C2/短线区 + 提交区）、`src/RingConfigDialog.cpp` applyConfig UI 前置同规则 | 服务端实测拒绝含通道/波长/D/Nt（§5）；UI 正负例（§6 T2） |
| R2 Nyquist 动态 setMaximum 静默压值 | 删除 setMaximum 动态改上限；保留 0.0001–500 稳定范围 + 动态提示；应用时校验 | `src/RingConfigDialog.cpp` setAcquisitionParams | fs 变化保值/越界应用拒/恢复后原值在（§6 T2） |
| R3 应用边界无忙时拒绝 | `ImagingController::configureRing` 返回 bool、忙时拒绝；svc 端 m_running 时 configure 拒绝（error 2014）；移除"自动重启"链路 | `include/ImagingController.h`、`src/ImagingController.cpp`、`src/ImagingSvc/ImagingSvc.cpp`、`src/MainWindow.cpp`（启动路径处理拒绝）、`include/MainWindow.h`（移除 m_restartRingOnSvcStop） | svc 实测运行中再配置被拒（§5）；停止态可正常应用（§6 T2 R3a/R3b） |
| R4 参考缺失仍 PASS | D 区参考对照强制执行：目录/文件缺失/长度错误非零失败；对照数量守门（14 文件） | `tests/ring_zero_phase_filter_test.cpp`（D0/D2）、`tools/prepare_zpf_reference.cpp` + `tests/CMakeLists.txt`（干净 checkout 自动再生成） | 正例 43 向量全过 + 工具输出与 MATLAB 导出逐字节一致；负例 3 类非零退出（§6 T1） |
| R5 交付占位/性能口径不清 | 本报告全部真实路径与命令；性能统一口径复测（预热+3 次取中位），澄清 A-line 计数 | 本报告 §5–§7；交付目录 §8 | 见下文 |

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
| tests/CMakeLists.txt | prepare_zpf_reference 目标（构建时从阶段 A MAT 证据自动生成参考向量，无 MATLAB 运行时依赖）+ ring_config_busy_reject_test 目标 |

未改动：zero_phase_filter.cpp/ring_recon.cpp 数值核心（阶段 A 参考对照逐样本通过——见 §6 T1）；ring_das_kernel/snapshot（CUDA ABI 未变，`_migration_pack/prebuilt_cuda` 既有 DLL 继续使用）；采集/raw 格式/RoundIdentity/timeout/stale/save 链路。

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

说明与澄清（对应 R5 审查项）：

1. **每线滤波增量**：block=40 仅 HP 196.5ms/320 线 ≈ 0.61 ms/线；仅 LP 0.62 ms/线；HP+LP 409ms ≈ 1.28 ms/线（两滤波叠加）——单滤波每线增量与 45cd050 报告的 0.55–0.60 ms 一致。原报告"HP+LP 104ms"与其自身每线数据（320 线 × 2×0.58 ≈ 371ms）不符：经本次复测证实 104ms **不能**在 320 线/块口径下复现（本机三次中位 409ms）。原 104ms 的测量条件已无法追溯（疑为线数口径混用或包含了部分预热块），如实标记为**口径不明、不可复现**；104ms→965ms 的"线性"结论随之作废。以本次统一口径数据为准：800 线 vs 320 线，HP+LP 1060.6/409.2 = 2.59（A-line 比 2.5，考虑每块固定开销后基本线性）。
2. **块节拍与积压（预计，非实测）**：真实触发节拍取决于 FPGA/采集（本仓库回放链路无真实触发间隔数据）。按 45cd050 报告引用的节拍假设（block=40/通道 ≈ 320ms 采集/块）：关 13.2ms、单滤波 ~200ms、HP+LP 409ms 均低于节拍；block=100/通道（~800ms 采集/块）时 HP+LP 1060.6ms **预计**超出节拍、缓慢积压（每块落后约 260ms）。此为预计——未做真实节拍对齐的实测，`slot_busy_before_submit=0` 仅说明本次回放（同步等快照）无提交冲突，不构成"无丢块"证明。
3. **部署网格**：默认 grid 0.01mm（nx=3600）下全关 183.8ms/块——重建/快照成本主导（90×90 时仅 13.2ms），HP+LP 656.3ms/块。该网格×block=40 节拍假设下关/滤波均**预计**可跟（<320ms 假设节拍）；更高块或滤波组合未逐一扫参。
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

### T4 既有回归（受接口变更影响面：ImagingController/MainWindow/ring JSON）

45 项 + 新增 2 项 = 47 项 CTest 全量：44 项通过；`physical_round_normalizer_test` 在全量并发时 1 次 SEGFAULT（见下），单独重跑 8/8 通过。整改期间该测试失败记录与重跑记录如实附上：
- 全量 ctest（整改构建）：`physical_round_normalizer_test` SEGFAULT 0.33s；
- 单独重跑 8 次：全部 PASS；
- 该测试源码（tests/paimage_core/physical_round_normalizer_test.cpp）与被测 PhysicalRoundNormalizer.cpp 本次**零改动**；ImagingController/MainWindow 的 R3 改动不在其链接单元内。该 SEGFAULT 与 45cd050 报告记录的偶发一致（同环境同现象），本次无法归因到整改改动；不声明"无关"——如实标记为**既有环境偶发，未定位根因**。若后续可复现稳定化，另行处理。

## 7. GUI 验证范围

- **已验证（offscreen 自动化，T2/T3，45 断言）**：控件存在/出厂默认/开关联动（禁用+保留值）/fs 变化保值/越界应用拒/**defaults 六键持久化往返（真实 QSettings INI、设为默认→销毁→新对话框恢复）**/取消不改活动配置/合法配置字段到控制器/服务端忙时拒绝（§5.2 真实子进程）。
- **UNVERIFIED（需实机）**：真实屏幕像素渲染、真实鼠标完整人工操作流（双击/滚动/多显示器）。本机为自动化环境无真实显示交互，单列不冒充。
- 参数只在停止时应用：三层边界（UI 弹窗/控制器 isBusy 拒绝/服务端 2014）+ 自动重启链路移除；主窗口运行时禁用参数按钮的既有保护（setImagingParamControlsEnabled）不变，与本次补强互补（前者挡正常 UI 入口，后者挡应用边界与直调）。

## 8. 交付（staging，真实路径）

- 构建：`cmd /c build_mingw_debug.cmd`（configure+build 于最终提交，见 §10 回执核对命令）。
- 交付目录：`artifacts/build-delivery/<timestamp>_<short-sha>/`（提交后生成；manifest/hash 见该目录 build-manifest.txt、dependency-sha256.txt、file-sha256.txt、git-receipt.txt、validation.txt、build.log）。
- 关键依赖 SHA256 与 45cd050 交付一致（CUDA ABI 未变复用 `_migration_pack/prebuilt_cuda`：ring_recon_cuda.dll `bf40472d...`、cudart64_12.dll `c2c9a9c2...`、cufft64_12.dll `2480d8ab...`、pa_recon_core.dll `c1a37e73...`，全列于交付目录 file-sha256.txt）。
- BuildIdentity：最终提交上重新 configure，产物 `PAIMAGE_GIT_SHA` = 最终 SHA、`PAIMAGE_TRACKED_DIRTY=false`（构建回执记录）。
- 本机依赖 cufft64_12.dll 为 .gitignore 忽略文件，来源同 45cd050（标准工作区，SHA256 `2480d8ab...` 与交付 manifest 一致）。

## 9. 未验证项 / 限制

1. 实机真实成像质量：回放数据不证明真实采集卡/NIC/FPGA/图像质量（UNVERIFIED）。
2. GUI 实机人工操作流（§7）。
3. 真实触发节拍下的实际积压/丢块：无真实触发间隔数据，§5.3 之 2 仅为预计。
4. 性能为 mingw-debug 未优化构建 + CPU 滤波：debug 构建含断言/零优化，数值与路径相同；release 性能未测（B2/后续任务决策时按需）。压力配置（block=100 HP+LP、部署网格大块）实时性受限如实交付，支持范围：默认网格 block=40 四态预计可跟节拍（§5.3 假设下）。
5. physical_round_normalizer_test 全量并发 SEGFAULT：既有偶发未定位（§6 T4），单独重跑通过。
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
