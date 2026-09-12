# 验证报告

## 工作树与构建

- 固定基线：`9d0fd7a84b3725a3cfa6e1aefb9be2559add0cda`，独立分支 `codex/ring-pipeline-refactor-20260912`。
- Debug/Release 均用 Qt 6.8.0、MinGW 13.1.0、CMake/Ninja 配置并构建；`USE_ZEROMQ=ON` 使用工作区本地 ZeroMQ，CUDA import/bin 使用任务书迁移包。
- 基线核心构建日志为 `stage-a-baseline-build.log`，完成 `[182/182]`；基线原始 CTest 的 17 个失败是未注入 Qt/MinGW DLL PATH 的启动错误 `0xc0000135`，不是源码断言失败。
- `build_refactor.ps1` 和 `test_refactor.ps1` 将 DLL PATH、配置、构建、单测和日志固定化，避免该环境误报。

## 回归

`test-summary.json` 记录 30 项测试，30 passed、0 failed。覆盖 ring block、RoundTracker、IPC v3、旁路/隔离、旧网络/会话/诊断、PAimage core checks、保存与输出、LoopLog、协议和转换检查。

关键直接结果：

- `ring_block_assembler_test`：PASS，固定位置、缺口、尾块和显式 wavelength。
- `round_tracker_test`：PASS，回绕、乱序、idle/reset、oracle 和 epoch 隔离。
- `ring_shm_v3_test`：PASS，布局/offset、双槽状态、generation、越界拒绝。
- `paimage_core/paimage_worker_checks.exe`：PASS，保存边界和启动策略。
- `paimage_core/paimage_looplog_checks.exe`：PASS，预算、保留和失败字段。
- `paimage_host_output_test`：PASS，A/B、index commit、manifest、accepted/written 计数。

## 端到端模拟自测

`ring-svc-selftest-v3-final.log` 使用工作区已有 `testdata/14.dat`，启动 Debug `ImagingSvc.exe`，配置 `block=50`，执行两轮、40 blocks。末尾观察值为：`notifications=40`、`consumed=40`、`mismatch=0`、`ready_zero=0`、`duplicate=0`、`gap=0`、producer `submitted=40`、`slot_busy=0`、`last_seq=39`；进程 exit code 0。此前的完整日志保留在 `ring-svc-selftest-v3.log`。

这证明了本地模拟输入到 v3 服务消费的协议闭环和无覆盖计数；它不证明真实采集卡、实时网卡、FPGA 时序或 CUDA 硬件性能。

## 制品

打包目录：`artifacts/RingPipelineRefactor`。其中 `bin` 包含主程序、`ImagingSvc.exe`、`ring_svc_selftest.exe`、`ring_udp_replay.exe`、CUDA/Qt/ZeroMQ 运行库和插件；`build-manifest.json` 记录 baseline、实现 HEAD、编译器、Qt、IPC v3、依赖和 SHA256。

## 结论

源码级和本地模拟级验收通过；真实硬件验收保留为下一步，执行清单见 `hardware-run-card.md`。
