# 主窗口启动拒绝修复回执

审核对象为 `18b318236bdca26574304179b84e5d77d1f490c0`，继续同一实现分支
`codex/ring-timevar-lowpass-20261016`，不合并 main、不改写历史。
执行前 fetch 并读取最新 origin/main 的 PROJECT_STATUS、REPOSITORY_BASELINE、BUILD_STANDARD、HANDOFF，
以及本地双端协作指南、开发工具使用指引与原任务书
`4687ebb61f827d100720220f0f1f5119bb7af948:TASKS/环形旧增强移除与时变低通接入_20261016-132206.md`。
origin/main 仍为 `a8c15cff203da959bd935120605e69dc32ff7145`；本地和远端实现分支起点均等于审核对象。
已有 `.vscode/settings.json` 和 `delivery/tests/CMakeUserPresets.json` 不纳入提交、不修改。

## 修复与状态边界

MainWindow.cpp 的 `onRealtimeImagingToggled()` 在 `applyConfig()` 返回 false 时立即返回。
用 QSignalBlocker 取消实时成像勾选，恢复复选框及成像参数按钮、采集时间、A/B 延时控件。
追加 `imaging_start_rejected / ring_config_invalid` 诊断动作。
此前尚未设置新的 m_imagingEnabled、重启意图、ready 等状态，也未安装服务 ready/timeout 回调；
拒绝路径保持这些状态、状态文字和已接受配置，不调用停止槽或 stopSvc。
因此不会初始化/重置组包器、清空/开启馈送、创建 SHM 或进入 startSvc，也不会误停常驻服务。
有效配置继续走原启动、停止和重启流程。

生产行为变更仅为上述拒绝分支。MainWindow/ImagingController 中的 friend 和调用计数仅在
`MAINWINDOW_START_TEST_SEAM` 下编译，生产目标没有该宏。没有改变滤波数值、默认值、顺序、
采集/save/timeout/RoundIdentity、RingBlockAssembler 补零逻辑或 CUDA ABI。

## 真实入口集成测试

`delivery/tests/mainwindow_start_integration_test.cpp` 使用真实 MainWindow 构造函数、连接、RingConfigDialog、
ImagingController、RingBlockAssembler、ImagingBypass 和 ImagingSvc 子进程/共享内存/GPU。
可选目标从生产 MC410T_Receiver 读取完整 SOURCES、includes、defines 和 links，只替换 main()；
测试通过真实复选框 `setChecked(true/false)` 发出信号，进入生产 `onRealtimeImagingToggled()`，不复制启动逻辑。
独立 `startup-isolated` 输出目录隔离 INI，不覆盖现场配置。

覆盖：

1. 启用滤波，N=5000、p2=2000 经主窗口启动成功，再经真实 UI 停止。
2. 采集时间缩短到 6000 ns（N=1500），p2 保持 2000，再次启动被拒绝。
   四个函数入口计数证明未调用组包器初始化、馈送启动、服务启动/停止；SHM/进程指针不变。
   组包器非空 sentinel 不变，证明没有重置；完整 CUDA 配置字节、逐通道延时、新滤波快照保持。
   草稿 enabled/p2 不变，复选框恢复不额外发出 toggled，全部参数入口可操作。
   继续处理事件 5.2 秒，确认没有遗留启动 timeout 回调改变状态。
3. 用户修改 p1=500、p2=1400 后，从同一个 UI 入口正常启动/停止，接受 N=1500。
4. 真实服务启动成功后，仅把 UI/馈送请求置为 inactive，保留真实 resident 子进程与 SHM；
   再次执行非法 UI 请求，验证 PID、SHM、running/ready、配置、重启意图和状态文字均不变。
5. 清理 resident 服务后，再用有效启用配置从 UI 正常重启/停止。

## 已执行命令、结果与最终交付

工具路径使用 `D:/Qt/Qt6.8.0` 下 Qt 6.8.0 / GNU 13.1.0 / CMake 3.30.5 / Ninja 1.12.1。
独立 tests 工程显式指定原任务使用的 Python，保留 51 个测试；主窗口集成测试在标准工程另计 1 个。
执行环境包括 Qt/MinGW PATH、QT_QPA_PLATFORM=offscreen、Qt plugins 路径，以及本任务独立 TEMP/TMP。

已执行的开发阶段命令（相对仓库根目录；cmake/ctest 为上述工具链绝对路径）：

```powershell
# cwd: MC_410T_MultiCard/delivery
cmake --preset mingw-debug -DBUILD_MAINWINDOW_START_TEST=ON
cmake --build build/mingw_debug --target mainwindow_start_integration_test -j 8
# cwd: repository root
cmake -S MC_410T_MultiCard/delivery/tests -B MC_410T_MultiCard/delivery/build/timevar_tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe -DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6 -DPython3_EXECUTABLE=C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
cmake --build MC_410T_MultiCard/delivery/build/timevar_tests -j 8
ctest --test-dir MC_410T_MultiCard/delivery/build/timevar_tests -V
ctest --test-dir MC_410T_MultiCard/delivery/build/mingw_debug -V -R '^mainwindow_start_integration_test$'
```

configure/build 均 exit=0。51/51 回归 exit=0（47.88 秒），包括数值 fixtures 36 组合/327 列、
128 OFF 基线 fingerprints、服务、UI/INI、前端、显示、save、count-boundary、PhysicalRound/timeout/stale/assembler。
数值 worstNormalizedAbs=5.03385e-13，floatWorst=0。
新增真实入口集成测试 1/1 exit=0（22.26 秒），五项场景全部 PASS。
第一次 CTest 调用早于增量链接完成，exit=8 / BAD_COMMAND / Not Run，无断言执行；
等待构建 exit=0 后重跑通过。原失败输出与成功输出均保留在 evidence，不将未运行计为通过。

最终提交后执行完整标准入口 `cmd /c build_mingw_debug.cmd`（缓存中仅额外启用独立集成测试目标），
在 exact final SHA 重新 configure/build。随后运行：

```powershell
python CODEX_REPORTS/ring-timevar-lowpass-20261016/start-rejection-fix/final_delivery.py validate
git push origin codex/ring-timevar-lowpass-20261016
python CODEX_REPORTS/ring-timevar-lowpass-20261016/start-rejection-fix/final_delivery.py package --build-log artifacts/ring-timevar-lowpass-start-fix/build-final.txt
```

脚本记录实际绝对命令、cwd、exit_code、耗时和 exact SHA；顺序运行新增测试、51 项回归、
新 OFF 回放与历史 exact baseline 的 20 个双波长 raw 文件逐位对比、ON identity barrier、
incomplete-round、2 秒 timeout（有意停 2.5 秒）回放。拒绝非零退出、stale BuildIdentity、
tracked dirty、runtime 差异、服务/工具二进制差异或 local/remote HEAD 不一致。

最终回执入口为本地 `artifacts/ring-timevar-lowpass-start-fix/final-validation.json`、
`final-build-receipt.json`、`final-delivery.json`。正式交付目录为
`artifacts/build-delivery/<timestamp>_<final-short-sha>/`，包括完整 bin、BuildIdentity、manifest、
git receipt、五项依赖哈希、全包文件哈希、构建日志、validation 及本次追加证据。
确切 final SHA、包路径和各命令结果以这些提交后生成的回执为准。

原始回执与性能/硬件操作卡均保留。此次不重复未变更服务/滤波的性能矩阵；
packager 校验服务和回放工具二进制与原任务验证时 SHA256 相同，五项 runtime 也相同。
Ring CUDA 仍为 `_migration_pack/prebuilt_cuda`，未重编；cudart v12.8 与既有 NVIDIA cuFFT 12.1.0.31
依赖组合原样保留。实机视觉、真实采集链路和真实 T_arr 仍为 UNVERIFIED，不扩大原验收结论。
