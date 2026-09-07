# MC410T_Receiver — 多卡UDP高速采集上位机

## 概述

MC410T_Receiver 是一个面向多网口高速采集场景的 Qt 上位机接收程序，基于 **WinSock UDP** 协议栈实现多路数据接收、处理、显示与存储。

### 功能特性

- Qt 图形界面实时显示与控制（QCustomPlot 波形绘制）
- 多路 UDP 数据接收（每卡独立端口，select 多路复用）
- 数据包重组与差分相位→频率转换
- 文件保存（含写缓冲优化，减少 I/O 次数）
- 可选 ZeroMQ 帧发布扩展（`USE_ZEROMQ=ON`，用于对接外部图像处理进程）
- 成像扩展子进程（ImagingSvc，GPU 光声图像重建）
- 卡片就绪检测与主动探测机制
- 多线程架构：接收线程组 + 处理线程组 + 存储线程组

## 目录结构

```
delivery/
├── CMakeLists.txt              # 主工程构建配置
├── CMakePresets.json           # CMake 预设（MSVC Release / MinGW Debug）
├── resources.qrc               # Qt 资源文件
├── README.md                   # 本文件
├── src/                        # 核心业务代码
│   ├── main.cpp                # 程序入口
│   ├── DataProcessor.cpp       # 数据处理线程
│   ├── FileSaver.cpp           # 文件保存线程
│   ├── FramePublisher.cpp      # ZeroMQ 帧发布线程
│   ├── MultiPortReceiver.cpp   # WinSock 多路接收线程
│   ├── NetworkController.cpp   # 多线程协调器
│   ├── MainWindow.cpp          # 主窗口 UI 逻辑
│   ├── ImagingController.cpp   # 成像控制
│   └── ImagingSvc/             # GPU 成像子进程
│       ├── CMakeLists.txt
│       ├── main.cpp
│       ├── ImagingSvc.cpp
│       └── ImagingSvc.h
├── include/                    # 头文件
│   ├── *.h                     # 各类业务头文件
│   └── third_party/
│       └── concurrentqueue.h   # 无锁并发队列（moodycamel）
├── ui/
│   └── MainWindow.ui           # Qt Designer 界面文件
├── qcustomplot/                # QCustomPlot 绘图库
│   ├── qcustomplot.cpp
│   └── qcustomplot.h
├── resources/                  # 资源文件
│   ├── styles.qss              # 界面样式表
│   └── icons/                  # 图标资源
├── libs/imaging/               # 成像算法运行时
│   ├── pa_recon.h
│   ├── pa_recon_qt.hpp
│   ├── pa_recon_stl.hpp
│   ├── pa_recon_core.dll
│   ├── pa_recon_core.lib
│   └── cufft64_12.dll
└── docs/
    └── ZeroMQ编译与集成指南.md  # ZeroMQ 编译集成文档
```

## 环境要求

- Windows 10/11（x64）
- CMake >= 3.21
- Ninja 构建系统
- Qt 6.8.0（msvc2022_64 或 mingw_64）
- Visual Studio 2022 Build Tools 或 MinGW 13.1.0
- 支持 AVX2 指令集的 CPU（Intel Haswell / AMD Excavator 及以上）

### 可选依赖

- **ZeroMQ 4.3.5**：启用帧发布功能（`USE_ZEROMQ=ON`）
  - 编译方式参见 `docs/ZeroMQ编译与集成指南.md`

## 构建方式

### 构建命令

使用 Visual Studio 2022 BuildTools 的 clang-cl 编译器 + Qt MSVC 运行时库。

```powershell
cmd /c "call ""D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" & cd /d <源码目录> & cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl.exe -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/msvc2022_64/lib/cmake/Qt6 -DUSE_ZEROMQ=ON & cmake --build build\release -j4"
```

> ⚠️ 请根据实际安装路径调整 `Qt6_DIR`、`CMAKE_MAKE_PROGRAM` 和 `vcvars64.bat` 路径。

**产物**：`build/release/bin/MC410T_Receiver.exe`

构建成功后，`windeployqt` 会自动将 Qt 运行时 DLL 和插件复制到 bin 目录，直接执行 exe 即可运行。

## 运行说明

```powershell
cd <源码目录>\build\release\bin
.\MC410T_Receiver.exe
```

程序启动后，会在 exe 同目录下生成 `app_log.txt` 运行日志。

### 网卡配置建议（ConnectX-5 双口 25GbE）

为优化多路 UDP 接收性能，建议对 ConnectX-5 网卡进行以下配置（需管理员权限）：

```powershell
# RSS 多队列（8个独立DMA缓冲，防止单缓冲溢出）
Set-NetAdapterRss -Name "以太网 9" `
    -NumberOfReceiveQueues 8 `
    -BaseProcessorNumber 0 `
    -MaxProcessors 6 `
    -Enabled $true

# 中断聚合
Set-NetAdapterAdvancedProperty -Name "以太网 9" `
    -DisplayName "Interrupt Moderation" -DisplayValue "Enabled"

# 增大接收描述符
Set-NetAdapterAdvancedProperty -Name "以太网 9" `
    -DisplayName "Receive Buffers" -DisplayValue "4096"
```

> 网卡名称以实际 `Get-NetAdapter` 查询结果为准。

## 架构说明

### 多线程模型

```
┌─────────────────────────────────────────────────────────┐
│  Layer 5: UI层   QCustomPlot · QTimer 33ms pull        │
├─────────────────────────────────────────────────────────┤
│  Layer 4: 显示缓冲层   DisplayBuffer × N（原子双缓冲）   │
├─────────────────────────────────────────────────────────┤
│  Layer 3: 数据处理层   DataProcessor × N                │
│           包重组 → 相位转换 → 频率计算 → 降采样           │
├─────────────────────────────────────────────────────────┤
│  Layer 2: 接收层   MultiPortReceiver × N                │
│           WinSock select 多路复用 · 每线程 4 端口        │
├─────────────────────────────────────────────────────────┤
│  Layer 1: ConnectX-5 MCX512A-ACAT · 双口2×25GbE        │
└─────────────────────────────────────────────────────────┘
```

### 线程分配方案（i5-12490F，6P核）

| CPU 核心 | 用途 |
|---------|------|
| P核0 | Qt 主线程 + 系统 |
| P核1 | 辅助线程（存储等） |
| P核2~5 | MultiPortReceiver 接收线程（每核负责 4 张卡） |
| 超线程 | DataProcessor 处理线程组（条件变量唤醒，大部分时间阻塞） |

### 数据流

```
采集卡 UDP 包 → WinSock select() → MultiPortReceiver
  → ConcurrentQueue<DataPacket> → DataProcessor
    → PacketAssemblyBuffer 重组 → 相位/频率计算
      → DisplayBuffer（UI 显示）
      → SaveQueue → FileSaver（磁盘写入）
      → PublishQueue → FramePublisher（ZeroMQ 发布，可选）
```

## 状态栏指标说明

| 字段 | 含义 |
|------|------|
| **触发** | 接收完整的触发次数（所有 UDP 包均已到达） |
| **丢失** | 触发数据不完整次数（部分 UDP 包丢失） |
| **丢弃** | 被丢弃的 UDP 数据包数量（序号重复/乱序/重传） |
| **跳帧** | 被整体跳过的触发次数（内存不足/队列积满/序号复位） |
| **队列** | 处理队列当前积压深度 |
| **速率** | 当前 UDP 接收带宽（Mbps） |

## 常见问题

### 1. MinGW 构建时出现 OpenGL/Qt DLL 没有自动复制

Qt 运行时复制发生在链接成功之后的 `POST_BUILD` 阶段。如果链接失败，`windeployqt` 不会执行，因此 `Qt6OpenGL.dll`、`opengl32sw.dll`、`platforms/qwindows.dll` 等文件都不会进入 bin 目录。`opengl32.dll` 属于 Windows 系统 DLL，不需要也不会自动复制。

### 2. 磁盘写入速度远低于接收速率

**已实施优化**（`WRITE_BUFFER_TRIGGERS = 16`）：FileSaver 在内存中积累 16 个触发再一次性写盘，减少 I/O 频次。若磁盘仍是瓶颈，可增大该值（如 32），代价是停止保存时最多损失 ~160ms 数据。

### 3. 接收线程 CPU 占用过高

- 确认 RSS 多队列已正确配置（见网卡配置建议）
- 检查 `SO_RCVBUF` 是否足够大（程序默认设为 64MB）
- 减少每接收线程负责的卡数（修改 `CARDS_PER_RECEIVER`）

## 数据格式

| 参数 | 值 |
|------|-----|
| 原始采样率 | 250 MS/s |
| 发送侧抽取 | 2抽1 → 125 MS/s |
| 每触发采样点数 | 25,000 点（200 µs 窗口） |
| 通道数 | A + B 双通道 |
| 每触发数据量 | ~98 KB |
| UDP 载荷 | 1440 字节/包 |
| UDP 包头 | 4 字节（packetSeq + triggerSeq） |
| 每触发包数 | ≤ 70 包 |
| 最大触发频率 | 200 Hz |

## 许可

 proprietary
