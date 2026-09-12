# START Fence 语义修复执行报告

## 任务与基线

- 任务文档：`TASKS/PAimage_START_admission语义修复_20260913-003112.md`
- 基线：`main@f326056ee99e5f9c135635d7e97fe2027ee50fbc`
- 实现分支：`codex/start-admission-fence-fix-20260913-003112`
- 实现提交：`c75e2ee75c343fb9d83085fcdaa80301a3a43b12`
- 本报告目录：`CODEX_REPORTS/start-admission-fence-fix-20260913/`

## 已完成实现

1. `SocketReceiver` 负责生产 START Fence：按卡维护 `AwaitingStart / StartSendPending / StartSent / StartFailed`，在每卡真实 `sendto()` 前后更新边界。
2. 原始 stage1 ingress 仍只记录一次；边界内数据复制到有上限的 hold 队列，START 成功后按原顺序、原始时间和 ingress id 释放一次；发送失败、队列溢出、停止、关闭和新 START 重置均显式记录并 fail-closed。
3. `ControlSocket` 增加生产前后观察回调；发送结果以实际字节数和错误码判定。故障注入 seam 仅在 `PAIMAGE_SOCKET_TEST_SEAM` 下编译。
4. START Fence 决策加入 SourceCore observation/counters，并在诊断 trace 中保留 `(session, card, ingressId, trigger, packet)` 关联。
5. `startup_diagnostics_analyze.py` 增加 START 发送、stage1、stage2 的精确关联，区分 clean、gate drop、fence failed 和 inconclusive；trace 丢失时不作肯定结论。

## 验证结果

以下均在干净工作副本、上述实现分支上执行：

- `python -m py_compile MC_410T_MultiCard/delivery/tools/startup_diagnostics_analyze.py`：通过。
- `startup_diagnostics_analyze_test.py`：通过 clean、gate-drop、failed、incomplete、session-correlation 五类夹具。
- `cmake --build build/tests_release --parallel 2`：通过，179/179。
- `ctest --test-dir build/tests_release/paimage_core --output-on-failure`：通过，11/11。
- `ctest --test-dir build/tests_release -R '^paimage_start_race$' --output-on-failure`：通过，1/1。夹具执行 61 个会话（3 类时序各 20 个，另含一次发送失败恢复）；最新 `result.json` 显示 `passed=true`、`hardSocketErrors=0`、`emulatorSendErrors=0`、`traceQueueDropped=0`、`traceIncomplete=false`，首个会话验证 `rawPre=heldPre=releasedPre=32` 且 `preStartDiscard=1`。
- `ctest --test-dir build/tests_release -R '^paimage_network_test$' --output-on-failure`：通过，1/1。
- `cmake --build build/mingw_debug --parallel 2`：通过，主程序与 `ImagingSvc` 完整链接及 Qt 部署步骤完成。构建使用工作区已有 `_migration_pack/prebuilt_cuda/bin`；缺失的本地 `cufft64_12.dll` 仅作为未跟踪构建依赖临时复制，未进入提交。

整个 `tests_release` 目录的串行 CTest 调度中，17 个既有 Qt 目标报告 Windows `0xc0000135`，而同一目标逐项执行可通过；该现象属于当前 CTest/DLL 运行环境，不影响上述任务相关目标。未进行真实 FPGA/NIC 硬件采集或 system capture 验证。

## 交付约束

- 未修改原始脏工作区，未合并 validation 分支，未改写或强推历史。
- 实现分支已推送到 GitHub；最终交付已核对本地提交 SHA 与远端分支 SHA 一致。
