# RingReconCuda — production CUDA source / verification

本目录提供 CUDA DLL 的独立构建/验证入口；实际 CUDA ABI header/source 位于
`../RingRecon/ring_recon_cuda.h/.cu`。

当前 CUDA API 已不仅是早期“M2 kernel”：

- uniform / per-A-line angle append；
- per-A-line radius（multi-radius calibration）；
- sector/splice mask + blend；
- layered sound-speed model；
- `reset`；
- normalized `snapshot`；
- state/grid query；
- production ImagingSvc selftest。

## Source layout

```text
src/RingRecon/
  ring_recon_cuda.h
  ring_recon_cuda.cu
  ring_recon.cpp          CPU preprocessing/reference

src/RingReconCuda/
  CMakeLists.txt
  ring_recon_cuda_verify.cpp
  ring_svc_selftest.cpp
```

## Build boundary

CUDA 使用 MSVC host compiler + nvcc；canonical MinGW main build 通常消费已经验证的
`ring_recon_cuda.dll` + MinGW import library，而不是每次重新编 CUDA。

正式 dependency source、staging 和 SHA256 要求以根目录 `BUILD_STANDARD.md` 为准。

需要源码重编时可使用本目录 CMake 工程：

```powershell
cmake -S src/RingReconCuda -B build/ring_recon_cuda -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/ring_recon_cuda
```

实际环境必须显式提供可用的 MSVC/nvcc/Ninja 路径；不要把历史机器的绝对路径视为规范。

**ABI 规则：** `RingReconCudaConfig` 结构变化时，DLL 与所有消费者必须同步重建，并重新生成/验证
MinGW import library；混用不同 ABI 的 DLL/exe 属于禁止状态。

## Verification

独立 verify 用于 CUDA vs CPU / snapshot/reset 等数值检查。

production chain 软件自检使用主工程生成的：

```text
ring_svc_selftest.exe
ImagingSvc.exe
```

若使用 retained `testdata/14.dat`，必须先确认数据 provenance/hash；正式结果还需绑定 exact
source SHA、DLL hashes 和 build identity。

Session D 已在 accepted candidate 上完成真实 ImagingSvc + CUDA selftest。详细记录：

```text
CODEX_REPORTS/session-abcd-closeout-20260918/
```

## Historical benchmarks

早期 M2 和后续 reconstruction-core benchmark 已归档：

```text
CODEX_REPORTS/ring-reconstruction-history-202608/
```

不要把旧 benchmark 的单块耗时直接当作当前 main 在不同 grid/GPU/runtime 下的性能保证。
