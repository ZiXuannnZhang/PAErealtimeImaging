# ZeroMQ 编译与集成指南

> **适用版本**：ZeroMQ 4.3.5 / Windows 10 x64  
> **编译工具链**：MSVC 2022 (v143) + MinGW 13.1.0（Qt 6.8.0 工具链）  
> **撰写日期**：2026-04-01  

---

## 一、背景说明

本项目（`MC_410T_MultiCard`）的 `FramePublisher` 模块负责将多卡同步后的触发帧通过 **ZeroMQ PUSH socket** 转发给外部图像处理进程。  
ZeroMQ 官方不提供 Windows 预编译二进制包，需自行从源码编译。

**关键约束**：  
- Qt 6.8.0 使用 **MinGW 13.1.0** 工具链编译  
- ZeroMQ 源码使用 `sys/socket.h`，MinGW Qt 工具链缺少该 POSIX 头文件，无法直接用 MinGW 编译  
- 解决方案：**用 MSVC 编译 ZeroMQ DLL，再用 dlltool 生成 MinGW 兼容的导入库**

---

## 二、环境准备

| 工具 | 路径 | 说明 |
|------|------|------|
| ZeroMQ 源码 | `D:\Program Files\zeromq-4.3.5` | 官方源码包 |
| MSVC 2022 BuildTools | `D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | 仅需 BuildTools，不需完整 VS |
| CMake（MSVC 版）| `...\BuildTools\Common7\IDE\...\CMake\bin\cmake.exe` | 自带于 VS BuildTools |
| MinGW dlltool | `D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\dlltool.exe` | Qt 6.8.0 自带 |
| 安装目标目录 | `D:\Program Files\zeromq-4.3.5\install_msvc` | 编译后的头文件和库 |

---

## 三、编译步骤

### 3.1 cmake 配置（Visual Studio 2022 生成器）

在 PowerShell 中执行（**不需要**手动调用 `vcvars64.bat`，VS generator 自动处理）：

```powershell
cmake `
  "-S" "D:\Program Files\zeromq-4.3.5" `
  "-B" "D:\Program Files\zeromq-4.3.5\build_msvc" `
  "-G" "Visual Studio 17 2022" `
  "-A" "x64" `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DCMAKE_INSTALL_PREFIX=D:\Program Files\zeromq-4.3.5\install_msvc" `
  "-DBUILD_SHARED_LIBS=ON" `
  "-DBUILD_TESTS=OFF" `
  "-DWITH_DOCS=OFF"
```

**预期关键输出**：
```
-- Detected ZMQ Version - 4.3.5
-- The C compiler identification is MSVC 19.42.34435.0
-- Using polling method in I/O threads: epoll
-- Including wepoll
-- Configuring done
```

> ⚠️ PowerShell 会将 CMake Warning 误报为 exit code 1，但只要看到 `Build files have been written` 即为成功。

### 3.2 编译

```powershell
cmake --build "D:\Program Files\zeromq-4.3.5\build_msvc" --config Release --parallel 4
```

**预期产物**：
```
D:\Program Files\zeromq-4.3.5\build_msvc\bin\Release\libzmq-v143-mt-4_3_5.dll
D:\Program Files\zeromq-4.3.5\build_msvc\lib\Release\libzmq-v143-mt-4_3_5.lib
```

### 3.3 安装

```powershell
cmake --install "D:\Program Files\zeromq-4.3.5\build_msvc" --config Release
```

**安装后文件结构**：
```
D:\Program Files\zeromq-4.3.5\install_msvc\
├── bin\
│   ├── libzmq-v143-mt-4_3_5.dll    (424 KB, 运行时 DLL)
│   ├── msvcp140.dll                 (MSVC 运行时，随 DLL 一起安装)
│   └── vcruntime140.dll
├── lib\
│   ├── libzmq-v143-mt-4_3_5.lib    (16 KB, MSVC 导入库)
│   └── libzmq-v143-mt-s-4_3_5.lib  (静态库)
├── include\
│   ├── zmq.h
│   └── zmq_utils.h
└── CMake\
    ├── ZeroMQConfig.cmake
    └── ZeroMQTargets.cmake
```

### 3.4 生成 MinGW 兼容导入库

MSVC 编译的 `.lib` 是 COFF 格式的导入库，MinGW 链接器可以处理，但为了更好兼容性，用 `dlltool` 生成 `.dll.a`：

```powershell
$dlltool = "D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\dlltool.exe"
$dllSrc  = "D:\Program Files\zeromq-4.3.5\install_msvc\bin\libzmq-v143-mt-4_3_5.dll"
$libOut  = "D:\Program Files\zeromq-4.3.5\install_msvc\lib\libzmq.dll.a"

& $dlltool -l $libOut -D $dllSrc
```

**产物**：`libzmq.dll.a`（60 KB，MinGW ld 兼容格式）

---

## 四、集成到 CMake 项目

### 4.1 CMakeLists.txt 配置

```cmake
# 编译开关（默认关闭，不影响现有 MinGW 构建）
option(USE_ZEROMQ "Enable ZeroMQ PUSH publisher (FramePublisher)" OFF)

if(USE_ZEROMQ)
    set(ZMQ_INSTALL  "D:/Program Files/zeromq-4.3.5/install_msvc" CACHE PATH "ZeroMQ 安装目录")
    set(ZMQ_LIB_NAME  "libzmq-v143-mt-4_3_5")
    set(ZMQ_IMPORT_LIB "libzmq.dll.a")          # MinGW 兼容导入库

    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_ZEROMQ)
    target_include_directories(${PROJECT_NAME} PRIVATE "${ZMQ_INSTALL}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ZMQ_INSTALL}/lib/${ZMQ_IMPORT_LIB}")

    # 编译后自动拷贝 DLL 到输出目录
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZMQ_INSTALL}/bin/${ZMQ_LIB_NAME}.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying libzmq DLL..."
    )
endif()
```

### 4.2 cmake 构建命令

**启用 ZeroMQ**（cmake 配置时加 `-DUSE_ZEROMQ=ON`）：
```powershell
cmake <源码目录> -G Ninja -DUSE_ZEROMQ=ON
ninja -j4
```

**Qt Creator 中启用**：  
`项目` → `CMake` → 变量列表 → 添加 `USE_ZEROMQ` = `ON` → 重新配置

### 4.3 代码中使用（FramePublisher）

头文件保护：
```cpp
// include/FramePublisher.h
#ifdef USE_ZEROMQ
    void* m_zmqCtx    = nullptr;
    void* m_zmqSocket = nullptr;
#endif
static constexpr char ZMQ_ENDPOINT[] = "tcp://127.0.0.1:5556";
```

实现文件：
```cpp
// src/FramePublisher.cpp
#ifdef USE_ZEROMQ
#  include <zmq.h>
#endif

void FramePublisher::run() {
#ifdef USE_ZEROMQ
    m_zmqCtx    = zmq_ctx_new();
    m_zmqSocket = zmq_socket(m_zmqCtx, ZMQ_PUSH);
    int linger  = 0;
    zmq_setsockopt(m_zmqSocket, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_bind(m_zmqSocket, ZMQ_ENDPOINT);
#endif
    // ... 主循环
#ifdef USE_ZEROMQ
    if (m_zmqSocket) { zmq_close(m_zmqSocket);    m_zmqSocket = nullptr; }
    if (m_zmqCtx)    { zmq_ctx_destroy(m_zmqCtx); m_zmqCtx    = nullptr; }
#endif
}

void FramePublisher::assembleAndPublish(...) {
    // ... 组帧
#ifdef USE_ZEROMQ
    if (m_zmqSocket)
        zmq_send(m_zmqSocket, sf.data.data(),
                 sf.data.size() * sizeof(float), ZMQ_NOBLOCK);
#endif
    emit framePublished(triggerSeq, m_nCards);
}
```

---

## 五、部署注意事项

### 5.1 运行时 DLL 依赖

部署 exe 时需要携带以下 DLL：

```
libzmq-v143-mt-4_3_5.dll   (ZeroMQ 主体)
msvcp140.dll               (MSVC C++ 运行时)
vcruntime140.dll           (MSVC C 运行时)
vcruntime140_1.dll
```

> `cmake POST_BUILD` 已自动拷贝 `libzmq-v143-mt-4_3_5.dll`，但 MSVC 运行时需手动拷贝或在部署机安装 [Visual C++ Redistributable 2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)。

### 5.2 MSVC 运行时处理方案

**方案 A（推荐）**：部署机安装 Visual C++ Redistributable 2022 x64

**方案 B**：将 install_msvc/bin 下的运行时 DLL 随程序打包
```
msvcp140.dll / msvcp140_1.dll / msvcp140_2.dll
vcruntime140.dll / vcruntime140_1.dll
concrt140.dll
```

### 5.3 接收端示例（Python）

```python
import zmq, struct, numpy as np

ctx    = zmq.Context()
sock   = ctx.socket(zmq.PULL)
sock.connect("tcp://127.0.0.1:5556")

while True:
    raw = sock.recv()
    # 帧格式: float32 数组，shape = (n_cards * 2, samples_per_card)
    arr = np.frombuffer(raw, dtype=np.float32)
    print(f"Received frame: {arr.shape}")
```

---

## 六、常见问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| MinGW 编译 ZeroMQ 报 `sys/socket.h: No such file` | MinGW Qt 工具链无 POSIX 头 | 改用 MSVC 编译（本文方案）|
| `cmake` 报 Warning 但 exit code=1 | PowerShell 误把 stderr 的 Warning 当错误 | 只要 `Build files have been written` 即为成功 |
| 链接时找不到 `zmq_ctx_new` | 使用了 MSVC `.lib` 而非 `.dll.a` | 用 dlltool 生成 `libzmq.dll.a` 再链接 |
| 运行时报 `libzmq-v143-mt-4_3_5.dll not found` | DLL 未在 PATH 或 exe 同目录 | 确认 POST_BUILD 拷贝命令执行，或手动拷贝 |
| ZeroMQ 接收进程不在线时崩溃 | `zmq_send` 阻塞 | 使用 `ZMQ_NOBLOCK` 标志（已在代码中设置）|

---

## 七、文件清单

```
D:\Program Files\zeromq-4.3.5\
├── build_msvc\          MSVC 构建目录（可删除，节省 ~500MB）
├── install_msvc\        安装目录（保留）
│   ├── bin\libzmq-v143-mt-4_3_5.dll
│   ├── lib\libzmq.dll.a              ← MinGW 链接用
│   ├── lib\libzmq-v143-mt-4_3_5.lib ← MSVC 链接用（备用）
│   └── include\zmq.h
└── (source files)
```
