# 系统接收诊断增强实现清单

日期：2026-09-12

## 工作边界

- 独立 worktree：`D:/ChatGPT/PAERealtimeImaging/_system_capture_diagnostics_20260912`
- 分支：`codex/system-capture-diagnostics-20260912`
- 基线提交：`9d0fd7a84b3725a3cfa6e1aefb9be2559add0cda`
- Git common dir：`D:/ChatGPT/PAERealtimeImaging/_session_boundary_stage_20260908/.git`
- 主工作目录存在大量用户未提交修改；本任务不在主工作目录编辑，也不覆盖基线 worktree。
- 不扫描大体量 DAT、压缩包，不启动 GUI，不执行长时间实机采集，不操作采集卡控制命令。

## 已确认的上轮事实与缺陷

- 上轮只完成非管理员负向系统抓取验证；真实管理员 Pktmon/WPR 正向链路未验收。
- 既有脚本只在等待结束后写状态，过滤器 add 逐条退出码未形成失败门控，停止时使用整体 `filter remove`，并以启动时空过滤器推断停止时归属。
- burst 关联只使用通知 wall time，没有校验 burst 自身的 monotonic 时间；非空 ETL/转换成功不等于目标 UDP、时间覆盖或调度证据有效。
- 既有 LoopLog 是从监听开始持续写入 256 MiB 文件；预算记录 80 字节不整除时会留下尾余量，未保存窗口外的缺口可能被旧分析器误读。
- 应用已有逐包 trace、timing、基础诊断 ZIP 和 `system-capture-channel`，但没有系统抓取状态机握手、统一系统产物导出和明确的系统状态展示。

依据：

- `CODEX_REPORTS/启动段有效数据缺失_本地验证报告_20260911.md`
- `CODEX_REPORTS/启动段有效数据缺失_实机操作卡_20260911.md`
- `MC_410T_MultiCard/delivery/tools/startup_ingress_capture.ps1`
- `MC_410T_MultiCard/delivery/tools/startup_ingress_capture_stop.ps1`
- `MC_410T_MultiCard/delivery/tools/paimage_trace_analyze.py`
- `MC_410T_MultiCard/delivery/tools/startup-looplog-schema.md`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/LoopLog.h`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/SocketReceiver.h`

## 环境与权限快照

- 当前账户：`LAPTOP-BUQLMV81\\CodexSandboxOffline`
- 管理员令牌：`false`；因此本次不启动真实 Pktmon/WPR 抓取，不弹 UAC。
- PowerShell：`C:/Program Files/PowerShell/7/pwsh.exe`，7.6.6
- MinGW：`D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin`，GCC/G++ 13.1.0，GNU Make 4.2.1
- Qt：`D:/Qt/Qt6.8.0/6.8.0/mingw_64`，Qt 6.8.0
- CMake：`D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe`（未加入当前 PATH）
- Python：`C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`
- `pktmon.exe`、`wpr.exe` 可见于 `C:/Windows/System32`，但当前权限不足以完成真实抓取。
- 未安装依赖、未联网、未改驱动/网卡/电源/亲和性/线程优先级策略。

## 交付与验收清单

- [x] LoopLog 独立滚动窗口：64 MiB、最长 5 秒、BurstMark 前后窗口、首次来包、重叠 burst、预算守恒和新 schema。
- [x] 受控系统抓取状态机：原子状态、权限/工具错误分类、过滤器归属、握手、NoWpr 降级、幂等 stop、转换与验证。
- [x] 应用端系统抓取请求/500 ms 状态展示/统一导出，诊断模式不改变普通采集路径。
- [x] `startup_diagnostics_analyze.py` 统一应用 trace、窗口 schema、system-capture 和 rounds-json 分析。
- [x] 默认关闭的 socket timestamp 实验接口及能力/回退记录。
- [x] Release 编译、普通权限模拟验证、UDP 短回放、导出接线检查和负向权限验证。
- [x] 交付目录、操作卡、实现报告、artifact-manifest 和 SHA256；真实本机抓取标记为 `not_verified`，Realtek 实机标记为 `awaiting_user_test`。

## 结果边界

- 应用目标已完成 C++ 编译与链接；标准发布后处理在复制环境缺失的 `cufft64_12.dll` 时停止，因此本地二进制标记为 `compiled_linked_postbuild_incomplete`，不冒充完整 Release 包。
- 9/9 `paimage_core` CTest、系统抓取 WPR 正向合成、NoWpr 合成、无管理员权限负向和重复 stop 幂等均通过。
- 本机管理员令牌为 `false`，未执行真实 Pktmon/WPR；真实抓取状态为 `not_verified`。
- Realtek USB 10GbE 实机验证为 `awaiting_user_test`，不与合成抓取结果混淆。
