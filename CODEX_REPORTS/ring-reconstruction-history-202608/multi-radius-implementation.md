# 多通道传感器配准重建（多扫描半径）实现方案

> 状态：方案已确认 → 已实施 → 已验收（提交 `7f13256`）
> 关联文档：`HANDOFF_接力手册.md`（沙箱/构建注意事项）、`环扫新链路重构设计.md`（链路基线）
> 本文档前半为确认后的实现方案（4.1→4.6），后半为实际已实施的改动对照与验证记录。

---

## 一、背景与目标

### 1.1 背景

环形扫描相干成像采用 8 通道传感器阵列，将单圈物理扫描路径缩短为 1/8，从而成倍提升成像速度。
但由于装配公差，各通道传感器的**实际旋转半径互不相同**（毫米级以下偏差）。当前程序对全部
A-line 使用同一个重建半径（6.57 mm），导致各通道探测器几何位置偏离实际位置，相干合成
（DAS 反投影）质量下降。

### 1.2 目标

- 每个通道使用**各自的重建半径**参与 DAS 反投影（逐 A-line 半径）。
- 不勾选配准模式时，行为与旧版完全一致（全部通道统一半径），且**数值逐字节不变**。
- 改动范围：环形扫描分支（对话框 → 配置 → JSON → svc → CUDA），不触碰线性模式。

### 1.3 参考实现

线性扫描"成像参数配置"对话框中"光纤复用（单光纤直连 / 配准校准）"：每通道时延数组
`caliCardDelay/caliFiberDelay`（逗号分隔、长度=通道数）→ 对话框 → `m_imagingReconParams`
→ ImagingController JSON 数组 → ImagingSvc → `pa_recon` C 库。本方案复用同一模式：
**每通道数组 + 勾选使能联动 + JSON 数组下发 + 未配置回退**。

---

## 二、现状分析（统一半径数据流）

```
RingConfigDialog.config()  →  RingReconCudaConfig.radius（单一值）
  →  ImagingController sendConfigureAndStart()：ring["radius"]（JSON 单值）
  →  ImagingSvc processRingConfigure()：cfg.radius
  →  ring_recon_cuda_create()：h->R = (float)cfg.radius；h->R2 = h->R * h->R
  →  内核 ring_das_kernel(..., float R, float R2, ..., const int* sIn, ...)
      host 侧 detx/dety 按统一 R 计算；sIn[bi] = (R2 <= rb2)（分层声速判断）
```

SHM 每根 A-line 已携带物理通道号（`ch[]`，uint8，0..7），但 svc 在 wl1/wl2 展平时将其丢弃，
未用于几何参数。这是多半径改造的入口。

---

## 三、方案总览

| 步骤 | 模块 | 改动 |
|---|---|---|
| 4.1 | 对话框 `RingConfigDialog` | "扫描参数"页加配准模式勾选框 + 8 个每通道半径输入；删"重建参数"页 Radius |
| 4.2 | `ring_recon_cuda.h` | config 加 `radiusPerChannel[8]` + `multiRadius`；新 API `append_angles_radii` |
| 4.3 | `ImagingController` / `ImagingSvc` | JSON 下发/解析 `multiRadius` + `radiusPerChannel` |
| 4.4 | `ring_recon_cuda.cu` | 内核逐线半径 `Rj/Rj²`；删除 `sIn`；新 API 实现 |
| 4.5 | `ImagingSvc` | 按 SHM 通道号构造逐 A-line 半径向量（含 wl2 跨块对齐） |
| 4.6 | 构建与验证 | CUDA DLL 直调重建、导入库重生成、MinGW 全量、数值 A/B |

---

## 四、分步方案

### 4.1 前端对话框（RingConfigDialog）

- 环形扫描参数设定 → "扫描参数"页：
  - "启用通道"复选框行**上方**新增 **"配准模式（多扫描半径重建）"** 勾选框
    （tooltip：勾选=各通道按各自半径重建；不勾选=全部通道统一使用通道1下方输入的重建半径）。
  - "启用通道"复选框行**下方**新增 **8 个"通道N重建半径"** 输入
    （QDoubleSpinBox，0.1~50 mm，4 位小数，默认 6.57，后缀 " mm"）。
- 使能联动 `syncRadiusEditable`：
  - 勾选配准模式 → 通道半径输入随该通道勾选状态可编辑；
  - 不勾选配准模式 → 仅通道1可编辑，通道2~8 只读（统一使用通道1半径）。
- "重建参数"页原 Radius 输入**删除**。
- 持久化：`saveDefaults` 存 `multiRadius` + `radiusCh0..7`（不再写旧键 `radiusMm`）；
  `restoreDefaults` 读取新键，旧键 `radiusMm` 一次性迁移为通道1默认值。
- `config()`：`cfg.multiRadius`；`cfg.radiusPerChannel[c] = (multiRadius ? 通道c输入 : 通道1输入) × 1e-3`
  （mm→m）；兼容字段 `cfg.radius = cfg.radiusPerChannel[0]`。

### 4.2 配置结构与 API（ring_recon_cuda.h）

```c
typedef struct RingReconCudaConfig {
    ...
    double radius;
    double radiusPerChannel[8];  // 每通道重建半径(m)；multiRadius=0 时全部=radiusPerChannel[0]
    int    multiRadius;          // 1=各通道按各自半径重建；0=统一半径
    ...
} RingReconCudaConfig;

RING_RECON_CUDA_API int ring_recon_cuda_append_angles_radii(
    void* handle, const float* bscan, int nt, int nd,
    const float* thetaDeg, const float* radii);   // radii 长度 nd，单位米；可为 NULL（统一半径）
```

`set_defaults` 中 `radiusPerChannel[i]=6.57e-3`、`multiRadius=0`。

### 4.3 JSON 下发链路

- `ImagingController::sendConfigureAndStart()`：`ring["multiRadius"]`、
  `ring["radiusPerChannel"]`（8 元素数组）。
- `ImagingSvc::processRingConfigure()`：解析数组（元素>0 才覆盖默认值）；
  **`!multiRadius` 时把全部 `radiusPerChannel[i]` 回填为 `cfg.radius`**
  （旧配置/无数组时保证统一半径语义与数值不变）。

### 4.4 CUDA 内核逐线半径（ring_recon_cuda.cu）

内核签名变化：标量参数 `float R, float R2` 与 `const int* sIn` 替换为
逐线半径数组 `const float* __restrict__ radius`。

| 量 | 旧（统一半径） | 新（逐线半径） |
|---|---|---|
| 探测器位置（host） | `detx=R*cos, dety=R*sin` | `Rj = radii ? radii[j] : h->R`；`detx[j]=Rj*cos, dety[j]=Rj*sin` |
| 半径平方 | `R2`（host 预计算） | `R2j = Rj * Rj`（核内逐线） |
| 投影点积 | `dotp = proj - R2` | `dotp = proj - R2j` |
| 距离平方 | `dist2 = (r2-R2) - 2*dotp`（`r2mR2` 循环外预计算） | `dist2 = (r2-R2j) - 2*dotp` |
| 分层声速 in-判定 | `bothIn = sIn[bi] && r2<=rb2`（sIn 由 host 算） | `bothIn = (R2j<=rb2) && (r2<=rb2)`（核内直算） |
| 判别式 | `discr4 = dotp² - dist2*(R2-rb2)` | `discr4 = dotp² - dist2*(R2j-rb2)` |
| DAS 权重 | `w = wscale[j]*dotp/(R*dsafe^p)` | `w = wscale[j]*dotp/(Rj*dsafe^p)` |

配套改动：

- `Handle`：删除 `sIn[8]`/`d_sIn`，新增 `float* d_radius`（容量随 `d_detCap` 一起扩容）。
- `create()`：删除 host `sIn` 计算与 `d_sIn` 分配/上传。
- `destroy()`：删除 `d_sIn` 释放，新增 `d_radius` 释放。
- `appendImpl` 增加 `radii` 参数：构建 `rad[]` 向量（radii 为空时全部填 `h->R`），
  上传 `d_radius`，内核启动时传入。
- 新导出 API `ring_recon_cuda_append_angles_radii`；原 `append/append_angles` 传 `nullptr`
  走统一半径路径。

**逐字节一致性论证**（统一半径路径与旧版必须 bit-identical）：

1. `radii==nullptr` → `rad[j] = h->R`，与旧 host `detx=R*cos` 用的同一个 float 值。
2. 核内 `R2j=Rj*Rj` 与旧 host `h->R2=h->R*h->R` 均为 IEEE-754 单精度 round-to-nearest
   乘法，结果逐位相同。
3. 其余算式操作数逐位相同、运算顺序相同（`r2mR2` 移入循环内不影响结果）。
4. `bothIn`：host `sIn[bi]=(h->R2<=rb2)` 与核内 `(R2j<=rb2)` 比较的数值逐位相同。

### 4.5 svc 逐 A-line 半径（ImagingSvc）

`processRingPulse()` 中，与 `wlAng/wlData` 展平**同序**构造半径向量：

- 通道 c 的第 j 根 A-line：`pos = j*M + selIdx[c]`，物理通道号 `phCh = ch[pos]`，
  半径 `rads[w].push_back(radiusPerChannel[phCh])`（`phCh` 不在 0..7 时回退通道0半径）。
- wl2 跨块对齐（`shiftWL2 && blockIndex>0`）：与 A-line/角度同步
  `rads[1].insert(begin, m_ringPrevRadius[c]); rads[1].pop_back();`
  末根存入 `m_ringPrevRadius[c]`（新增成员，`start/configure/resetRingRecon` 三处复位）。
- 展平后按波长调用 `ring_recon_cuda_append_angles_radii(m_ringCuda[w], flat, nt, nd,
  wlAng[w], wlRad[w])`。

### 4.6 构建与验证

- CUDA DLL：沙箱 ninja 不可用 → 从 `build.ninja` 提取 nvcc/link 命令，
  `vcvars64.bat` 环境内直调（sm_89，MSVC host 编译器）。
- 导入库：`gendef` + `dlltool` 重生成 `libring_recon_cuda.dll.a`。
- **ABI 同步**：config 结构变化 → DLL、ImagingSvc、MC410T_Receiver、selftest、
  MSVC verify 全部同步重建。
- 验证矩阵（详见后半"实际验证结果"）：
  1. 内核 A/B：与改动前内核逐字节对比（统一半径，单声速 + 分层声速）；
  2. 新 API 自洽：radii 全相等 ≡ 统一半径（逐字节）；radii 逐线变化结果有限且不同；
  3. 真实数据端到端：14.dat 新旧 verify 输出逐字节对比 + snapshot/reset 自检；
  4. svc 端到端：统一半径与多半径各跑一轮，输出无 NaN/Inf、两者有差异。

---

## 五、关键设计决策

1. **radius 兼容字段 = 通道1半径**：`radiusPerChannel[0]` 始终与 `radius` 一致，
   旧代码/旧配置读 `radius` 行为不变。
2. **未配准时回填**：svc 解析 `!multiRadius` 时强制 `radiusPerChannel[i]=cfg.radius`，
   即使 JSON 只下发旧字段也得到统一半径数值。
3. **统一半径路径不引入分支代价**：`radii==nullptr` 仅影响 host 填充，内核逐线读
   `d_radius`（全相等）与旧标量路径运算完全同值同序，保证逐字节一致。
4. **删除 sIn**：`bothIn` 改核内 `(R2j<=rb2)` 直算，消除 host/device 双份状态，且与旧值逐位相同。
5. **不改协议/采集**：多半径仅是重建侧几何参数，SHM 布局与 ZMQ 协议不变。

---

## 六、风险与注意事项

- **结构体 ABI**：`RingReconCudaConfig` 增字段后任何旧二进制（DLL/exe 混用）都会因
  `set_defaults` 的 `memset sizeof(*cfg)` 不一致导致越界写/未初始化 → 必须全量同步重建。
- 每通道半径范围 0.1~50 mm 由 UI 约束；数值异常（0/负）在 svc 解析时以正数校验兜底。
- 用户约束：不触碰线性模式；不改原程序日志反馈文本；改动前复核影响面。

---
---

# 实际已实施的改动（方案落地对照）

## 七、落地对照（4.1→4.6，提交 `7f13256`）

| 步骤 | 方案要点 | 实际落地 | 一致性 |
|---|---|---|---|
| 4.1 | 配准模式勾选框置"启用通道"上方；8 个每通道半径输入置其下方；勾选联动使能；删 Radius 输入 | `RingConfigDialog.h/.cpp`：`m_chkRegister` + `m_spnRadiusCh[8]` + `syncRadiusEditable` lambda（reg ? 通道勾选 : 仅通道0）；重建参数页 Radius 行已删 | ✅ 按方案 |
| 4.1 | 持久化与旧键迁移 | `saveDefaults` 存 `multiRadius/radiusCh0..7`；`restoreDefaults` 读新键，`radiusMm` 迁移为通道1默认值；`config()` 统一路径取 `m_spnRadiusCh[0]`，`radius`=通道1半径 | ✅ 按方案 |
| 4.2 | config + API | `ring_recon_cuda.h`：`radiusPerChannel[8]`（radius 之后）+ `multiRadius`；`set_defaults` 填 6.57e-3/0；新 API 声明 | ✅ 按方案 |
| 4.3 | JSON 下发/解析 | `ImagingController.cpp`：`ring["multiRadius"]` + `ring["radiusPerChannel"]` 数组；`ImagingSvc.cpp`：解析数组（>0 覆盖），`!multiRadius` 全部回填 `cfg.radius` | ✅ 按方案 |
| 4.4 | 内核逐线半径 + 删 sIn | `ring_recon_cuda.cu`：`ring_das_kernel` 参数改 `const float* radius`（删 R/R2/sIn）；核内 `R2j=Rj*Rj`、`dotp=proj-R2j`、`dist2=(r2-R2j)-2*dotp`、`bothIn=(R2j<=rb2)&&(r2<=rb2)`、`w=dotp/(Rj*dsafe^p)`；`Handle` 删 sIn/d_sIn 增 d_radius；`create/destroy/appendImpl` 配套；`append_angles_radii` 实现（radii=nullptr → 全填 h->R） | ✅ 按方案 |
| 4.5 | svc 逐 A-line 半径（含 wl2 对齐） | `ImagingSvc.h/.cpp`：`wlRad[2]/rads[2]` 与 angs 同序构造（`ch[pos]` → `radiusPerChannel`，越界回退通道0）；wl2 前插/丢弃用 `m_ringPrevRadius[8]`（start/configure/resetRingRecon 三处复位）；调用 `append_angles_radii` | ✅ 按方案 |
| 4.6 | 构建与验证 | nvcc/link 直调重建 DLL → gendef/dlltool 导入库 → mingw32-make 全量 → MSVC verify 重建 → 验证矩阵全部通过（见第九节） | ✅ 按方案 |

## 八、实施中新增/调整项（方案外的补充）

1. **selftest 增强**（`ring_svc_selftest.cpp`，随本次提交）：
   - `--no-launch 1`：沙箱内 QProcess 无法启动子进程（`CreateFile 拒绝访问`），
     svc 改由外部后台任务先启动，selftest 只连 ZMQ；bind 后等待 1.5s 让外部 svc 完成
     ZMQ 重连，否则连接建立前的 configure 被丢弃（表现为 `timeout at block 0`）。
   - `--radius-per-ch "6.57,6.55,..."`（毫米）：下发 `multiRadius=1` + 每通道半径数组，
     支持多半径端到端验证；空=统一半径。
2. **A/B 基准调整**：原计划用 08-08 备份 DLL 对比，但该 DLL 早于 08-12 内核提交
   （snapshot/reset、双声速模型），不能作基准 → 改为从 git 提取改动前源码
   （`36c5a3f` 的 `.cu/.h/.cpp`）以相同 nvcc 标志编译参考 DLL（`build/ring_recon_cuda/ab_ref_src/`），
   另写 A/B 测试程序 `build/ring_recon_cuda/ab_multir_radius_test.cpp`（LoadLibrary 双 DLL 对比）。
3. **测试数据**：原验证数据不在工作区，用户将 14.dat 提供至工作区根
   （265,600,000 字节 = 8300 列 × 4000 采样 × float64 列主序，未跟踪入库）。
4. **旧设置键迁移**（4.1 内）：`radiusMm` → 通道1默认值，一次性兼容历史注册表。

## 九、实际验证结果

### 9.1 内核 A/B 逐字节（合成数据，57600 像素/场景）

| 场景 | 对比 | 结果 |
|---|---|---|
| 单声速，统一半径 | 旧参考 DLL vs 新 DLL `append_angles` | acc/accW **逐字节一致** ✅ |
| 单声速，radii 全相等 | 新 DLL `append_angles` vs `append_angles_radii(全6.57e-3)` | acc/accW **逐字节一致** ✅ |
| 单声速，radii 逐线变化（±0.15mm/8 通道阶梯） | 新 DLL 多半径 | 有限（nonFinite=0）、与统一半径差异显著 ✅ |
| 分层声速（边界 3mm，1490/1540 m/s），统一半径 | 旧参考 DLL vs 新 DLL | acc/accW **逐字节一致** ✅（sIn→bothIn 改动验证） |
| 分层声速 + radii 逐线变化 | 新 DLL | 有限、与统一半径差异显著 ✅ |

### 9.2 真实数据端到端（14.dat，360×360，40 块）

- 新 DLL `ring_recon_cuda_verify` 与旧内核 `verify_old`（链接参考 DLL）输出对比：
  **242 个文件（40 块 × 2 波长 × 3 类 + grid）全部逐字节一致** ✅
- snapshot == get_state 归一化（worst diff = 0）、reset 全零自检通过 ✅
- 耗时：40 块约 2s（avg 49ms/块），与旧内核同量级 ✅

### 9.3 svc 端到端（8 通道、2 圈、10 块，`--no-launch 1`）

- 统一半径：10 块全部跑通，逐块显示帧与上块差异正常 ✅
- 多半径（`--radius-per-ch "6.57,6.55,6.59,6.53,6.61,6.56,6.58,6.54"`）：
  10 块跑通；与统一半径输出对比差异显著（wl1 maxDiff 810~2038、wl2 417~890），
  **无 NaN/Inf** ✅——证明逐通道半径真实进入重建。

### 9.4 实机验收

用户已实机验收配准模式效果 ✅（2026-08-15）。

## 十、提交与文件清单

- 提交 `7f13256`（8 文件，+161/-42）：
  `RingConfigDialog.h/.cpp`、`ImagingController.cpp`、`ImagingSvc.h/.cpp`、
  `ring_recon_cuda.h/.cu`、`ring_svc_selftest.cpp`
- 验证产物（不入库）：`build/ring_recon_cuda/ab_ref_src/`（旧内核参考 DLL/源码）、
  `build/ring_recon_cuda/ab_multir_radius_test.{cpp,exe}`、
  `build/verify_out14_old|new/`、`build/svc_out14_uni|multi/`。
