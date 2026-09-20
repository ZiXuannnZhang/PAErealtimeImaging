# 阶段 B1 执行报告：环形成像高低通零相位滤波实现

任务：`TASKS/阶段B1环形成像高低通零相位滤波实现_20260920-144420.md`（codex/task-docs 发布 SHA 见远端）
实现分支：`codex/ring-zero-phase-pa-inversion-20260919-025754`
阶段 A 已验收起点 SHA：`91ca68b36dac9c14ccd756c52896ea20ad794e0a`
B1 最终 SHA：见本提交（`git rev-parse HEAD`，本文档随最终提交入库）
阶段：**B1（高低通零相位滤波生产实现）**。B2（光声反演滤波与距离指数退役）未开始，未混入任何 B2 代码。

## 1. 改动文件（相对 MC_410T_MultiCard/delivery）

| 文件 | 改动 |
|---|---|
| src/RingRecon/zero_phase_filter.h/.cpp | **新增**独立数值模块：双精度 Butterworth SOS 设计（1–8 阶，与 MATLAB butter 逐位一致的数字化路径）+ 零相位前后向滤波（奇对称延拓 nfact=3n、逐节稳态初始化、DF2T）+ C2 规则表 + E 计算 |
| src/RingRecon/ring_recon.h/.cpp | `ZeroPhaseConfig` 并入 `PreprocessParams`；`preprocessBlock` 在 delayCut 后接入 HP→LP（全关不进分支，逐样本保持旧路径；失败抛 `std::runtime_error`） |
| src/RingRecon/ring_recon_cuda.h | reserved 字段 `filterLow/wLow/n1`（=高通）、`filterHigh/wHigh/n2`（=低通）注释启用为真实语义（无 ABI 改动，未动 .cu/kernel） |
| src/RingRecon/zpf_numeric_smoke.cpp | 开发期数值 smoke 工具（独立编译，不进 build） |
| src/ImagingSvc/ImagingSvc.h/.cpp | 服务端重复校验（有限性/范围/阶数/hp<lp/短线/逐启用通道×波长 C2）→ 一次设计缓存 `m_ringZeroPhase` → 逐线接入；配置拒绝 error 2012、运行时失败 error 2013（不发布看似正常的图像） |
| src/ImagingSvc/CMakeLists.txt | 编入 zero_phase_filter.cpp |
| src/ImagingController.cpp | ring JSON 增加 filterLow/wLow/n1/filterHigh/wHigh/n2 六键 |
| include/RingConfigDialog.h、src/RingConfigDialog.cpp | 预处理区新增"零相位滤波"分组：两独立开关 + MHz 截止 + 1–8 阶数 + 有效范围提示（真实 daqHz 动态上限）；开关联动禁用/保留值；applyConfig 前置校验（截止/hp<lp/C2 逐通道，含具体通道提示）；`设为默认/恢复默认`新增 6 键（旧配置缺键=关闭） |
| tests/ring_zero_phase_filter_test.cpp | 数值/配置/回归 31 断言（A–G 区），接入 CTest |
| tests/ring_config_zero_phase_dialog_test.cpp | Dialog 逻辑联动 20 断言（offscreen），接入 CTest |
| tests/CMakeLists.txt | 两个新测试目标 |
| src/RingReconCuda/ring_svc_selftest.cpp | `--zp/--zp-hp-mhz/--zp-lp-mhz/--zp-order/--delay-cut/--mask-len/--expect-config-reject`：四态全链对比与配置拒绝链路验证 |
| CODEX_REPORTS/.../stage-B1/ | 本报告、参考向量导出脚本与数据（见 §4） |

未改动：ring_das_kernel/snapshot（CUDA ABI 未变，`_migration_pack/prebuilt_cuda` 既有 DLL 继续使用）；采集/raw 格式/RoundIdentity/timeout/stale/save 链路零改动；距离指数 UI/defaults/后端保留（退役留 B2）。

## 2. 数值实现约定（与阶段 A 冻结约定逐条对齐）

- 运行时 fs（`RingConfigDialog::setAcquisitionParams` 真实 250MHz → JSON daqHz → 设计/校验全程使用，非固定常量）；Wn=2fc/fs；单程阶数 1–8，默认 4。
- SOS 设计路径与 MATLAB R2023a `butter` 逐位一致（本报告 §5 验证）：模拟原型极点 `exp(jπ(2k+n+1)/2n)` → 低通 `u·p`/高通 `u/p`（u=4·tan(πWn/2)）→ 双线性 fs=2 `z=(2+p/2)/(2−p/2)` → 增益 LP `Π(1−z_p)/2ⁿ`（DC=1）/ HP `Π(1+z_p)/2ⁿ`（Nyquist=1）按节开方分配。奇数阶实极点做一阶节（写成退化 SOS，b2=a2=0）。
- 零相位应用与 `refZeroPhase.m` 相同：奇对称延拓两端各 3n、逐节稳态初始化（DF2T 级联传递）、前向→翻转→前向→翻转、复合增益 g²（本实现通带归一 g=1）；线长≤3n 或非有限输入报错拒绝。
- 滤波内部全程 double，完成后转现有 float 输出；全关路径不进滤波分支，转换顺序与旧路径逐位一致（基线对照 §5）。
- 顺序：DBR置零 → 削顶 → delayCut → HP → LP，作用于当前完整输出线（裁剪线）；不搬入 MATLAB 历史切片。
- 设计缓存：服务层每配置一次（`designSet` 成功后缓存 `FilterSet`，`preprocessBlock` 经 `designedSet` 指针复用，不逐 A-line 重复设计）；无跨线/跨块状态。
- 每根完整 A-line 独立滤波，无跨通道/波长/触发/块状态；wl2 尾线缓存（`m_ringPrevWL2`）保存的是已预处理输出，跨块前插直接进 CUDA，不再次滤波（G1/G2 测试 + 代码结构核实）。

## 3. 配置有效性规则（C2 冻结规则表，服务端强制）

逐启用通道 c∈{0..7}×波长 w∈{1,2}（D=该通道/波长 sysDelay，E=实际置零数）：

```
HP/LP 均关：不引入新的滤波专属拒绝（保留值不校验，不阻塞旧路径）
任一滤波开，E=0：允许（DBR 关闭或有效置零 0）
任一滤波开，E>0，delayCut=true：仅 E<D 允许
任一滤波开，E>0，delayCut=false：本版明确拒绝（"未支持组合"文案）
```

E = `actualZeroRows(dbrOn, maskLength, extra_w, sampDepth)` = min(maskLength+extra_w, sampDepth)，下限 0；extra_w=0（wl1）/sysDelayCh[c][1]−sysDelayCh[c][0]（wl2）。

附加校验（仅启用滤波器）：截止有限且 0<fc<fs/2；双开 hpHz<lpHz；阶数 1–8；输出线长>3·max(n1,n2)。所有启用通道/波长均通过才应用；任一失败 `sendError(...,2012)` 整组拒绝并给出具体通道/波长/数值。UI（applyConfig）与服务端（processRingConfigure）各自独立校验，服务端不信任 UI。

## 4. 字段与 settings 映射

reserved 字段语义启用（无 ABI 改动）：
```
filterLow(1) / wLow(Hz) / n1      → 高通零相位（内部命名 highpass）
filterHigh(1) / wHigh(Hz) / n2    → 低通零相位（内部命名 lowpass）
```
JSON 键（ImagingController → ImagingSvc）同名六键；MHz 显示、Hz 传递（`RingConfigDialog::config()` 一次转换）。

QSettings `RingConfigDialog/Defaults` 新增键：`zpHp`(bool, false)、`zpHpMhz`(0.4)、`zpHpOrder`(4)、`zpLp`(false)、`zpLpMhz`(40)、`zpLpOrder`(4)。旧配置缺键 → 全关；关闭时数值输入禁用但保留值并随"设为默认"保存。采样率刷新（`setAcquisitionParams`）动态更新有效范围提示与 spin 上限，不静默改已存值；启用/应用前校验并提示（Dialog 测试 D13/D15 覆盖）。

## 5. 构建与测试证据

### 构建

- 入口：`cmd /c build_mingw_debug.cmd`（configure+build；Qt 6.8.0 mingw_64 / MinGW 13.1.0 / Ninja / CMake，与 BUILD_STANDARD 一致）
- 产物：`build/mingw_debug/bin/{PAimageReceiverDiagnostics,ImagingSvc,ring_svc_selftest,ring_udp_replay}.exe` 全部生成；windeployqt 完成（Qt runtime/plugins 就位）
- Ring CUDA：`_migration_pack/prebuilt_cuda` 既有已验证 DLL（ABI 未变不重编，见 §8 依赖哈希）
- 注：本机依赖 `libs/imaging/cufft64_12.dll` 为 .gitignore 忽略的本机文件（构建时从标准工作区 `D:/ChatGPT/PAERealtimeImaging` 复制，来源与哈希已记录于交付 manifest）

### CTest（tests 树独立配置 `cmake -S tests -B build/tests_all_mingw_debug`）

- 环境注记：本工作区绝对路径较长，MinGW depfile 在 260 字符限制下失败；用 `subst X:` 短路径映射完成测试树配置/构建/运行（构建产物同一物理目录，非代码改动）
- 命令：`ctest`（45 项）→ **100% passed, 0 failed**（45 = 既有 43 + 新增 2）
- 既有偶发注记：`physical_round_normalizer_test` 在单次全量并发时出现一次 SEGFAULT，单独重跑与后续全量重跑均通过（与本改动无关的测试环境偶发；未改动该测试及其源码）
- 新增测试详情：
  - `ring_zero_phase_filter_test`（31 断言）：A 设计系数（butter(2,0.5) 解析精确一致；fc 单程 −3dB；通带增益 1@阶数 1..8）；B 非法配置（n 越界/fc 越界/NaN/Inf/短窗/HP>LP）；C C2 规则矩阵（D∈{358,371}×E∈{0,D−1,D,D+1}×delayCut×filterEnabled 全组合）；D 与阶段 A 参考向量逐样本对照；E 零相位（fs∈{250,200}MHz×阶数{1,3,4,5,7,8} 互相关零位移）；F preprocessBlock 回归（全关逐样本=旧路径、滤波=先裁剪再滤、分块无关、delayCut 几何不变、E=0 允许）；G wl2 只滤一次语义
  - `ring_config_zero_phase_dialog_test`（20 断言，offscreen）：出厂默认/开关联动/值保留/MHz→Hz 单次转换/非法截止拒绝且不下发/C2 组合拒绝且不下发/合法应用字段到控制器

### 阶段 A 参考向量逐样本对照（D 区，`--ref stage-B1/matlab/reference_vectors`）

参考来源：阶段 A 已验收 `evidence/filter_reference_vectors.mat`（91ca68b，MATLAB R2023a 9.14.0.2206163 + butter/zp2sos/refZeroPhase），经 `stage-B1/matlab/export_zpf_reference.m` 导出 43 个紧凑文本向量（%.17g 逐行；README 记录来源 SHA/MATLAB 版本/约定；MATLAB 仅用于生成独立参考，非运行时依赖）。

| 案例 | HP maxAll | LP maxAll | 参考 scale | 判定 |
|---|---|---|---|---|
| dc | 0.000e+00 | 2.776e-16 | 0.5 | PASS |
| sine_in | 2.519e-13 | 9.992e-16 | 1.42 | PASS |
| sine_out | 9.258e-14 | 8.882e-16 | 1 | PASS |
| multitone | 3.290e-13 | 1.554e-15 | 1.82/1.58 | PASS |
| pulse | 1.149e-14 | 1.665e-16 | 0.997/0.324 | PASS |
| burst | 1.274e-10 | 1.137e-12 | 2.13e3/1.84e3 | PASS |
| noise | 5.210e-14 | 1.943e-16 | 0.391/0.191 | PASS |

容差：全段相对 1e-6（全部案例实测 ≤1.3e-10，burst 相对差 6e-14）；DC 近零用绝对尺度对照（不做失真相对除法）。端点区（nfact 区间）与参考逐样本一致（无跳过），已按阶段 A 同约定核对。

### 全链路与基线回归（ring_svc_selftest，真实 14.dat）

- 数据：`D:/ChatGPT/PAERealtimeImaging/testdata/14.dat`（265,600,000 字节，SHA256 `fbcbc105343d8caf8d1b231a93cf00cdbf4710f94a19b93b9f1a86f812e9e056`，8000 A-line×4000 samp double）
- 命令：`ring_svc_selftest --data 14.dat --svc ImagingSvc.exe --grid-mm 0.4 --block 40 --channels 255 --id 14 [--zp 0|1|2|3]`
- **全关 vs 基线 91ca68b**：分别构建 91ca68b worktree 的 ImagingSvc，同数据同配置跑全关，100 个输出帧（wl1+wl2 各 50）与基线**逐字节一致**（cmp same=100, diff=0）——关闭路径完全保持旧行为
- **确定性**：同配置两次运行输出逐字节一致
- **滤波生效**：mode3 vs mode0 同名帧 28243/32400 字节不同
- **配置拒绝链路**（`--expect-config-reject 1`）实测 4 类全部 PASS：
  - C2 未裁剪组合（zp1+delayCut0+DBR on）：`零相位滤波与未裁剪的 DBR 置零前缀不兼容…（通道1/波长1：E=300>0、延时裁剪关闭）`
  - E≥D（mask-len 400）：`DBR 置零末端必须早于延时裁剪起点（通道1/波长1：E=400 ≥ D=358…）`
  - 截止越界（HP 200MHz）：`高通截止 200000000 Hz 超出有效范围 (0, 125000000)`
  - HP>LP（50M/10M）：`高通截止 50000000 Hz 必须低于低通截止 10000000 Hz`
- **合法配置**（E=300<358 默认）四态全部正常出图（round barrier/完成语义无异常，`consumed=50 mismatch=0`）

## 6. 性能四态实测（同机器/同数据/同网格，CPU 滤波）

grid 0.4mm（nx=90），sampDepth=4000，8 通道，250MHz：

| 状态 | block=40/通道（80 触发/块，1600 线） | block=100/通道（200 触发/块，4000 线） |
|---|---|---|
| 全关 | avg_process ≈ 9.5 ms/块 | ≈ 25.9 ms/块 |
| 仅 HP (0.4M,4阶) | ≈ 58.9 ms/块 | — |
| 仅 LP (40M,4阶) | ≈ 58.8 ms/块 | — |
| HP+LP | ≈ 104 ms/块 | ≈ 964.8 ms/块 |

- 每线滤波增量 ≈ 0.55–0.60 ms（HP 或 LP 单个）；线性无异常放大
- 处理节拍：块间隔由上游触发决定（40 触发/通道/块 ≈ 320ms 采集）；全关 9.5ms 远低于节拍无积压；HP+LP 104ms 亦低于节拍
- 压力态（block=100/通道）：采集 200 触发/通道/块 ≈ 800ms，HP+LP 处理 964ms > 采集节拍——**该压力配置下 HP+LP 全开时队列将缓慢积压**（每块落后 ~165ms）；单滤波器开或块=40 配置可实时。如实记录为 CPU 实现边界；按任务要求不提前引入 GPU 滤波算法（B2/后续任务决策）
- 丢块/观察链路：四态 `ring_shm_obs` 无 anomaly、无 stale/duplicate（与本改动无关联的既有语义保持）

## 7. GUI 验证范围

- **已验证（offscreen 自动化）**：控件存在/出厂默认值/开关联动（禁用+保留值）/MHz→Hz 单次转换/非法参数弹窗且拒绝下发/C2 组合弹窗且拒绝下发/合法配置字段正确到达 ImagingController（20 断言全过）
- **UNVERIFIED（需实机）**：真实屏幕像素渲染、真实鼠标操作"设为默认→重启→恢复默认"持久化往返、取消对话框不改变活动配置、运行中(忙时)应用被 UI 拦截的完整人工操作流。本机为自动化环境无真实显示交互，按任务要求单列不冒充。参数只在采集/成像停止时应用的机制：成像运行中修改环形参数走既有 `ringConfigChangedWhileRunning → 自动重启子进程`链路（本任务未改动该链路；滤波配置遵循同一机制，与其它 ring 参数一致）

## 8. 交付（staging）

交付目录：`artifacts/build-delivery/<timestamp>_<short-sha>/`（提交后生成，manifest/hash 见该目录 `build-manifest.txt`、`dependency-sha256.txt`、`file-sha256.txt`、`git-receipt.txt`、`validation.txt`）

关键依赖 SHA256（mingw-debug bin）：

```
ring_recon_cuda.dll  bf40472d5a46363a35dd1084a01a8c15f13ef203f3ed5b4bb5edf767eeec14b5
cudart64_12.dll      c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344
cufft64_12.dll       2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590
pa_recon_core.dll    c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893
libzmq-v141-mt-4_3_5.dll（交付目录内 file-sha256.txt 全列）
```

CUDA ABI 未变：ring_das_kernel/snapshot 未改动，`_migration_pack/prebuilt_cuda` 既有已验证 DLL 直接复用（来源、哈希、复用理由已记录于 manifest）。

## 9. 未验证项 / 限制

1. 实机真实成像质量：无真实设备/采集（回放为 14.dat 离线数据），滤波前后图像"改善/劣化"结论 UNVERIFIED（B1 只验收正确实现，不要求图像指标提升）
2. GUI 实机人工操作流（§7 UNVERIFIED 清单）
3. 极端合法频率（如 fc→fs/2⁻、fc→0⁺ 的 8 阶设计）数值支持边界：设计端做有限性+通带增益校验并明确报错，未逐一扫频验证所有极端组合的系数条件数（非法/病态被拒绝而非静默）
4. 压力配置 HP+LP 全开的实时性（§6）：块=100/通道时 CPU 滤波略慢于采集节拍；不提前引入 GPU 算法（任务禁止）
5. 双声速/DBR/timeout/RoundIdentity/save：行为未改动，回归证据为全关路径逐位一致 + 全部既有测试通过；未新增这些链路的专项测试（不属 B1 范围）

## 10. 回执核对命令

```powershell
git push origin HEAD:refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
git fetch origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754:refs/remotes/origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git rev-parse HEAD
git rev-parse origin/codex/ring-zero-phase-pa-inversion-20260919-025754
git ls-remote origin refs/heads/codex/ring-zero-phase-pa-inversion-20260919-025754
```

三处 SHA 一致后报告完成。不合并 main、不开始 B2。
