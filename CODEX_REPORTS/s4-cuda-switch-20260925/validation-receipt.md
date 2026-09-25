# S4 终版 CUDA 侧改造 — 编译与验证回执

- 分支：`codex/das-dual-wavelength-quality-b-tier-20260925`
- `IMPLEMENTATION_SOURCE_SHA` = `cd783c3bad871f1700ed9318c8c28bcac95e8cfa`
- `REPORT_HEAD` = 本提交（receipt-only）
- 日期：2026-09-25（UTC+8）
- 工具链依据：`开发工具使用指引.md` §5（CUDA DLL 重编）

---

## 0. 结论

**CUDA 核已真实编译、链接并验证。** 开关默认关（`Das`）、未接线。
编译过程抓到并修掉一个**只有编译才能发现**的缺陷（见 §2）。

```text
source/code correctness                是（编译通过 + 新旧 DLL 默认路径逐字节一致）
automated tests/build/selftest         是（46/46 ctest + verify + ring_svc_selftest）
real hardware validation               未声称（本机 GPU 数值验证，非实机采集）
hardware root-cause attribution        未声称
```

---

## 1. 执行的步骤（指引 §5.3）

| 步 | 动作 | 结果 |
|---|---|---|
| 0 | 结束相关进程（`MC410T_Receiver` / `ImagingSvc` / `PAimageReceiverDiagnostics`） | 无在跑 |
| 1 | `vcvars64.bat` + `nvcc` 编译 `.cu` + `link` 出 DLL | **首次失败，见 §2；修复后通过** |
| 2 | `gendef` + `dlltool` 重建 MinGW 导入库 | 通过，导出表含 `ring_recon_cuda_set_inversion` |
| 3 | 主工程 `build_mingw_debug.cmd configure` + `build` | 通过，`ImagingSvc.exe` / `PAimageReceiverDiagnostics.exe` 均重新链接 |
| 5 | `ring_recon_cuda_verify` + 新旧 DLL 对比 + `ring_svc_selftest` | 全部通过，见 §3 |

> 注：指引 §5.2 的告诫成立——MSVC BuildTools 装在截断路径 `C:\Program\`，
> 按常规路径（`C:\Program Files\Microsoft Visual Studio`）查找会一无所获。
> 此前「本机无法编译 CUDA」的判断即因此误报。另：CUDA 目录**不可**用 ninja /
> `cmake --build`（历史验证会卡死），必须 vcvars64 + nvcc + link 直调。

---

## 2. 编译抓到的缺陷（本次最有价值的一条）

首次 `nvcc` 报 **6 errors**，全部同源：

```text
error: calling a __host__ function("ringrecon_inv::derivativeCentral<float>")
       from a __global__ function("ring_derivative_kernel") is not allowed
error: calling a __host__ function("ringrecon_inv::signalValue<float>")
       from a __global__ function("ring_das_kernel") is not allowed
error: calling a __host__ function("ringrecon_inv::weight<float>")
       from a __global__ function("ring_das_kernel") is not allowed
```

**共享头 `RingReconInversion.h` 的 `inline` 模板默认是 `__host__`，不能从 `__global__` 核调用。**

这一点**任何 CPU 测试都不可能发现**——同一份代码在 CPU reference 里合法且全部通过
（守卫 20/20）。它只能被 CUDA 编译抓出来。修复：按 `__CUDACC__` 条件给六个函数加
`__host__ __device__`，非 CUDA 翻译单元仍展开为空，头保持零依赖。

这直接印证了回执的意义：**上一版「代码已写好但未编译」是不能称可用的。**

---

## 3. 验证结果

### 3.1 编译 / 链接

```text
ring_recon_cuda.dll        产出（link 完成，仅 LNK4098 默认库提示——静态 cudart 常规）
libring_recon_cuda.dll.a   gendef + dlltool 重建
导出表含 ring_recon_cuda_set_inversion（ring_recon_cuda.def:20）
主工程 ImagingSvc.exe / PAimageReceiverDiagnostics.exe 重新链接成功
```

### 3.2 数值验证（指引 §5.3 步骤 5）

`ring_recon_cuda_verify.exe`（MSVC 编译，链新 DLL），`testdata/14.dat`，id=14：

```text
wl1 snapshot vs get_state worst diff = 0.000e+00
wl1 reset ok
wl2 snapshot vs get_state worst diff = 0.000e+00
wl2 reset ok
done: nx=360 ny=360 blocks=40 total=477.2 ms avg=11.93 ms
```

**worst diff = 0**，符合指引期望。

### 3.3 新旧 DLL 逐字节对比（R4「全关回归」的 CUDA 侧证明）

把备份的旧 DLL 换回、同一输入重跑，与新 DLL 输出逐文件 `diff -r`：

```text
==> 逐字节一致：CUDA 全关路径未被改动
```

这是本次最直接的「没偷偷改现有行为」证据——`Das` 默认路径下，新核产出与
改动前的 prebuilt DLL **完全相同**。

### 3.4 生产自检

`ring_svc_selftest.exe`（`testdata/14.dat`，8 通道）：

```text
done: channels=8 K=500 rounds=2 blocks=10 total=708.3 ms avg=70.83 ms
[RingSHMObs] service kind=final session=1 ... submitted=10 consumed=10
             mismatch=0 ready_zero=0 duplicate=0 gap=0
[RingSHMObs] producer session=1 submitted=10 slot_busy=0 last_seq=9
EXIT=0
```

### 3.5 CPU 侧回归（头文件改过，必须重跑）

```text
ctest 全量          46/46 PASS
ring_recon_guard_test      20/20（G4 全关冻结基准仍逐位命中）
ring_phantom_forward_test  25/25
```

### 3.6 ABI（承前一提交的实证）

```text
sizeof(RingReconCudaConfig) = 696（改动前后一致）
```

本次仅新增导出函数、未动结构体 ⇒ 旧消费者仍可加载新 DLL；
但调用新符号的接线层需重链新导入库。

---

## 4. CUDA 依赖回执（指引 §5.4）

```text
Ring CUDA source         = local build @ cd783c3bad871f1700ed9318c8c28bcac95e8cfa
ring_recon_cuda.dll      = ec7723d70037a3c4905e677c68ae8a42fab67cc604923778b3d003f06f2e546f
libring_recon_cuda.dll.a = 394458eb712b84fd9e779af8a210c2609bc3382566fae8dd6c0662c50b17fa9a
cudart64_12.dll          = c2c9a9c22a9bcba90e261825968836787b331038047a26770cffb7a583c28344
pa_recon_core.dll        = c1a37e73147c1b8250f96ccdc1fcfc08ca28cecbf1cb3c046d4ab426f2ea0893
cufft64_12.dll           = 2480d8ab849d7e9a375275f6c0278b8764c14ac0c1a3bdacaf256ae4a93c5590
```

工具链：

```text
nvcc   CUDA compilation tools 12.8, V12.8.93
MSVC   Microsoft (R) C/C++ Optimizing Compiler 19.44.35228（MSVC 14.44.35207）
GPU    NVIDIA GeForce RTX 4060 Laptop，arch=compute_89,sm_89
```

编译命令（指引 §5.3 原样）：`-std=c++17 -Xcompiler=/utf-8 -Xcompiler="-O2 -Ob2" -DNDEBUG`
+ `--generate-code=arch=compute_89,code=[compute_89,sm_89]`；
链接 `cudadevrt.lib cudart_static.lib` + 系统库，`/LIBPATH:CUDA/v12.8/lib/x64`。

> `/utf-8` 不能省：`.cu` 内有中文注释，MSVC host 默认按 GBK 解析会破坏代码。

---

## 5. 未验证项 / 限制

1. **未接线**：UI / ImagingSvc / MainWindow 未接 `ring_recon_cuda_set_inversion`，
   运行时行为仍为 `Das`，与改动前一致（§3.3 已证）。
2. **Ubp 路径未在 GPU 上跑过**：开关默认关，本次验证全部走 `Das`。
   `Ubp` 的设备侧正确性要等参数定完、接线后由 D6/D7 的 A/B 与 CUDA vs CPU 对照覆盖。
3. **五个开放参数仍空**：B2 低通截止（D1）、逐线开销预算（D2）、环外 `cosα` 变号（D3/D5）、
   `t = tf/fs` vs delayCut 校准（D5）、归一化 `Σ|w|` vs `Ω₀`（D3）。
4. `testdata/14.dat` 的 provenance/hash 需在正式交付回执中绑定；
   本机 GPU 数值验证 ≠ 实机采集验证。
5. LNK4098（默认库 `LIBCMT` 冲突提示）为静态 cudart 的常规现象，未做 `/NODEFAULTLIB` 处理；
   若正式构建要求消除该警告，需单独决定。

---

## 6. 证据目录

```text
CODEX_REPORTS/s4-cuda-switch-20260925/
  validation-receipt.md   本文件
```

`REPORT_HEAD` 相对 `IMPLEMENTATION_SOURCE_SHA` 的 diff 只包含该目录。
