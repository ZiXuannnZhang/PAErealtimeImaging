# HANDOFF_接力手册（新对话入口）

> 更新日期：2026-08-15
> 最近代码提交：`7f13256`（环形扫描多通道传感器配准重建·多扫描半径，已验收）
> 工作区：`D:\DSHWorkspace\realtime_imaging_migration`（DSH 会话可写根）
> 说明：本手册以工作区内文件为准；沙箱禁止写工作区外，不再维护 `D:\ChatGPT` 等外部副本（旧版手册的"同步提交"已废止）。

## 0. 最重要：工作区与沙箱（新会话第一步）

- 当前可写根 = `D:\DSHWorkspace\realtime_imaging_migration`。文件策略 workspace-write：**只能写工作区**，
  不能写 `D:\ChatGPT\...` 等外部路径。
- 新会话先确认：
  - `git log --oneline -1` = `7f13256`
  - `git status --short` 仅有未跟踪文件：`14.dat`、`测试截图/`、`环扫新链路重构报告.md`、`环扫新链路重构设计.md`
    （这些按约定不入库）
- git CLI 为便携版（MinGit 2.55.0.4）：`_tools\git\cmd\git.exe`（本机无系统 git）。若缺 git 且需重装，
  走明文 HTTP 镜像（沙箱 HTTPS 不可用）：
  `http://mirrors.tuna.tsinghua.edu.cn/github-release/git-for-windows/git/LatestRelease/MinGit-2.55.0.4-64-bit.zip`
- 沙箱注意事项与构建限制见 **第 7 节**，务必先读，避免在 ninja/管道上浪费时间。

## 1. 项目结构

| 目录/文件 | 说明 |
| --- | --- |
| `MC_410T_MultiCard/delivery` | 开发版（当前活跃），主程序 MC410T_Receiver + ImagingSvc |
| `PALiveImaging/delivery` | 全真采集交付包源码（与开发版同步，约定暂不构建） |
| `PALiveImagingSimSender` | 包外数据发送模拟器（同机 UDP 联调验收） |
| `ogprog` | 线性扫描实时成像原版工程（环形移植参照实例） |
| `Handoff` | M1 MATLAB 环形 DAS 参考脚本（DAQ=200e6） |
| `14.dat` | 环形测试数据（工作区根，265,600,000 字节，未跟踪） |
| `HANDOFF_接力手册.md` | 本手册（新对话入口） |
| `环扫新链路重构设计.md` / `环扫新链路重构报告.md` | 链路重构设计/参考报告 |
| `崩溃调查报告/`、`显示最大点数改动风险评估.md` | 历史问题分析 |
| `测试截图/` | 前端/弹窗验证截图 |

## 2. 当前进度

### 2.1 主线：环扫新链路重构（已完成）

| 提交 | 内容 |
| --- | --- |
| `d74846f` | 第 0 步：SHM v2 布局 + 强校验 |
| `7bdfcdd` | 第 1-2 步：CUDA snapshot/reset API + svc 快照链路 |
| `36c5a3f` | 第 3 步：固定双缓冲显示、跨圈复位、超时重置、前端效果调整 |
| `e1343ca` | 第 4 步：窗口级 PNG 保存（圈末/超时到点，1600×1600 ARGB32） |
| `74315c2` | 第 5 步：清理旧协议与死代码（重构完成） |

### 2.2 重构后功能与优化（`74315c2` 之后，已全部落库）

| 提交 | 内容 |
| --- | --- |
| `6eb0141` | 监听程序修改：环形参数"设为默认"启动生效；重建显示顺时针展开且首通道保持 9 点；平铺显示频域窗提前隐藏 |
| `f023656`/`098e5cd`/`cbf3882`/`e2c5d1e` | 信号/成像显示性能优化（自适应采样、批量布局、曲线 1px 无抗锯齿、32×32 分块转置） |
| `73e2223` | 成像/信号两条显示链路解耦（快照转换移入 QThreadPool） |
| `73d091b`/`b82091f` | 显示功能批量修改：峰峰值统计范围联动、坐标轴双击编辑、色标范围记忆 |
| `730decc`/`5b84d38` | 色标颜色与数值映射修正 + 视觉方向调整（白=上限/黑=下限） |
| `8487c22`/`74dd583` | PNG 保存去重（圈末与超时到点两种保存互斥） |
| `4faa4fa`/`ba7f807`/`15f08cf`/`d02e040` | 自动保存功能：自动保存勾选、会话代精确分界、会话 PNG、A+B+C 修复（边界落盘/重启恢复/未注册目录丢弃） |
| `7f13256` | **多通道传感器配准重建（多扫描半径）**——最新，已实机验收 ✅ |

### 2.3 多扫描半径配准（`7f13256`，本轮已完成验收）

背景：环形扫描相干成像用 8 通道传感器阵列缩短物理扫描路径提速；因装配公差各通道传感器实际旋转
半径不同，旧程序全部按同一半径重建，影响相干合成质量。

改动内容（4.1→4.6）：

- **前端**（`RingConfigDialog.h/.cpp`）：环形参数弹窗"扫描参数"页：
  - "启用通道"上方新增 **"配准模式（多扫描半径重建）"** 勾选框；
  - 通道勾选下方新增 **8 个"通道N重建半径"输入**（mm，0.1~50，4 位小数，默认 6.57）；
  - 联动：勾选配准模式 → 勾选通道的半径输入可编辑；不勾选 → 仅通道1可编辑，其余只读，
    全部通道统一使用通道1半径；
  - "重建参数"页原 Radius 输入**已删除**；旧设置键 `radiusMm` 自动迁移为通道1默认值。
- **配置/链路**：`RingReconCudaConfig` 新增 `radiusPerChannel[8]` + `multiRadius`；
  JSON 下发/解析（`ImagingController.cpp`、`ImagingSvc.cpp`，未配准时全部回填统一半径）。
- **CUDA**（`ring_recon_cuda.cu/.h`）：新增 `ring_recon_cuda_append_angles_radii(handle, bscan, nt, nd,
  thetaDeg, radii)` API；内核 `ring_das_kernel` 改逐 A-line 半径：`Rj=radius[j]`、`R2j=Rj*Rj` 核内逐线计算，
  `detx/dety` 按 `Rj` 计算，`bothIn` 改核内 `(R2j<=rb2)`（删除 host `sIn`/`d_sIn`），
  `w=wscale[j]*dotp/(Rj*dsafe^p)`。radii=nullptr 时全部用 `h->R`，统一半径路径与旧内核**逐字节一致**。
- **svc**（`ImagingSvc.h/.cpp`）：`processRingPulse` 按 SHM 通道号数组构造逐 A-line 半径向量
  `wlRad[2]`，与 wl1/wl2 展平同序（含 wl2 跨块前插对齐，新增 `m_ringPrevRadius[8]`），
  调用新 radii API。
- **验收记录**：
  - 内核 A/B（git 提取 `36c5a3f` 旧源码编译参考 DLL）：新旧 DLL 统一半径 acc/accW 逐字节一致
    （单声速 + 分层声速两场景，各 57600 像素）；
  - 真实 14.dat 端到端：新旧 verify 输出 **242 个文件全部逐字节一致**；
  - svc 端到端（8 通道 2 圈 10 块）：统一半径跑通；多半径（每通道 ±0.04mm 偏差）跑通、
    输出与统一半径差异显著且无 NaN/Inf；
  - 用户已实机验收配准模式效果 ✅。

### 2.4 最终链路（未变）

```
采集触发 → RingBlockAssembler（按勾选通道组包，触发级超时检测）
→ ring_block_ready → ImagingSvc（CUDA 逐块 append_angles_radii，双波长 d_acc/d_accW）
→ 每块 snapshot：acc/accW 归一化 3600² 帧 → SHM 全分辨率区
→ ring_snapshot_ready → 接收端 worker（固定双缓冲，consumed 握手 latest-wins）
→ ringSnapshotReady(seq, index) → 显示窗口 → 圈末/超时到点 → 保存 PNG
```

## 3. 数据链路基本事实（采集侧，未变）

- 1 触发 = 8 通道同步 = 8 根 A-line；双波长由外部 LabVIEW 交替出光；
  UDP 载荷为已解调时域信号（int16/int32 → float 透传，不二次解调）。
- 端口：每卡 8001+cardIndex；4 卡 × 2 通道 = 8 物理通道。
- SHM v2：`RingImagingShmHeader` version=2；显示区=全分辨率区；create/attach 强校验。
- 协议：`ring_block_ready`、`ring_snapshot_ready`、`ring_reset`（`ring_frame_ready` 已移除）。
- 测试数据格式：float64（8 字节/采样）列主序，每列 = 一根 A-line；
  14.dat = 8300 列 × 4000 采样（wlOffset=301 + 8000 A-line 双波长合计）；11.dat 为 4151 列。

## 4. 构建与验证（沙箱内实测可用流程）

### 4.1 主程序（MinGW，开发版）

活跃构建目录为 `MC_410T_MultiCard/delivery/build/mingw_make`（MinGW Makefiles 生成器，Debug，
Qt 6.8.0 mingw_64）。**不要用** `build_mingw_debug.cmd`（指向已废弃的 mingw_debug 目录）。

```
$env:PATH = "D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin;D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin;$env:PATH"
cd MC_410T_MultiCard\delivery\build\mingw_make
& mingw32-make.exe -j8 [ImagingSvc|MC410T_Receiver|ring_svc_selftest|ring_udp_replay]
```

- 若 make 重新触发 `*_autogen` 并报 libuv spawn 失败：把所有 `*_autogen` 目录内文件时间戳改为
  当前时间（touch）跳过 autogen 重跑。
- 改 `.ui` 或含信号的头文件后需手动直调 Qt 工具重生成：
  `uic.exe xxx.ui -o ui_XXX.h`、`moc.exe XXX.h -o moc_XXX.cpp`（历史上
  `moc_RingColorBarWidget.cpp`、`ui_MainWindow.h` 过期曾导致 undefined reference / 控件缺失）。
- 构建前先结束运行中的 `MC410T_Receiver.exe` / `ImagingSvc.exe`（输出文件被锁 → `ld: Permission denied`）。

### 4.2 CUDA DLL 重建（改 `ring_recon_cuda.cu/.h` 或 config 结构后必做）

沙箱下 **ninja 不可用（卡死 0 CPU）**，从 `build/ring_recon_cuda/build.ninja` 提取命令后直调：

```
# 编译（MSVC host 编译器 + CUDA 12.8，sm_89）
cmd /c "call C:\Program\VC\Auxiliary\Build\vcvars64.bat >nul && ^
  "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe" ^
  -forward-unknown-to-host-compiler -DRING_RECON_CUDA_EXPORTS -Dring_recon_cuda_EXPORTS ^
  -I<delivery>\src\RingRecon -lineinfo -Xcompiler="-O2 -Ob2" -DNDEBUG -std=c++17 ^
  "--generate-code=arch=compute_89,code=[compute_89,sm_89]" -Xcompiler=-MD -Xcompiler=/utf-8 ^
  -x cu -c <delivery>\src\RingRecon\ring_recon_cuda.cu -o <build>\ring_recon_cuda\ring_recon_cuda_new.obj"

# 链接 DLL（同样在 vcvars64 环境内）
link.exe /nologo ring_recon_cuda_new.obj /out:bin\ring_recon_cuda.dll ^
  /implib:ring_recon_cuda.lib /pdb:bin\ring_recon_cuda.pdb /dll /version:0.0 /machine:x64 /INCREMENTAL:NO ^
  cudadevrt.lib cudart_static.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ^
  ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib ^
  /LIBPATH:"C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/lib/x64"
```

然后**必须重新生成 MinGW 导入库**（否则 MinGW 链接报 undefined reference）：

```
gendef.exe bin\ring_recon_cuda.dll          # 生成 ring_recon_cuda.def
dlltool.exe -d ring_recon_cuda.def -D ring_recon_cuda.dll -l libring_recon_cuda.dll.a
```

- **ABI 提醒**：`RingReconCudaConfig` 增删字段后，svc、MC410T_Receiver、selftest、verify 必须与
  DLL 同步重建（`set_defaults` 的 `memset sizeof(*cfg)` 按编译进各二进制的大小执行，
  新旧混用会越界写/未初始化）。
- MSVC 版 `ring_recon_cuda_verify.exe` 同理需用新 header 重编（cl/link 直调，命令同 build.ninja）。
- LNK4098（LIBCMT 冲突）警告为 cudart_static 常态，可忽略。

### 4.3 验证工具

- `ring_recon_cuda_verify.exe --data <14.dat> --id 14 [--grid-mm 0.1] [--block 200] [--out <dir>]`：
  CUDA 数值自检（snapshot==get_state 归一化、reset 全零），逐块输出 wl*/acc/accw raw。
- `ring_svc_selftest.exe --data <14.dat> --svc <ImagingSvc.exe> --id 14 --grid-mm 0.1 --block 200
  --channels 255 --sector-start 180 [--no-launch 1] [--radius-per-ch "6.57,6.55,..."] [--out <dir>]`：
  8 通道扇区模型端到端驱动真实 svc（ZMQ+SHM），两圈逐块对比。
  - **沙箱必加 `--no-launch 1`**：QProcess 在沙箱内无法启动子进程；
  - `--radius-per-ch`（毫米）下发多扫描半径，空=统一半径。
- A/B 逐字节回归（内核改动后）：git 提取改动前源码编译参考 DLL 再对比，见 7.4。
- 测试数据：工作区根 `14.dat`；旧路径 `D:\zzx\data\20260519\14.dat`、`D:\zzx\data\20260716\11.dat`
  仍存在（只读可用）。
- 回放：`ring_udp_replay.exe --data <14.dat> --id 14 --bits 32 --rate 100 --triggers 3000`。

## 5. 重要参数

- 采样率：250 MSa/s（真实）/ 200 MSa/s（回放）；注册表
  `HKCU\Software\MC410T\MC410T_Receiver\AcquisitionParams\SampleIntervalNs`（4.0=250M，5.0=200M）。
- 重建网格：默认 gridSize=10μm → 3600×3600（显示=重建矩阵，每像素=10μm）。
- 超时重置：环扫参数弹窗"超时重置(0=关闭)"，单位秒。
- 每通道每波长每圈 A-line 数 = 单圈总 A-line 数 /（启用通道数 × 2）。
- **多扫描半径**：配准模式勾选=各通道各自半径；不勾选=统一通道1半径（mm，默认 6.57，对应 6.57e-3 m）。

## 6. 沙箱环境注意事项（新会话必读）

1. **文件策略 workspace-write**：可写 `D:\DSHWorkspace\realtime_imaging_migration` 及部分临时区；
   工作区外（如 `D:\ChatGPT\...`）**不可写**。不要尝试同步/写入外部副本；遇到拒绝按系统提示
   一次性提权（`sandbox_permissions`，需一句话 justification，由用户批准；被拒则放弃该操作，
   不要换方式重试）。
2. **命名管道禁用**：任何"捕获子进程输出管道"的方式都会失败：
   - ninja 构建**卡死（0 CPU）**——不要用 ninja/cmake --build；
   - cmake AutoMoc 报 `libuv process spawn failed: operation not permitted`；
   - Node `child_process` pipe stdio 报 EPERM。
   - **可行方式**：pwsh 内直接 `& exe`（stdio 继承：g++/nvcc/moc/uic/cl/link 均验证可用）；
     后台任务用 `run_in_background: true`（启动 ImagingSvc 等长驻进程可行）。
3. **程序内部 QProcess 被拒**：`ring_svc_selftest` 内 QProcess 启动 svc 报
   `CreateFile failed (拒绝访问)`。已加 `--no-launch 1` 参数：svc 由外部后台任务先启动，
   selftest 只连 ZMQ。
4. **ZMQ 重连时序**：外部 svc 先启动时 connect 未建立；selftest bind 后必须等 ~1.5s 再发
   configure（`--no-launch` 模式已内置等待），否则连接建立前的 configure 被丢弃，
   表现为 `timeout at block 0`。
5. **PowerShell 语言模式**：只读命令运行在 ConstrainedLanguage（.NET 静态调用/Add-Type/COM 受限，
   核心 cmdlet 与核心类型可用）；workspace-write 下为 FullLanguage。
6. **网络**：HTTPS 不可用，下载走明文 HTTP 镜像。
7. 环境事实通过 `$env:DSH_*` 变量暴露，需要时可查询。

## 7. 重要问题与解决方案（历史教训）

| 问题 | 现象 | 解决方案 |
| --- | --- | --- |
| ninja 沙箱卡死 | 构建 0 CPU 挂起 | 改用 `build/mingw_make`（MinGW Makefiles）+ `mingw32-make -j8` |
| cmake AutoMoc | `libuv process spawn failed` | 跳过 autogen（touch `*_autogen` 时间戳）；改头文件含新信号或改 .ui 后手动直调 `moc.exe`/`uic.exe` 重生成 |
| QProcess 沙箱拒绝 | `CreateFile failed (拒绝访问)` | selftest `--no-launch 1` + 外部后台启动 ImagingSvc |
| ZMQ configure 丢失 | selftest `timeout at block 0` | bind 后等待 svc 重连（≥1.5s）再发 configure |
| CUDA 导入库未更新 | MinGW 链接 `undefined reference` | `gendef` + `dlltool` 重生成 `libring_recon_cuda.dll.a` |
| config 结构 ABI 不一致 | 越界写/未初始化字段 | 改 `RingReconCudaConfig` 后 DLL、svc、主程序、verify、selftest 全部同步重建 |
| 输出文件被锁 | `ld: Permission denied` | 构建前结束 MC410T_Receiver.exe / ImagingSvc.exe（用户已授权） |
| moc 过期 | `undefined reference to RingColorBarWidget::rangeEdited` | 手动 `moc.exe` 重生成后入库 |
| uic 过期 | 控件缺失（如 chkAutoSave） | 手动 `uic.exe` 重生成 `ui_MainWindow.h` |
| 测试数据缺失 | verify/selftest 报 read failed | 14.dat 已放工作区根（未跟踪）；旧路径 `D:\zzx\data\...` 仍可用 |
| 历史崩溃（QList::at 等） | 堆被写坏后的随机受害者 | 真凶在帧/重建链路；0~5 步重构已消除旧路径；再现时先查 SHM 校验、双缓冲握手、CUDA ABI |

## 8. 待办与下一步

1. **新会话将开展"另一个改动方向"**（具体内容由用户在新会话中说明）——本手册为其交接基础。
2. 重构后真实采集/长时稳定性回归（多轮成像、断流恢复、自动保存验证）。
3. 交付包（`PALiveImaging/delivery`）同步与构建需用户明确要求。

## 9. 工作原则

- 最简可实现；不为极小概率情况做兜底。
- 每完成一部分及时同步进度，由用户复核推进方向；先出设计、用户确认后再动手。
- 遇到权限/环境问题先按本手册第 6/7 节处理；仍受阻再与用户同步或一次性提权。
- 后续所有改进在 C++ 上进行，MATLAB 脚本不再维护。
- 用户约束：未明确指示时**不改原程序日志反馈文本**；不删除工作区外文件；改动前先复核影响面。
- git 提交用中文消息；`14.dat`、`测试截图/`、两份报告 md 保持未跟踪。
