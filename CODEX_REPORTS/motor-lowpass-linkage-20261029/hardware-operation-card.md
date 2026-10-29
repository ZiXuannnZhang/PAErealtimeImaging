# 低通滤波与电机联动联合实机操作卡

使用交付包的完整 `bin/`，记录包目录、Git SHA、`BuildIdentity.h` 和依赖哈希；原运行配置及采集数据保留。新包只带最小电机联动 INI，实际采集参数、通道、半径、延时、声速和网格请使用已确认的现场值。

## 接线和参数

1. LabVIEW 打开正在操作的工程和主 VI。发送端桥接的 `outPath` 指向接收端 exe 目录下的 `motor_scan_params.json`。接收端最小 INI 使用 exe 目录默认路径，不导入异机绝对路径。
2. 发送端 `bridge_config.txt` 的 `dirTrueIsCcw=false`；接收端 `DirectionInverted=false`。面板逆应写 `CCW/xDir=false`，顺应写 `CW/xDir=true`。改语义时只翻转一侧映射。
3. 接收端 `[MotorScanLinkage]`：`Enabled=true`、`TotalAlinesRule=directionOnly`、`TriggerLatch=true`、`PollMs=500`。只联动方向，步数只用于观察，不自动改圈长。源码仍保留显式选择的步数换算模式。
4. 桥接日志必须显示当前工程面板 `FPWinOpen=True`；仅有旧 JSON 文件不算活链路。包内 sender-bridge-reference 是 handoff 的参考文件，需要现场已有的 LabVIEW、Python/pywin32 环境；启动脚本和路径按现场调整。
5. 选择环形成像，打开参数窗口时应能看到当前联动状态。参数窗口打开期间自动联动暂缓，关闭后在空闲期应用；不要通过手动参数应用来模拟闩锁。

## 联合验收

按“时变低通关/FilteredDAS关、开/关、关/开、开/开”四组执行；每组记录配置、JSON、日志和相同实验对象的图像。

| 场景 | 预期 | 保留证据 |
|---|---|---|
| 无参数文件或联动禁用 | 原环形重建与低通设置仍可用；状态准确 | 启动日志、参数截图 |
| 逆向活动面板 | CCW；九点钟起点 180° | 桥接日志、实时 JSON、motor_linkage_applied、图像 |
| 顺向活动面板 | CW；三点钟起点 0°；方向与实物位置一致 | 同上，必须是活动桥接，不能只用旧快照 |
| startupFilter=0，扫描中翻向 | 本轮不改方向/不重启，提示已闩锁 | physical_round_started、状态、service PID/日志、图像 |
| startupFilter=1 或现场过滤值 | 同样闩锁，过滤/计数口径不变 | distinct trigger、RoundIdentity、保存计数 |
| 仅超时边界模式 | 超时后开放；下一轮前应用最终值一次 | timeout_boundary_reset、motor_linkage_applied 时序 |
| 计数边界模式 | CountBoundary 后开放，不必等超时 | count_boundary_reset、参数应用时序 |
| 冻结期间多次翻向 | 只保留最终方向，轮间有足够空闲时生效 | JSON seq、状态、应用事件 |
| 修改步数（directionOnly） | 圈长/设计触发数不变 | totalAlines=0、计数、原始文件大小 |
| 打开参数窗口后修改面板 | 不自动提交滤波草稿，关闭后等待安全时机应用 | 状态、滤波参数前后截图 |
| 停止/重启成像服务 | 单纯停止服务不释放活动轮闩锁；滤波配置保留 | service 日志、四组开关快照 |
| 停止监听后重新监听 | 闸门与新会话恢复，首触发再次冻结 | session、RoundIdentity、应用记录 |

多通道拼接和非拼接均需观察顺逆成像，确认无整扇区裁空或错位。硬件图像镜像、真实采集稳定性、实时 T_arr、保存完整性和扫描中切换行为由本次现场证据判定；自动测试通过不能代替这些结果。

`paimage-diagnostics` 中保存 `runtime.log` 和 `events.jsonl`，同时保留桥接日志与测试期间 JSON，结果按场景逐项记录 PASS/FAIL/未执行。若轮间隔小于轮询周期，参数可能延后到之后的空闲轮间隔，应看到持续闩锁而非扫描中途应用。
