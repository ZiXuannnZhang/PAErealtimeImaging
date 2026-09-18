# RingRecon — CPU reference / verification implementation

`src/RingRecon` 是环形双波长 DAS 的 **CPU reference 与验证工具**，不是 canonical production
实时重建 owner。production 环形重建由 ImagingSvc 调用 `ring_recon_cuda.dll`。

本目录保留 CPU 版的价值是：

- 数值 reference；
- CUDA A/B / regression；
- 独立 verify/view 工具；
- 算法问题定位。

主要文件：

- `ring_recon.h/.cpp` — CPU preprocessing / incremental DAS reference；
- `ring_recon_verify.cpp` — 命令行验证；
- `ring_recon_view.cpp` — Qt 可视化 reference viewer；
- `ring_recon_cuda.h/.cu` — CUDA C ABI source（实际 CUDA 工程入口在 ../RingReconCuda）。

## Build

独立 CPU reference：

```powershell
cmake -S src/RingRecon -B build/ring_recon_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/ring_recon_release
```

本地 Qt/MinGW 路径通过 CMake cache/toolchain 指定，不把旧机器绝对路径当规范。

## CUDA viewer

`ring_recon_view` 可以在提供 MinGW import library 和 CUDA runtime 目录时启用 CUDA engine。
具体 CUDA rebuild 与 production selftest 见：

```text
../RingReconCuda/README.md
仓库根 BUILD_STANDARD.md
```

## Historical validation

早期 M1/M2 的 11.dat / 14.dat CPU/CUDA 数值对照和 360/720 网格性能数字属于历史开发里程碑，
现已归档到：

```text
CODEX_REPORTS/ring-reconstruction-history-202608/
```

后续 4000×4000 benchmark 也在该目录保存。历史数字用于比较，不代表当前 canonical main 的
formal performance acceptance。

## Production boundary

不要从本 CPU README 推导当前：

- physical round boundary；
- startup trigger filter；
- RingBlockAssembler ownership；
- ImagingSvc timeout/reset；
- AutoSave directory rollover。

这些以 canonical source、`MC_410T_MultiCard/delivery/README.md` 和 Session A–D closeout 为准。
