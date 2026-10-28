# motor-scan-linkage-20261014 — LabVIEW 电机参数联动（源码实现报告）

> 执行环境：实机（LabVIEW 2015 + aa9097e 预编译包所在机器）。
> 本报告为本地工作区副本（GitHub main 解压）上的实现记录，尚未提交/推送，
> 尚未在本机构建（该机无 Qt/MinGW 工具链）。
>
> **2026-10-29 状态更新**：内容已随
> `codex/motor-scan-linkage-direction-20261029-0048` 分支提交到远端（基于
> `origin/main` a8c15cf 重建的分支），构建/测试已在 Qt 6.8.0 + MinGW 13.1
> 工具链上执行，见下文「测试与构建状态」。后续工作（触发闩锁、计数重置开闸、
> 起点角联动、方向映射统一）见
> `CODEX_REPORTS/motor-scan-linkage-direction-20261026/README.md`。

## 任务

LabVIEW 上位机控制程序的电机扫描方向（顺/逆）与电机步数（X Steps Num）
下发给 PAimageReceiverDiagnostics，读取后按扫描方向实时重建。

## 接口设计

- 外部参数文件 `motor_scan_params.json`（formatVersion 1）：
  `direction`（"CCW"/"CW"，也接受 "逆"/"顺"）、`xStepsNum`、可选 `xStepsPerPeriod`、
  `seq`/`writtenAt`/`source`。写入方为实机侧 ActiveX 桥接（不属本仓库），
  也可由 LabVIEW VI 内直写文件（格式相同）。
- 消费端 `MotorScanLinkage`（新模块）：MainWindow 构造时按 INI
  `[MotorScanLinkage]`（Enabled/ParamsFile/DirectionInverted/TotalAlinesRule/PollMs，
  均有默认值）创建监视器，500ms 轮询（沿 System Capture 状态文件口径），
  有效变化时经环形参数对话框走既有 applyConfig 下发路径。

## 方向（sectorCcw）消费点

`RingReconCudaConfig.sectorCcw` 原为休眠字段（硬编码 1，全程传递、无消费）：

1. `RingBlockAssembler::configure` 新参 `sectorCcw`（默认 1 = 旧行为）：
   A-line 角度 = sectorStart + s*sectorWidth ± k*step（顺时针取负，
   整圈图像为逆时针结果的镜像；扇区分配 s 项与方向无关）。
2. `ImagingSvc::processRingPulse`：拼接扇区锚点 `sectorAnchorIdx = ccw ? s : s-1`
   （CUDA 按 floor((phi-sectorStart)/sectorWidth) 划分像素扇区，顺时针覆盖区间
   落在 s-1 扇区，锚点不前移则拼接掩膜裁错扇区）。
3. `RingConfigDialog::config()` 的 sectorCcw 改由联动方向写入
   （`setMotorScanDirection`），无联动输入时保持默认 1（现状行为）。

## 步数（xStepsNum）→ 单圈总A-line数

默认规则 `perEnabledChannel`：totalAlines = XStepsNum × 启用通道数。
物理口径：电机每步 1 次激光触发，每圈触发数 = XStepsNum；每触发每启用通道
1 根 A-line。现场数值验证：2000×2=4000，与既有实机配置 totalAlines=4000、
X_steps_per_Period=2 一致。备选规则 `perPeriod`（XStepsNum×X_steps_per_Period）
与 `directionOnly` 经 INI 切换。写入前过 `validateMotorScanTotalAlines`
（与 applyConfig 整除约束同口径），不通过只记录 `motor_linkage_rejected`。

## 变更文件

- 新增 `include/MotorScanLinkage.h`、`src/MotorScanLinkage.cpp`、
  `tests/motor_scan_linkage_test.cpp`；
- 修改 `include/src RingBlockAssembler`、`src/ImagingSvc/ImagingSvc.cpp`、
  `include/src RingConfigDialog`、`include/src MainWindow`、
  `CMakeLists.txt`、`tests/CMakeLists.txt`。

## 测试与构建状态

- **2026-10-14 本地验证构建已执行**（实机新装 Qt 6.8/MinGW 工具链，
  `build_mingw_debug.cmd` 退出码 0，四个 exe + windeployqt 产物齐全，
  构建/测试日志归入本目录 evidence/（build.log、ctest-final.log 等，
  2026-10-29 归档时由 delivery 根目录移入，内容未改）。
- tests 工程按仓库惯例单独 configure（build\tests_mingw_debug），全量 CTest
  **52/52 通过（100%）**，含新增 motor_scan_linkage_test。首轮 6 项失败为
  环境问题（UDP 8000 被旧部署进程占用，释放后复跑通过，见
  evidence/ctest-full.log / ctest-rerun1..3.log）；motor_scan_linkage_test 首轮失败
  为测试自身对首圈末行 kNext 回绕的错误假设（既有组包器语义为首圈不回绕，
  生产镜像逻辑正确），已修正测试期望并复跑 PASS——生产代码未改动。
- **运行冒烟验证通过**：bin 目录放入 motor_scan_params.json 后启动主程序，
  runtime.log 出现"电机联动已启用/已联动/已下发"，events.jsonl 记录
  motor_linkage_applied（direction=逆时针(逆), xStepsNum=2000,
  totalAlines=16000=2000×8 默认通道, totalAlinesRule=perEnabledChannel）。
  证据：build\mingw_debug\bin\paimage-diagnostics\run-20261014T191546840-*\。
- 本机构建为**验证构建**（非 git 工作副本，BuildIdentity 无 SHA）；正式交付
  仍须按 BUILD_STANDARD 在开发机 exact commit 重新 configure/build。

## 验收建议（硬件）

1. 逆时针扫描与旧版逐位一致（sectorCcw=1 行为保持）；
2. 顺时针扫描重建图像镜像正确（若镜像方向不符，INI DirectionInverted 翻转）；
3. XStepsNum 变化后每圈设计触发数自动跟随（诊断事件 motor_linkage_applied）；
4. 运行中切换方向/步数走既有自动重启流程，轮次身份无串扰。
