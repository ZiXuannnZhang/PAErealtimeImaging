# RingRecon — 双波长环形扫描 DAS 实时重建（C++ CPU 移植）

对应 实时重建脚本（原 Handoff）MATLAB 逐块链路的 C++ 实现，当前为 M1 CPU 版本：

- `ring_recon.h/.cpp`：纯 C++17 重建模块（分块读取/解交织/预处理/增量 DAS/归一化）
- `ring_recon_verify.cpp`：命令行验证工具，逐块导出 wl1/wl2 归一化图、acc、accW
- `CMakeLists.txt`：独立构建（不依赖 Qt）

## 构建

```powershell
$env:PATH = "D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin;$env:PATH"
cmake -S src\RingRecon -B build\ring_recon_release -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe `
      -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe
cmake --build build\ring_recon_release
```

## 运行（360x360、0.1mm 网格）

```powershell
build\ring_recon_release\ring_recon_verify.exe `
  --data D:\zzx\data\20260716\11.dat --id 11 --grid-mm 0.1 --block 200 --out build\ring_recon_out11
build\ring_recon_release\ring_recon_verify.exe `
  --data D:\zzx\data\20260519\14.dat --id 14 --grid-mm 0.1 --block 200 --out build\ring_recon_out14
```

## MATLAB 参考与数值对照

```matlab
cd 实时重建脚本
export_ring_recon_reference('D:\zzx\data\20260716\11.dat', 'build\ring_recon_ref11', 11, 0.1, 200)
export_ring_recon_reference('D:\zzx\data\20260519\14.dat', 'build\ring_recon_ref14', 14, 0.1, 200)
```

```powershell
python tools\compare_ring_recon.py build\ring_recon_ref11 build\ring_recon_out11 360 360 20 --dataset 11
python tools\compare_ring_recon.py build\ring_recon_ref14 build\ring_recon_out14 360 360 40 --dataset 14
```

## M1 验证结果（0.1mm 网格，200 A-line/块）

| 数据集 | 累加 acc 最坏相对差 | 权重 accW 最坏相对差 | 归一化图最坏相对差（排除探测器奇异区） |
|---|---|---|---|
| 11.dat（20 块） | 3.7e-4 | 4.0e-6 | 3.2e-4 |
| 14.dat（40 块） | 1.5e-4 | 4.0e-6 | 1.1e-4 |

说明：DAS 权重含 1/dist^2，探测器附近像素对浮点舍入极敏感（GPU 与 CPU
求和顺序/FMA 不同）；对照时排除距离任一已出现探测器 10 倍网格步长内的像素。
非奇异区结果与 MATLAB 一致，误差量级符合单精度浮点预期。
## 可视化查看器（VSCode 一键运行）

`ring_recon_view` 是 M1 的 Qt 可视化验证工具：逐块读取数据并实时刷新左右两幅灰度图
（左=532nm，右=1064nm），显示效果与 MATLAB 主脚本的 imagesc 双图一致。

### VSCode 运行

1. 用 VSCode 打开工作区 `C:\Users\yyps\Documents\ChatGPT\realtime_imaging_migration`；
2. 运行任务 `build_ring_recon_view`（或直接按 F5）；
3. 调试/运行面板选择：
   - `Run RingRecon View (11.dat)`：D:\zzx\data\20260716\11.dat，20 块；
   - `Run RingRecon View (14.dat)`：D:\zzx\data\20260519\14.dat，40 块。

窗口会显示两块实时累积重建图、当前块号和单块耗时；全部处理完后状态栏显示总耗时。

### 命令行参数

```powershell
build\ring_recon_release\bin\ring_recon_view.exe `
  --data D:\zzx\data\20260716\11.dat --id 11 `
  --grid-mm 0.1 --block 200 --sleep-ms 50
```

- `--sleep-ms`：块间停顿（模拟 MATLAB 的 pause，默认 50ms）；
- `--auto-quit 1`：跑完全部块后自动退出（用于无界面自检）。
### CUDA 引擎（M2 可视化验收）

`ring_recon_view` 已支持 `--engine cuda`：重建核心走 `ring_recon_cuda.dll`，
预处理仍为 CPU。构建时 `build_ring_recon_view.cmd` 会自动链接 CUDA 导入库并部署
`ring_recon_cuda.dll`/`cudart64_12.dll` 到查看器 bin 目录。

```powershell
build\ring_recon_release\bin\ring_recon_view.exe `
  --data D:\zzx\data\20260519\14.dat --id 14 --grid-mm 0.1 --block 200 `
  --engine cuda --sleep-ms 50
```

完整验收步骤见 `docs/M2_可视化验收方案.md`。