# M2 CUDA 编译必要性评估与文件清单

> 状态：待复核。本阶段只做评估与工具链安装，不开始 CUDA 内核编译。

## 1. 工具链安装结论（已完成）

| 组件 | 版本 | 路径 | 说明 |
|---|---|---|---|
| CUDA Toolkit | 12.8.1 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8` | 选择 12.8：与现有 `cufft64_12.dll` 同属 CUDA 12.x 运行时；支持 Ada（RTX 4060, sm_89）；官方支持 VS2022 |
| 宿主编译器 | VS2022 Build Tools 17.14 | `C:\Program\VC\Tools\MSVC\14.44.35207` | nvcc 在 Windows 上必须用 MSVC；环境变量入口 `C:\Program\VC\Auxiliary\Build\vcvars64.bat` |

注意：VS 安装参数中的 `--installPath` 因参数空格被拆开，装到了非标准位置 `C:\Program\VC`（功能已验证可用，`cl` 编译与 nvcc 最小 kernel 运行均通过）。若后续出现问题，可重装到标准路径。

## 2. 评估结论

当前实时成像链路的计算热点只有一处：**环形 DAS 重建核心**（逐像素 × 逐 A-line 的反投影、插值、权重累加）。其余环节均为网络 I/O、磁盘 I/O、消息传递或逐元素小规模运算，CPU 已足够，且 GPU 拷贝/启动开销反而可能拖慢实时性。

## 3. 需要 CUDA 编译（nvcc）的文件清单

| 文件 | 实时成像流程中的功能 | 是否必须 CUDA 编译 |
|---|---|---|
| `src/RingRecon/ring_recon_cuda.cu`（新建） | 重建核心：分块双波长 A-line 的环形 DAS 逐像素反投影、线性插值、距离权重、FOV 掩膜、逐块累加（acc/accW） | 必须，nvcc 编译 |
| `src/RingRecon/ring_recon_cuda.h`（新建） | CUDA 模块对外 C 接口声明（宿主调用入口） | 否（头文件） |
| `src/RingRecon/ring_recon_cuda.cpp`（新建，可选并入 .cu） | 设备内存管理、kernel 启动、结果回拷的宿主封装 | 否，普通 C++ 编译即可 |

建议最小实现：只建 `ring_recon_cuda.cu` + `ring_recon_cuda.h`，kernel 与启动函数都放在 .cu 内，对外暴露 C 接口，供 RingRecon/ImagingSvc 调用。

## 4. 不需要 CUDA 编译的现有文件（含功能说明）

| 文件 | 实时成像流程中的功能 | 不编译 CUDA 的理由 |
|---|---|---|
| `src/RingRecon/ring_recon.cpp/.h` | 分块读取、双波长解交织、预处理（DBR 扣除/延时截断）、CPU 参考实现 | 仅宿主逻辑；保留 CPU 版作为对照基准 |
| `src/RingRecon/ring_recon_verify.cpp` | M1 验证入口 | 宿主程序 |
| `src/ImagingSvc/ImagingSvc.cpp/.h` | 成像子进程：ZMQ 信令、共享内存脉冲读取、调用重建、帧回传 | 宿主编排，不写 kernel |
| `src/ImagingController.cpp/.h` | 主进程成像控制：子进程生命周期、ZMQ/共享内存 | 宿主编排 |
| `src/DataProcessor.cpp/.h` | UDP 包重组、差分相位→频率换算、降采样 | 数据量约 20MB/s@200Hz，CPU 单线程足够 |
| `src/MultiPortReceiver.cpp/.h` | 多路 UDP 接收 | 网络 I/O 密集，GPU 无收益 |
| `src/NetworkController.cpp/.h` | 卡片扫描/ICMP/ARP 探测 | 低频控制逻辑 |
| `src/FileSaver.cpp/.h` | 磁盘写入（float32→float16 已 AVX2 优化） | 磁盘 I/O |
| `src/FramePublisher.cpp/.h` | ZeroMQ 帧发布 | 消息 I/O |
| `src/MainWindow.cpp/.h`、`ui/`、`qcustomplot/` | 界面与波形/图像显示 | UI/OpenGL 绘制 |
| `libs/imaging/pa_recon_core.dll` 等 | 旧 GPU 重建运行库 | 二进制参考，不参与源码编译 |

## 5. 暂不 CUDA 编译（未来按需评估）

| 功能 | 所在位置 | 说明 |
|---|---|---|
| 带通/高低通滤波（IIR） | `preprocessBlock` | 当前管线默认关闭；即使启用，16M 采样/帧级别 CPU 也可承受 |
| 中值滤波（2D） | `preprocessBlock` | 同上 |
| Hilbert 包络（FFT） | 重建后处理（MATLAB 中 `UseEnvelope`） | 当前未启用；启用时可考虑 cuFFT，届时再加 |
| 伪彩色映射 `frameDataToImage` | `ImagingController.cpp` | 逐元素运算，CPU 足够 |

## 6. 后续编译计划（待复核后执行）

1. 新建 `ring_recon_cuda.cu/.h`，用 nvcc + MSVC 编译成独立 CUDA 模块（静态库或 DLL，C 接口）。
2. 以 M1 CPU 输出为基准，逐块对比 CUDA 版 acc/accW/归一化图（沿用 `compare_ring_recon.py`）。
3. 达标后接入 ImagingSvc/主程序（M3），并按更高速采集目标做性能基准。
## 7. M2 实施结果（CUDA 内核已完成并通过对照）

- CUDA 模块：`src/RingRecon/ring_recon_cuda.cu/.h` + `src/RingReconCuda/` 验证工程。
- 参数结构 `RingReconCudaConfig` 保留 MATLAB 主脚本初始化全部输入参数，供前端交互控件填充。
- 数值对照（CUDA vs CPU，排除探测器奇异区）：
  - 11.dat：acc 3.8e-4，accW 3.4e-6，归一化图 4.1e-4
  - 14.dat：acc 1.8e-4，accW 3.4e-6，归一化图 1.1e-4
- 性能（单块平均，200 A-line/块，双波长）：

| 网格 | CPU 11.dat | CUDA 11.dat | CPU 14.dat | CUDA 14.dat |
|---|---|---|---|---|
| 360x360 | 86 ms | 19.5 ms | 155 ms | 21.1 ms |
| 720x720 | 329 ms | 22.3 ms | 583 ms | 23.9 ms |

- 分层声速等未启用功能参数已保留，启用时返回明确错误，后续按需实现。