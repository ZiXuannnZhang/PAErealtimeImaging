# 系统接收诊断增强实现报告

日期：2026-09-12  
工作树：`D:/ChatGPT/PAERealtimeImaging/_system_capture_diagnostics_20260912`  
分支：`codex/system-capture-diagnostics-20260912`  
基线：`9d0fd7a84b3725a3cfa6e1aefb9be2559add0cda`

## 结论

软件侧交付已完成。应用逐包原始入口证据、接收循环证据、受控 Pktmon/WPR 状态机、应用握手/轮询、系统文件统一导出和统一分析器已经接线；普通接收路线仍由原 WinSock 接收线程执行，诊断增强不改变控制命令、启动策略、装配和保存路径。

当前账户没有管理员令牌，因此真实 Pktmon/WPR 流程未执行。软件状态为 `passed`；本机真实抓取为 `not_verified`；Realtek USB 10GbE 实机为 `awaiting_user_test`。合成系统抓取只证明状态机和解析链路，不替代实机结论。

## 已实现内容

### LoopLog

`LoopLog` 改为固定容量 MPSC 队列加后台窗口 writer。滚动保留 64 MiB，BurstMark 冻结前后最多 5 秒；首次来包、监听器尚未启动、重叠 burst、窗口/内存截断、队列丢弃、写盘未完成、预算省略、保留窗口淘汰和尾余量分别记录。无 burst 时不持续写接收循环二进制，只保留低频摘要/生命周期/控制记录；冻结预算 256 MiB 且不覆盖旧窗口。80 字节记录 ABI 不变，Schema 2 追加字段，旧 Schema 1 可由分析器读取。

### 系统抓取

新增 `system_capture_common.ps1`、`start_system_capture_admin.ps1`、`stop_system_capture_admin.ps1`。状态按 `idle → preflight → starting → ready → collecting → stopping → validating → complete` 推进，失败、权限不足、工具失败、忙、部分、无目标包等分类保留。状态、命令记录和 manifest 原子写入，命令记录含参数数组、退出码、stdout/stderr、超时、UTC/QPC 起止。

系统抓取只在应用握手匹配 `trialId`、token、run/listen/session、目标 IP、8000–8004 端口和独立输出目录后进入 ready；Pktmon 过滤器按本次精确登记，停止时无法确认归属不做整体清理。默认采集 all/flow/drop、128 字节、memory、256 MiB；WPR 使用 `GeneralProfile`。`-NoWpr` 是显式降级，manifest 会标记 WPR skipped。停止阶段保留 ETL、PcapNG、文本、drop-only、WPR ETL、clock anchors 和验证摘要。

### 应用与导出

应用菜单新增“准备系统抓取”，状态栏每 500 ms 显示 app-only/running/captured/verified/failed 等状态；应用不自动提权。burst 通知包含 monotonic 时间、run/listen/session/token，只是时间标记，不作为成功结论。统一诊断导出把 `system-capture` 目录复制进 ZIP，manifest 保留字节数和 SHA-256，并区分未完成系统抓取。

### 分析器与时间戳实验

新增 `startup_diagnostics_analyze.py`，复用既有 trace/LoopLog/PcapNG 解析器，输出逐包入口 IP/端口/trigger/packet/length、首包数字、间隔、四卡启动一致性和 25 ms 节奏、前后两秒、每秒诊断写/队列/loop gap、ETL 停止 I/O、QPC/UTC/steady 锚点、round 存储损失和证据结论矩阵；不跨 round 关联 16 位编号。

新增默认关闭的 socket timestamp 实验接口：`off/software/hardware/auto`，通过 `WSARecvMsg`、`SIO_TIMESTAMPING`、`SO_TIMESTAMP` 能力/回退状态记录；只改本 listener 拥有的 socket，不修改系统级配置。当前默认值保持 `off`。

## 验证证据

已通过（合并后的最终源码状态）：

* `ctest --test-dir MC_410T_MultiCard/delivery/build/paimage_core --output-on-failure`：11/11 通过，包含 2 秒 UDP socket replay、trace、worker、protocol、conversion、LoopLog 检查及采集卡发现组件 2 项测试；
* `ctest --test-dir MC_410T_MultiCard/delivery/build/tests_release`：29/30 通过；唯一失败 `paimage_trace_bundle_test` 在基线 `52ddf3d` 上可复现，属既有问题，与两轮现场修复无关；
* 系统抓取合成正向：`pktmon+wpr`，完成、PcapNG 可解析、目标 UDP 包存在、`analysisReady=true`；
* 系统抓取合成 `NoWpr`：`pktmon_only_wpr_skipped`，目标 UDP 包存在；
* 普通权限负向：状态 `failed`，原因明确为需要管理员权限；
* 已完成状态重复 stop：退出码 0；
* PowerShell 全部脚本解析通过；
* `startup_diagnostics_analyze.py` 使用应用 trace + 合成系统抓取运行通过，产生 13,440 个应用入口包、2 个系统目标包、5 项证据矩阵，未发现存储轮次损失。

以上 C++ 测试均针对包含采集卡发现修复与系统抓取修复的合并源码执行。

限制：第二轮脚本改动（活请求校验、会话级过滤器与 WPR 具名实例、drop-only PcapNG、BOM）在本工作站未完成 pwsh 功能级合成回归——pwsh 7 启动持续挂起（本会话与并行代理会话均复现），Windows PowerShell 5.1 又因执行环境存在 `Path`/`PATH` 双写而无法运行 Start-Process，自定义环境块启动被子进程策略拒绝。脚本已通过 PowerShell 解析器语法检查并由人工审阅；第一轮已验证的合成结果保留在 `verification/`。建议在 pwsh 7 正常的终端复跑 `verification/run_system_capture_synthetic.ps1` 与 `run_system_capture_synthetic_no_wpr.ps1` 完成功能级回归。

## 现场反馈后的补充修复

部署包现场首次运行发现，应用状态字段使用 `captureSessionToken`，启动脚本复用已有状态时误读旧字段 `sessionToken`，在严格属性检查下导致启动异常。现已改为兼容读取两个字段，并将拒绝状态统一写为 `captureSessionToken`。

同时修正外部工具输出编码：本机 `pktmon.exe` 重定向输出为 UTF-8，脚本此前按默认编码读取会把中文“没有运行”变成乱码并误判为 `unknown`。命令适配层现固定使用 UTF-8；目标部署包脚本语法、SHA-256、已有状态合成场景和 NoWpr 合成回归均通过。

无参数管理员入口不再按文件时间选"最新"请求，而是解析通道内请求并校验活性：请求字段完整、`active` 为真、`applicationPid` 进程仍存活且创建时间不早于进程出生时间、可执行文件一致。上一轮失败会话遗留的旧请求不再被误选，无活请求或存在多个活请求都会明确失败，提示在应用中重新准备系统抓取。输出目录只允许全新目录，或属于同一 `captureSessionToken` 且状态为 `failed`、未取得任何资源的失败预检续做；活动或已使用过的 trial 一律拒绝覆盖。

应用侧新增 `invalidateSystemCaptureRequest`：读到失败或过期状态时使旧请求失效，避免状态栏停留在指向已失效会话的"未准备"；导出按 trial 上报 `systemCaptureTrials`（含 `inProgress`、`analysisReady`、`manifestMissing`），跳过 `.tmp` 与会话锁文件。

会话资源命名加入会话身份：Pktmon 过滤器带 `StartupDiag-<token>` 前缀逐名登记，WPR 以具名实例 `StartupDiag-<token>` 启动、查询和停止。停止前重新核实过滤器集合与状态完全一致，归属不符时记录 `ownership_unknown`，不做无归属的 `pktmon stop` 或整体 filter remove。drop-only 证据改为 `pktmon etl2pcap --drop-only` 生成 `pktmon-drop.pcapng`，与主 PcapNG 一样解析目标协议头与地址，且仅在确认抓取停止后转换。

为兼容含中文正则的脚本，`system_capture_common.ps1`、`stop_system_capture_admin.ps1` 及测试脚本补写 UTF-8 BOM；`Open-AdminCapture.cmd` 在非零退出时显示失败原因并提示重启应用后重新准备系统抓取。

## 与采集卡误识别修复的合并

并行任务在同一工作树的新分支 `codex/card-discovery-config-ack-20260912` 上提交了 `52ddf3d`（本任务受测基线快照）与 `56f0174`（动态发现仅凭 60 字节 CONFIG ACK 接纳采集卡）。本轮以 `56f0174` 为基础提交系统抓取修复，两条工作线在同一分支合并为线性历史；交付 exe 为合并后源码的 Release 构建（`mingw_release/bin`，含 cufft/Qt/MinGW 运行库闭包），交付包脚本与工作树工具逐字节一致。

应用目标 `PAimageReceiverDiagnostics.exe` 已完成 C++ 编译和链接。源码构建的完整 post-build 步骤曾因工作区缺失的 `cufft64_12.dll` 中断；这是现有成像运行时依赖缺失，不是本次新增源文件的编译/链接错误。随后使用本机已有且已核验的成像运行库、Qt/MinGW 运行库组装异机部署包，完成 PE 依赖闭包和 SHA-256 校验；这不等同于 CUDA/Realtek 实机正向验收。

## 不在本次结论内

未执行管理员真实 Pktmon/WPR、Realtek 实机采集、50 μs 试验、硬件/驱动/网卡/电源/亲和性/线程优先级改动，也未把合成包存在解释成硬件链路不丢包。任何缺少应用存储记录的 round 按存储损失处理；系统包缺失只作为系统层证据不足，不反推卡/NIC/线缆根因。
