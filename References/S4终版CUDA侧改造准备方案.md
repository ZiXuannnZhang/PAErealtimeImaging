# S4 终版 CUDA 侧改造准备方案

> 日期：2026-09-25
> 范围：**准备**，不实施 ABI 变更。开放参数（D1/D2/D3/D5）未回，本方案只定结构与影响面。
> 前提：`References/UBP与FBP重建算法调研报告_审核更正说明.md`（R1–R4 / E1 / G2）
> 已落地基线：CPU reference 侧 S4 骨架 @ `223749f`（`RingReconInversion.h` + 成对开关 + G2 同源）

---

## 0. 结论先行

**推荐：新增导出函数，不改 `RingReconCudaConfig` 结构体。**

```c
RING_RECON_CUDA_API int ring_recon_cuda_set_inversion(void* handle, int mode);
// mode: 0 = Das（默认，现有行为）, 1 = Ubp
// 仅允许在未 append 时调用；已开始累积则返回错误，不静默改
```

理由见 §1。需要重建的二进制因此从「全部消费端」缩到「接线的 4 个」。

---

## 1. ABI 决策：三个选项

| | 方案 | 结构体布局 | 需重建消费端 | 错配风险 |
|---|---|---|---|---|
| **A** | **新增导出函数**（推荐） | **不变** | 仅调用新符号的接线层（4 个） | **无**：旧 DLL 缺符号是**链接期响亮失败**，不会静默错读 |
| B | 结构体末尾加字段 + 版本/尺寸检查 | 变（sizeof 改变） | **全部**（ImagingController / ImagingSvc / NetworkControllerPaimage / RingConfigDialog / MainWindow / ring_recon_view / verify / selftest） | **高**：新旧混用会静默读错偏移；需额外版本字段 + create 时校验 |
| C | 复用 reserved 字段 | 不变 | 仅接线层 | 中：语义误导。任务明文「反演选项不得借用不相关字段」，且 `distanceWeightExponent` 退役属另一范围，不宜夹带 |

**为什么不把 UBP 塞进 `distanceWeightExponent`**（常被误认为可行）：

```text
现有 DAS：  w  = wscale·dotp / (R·dsafe²)      （pw = q+1，q=1 ⇒ pw=2）
UBP 立体角：dΩ = wscale·dotp / dsafe³
设 q=2 ⇒ pw=3 ⇒ w = wscale·dotp/(R·dsafe³) = dΩ / R
```

差一个 **`R`**。单半径下是全局常数、可忽略；但本项目支持 `multiRadius`
（`radiusPerChannel[8]`，逐 A-line 半径 `Rj`）⇒ 该因子**逐线变化**，不是全局标度。
且语义上就不是文献的 `dΩ`。**因此必须是独立的模式字段，不能伪装成指数。**

（该结论与审核 E1 的 `dΩ/w = R/d` 一致：`R/d` 中的 `R` 就是这里漏掉的因子。）

---

## 2. CUDA 核改造方案（具体落点）

### 2.1 复用 S4 骨架的共享头

`src/RingRecon/RingReconInversion.h` 是**纯标量、零依赖**（不引 std 容器 / Qt / CUDA），
`ring_recon_cuda.cu` 可直接 `#include`。好处：CPU reference 与 CUDA 核走**同一份表达式**，
`ring_recon_guard_test` 的 G5/G6 因此真的守到生产算术。

### 2.2 权重切换（`ring_das_kernel`，:223 起；权重算式在 :337-346）

现状：

```cpp
const float pw = wExponent + 1.0f;
/* dsafeP = dsafe^pw（含 pw==2/==1 快路径）*/
const float wBase = wscale[j] * dotp / (Rj * dsafeP);
```

改为（`Das` 分支保持**逐字同形**，不动浮点）：

```cpp
const float wBase = ringrecon_inv::weight(mode, wscale[j], dotp, Rj, dsafe, dsafeP);
// Das ⇒ wscale*dotp/(R*dsafeP)   与现状逐字相同
// Ubp ⇒ wscale*dotp/(dsafe³)     （分母无 Rj，幂次 3）
```

注意 CUDA 的 `wscale[j] = -arc·apod`（含变迹），CPU reference 无 apod —— 公式同形，
`wscale` 只是多带一个乘子，**不需特殊处理**。

### 2.3 信号项（同为 `ring_das_kernel`，:317-335 插值之后）

现状：`vv` 为 `p` 的插值。
改为：

```cpp
float sv = vv;
if (mode == Ubp) {
    float dvv = /* 用同一 tf 对 dscan 做同型插值 */;
    const float tSec = tf / fs;                 // t 用秒（前提 G2）
    sv = ringrecon_inv::signalValue(mode, vv, dvv, tSec);
}
acc += wBase * sv;   // 现状是 wBase * vv
```

**同源约束**（前提 G2）：`p` 与 `p′` 都从同一 `d_bscan`/`bscan` 数组、用**同一个 `tf`**
插值取得。不要为导数另存一份已滤波信号。

### 2.4 导数数组

主机侧逐 A-line 调 `ringrecon_inv::derivativeCentral` 生成 `d_bscan`（新增 device buffer，
仅 `Ubp` 时分配），或起一个独立 kernel 并行算导数（开销 O(Nt·nd)，远小于反投影）。

**【开放项 D1】** 导数前是否先低通。骨架未加；若 D1 判定需要，应加在 `derivativeCentral`
的调用点前，**且不得只对导数支路滤波**（会破坏 G2 同源，见前提文档 S3 结论）。

### 2.5 handle 与 API

```cpp
struct Handle {
    ...
    ringrecon_inv::InversionMode inversion = ringrecon_inv::InversionMode::Das;
    bool appendStarted = false;      // 用于「已累积后禁止改模式」
};

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_set_inversion(void* h, int mode) {
    // mode 越界 / h 空 / appendStarted ⇒ 返回错误码，不静默
}
```

`ring_recon_cuda_reset()` 复位 `appendStarted`（与「参数只在停止时应用」一致）。

> **公开 ABI 头保持零依赖**：setter 形参用 `int mode` 而不是 `ringrecon_inv::InversionMode`
> 枚举，因此 `ring_recon_cuda.h` **不需要** include 共享头 —— 所有现有消费者继续只依赖
> 该头，不引入新的头文件依赖链。`RingReconInversion.h` 只在 `.cu`/`.cpp` 实现侧 include。

---

## 3. 必须重建 vs 可不重建

`ring_recon_cuda.cu` 一旦改动 ⇒ `ring_recon_cuda.dll` 必须重建（BUILD_STANDARD §8.2
「未改 CUDA 核心不应无条件重编」的例外：**本次改了核心**）。

| 二进制 | 是否需重建 | 说明 |
|---|---|---|
| `ring_recon_cuda.dll` | **必须** | 核心改动 |
| `ImagingSvc.exe` | **必须**（走方案 A 时） | 调用新导出符号 ⇒ 需新 import lib |
| `PAimageReceiverDiagnostics.exe`（主程序） | **必须**（若 UI 接线） | 同上 |
| `ring_recon_cuda_verify` | 可不重建 | 不调用新符号；旧 import lib 仍解析原有符号 |
| `ring_svc_selftest.exe` | 可不重建 | 同上（除非要加反演自检项） |
| `ring_recon_view` | 可不重建 | 同上 |
| 线性 `pa_recon_core.dll` | **不动** | 任务禁止修改线性 DLL |

**关键性质**：方案 A 下，`ring_recon_cuda.dll` 换新后，未调用新符号的旧消费者**照常工作**
（它们解析的符号集未变）。这是方案 A 相对 B 的主要收益。

---

## 4. 防错配的兼容性检查（即使方案 A 也要做）

1. **链接期**：新消费端调用 `ring_recon_cuda_set_inversion` ⇒ 必须链新 import lib
   `libring_recon_cuda.dll.a`；用旧 lib 会**链接失败**（响亮，不静默）。
2. **加载期**：DLL 版本与 import lib 不匹配时按 BUILD_STANDARD 禁止「旧 DLL fallback
   掩盖构建失败」。
3. **运行期**：`set_inversion` 在 `appendStarted` 后调用 ⇒ 返回错误，不改变已累积图像
   （对应「不能在一幅累积图像中混合不同算法参数」）。
4. **回执**：按 §8.3 记录 `ring_recon_cuda.dll` / `cudart64_12.dll` / `cufft64_12.dll` /
   `pa_recon_core.dll` 的来源与 SHA256；注明是 `build/ring_recon_cuda` 本地构建还是
   `_migration_pack/prebuilt_cuda` fallback（本次**不得**用 prebuilt 兜底，核心已改）。

---

## 5. 开放参数与待填位置（等分发任务）

| 开放项 | 待填位置 | 等谁 |
|---|---|---|
| B2 导数前低通截止 / 是否需要 | `derivativeCentral` 调用点之前 | **D1** 实测噪声底 |
| 逐 A-line 新增开销预算 | 导数 kernel 的实现方式（主机侧 vs 独立 kernel） | **D2** 实时余量 |
| 环外 `cosα` 变号处理（`dΩ` 带符号，环外翻转） | `ubpWeight` 后的符号策略；`accW` 用 `\|w\|` 还是带符号 | **D3 / D5** |
| `t = tf/fs` vs `systemDelay`/`delayCut` 校准 | 信号项的 `tSec` 表达式 | **D5** |
| 归一化语义（B3：`Σ\|w\|` vs 常数 `Ω₀`） | `ring_snapshot_kernel`（现 `acc/max(accW,1e-12)`） | D3（拼接/全局 `Σ\|w\|` 量级） |

> 上述每一项填入前，`ring_recon_guard_test` 的 G4（全关逐位）必须保持 PASS——
> 这是「没有偷偷改变现有行为」的底线。

---

## 6. 验收与回归（S4 终版）

1. `ring_recon_guard_test` 全绿（G1–G6），**G4 冻结基准仍逐位命中**。
2. 新增 CUDA 侧对照：`ring_recon_cuda_verify` 跑 CPU reference ↔ CUDA 的 A/B，
   在 `Das`（全关）下两者一致；`Ubp` 下两者一致且都满足 G5/G6 闭式。
3. 全量 `ctest`；Ring/CUDA 自检（`ring_svc_selftest`）。
4. 双模式（`spliceMode` 0/1）各跑一遍 D4 全套指标，与 D6 基线对照。
5. 证据四层分开报告；**不把软件 PASS 写成实机 PASS**。

---

## 7. 不做

- 不改 `RingReconCudaConfig` 结构体（除非另有明确授权并按方案 B 全量重建）。
- 不改线性 `pa_recon_core.dll`、不改 CUDA ABI 的既有函数签名。
- 不改保存链路、RoundIdentity/timeout 语义、`PhysicalRoundNormalizer`。
- 不引入第三方数值库。
- 不在开放参数未定前把任何经验值写死进核里。
