# CODEX_REPORTS/ring-enhance-budget-20260927-151919 — 环形重建增强实时预算测试

任务：`TASKS/环形增强实时预算测试_20260927-141217.md`（测量任务，只测不改生产代码）
分支：`codex/ring-reconstruction-enhancement-20260926-181824`，测试代码 tip `162b712`
（父链 `4bf72c9` ← `ee8049e`（待测实现提交）← main `c95917f`）；main 侧基线 `c95917f`。
两侧 BuildIdentity、依赖哈希、tracked-clean 证明见 `git-receipt.txt` / `dependency-sha256.txt`。

## 证据索引

| 文件 | 内容 |
| --- | --- |
| `git-receipt.txt` | 两侧 exact SHA、BuildIdentity、tracked-clean、生产源码 diff=ee8049e 零改动证明 |
| `dependency-sha256.txt` | ring_recon_cuda.dll / cudart / pa_recon_core / cufft / libzmq / ImagingSvc.exe / selftest / 14.dat 的 SHA256（四棵构建树交叉记录） |
| `layer1_summary.csv` | 微基准汇总（debug 两轮 + release 两轮，矩阵行明细见 raw/bench_*.log 的 CSV 行） |
| `layer2a_summary.csv` | 五配置单元 × 指标（debug）+ release 复核三单元 + 敏感性替代跑 |
| `raw/block_stats_debug.txt` / `raw/block_stats_release.txt` | 逐块耗时（第 1 圈预热剔除后 25 块）统计 |
| `raw/memcmp_main_vs_branch_off.txt` / `raw/memcmp_release_main_vs_branch_off.txt` | 全关逐位比较结果（各 60 文件 / 0 差异） |
| `criteria_s8_substitution.txt` | §8 预注册公式代入过程（Debug 主口径 + MinGW-Release 复核口径 + 敏感性 + 每 A-line 预算） |
| `layer2b_blocked.md` | Layer 2b 未执行原因（GUI+CONFIG-ACK 阻塞）与用户在场复跑命令 |
| `raw/out_*/` | 各单元逐块帧（**未入库**，见 .gitignore 与 frame_manifest.txt） |
| `raw/layer2a_*.log`, `raw/rel_*.log`, `raw/bench_*.log` | 全部原始运行日志（含 RingSHMObs 周期/final 观测 JSON 全文） |
| `build_main_c95917f.log`（main 工作树）/ `build_branch_*.log`（分支克隆） | 构建日志（main 侧在其工作树内，路径见 receipt；此处收集分支侧副本） |

## 硬件与负载

```text
CPU    : Intel Core i9-14900HX（24C/32T）
GPU    : NVIDIA GeForce RTX 4060 Laptop GPU 8188 MiB（驱动 610.88）
内存   : 31.7 GB
系统   : Windows 10.0.26200 x64（Git Bash 会话）
工具链 : Qt 6.8.0 mingw_64 / MinGW g++ 13.1.0 / Ninja / CMake（Qt Tools，路径=D:\Qt\Qt6.8.0\…，与 BUILD_STANDARD 预设一致）
```

负载状态（每次测量前 PowerShell `Win32_Processor.LoadPercentage` 采样，非受控实验室条件，如实记录）：
Layer 1 debug run1=4%、run2=12%；Layer 2a debug：main=20%、off=28%、bipolar=31%、freq=36%、both=15%；
release bench run1=11%、run2=20%；Layer 2a release：main=（未采样，与 bench run2 同期）；
两轮同口径差异已分别落在 `layer1_summary.csv`（极差列）与 `layer2a_summary.csv`（min/max 列），
未挑选任何一轮。

## 数据源（E3 口径）

- 实录数据：`D:\ChatGPT\PAERealtimeImaging\testdata\14.dat`
  SHA256 `fbcbc105343d8caf8d1b231a93cf00cdbf4710f94a19b93b9f1a86f812e9e056`
  （8300 列 × 4000 双精度样本；id=14 口径 wlOffset=301、alinesPerFrame=8000、360°、8 通道）。
- `11.dat` 本机不存在；Layer 1 的 3830 点矩阵行使用 14.dat 实录列截取（丢弃前 170 样本
  = DelayCut(sysDelay=171) 保留段语义），`11.dat` 的 wlOffset=151 口径未使用。
- 长度 12330 > 实录列深 4000：**合成数据**（高斯导数 N 形脉冲 + 0.5–60 MHz 随机相位
  带限噪声，RMS 对齐实录列 908.17），生成代码见 `tests/ring_enhancer_bench.cpp::syntheticLine`，
  种子固定可复现。此为任务文档 §4 预授权的"无实录数据才用合成"路径（按长度维度）。
- Layer 2a 全部单元使用 14.dat 实录数据。

## 口径与统计

- 主口径 mingw-debug（正式记录）；辅口径 MinGW-Release（**替代**预注册的 msvc2022-release，
  见偏差 1）。两口径数值禁止互比（任务文档 §3）。
- Layer 2a 每单元 6 圈（第 1 圈预热不计入，测 5 圈 × 5 块 = 25 块）；敏感性替代跑每单元
  4 圈（预热 1 + 测 3）。中位为主，均值/极差同报。
- `svc_avg_process_us` 为 RingSHMObs 会话累计均值（每单元独立会话），作为 §8
  `median_process_us` 的可用口径；逐块中位另见 block_stats（驱动侧含等待/拷贝）。
- RingSHMObs `block_copy_rejected` 与全部 anomaly 计数在所有单元均为 0（各日志 kinds 行）。

## 与任务文档的偏差（全部显式记录，未静默改口径）

1. **msvc2022-release 不可用**：本机 Qt 仅有 mingw_64 kit（无 msvc2022_64），且无 VS2022
   安装。§3/§8 的 release 复核以 **MinGW-Release（同源码、同编译器家族、O2）替代**；
   该替代口径下 §8 两判据 PASS（见 criteria 文档）。正式的 msvc 复核需另寻环境。
2. **Layer 2b 未执行**：接收端 GUI + 真卡 CONFIG-ACK 前置，无人值守不可达
   （原因与复跑命令见 `layer2b_blocked.md`）。T_arr 用设计节拍代入，结论为条件性。
3. **敏感性块大小 400 非法**：alinesPerFrame=8000、8 通道时 8000/(8×400)=2.5 非整数块
   （组包约束 K % (block/2) = 500 % 200 ≠ 0）。已用 block=100（T_arr=2.5 s 口径）替代，
   block=250/500 亦可作为后续补跑点；未强行运行非法配置。
4. **11.dat 缺失**：实录列全部取自 14.dat（wlOffset=301）；Layer 1 长度 3830 的
   DelayCut 语义按 id-11 的 sysDelay=171 换算（4000−171+1=3830），与 14.dat 的
   sysDelay(358) 无关——该长度是任务文档指定的矩阵维度。
5. **驱动二进制统一**：五配置单元（含 `main`）一律使用分支构建的 `ring_svc_selftest.exe`
   驱动（同一驱动二进制保证输入流一致），`--svc` 分别指向 main/分支构建的 ImagingSvc.exe；
   `main` 单元不注入 `ring.enhance` 字段。
6. **测试代码提交链**：ee8049e 之上有两个 test-only 提交（`4bf72c9` 基准与驱动扩展、
   `162b712` ZMQ 就绪加固——修复冷启动下 configure 被丢弃导致的 block-0 超时）。
   生产源码相对 ee8049e **零改动**（receipt 中 diff=0 证明）。
7. **帧文件命名扩展**：驱动逐块帧名加入圈号（`svc_wl1_r<gen>_<seq>.raw`），多圈不互相
   覆盖；测试代码改动，生产路径不受影响。

## 证据四层标注（沿 D2）

```text
source/code correctness          有限声称：全关 memcmp 60/60（debug+release 两口径）+ CTest 46/46
automated tests/build/selftest   构建成功（两侧四 exe 齐全）、基准可复跑（附命令与退出码）、CTest 46/46 PASS
real hardware validation         仅真实 GPU/CPU 上的性能实测（Layer 1/2a）；不等于实机采集验证
hardware root-cause attribution  未声称
```

## 复跑命令

```text
# 构建（两侧各自 exact SHA，tracked-clean）
cd <tree>\MC_410T_MultiCard\delivery && cmd /c build_mingw_debug.cmd
# tests 工程（短路径二进制目录，规避 Windows 260 字符限制）
cmake -S tests -B <short-path>\reb -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=D:/Qt/Qt6.8.0/6.8.0/mingw_64 \
  -DCMAKE_C_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe \
  -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6
ctest --test-dir <short-path>\reb            # 46/46（需 PATH 含 Qt/MinGW bin）
# Layer 1（实录数据矩阵）
ring_enhancer_bench.exe --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat --rounds 5 --warmup 20 --lines 64
# Layer 2a（五单元；<svc> 按 receipt 指向 main/分支构建；单元×5 + 敏感性见 layer2a_summary.csv）
ring_svc_selftest.exe --data D:\ChatGPT\PAERealtimeImaging\testdata\14.dat --id 14 \
  --grid-mm 0.01 --block 200 --rounds 6 [--enhance off|bipolar|freq|both] \
  --svc <ImagingSvc.exe 路径> --out <out 目录>
# 注意：每轮运行前确认无残留 ImagingSvc 进程（否则 PAIR 对端被抢占 → block-0 超时）
# Layer 2b：见 layer2b_blocked.md（需用户在场）
```
