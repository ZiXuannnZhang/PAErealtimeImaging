# RingReconCuda — 双波长环形 DAS 重建 CUDA 模块

M2 实现：重建核心 CUDA 化，其余环节保持 CPU。

- `ring_recon_cuda.h`：完整参数结构（与 MATLAB 主脚本初始化一一对应，供前端交互控件填充）+ C 接口
- `ring_recon_cuda.cu`：DAS kernel（逐像素 x 逐 A-line 反投影/插值/权重/FOV 掩膜/增量累加）与宿主封装
- `ring_recon_cuda_verify.cpp`：MSVC 验证入口（预处理复用 CPU 版 ring_recon.cpp）

## 构建（需 VS2022 Build Tools + CUDA 12.8）

```bat
call C:\Program\VC\Auxiliary\Build\vcvars64.bat
cmake -S src\RingReconCuda -B build\ring_recon_cuda -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=cl.exe ^
      -DCMAKE_CUDA_COMPILER="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe" ^
      -DCMAKE_MAKE_PROGRAM=D:\Qt\Qt6.8.0\Tools\Ninja\ninja.exe
cmake --build build\ring_recon_cuda
```

产物：`build/ring_recon_cuda/bin/ring_recon_cuda.dll`、`build/ring_recon_cuda/ring_recon_cuda_verify.exe`

## 运行（PATH 需含 CUDA bin 与 DLL 目录）

```powershell
$env:PATH = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin;" +
            "$PWD\build\ring_recon_cuda\bin;$env:PATH"
build\ring_recon_cuda\ring_recon_cuda_verify.exe --data D:\zzx\data\20260716\11.dat --id 11 --grid-mm 0.1 --block 200 --out build\ring_recon_cuda_out11
build\ring_recon_cuda\ring_recon_cuda_verify.exe --data D:\zzx\data\20260519\14.dat --id 14 --grid-mm 0.1 --block 200 --out build\ring_recon_cuda_out14
```

## 验证结果（CUDA vs CPU，360x360/0.1mm，200 A-line/块）

| 数据集 | acc 最坏相对差 | accW 最坏相对差 | 归一化图最坏相对差（排除探测器奇异区） |
|---|---|---|---|
| 11.dat | 3.8e-4 | 3.4e-6 | 4.1e-4 |
| 14.dat | 1.8e-4 | 3.4e-6 | 1.1e-4 |

## 性能（单块平均，200 A-line/块，双波长）

| 网格 | CPU 11.dat | CUDA 11.dat | CPU 14.dat | CUDA 14.dat |
|---|---|---|---|---|
| 360x360 (0.1mm) | 86 ms | 19.5 ms | 155 ms | 21.1 ms |
| 720x720 (0.05mm) | 329 ms | 22.3 ms | 583 ms | 23.9 ms |

## 未实现但已保留参数

分层声速（SoundSpeedRadii）、触发去抖、相位去卷积、Gaussfil、滤波、中值、去弧线、扫描伪影等：
配置字段已按 MATLAB 主脚本初始化完整保留，启用时 CUDA 模块返回明确错误，后续按需实现。