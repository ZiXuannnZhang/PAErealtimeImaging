# HANDOFF — realtime_imaging_migration 接力手册（新会话唯一入口）

> 更新日期：2026-09-05
> 工作区：`D:\DSHWorkspace\realtime_imaging_migration`
> 当前 HEAD：`8000b76`（模拟器原始数据切分点数按采集时间×源采样率自动计算）
> **重要：工作区有 12 个已跟踪文件的修改，另有交接更名产生的删除/新增，必须先读第 3 节再动手。**
> 本文件已按用户要求由 `HANDOFF_接力手册.md` 更名为 `HANDOFF.md`。

---

## 0. 新会话第一步（先做三件事）

```powershell
# 1. git 是便携版，本机无系统 git
$git = "D:\DSHWorkspace\realtime_imaging_migration\_tools\git\cmd\git.exe"
& $git -C "D:\DSHWorkspace\realtime_imaging_migration" log --oneline -1   # 应为 8000b76
& $git -C "D:\DSHWorkspace\realtime_imaging_migration" status --short      # 见第 3 节
& $git -C "D:\DSHWorkspace\realtime_imaging_migration" branch -a

# 2. 构建/链接前，结束运行中的程序，否则 ld: Permission denied
Get-Process | Where-Object { $_.ProcessName -match 'MC410T_Receiver|ImagingSvc|ring_' }
Stop-Process -Name "MC410T_Receiver","ImagingSvc" -Force -ErrorAction SilentlyContinue

# 3. 关键子文档
#    - 半径标定工作：直接读 RadiusCalibration\HANDOFF.md（独立、未跟踪）
#    - ogprog：只作参考，本轮明确无需审核，不要修改
#    - 实时重建脚本\HANDOFF.md：原 Handoff 文件夹的 MATLAB 参考实现说明
```

当前注册表（2026-09-05 已核实）：
- `HKCU\Software\MC410T\MC410T_Receiver\AcquisitionParams\SampleIntervalNs` = **REG_SZ "4.0"** ✅
- `HKCU\Software\MC410T\MC410T_Receiver\ImagingParams\General\DaqHz` = **REG_SZ "2.5e+08"** ✅

## 0.5 迁移到新工作区（不同路径/不同机器，必读）

把项目内容直接复制到新工作区会丢掉两样东西，导致新会话无法开工：

1. **隐藏目录 `.git` 通常不会被复制** → “当前目录不是 Git 仓库”，无法核实提交号/分支/未提交修改。
2. **`build\` 里的 CMake 缓存写死旧绝对路径** → 复制过去不能直接使用。

修复包已生成在 **`_migration_pack\`**（根目录下，约 231MB + 少量脚本）：

| 文件 | 作用 |
| --- | --- |
| `realtime_imaging_migration.bundle` | `git bundle --all`：master + 两个 backup 分支 + HEAD 的完整历史 |
| `prebuilt_cuda\bin\` | `ring_recon_cuda.dll` / `libring_recon_cuda.dll.a` / `cudart64_12.dll`（b6 源码构建，本轮 CUDA 未改） |
| `迁移修复Git.ps1` | 新工作区执行：`init + fetch bundle + git reset master`，**不覆盖已复制的未提交文件** |
| `迁移重建构建缓存.ps1` | 旧 build 改名保留 → 用新路径 configure `build\mingw_make` → 重建 ImagingSvc / MC410T_Receiver / selftest |
| `迁移后验证.ps1` | 核对 HEAD=8000b76、备份分支、status、CMakeCache 新路径、关键 exe/DLL |
| `迁移包说明.md` / `HANDOFF.md` 副本 | 完整步骤；若新工作区根 HANDOFF 较旧，用包内副本覆盖 |

新会话三步走（详见 `_migration_pack\迁移包说明.md`）：

```powershell
cd <新工作区>
pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移修复Git.ps1      # 无 git 时加 -GitExe "D:\...\git.exe"
pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移重建构建缓存.ps1   # Qt 路径不同时加 -QtRoot "..."
pwsh -ExecutionPolicy Bypass -File .\_migration_pack\迁移后验证.ps1
```

**注意**：如果新机器没有 Qt 6.8.0/MinGW/CUDA 运行时，先安装并传对应路径；不要复制旧 build 缓存来“改路径复用”。
新工作区应保留 `_tools\git`（便携版 git）；若复制时没带，请一并传过去或让脚本用系统 git。

---

## 1. 我们在做什么（任务背景一句话版）

给 **MC410T 环形扫描光声成像**做接近实时的重建接收机：

- 实际采集：532nm/1064nm 双波长交替激发，8 通道同步采集，UDP 载荷已是解调后的时域信号。
- 主程序 `MC410T_Receiver.exe`（Qt/C++，MinGW 构建）负责监听采集、组包、环形实时重建、显示与保存。
- `ImagingSvc.exe` 是 GPU 子进程：ZMQ 信令 + 共享内存，调用 `ring_recon_cuda.dll` 逐块增量 DAS。
- 当前未提交工作集中在两件事：
  1. **每通道独立双波长延时截断（sysDelay）** 重做（v2，无 CUDA ABI 变更）；
  2. **采样率全链路 250MHz 统一**（修掉环形参数窗口显示 200MHz 的问题）。
- 另有一套**独立** MATLAB 脚本 `RadiusCalibration/`：采集前做 8 通道扫描半径自校准。

---

## 2. 工作区地图

| 路径 | 说明 |
| --- | --- |
| `MC_410T_MultiCard/delivery` | **活跃开发版**：MC410T_Receiver + ImagingSvc + ring_recon_cuda 源码 |
| `MC_410T_MultiCard/delivery/build/mingw_make` | **活跃构建目录**（MinGW Makefiles，Debug），产物在 `bin/` |
| `MC_410T_MultiCard/delivery/build/ring_recon_cuda` | CUDA DLL 的 MSVC/Ninja 构建目录（ninja 不要用，见第 12 节） |
| `MC_410T_MultiCard/delivery/docs/` | M2/M3 等设计与验收文档 |
| `PALiveImagingSimSender` | 包外 UDP 数据发送模拟器（同机联调用），最新提交 `8000b76` |
| `实时重建脚本/` | **原 `Handoff/` 文件夹已按要求更名**。M1 版 MATLAB 双波长分块实时重建参考脚本，内有自己的 `HANDOFF.md` |
| `ogprog/` | 线性扫描实时成像**原版参照工程**（源码+编译前端+MATLAB 参考脚本）。**.gitignore 已忽略；本轮明确无需审核其中内容，不要改** |
| `RadiusCalibration/` | 独立 MATLAB 半径自校准脚本（未跟踪）。**详情直接读 `RadiusCalibration/HANDOFF.md`** |
| `testdata/` | `14.dat`（环形测试数据，265,600,000 字节）+ `testdata/01/`（Card{1..4}_Ch{A|B}_test_*.dat 真实保存数据，半径标定冒烟用） |
| `多通道传感器配准重建（多扫描半径）实现方案.md` | 多扫描半径方案与验证记录 |
| `环扫新链路重构设计.md` / `环扫新链路重构报告.md` | 0~5 步链路重构设计与报告 |
| `重建核心基准测试报告.md` / `重建核心基准测试_raw.csv` | 18 例重建核心基准测试记录 |
| `崩溃调查报告/`、`测试截图/`、`显示最大点数改动风险评估.md` | 历史问题分析/截图 |
| `_tools/git/` | 便携版 MinGit，唯一 git 入口 |
| `.harness_probe.txt` | 环境探测临时文件，忽略 |

---

## 3. Git 状态（2026-09-05 快照）

### 3.1 分支与最近提交

```
master  HEAD = 8000b76 模拟器原始数据切分点数改为按采集时间×源采样率自动计算
8000b76 → b6b00e7 → 3e5d379 → c38345a → 8f1e8ad → 450bc88 → ae0dcd8 → fd98556 → ...
```

- `fd98556`：拼接模式边界羽化（δ，默认 1.5°）——拼接 v1 与羽化已落库。
- `3e5d379`：频域显示频率轴换算修复；采样率控件移除、固定 250MHz。
- `b6b00e7`：采样率全链路统一 250MHz，移除 200MHz 模拟兼容（接收端读取注册表默认 4.0ns）。
- `8000b76`：模拟器原始数据切分点数改为 `acqTimeNs × sourceRateMHz / 1000` 自动计算。

保留分支（**不要删除**）：

| 分支 | 内容 |
| --- | --- |
| `backup/cf-dmas-pcf-20260816` | CF 门控 / signed DMAS / pCF 实验（用户曾要求回退，仅备份） |
| `backup/sysdelay-per-channel-20260817` | **每通道 sysDelay 的 v1 实现（`9203c66`，改 `RingReconCudaConfig` 结构体）**。用户因成像问题回退，仅备份。**不要把 v1 的“改结构体”做法合并回来** |

### 3.2 当前未提交改动（12 个已修改文件 + 交接更名）

改动分两组，当前都**未提交**（用户未指示提交）：

**A. 每通道双波长延时截断 v2（无 CUDA ABI 变更的重做）**
- `delivery/include/RingConfigDialog.h`、`delivery/src/RingConfigDialog.cpp`
- `delivery/include/ImagingController.h`、`delivery/src/ImagingController.cpp`
- `delivery/src/ImagingSvc/ImagingSvc.h`、`delivery/src/ImagingSvc/ImagingSvc.cpp`
- `delivery/src/RingReconCuda/ring_svc_selftest.cpp`
- `delivery/docs/M3_环形扫描并行接入说明.md`

**B. 采样率链路 250MHz 归一 + 注册表统一**
- `delivery/src/MainWindow.cpp`、`delivery/include/MainWindow.h`
- `delivery/include/AcqConfig.h`

**C. 交接更名与路径同步**
- `delivery/src/RingRecon/README.md`（`Handoff` 路径改为 `实时重建脚本`）

另外本文件（HANDOFF）自身也有待提交的更新。

**注意**：本轮交接还做了两处更名——`HANDOFF_接力手册.md → HANDOFF.md`、`Handoff/ → 实时重建脚本/`。
`git status` 会显示旧路径为 `D`、新路径为 `??`（未暂存时 git 不显示 rename）；把新旧路径一起
`git add` 后，`git diff --cached --find-renames` 会识别为 rename，属正常现象。

未跟踪（约定不擅自入库）：`_migration_pack/`（跨机器迁移包，见 0.5）、`RadiusCalibration/`、`testdata/`、`测试截图/`、`重建核心基准测试报告.md`、`重建核心基准测试_raw.csv` 等；提交前需用户确认。

---

## 4. 已经完成了什么

### 4.1 已落库的主线（简述）

- 环形实时重建新链路 0~5 步重构完成：SHM v2 强校验、CUDA snapshot/reset、固定双缓冲显示、跨圈复位、超时重置、窗口级 PNG 保存、旧协议清理。
- 多扫描半径配准（`7f13256`）：每通道 `radiusPerChannel[8]` + `multiRadius`，CUDA 逐 A-line 半径反投影；统一半径路径与旧内核逐字节一致，已实机验收。
- 拼接模式 v1 + 边界羽化 δ（`fd98556`）。
- 频域频率轴按 250MHz 固定换算（`3e5d379`）。
- 采样率链路统一 250MHz 与移除 200MHz 模拟兼容（`b6b00e7`）。
- 模拟器切分点数自动计算（`8000b76`）。
- 自动保存、PNG 去重、色标修正等历史功能均已落库。

### 4.2 未提交：每通道双波长延时截断 v2（重做，重点）

**背景/原理**：全系统链路延迟导致各通道时域信号不对齐；对 532nm(wl1)/1064nm(wl2) 分别做延时截断，
截断起点由 `sysDelay` 设置（1-based 采样点）。8 通道采集要求**每通道独立**的 wl1/wl2 延时。

**UI**：
- “重建参数 → 预处理”里的 `sysDelay1(532nm)` / `sysDelay2(1064nm)` 已删除。
- “扫描参数 → 通道勾选与扇区分配”中，`通道x重建半径` 右侧同一行新增 `波长1延时`、`波长2延时`
  QSpinBox（1~100000，默认 358/371；延时始终可编辑，半径仍按配准/勾选联动）。
- 默认值持久化用新键 `RingConfigDialog/Defaults/sysDelayCh{c}Wl1|Wl2`；旧 `sysDelay1/sysDelay2` 保留为回退。

**后端（关键设计：刻意不改 CUDA 结构体）**：
- `RingReconCudaConfig` **保持不变**（`sysDelay[2]` 仍是旧字段，CUDA 核心本就不使用 sysDelay）。
- 主进程新增 `m_ringSysDelayCh[8][2]`，`configureRing(cfg, sysDelayCh)` 接收；
  JSON 新增 `sysDelayPerChannel`（[8][2]），同时保留旧 `sysDelay`（通道1值）兼容。
- ImagingSvc 新增 `m_ringSysDelayCh[8][2]`：优先解析 `sysDelayPerChannel`，
  缺省/不完整时用 `sysDelay` 广播；`processRingPulse` 按物理通道号 `ch[pos]` 取
  `sysDelayCh[phCh][w]` 做 `preprocessBlock`，wl2 的 `dbrmaskExtra = sysDelayCh[phCh][1]-sysDelayCh[phCh][0]`。
- 这就是 v2 与 v1 的本质区别：**v1 改 `RingReconCudaConfig.sysDelay[8][2]` 导致结构体 ABI 变化，
  用户报告成像问题并回退；v2 把每通道延时放在 CUDA 结构体外，不重编 DLL，规避整类 ABI 风险。**

**已验证**：
- 所有通道延时与 b6 基线相同（358/371）时，8 通道 selftest 输出与 b6 基线
  `selftest_out14_b6_revert` **20/20 个 raw 文件 SHA256 完全一致**（`selftest_out14_final_equal`）。
- 仅启用通道0、把通道0延时改 360/373：160/160 输出文件与基线全部不同（参数生效）。
- 仅启用通道0、把未启用的通道5延时改 360/373：160/160 与基线完全一致（按物理通道隔离）。
- 验证数据在 `delivery/build/mingw_make/sdtest3_*`、`selftest_out14_*`。

### 4.3 未提交：采样率链路归一（重点）

**问题根因**（已修复）：注册表历史值 `SampleIntervalNs="5"`（200MHz 模拟时代遗留）使
`RingConfigDialog` 显示 200,000,000 Hz 并把 `daqHz=200e6` 一路下发给重建核心；而真实采集是固定 250MHz。
同时 `ImagingParams/General/DaqHz` 又是 2.5e+08，两个来源不一致。

**修复**：
- 全链路唯一来源改为 `Constants.h::FPGA_ADC_FREQ_HZ=250e6` / `FPGA_ADC_INTERVAL_NS=4.0`。
- `MainWindow::loadSettings` 仍读历史注册表键，但只用于告警日志，不再参与链路；
  发现 5.0 等历史值时强制归一为 4.0/250e6。
- `MainWindow::saveSettings` 保留键、不删除，写回 `SampleIntervalNs="4.0"`（REG_SZ）与 `DaqHz`。
- `AcqConfig.h`、`MainWindow.h`、`RingConfigDialog.h` 默认值改用常量。
- **注册表现在已经核实为 4.0 / 2.5e+08**（应用运行后已回写）。

**已验证**：
- `MC410T_Receiver.exe` 重建成功（日志 `build/mingw_make/_samplerate_chain_fix_build2.log`）。
- UI 自动化实际打开“环形扫描参数设定”窗口读到：`采样率(Hz) = 250000000`、
  采样深度 = `12500`（50000ns ÷ 4ns）。✅

### 4.4 RadiusCalibration（独立工作）

- 独立于实时程序的 MATLAB 半径自校准脚本已完成主体：读取真实保存文件 → 仅做延时截断预处理 →
  角度整数列互相关对齐（不改原始数据）→ 参考通道聚焦/其余通道互相关标定半径。
- **当前卡点**：缺少确认为“每通道全孔径扫描”的标定数据；现有 `testdata/01` 冒烟提示可能不是
  全孔径数据。**详情、文件清单、下一步、坑都读 `RadiusCalibration/HANDOFF.md`，不要重写。**

---

## 5. 当前卡在哪里

1. **未提交改动等待用户验收/提交**。v2 sysDelay 和采样率修复已编译并自测通过，但用户尚未
   指示提交。**新会话不要在未确认前提交**。
2. **RadiusCalibration 被数据阻塞**：需要用户确认并给出真正的“每通道全孔径扫描”标定数据，
   之后按其 HANDOFF 第 5 节顺序跑合成验证 → 小规模冒烟 → 全量标定。
3. 注册表采样率问题已解决（当前值 4.0/2.5e+08），无遗留。
4. 工作区外交付包**按用户明确要求不同步**（此前交付在 `D:\zzx\data\实时重建\...`，
   复制出工作区需要用户批准提权；不要自作主张）。
5. `实时重建脚本`（原 Handoff）是历史 MATLAB 参考实现，不参与 C++ 主线，维护优先级最低。

---

## 6. 下一步计划（建议顺序）

1. **先向用户确认**当前两批未提交改动是否可以提交；若同意，建议拆成：
   - 提交 A：每通道双波长延时截断 v2（无 ABI 变更设计）
   - 提交 B：采样率全链路 250MHz 归一 + 注册表回写
   - 提交 C：文档同步（M3、本 HANDOFF）
2. **真实采集验收 v2 sysDelay**：在环形参数窗口逐通道填入实测延时，跑真实 8 通道成像，
   确认每通道截断位置正确、重建无异常；有问题先怀疑物理通道索引和 `dbrmaskExtra` 符号，
   **不要回到改 CUDA 结构体的 v1 方案**。
3. **RadiusCalibration**：向用户要真正的全孔径数据 → 按其 HANDOFF 做合成真值验证和 3 通道冒烟。
4. **长稳回归**：多圈成像、断流恢复、自动保存、超时重置、拼接+配准+每通道延时组合跑一遍。
5. 若用户要求交付：从 `build/mingw_make/bin` 收集产物并复制到工作区外（需要一次性提权并说明理由）；
   未要求就不做。

---

## 7. 构建手册（当前机器已验证）

### 7.1 MinGW 主程序（最常用）

```powershell
# 先结束运行中的程序！
$env:PATH = "D:\Qt\Qt6.8.0\6.8.0\mingw_64\bin;D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin;D:\Qt\Qt6.8.0\Tools\Ninja;$env:PATH"
Set-Location D:\DSHWorkspace\realtime_imaging_migration\MC_410T_MultiCard\delivery\build\mingw_make
& D:\Qt\Qt6.8.0\Tools\CMake_64\bin\cmake.exe --build . --target ImagingSvc MC410T_Receiver ring_svc_selftest -j 8
```

- 活跃目录只有 `build/mingw_make`；不要用 `build_mingw_debug.cmd`（指向废弃目录）。
- 改 `.ui` 或含 Q_OBJECT/信号的头文件后，若 autogen 不刷新，手动直调：
  `uic.exe xxx.ui -o ui_XXX.h`、`moc.exe XXX.h -o moc_XXX.cpp`。
- 当前产物均已按未提交源码重建：`MC410T_Receiver.exe`、`ImagingSvc.exe`、`ring_svc_selftest.exe`。
- `ring_recon_cuda.dll` **本轮未重编**（v2 设计不碰 .cu/.h），仍是 b6 源码构建的 290,816 字节 DLL。

### 7.2 CUDA DLL（只有改了 `RingRecon/ring_recon_cuda.cu/.h` 才需要）

沙箱下 **ninja 卡死，不要 `cmake --build`**。从 `build/ring_recon_cuda/build.ninja` 提取命令直调：

```bat
call C:\Program\VC\Auxiliary\Build\vcvars64.bat >nul
"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe" ^
  -forward-unknown-to-host-compiler -DRING_RECON_CUDA_EXPORTS -Dring_recon_cuda_EXPORTS ^
  -I<delivery>\src\RingRecon -lineinfo -Xcompiler="-O2 -Ob2" -DNDEBUG -std=c++17 ^
  "--generate-code=arch=compute_89,code=[compute_89,sm_89]" -Xcompiler=-MD -Xcompiler=/utf-8 ^
  -x cu -c <delivery>\src\RingRecon\ring_recon_cuda.cu -o ring_recon_cuda_new.obj

link.exe /nologo ring_recon_cuda_new.obj /out:bin\ring_recon_cuda.dll ^
  /implib:ring_recon_cuda.lib /pdb:bin\ring_recon_cuda.pdb /dll /version:0.0 /machine:x64 /INCREMENTAL:NO ^
  cudadevrt.lib cudart_static.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ^
  ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib ^
  /LIBPATH:"C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/lib/x64"
```

然后**必须**重生成 MinGW 导入库，并把 DLL 复制到 `build/mingw_make/bin`：

```powershell
& D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\gendef.exe bin\ring_recon_cuda.dll
& D:\Qt\Qt6.8.0\Tools\mingw1310_64\bin\dlltool.exe -d ring_recon_cuda.def -D ring_recon_cuda.dll -l libring_recon_cuda.dll.a
```

- **ABI 铁律**：`RingReconCudaConfig` 增删字段后，DLL、ImagingSvc、MC410T_Receiver、selftest、
  verify 必须全部同步重建；否则 `set_defaults` 的 `memset(sizeof cfg)` 越界/漏初始化。
- MSVC `ring_recon_cuda_verify.exe` 同理用 cl/link 直调重编（命令在 build.ninja 里，参考历史
  `build/ring_recon_cuda/ring_recon_cuda_verify_b6.obj` 的构建方式）。
- release `ring_recon_view.exe`（`build/ring_recon_release`）只在 ring_recon_cuda.h 变化时重编，
  用 g++ 直调（历史命令可查会话记录或 build.ninja）。

### 7.3 构建前后检查

- 构建前结束 `MC410T_Receiver.exe`/`ImagingSvc.exe`，否则 `ld: Permission denied`。
- 构建日志写在 `build/mingw_make/_*.log`，完整输出以文件为准。

---

## 8. 验证手册

### 8.1 CUDA 数值自检

```powershell
Set-Location D:\DSHWorkspace\realtime_imaging_migration\MC_410T_MultiCard\delivery\build\ring_recon_cuda
& .\ring_recon_cuda_verify.exe --data D:\zzx\data\20260519\14.dat --id 14 --grid-mm 0.1 --block 200 --out .\verify_out14_check
# 期望：wl1/wl2 snapshot vs get_state worst diff = 0，reset ok
```

### 8.2 服务端到端 selftest

沙箱内 **QProcess 起不了子进程**，必须 `--no-launch 1` + 后台任务先起 ImagingSvc：

1. 后台任务运行：`& .\ImagingSvc.exe`（工作目录 `build\mingw_make\bin`，run_in_background）。
2. 再运行：

```powershell
Set-Location D:\DSHWorkspace\realtime_imaging_migration\MC_410T_MultiCard\delivery\build\mingw_make
& .\bin\ring_svc_selftest.exe --data D:\zzx\data\20260519\14.dat --svc .\bin\ImagingSvc.exe `
  --id 14 --grid-mm 0.1 --block 200 --channels 255 --sector-start 180 --no-launch 1 --out .\selftest_check
```

- 新选项：`--sys-delay-per-ch "d0w1,d0w2,...,d7w1,d7w2"`（16 个逗号分隔整数）。
- 全部相同延时应与 b6 基线逐字节一致；差异回归方法见 `sdtest3_base / sdtest3_perturb0 / sdtest3_disabled5`。

### 8.3 UI 抽查（环形参数窗口）

- 启动 `MC410T_Receiver.exe`（后台任务）→ UIA 找到“成像方式”ComboBox：
  **先 `InvokePattern.Invoke()` 展开，再在 Descendants 里找 `环形扫描` ListItem 并 `Select()`**
  （直接 SetValue 无效）。
- 点击“成像参数”按钮 → 窗口“环形扫描参数设定”：
  - 读 `采样率(Hz)` 文本应为 **250000000**；
  - 采样深度应为 **12500**（DataTime=50000ns / 4ns）。

---

## 9. 关键设计与协议速查

- 环形链路：
  `触发 → RingBlockAssembler 组包 → ring_block_ready → ImagingSvc(CPU预处理 + CUDA append) → snapshot → SHM → UI → 圈末/超时 PNG`。
- 测试数据：`testdata/14.dat` 是 float64 列主序（每列一根 A-line），14.dat=8300 列×4000 点；
  旧路径 `D:\zzx\data\20260519\14.dat`、`D:\zzx\data\20260716\11.dat` 只读可用。
- 每通道每波长每圈 A-line 数 = 单圈总A线数 ÷（启用通道数×2）。
- sysDelay：1-based 采样点截断起点；wl2 的 `dbrmaskExtra = sysDelayCh[ch][1]-sysDelayCh[ch][0]`；
  按 SHM 通道号数组 `ch[pos]` 取物理通道，不按遍历下标。
- 每通道延时 JSON：`sysDelayPerChannel` = `[8][2]`；旧 `sysDelay` = `[2]` 兼容广播。
- **绝对不要在 `RingReconCudaConfig` 里放每通道 sysDelay**（v1 教训）。
- 采样率：全链路固定 `FPGA_ADC_FREQ_HZ=250e6`；注册表历史值只检测/回写，不参与计算。
- 多扫描半径：`radiusPerChannel[8]` 已在 CUDA 结构体内；未配准时统一通道1半径 6.57mm。
- 拼接：仅配准模式有效；`spliceBlendDeg` 服务端 clamp 到半扇区宽以内。

---

## 10. RadiusCalibration（一句话 + 指向）

- **是什么**：独立 MATLAB 脚本，采集前做 8 通道全孔径扫描 → 估计每通道真实扫描半径与起始角度偏移，
  结果填入实时程序的“配准模式”。
- **当前**：代码主体完成；**卡在缺少确认的全孔径标定数据**。
- **怎么做**：完整读 `RadiusCalibration/HANDOFF.md`（2026-09-05 版），里面有文件清单、运行方式、
  下一步顺序和 10 条“绝对不要再踩的坑”。本文件不重复展开。

---

## 11. `实时重建脚本`（原 Handoff）与 `ogprog`

- `实时重建脚本/`：**原 `Handoff/` 文件夹已按要求更名**。内容是 M1 版 MATLAB
  双波长分块实时重建最终脚本包（`Ringscan_DAS_loop_realtime_dual.m` 及配套函数），
  自带 `实时重建脚本/HANDOFF.md`。内部 `cd('Handoff')` 已改为 `cd('实时重建脚本')`；
  其测试数据路径仍引用旧 `DASredo/testdata`，若实际运行请先改成 `..\testdata\14.dat`（未改动脚本本身）。
- `ogprog/`：线性扫描实时成像**原版参照工程**（含原版源码/编译前端与 MATLAB 参考脚本）。
  本项目环形链路以它为参照移植。**.gitignore 已忽略该目录；本轮用户明确说无需审核其中内容，
  只在交接文档里说明即可——不要修改、不要提交、不要深挖。**
- `PALiveImagingSimSender/`：独立 UDP 模拟发送器，同机联调用；其源码已入库，
  最新提交 `8000b76` 让源数据切分点数按“采集时间×源采样率”自动计算。

---

## 12. 绝对不要再踩的坑（新会话必读）

1. **每通道 sysDelay 不要放回 `RingReconCudaConfig`。**
   v1（备份分支 `9203c66`）改了 `sysDelay[8][2]` 结构体，用户报告成像问题并回退；
   v2 用主进程/服务端独立 `m_ringSysDelayCh[8][2]` + JSON `sysDelayPerChannel`，
   CUDA DLL 不动。以后继续按 v2 思路改。
2. **改了 `RingReconCudaConfig` 字段 = 必须同步重建所有 ABI 消费者**（DLL、svc、主程序、
   selftest、verify、viewer），并重新 `gendef`/`dlltool`。
3. **不要用 ninja / `cmake --build` 构建 CUDA 目录**：沙箱命名管道禁用，ninja 0 CPU 卡死。
   用 vcvars64 + nvcc + link 直调（见 7.2）。
4. **构建前必须杀进程**：`MC410T_Receiver.exe`/`ImagingSvc.exe` 会锁输出文件，
   报 `ld: Permission denied`（本会话就遇到过）。
5. **selftest 在沙箱必须 `--no-launch 1`**：QProcess 被拒（`CreateFile failed`）。
   外部先起 ImagingSvc；selftest 内置 1.5s 等 ZMQ 重连，否则 `timeout at block 0`。
6. **注册表采样率键不要删除、不要在沙箱里再试 `reg add`**：此前该命令被沙箱拒绝且提权被用户拒绝。
   正确做法是让程序 `loadSettings` 归一、`saveSettings` 回写（现在注册表已是 4.0/2.5e+08）。
7. **永远不要把 200MHz（5ns）重新引入链路**：真实采集固定 250MHz；历史 `SampleIntervalNs=5`
   只作为告警检测，不参与采样深度/显示/重建。
8. **UIA 自动化设置 QComboBox**：先 `InvokePattern.Invoke()` 展开，再从 Descendants 找
   `环形扫描` ListItem `Select()`；直接 `ValuePattern.SetValue` 无效。
9. **RadiusCalibration 的坑见其自己的 HANDOFF**，最关键三条：
   不要对原始数据做 circshift（只能改角度向量）；不要把 `testdata/01` 当成全孔径标定数据下结论；
   角度位移符号用 `offsetDeg = -shift*stepDeg`，改完必须合成数据验证。
10. **不要动 `ogprog/`**：只作参照，已 gitignore；本轮用户明确无需审核。
11. **不要擅自从工作区外向里/向外同步交付包**：工作区外不可写；需要复制必须一次性提权且用户批准；
    被拒就放弃，不换方式重试。当前交付包同步明确不做。
12. **不要擅自提交**：`RadiusCalibration/`、`testdata/`、报告截图等未跟踪，提交需用户确认；
    当前 12 个跟踪文件改动也等用户确认后再 commit。
13. **文件夹更名**：`Handoff` → `实时重建脚本`；根文档 `HANDOFF_接力手册.md` → `HANDOFF.md`。
    任何旧脚本/文档再写 `cd('Handoff')` 都要改。

---

## 13. 工作原则

- 最简可实现；先给方案，用户确认再动手。
- 每完成一部分及时汇报；
- 遇到沙箱/权限问题先查系统指导解法；仍受阻与用户同步或一次性提权。
- 后续功能改进都在 C++ 主线；MATLAB 脚本（`实时重建脚本/`、`RadiusCalibration/`）按用户明确要求维护。
- 不删除工作区外文件；不修改用户未要求动的代码路径。
