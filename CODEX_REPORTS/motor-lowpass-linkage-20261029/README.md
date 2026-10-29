# 低通滤波基线上的电机联动移植

用户要求：以最新低通滤波分支为基线，在新分支移植电机参数联动并修复问题，之后联合实机验收；不修改 main 或原低通滤波分支。

## 来源与范围

- 新分支：`codex/ring-lowpass-motor-linkage-20261029`。
- exact 基线：`3340fbff5ed93ff2ba87b496275e663e877fa693`，来自远端 `codex/ring-timevar-lowpass-20261016`。
- canonical main 仍为 `a8c15cff203da959bd935120605e69dc32ff7145`；本任务明确覆盖“默认从 main 开始”的一般规则。
- 原电机提交 `04e348b`、`50f41e5` 从 handoff bundle 经 `cherry-pick -x` 移植为 `20c46e9`、`a16264a`；测试 CMake 冲突保留双方注册。
- 不从有损坏历史 ASM 文件的 source-tree 覆盖源码。bundle 仅导出 HEAD，因此用 `git fetch <bundle> HEAD` 读取提交。
- 时变低通、FilteredDAS 核及默认值、既有启动拒绝修复、Normalizer newest-first 修复均沿用基线；CUDA/SHM 结构 ABI 未变。

## 修复

参数文件选择（后续界面更新）：电机联动状态栏右侧新增“参数文件选择”按钮，浏览并选择 JSON 参数文件后立即切换监视路径，写入 `MotorScanLinkage/ParamsFile`，下次启动恢复。取消浏览不改变路径。选择文件不解除物理轮闩锁；参数窗口关闭且轮次允许时，才应用新文件参数。按钮及状态栏悬停提示当前完整路径。

1. Normalizer 追加 `RoundStarted=3`（日志 `physical_round_started`），每轮首个新物理 identity 通知一次，与 startupFilter 数量独立；原分类、过滤、计数、timeout、save ownership 不变。迟到重复卡不产生重复通知。
2. MainWindow 和测试共用 Monitor 的 `onPhysicalRoundEvent`：RoundStarted 关闭应用闸门，CountBoundary/TimeoutBoundary 打开。成像服务停止不再被误当作物理轮结束；监听真正停止或新建监听时恢复闸门。
3. 闸门关闭与主线程同步下发事务互斥，解决“检查时开闸、下发时已开始下一轮”的窗口；源线程在返回首触发前完成关闸。信号仍由主线程同步消费，自动下发不弹模态参数警告。
4. 参数窗口创建时回填 `lastStatus()`，状态在参数处理后发布，初始文案不再误报未联动；禁用时有明确状态。
5. 自动联动限于环形模式；参数窗口编辑期间暂缓，避免提交正在编辑的滤波草稿。关闭后仍保留待应用值。拒绝下发时恢复方向、起点和总线数控件，保留待重试参数。
6. 步数乘法采用 64 位中间值，超出 int/UI 范围拒绝；非法总线数不再顺带提交方向。诊断保留独立的 `paramsSource`，避免与通用诊断 source 字段混淆。
7. 更正原报告的顺向证据边界：它来自真实但陈旧的文件快照，不是当时的活动面板链路；旧构建身份继续归属原提交。

闩锁按物理轮边界工作。轮间隔短于 500ms 轮询时，若下一轮已经开始，待应用值继续延后至后续空闲边界，不能为了追赶参数而改动正在采集的轮次。没有强制部分重建完成，也没有改变 timeout 的单一 owner。

## 验证与交付

开发态验证已完成：56/56 CTest、真实 MainWindow 入口 1/1（59.35 s）及合成服务自检通过。日志归档在 `evidence/`，其 Git 标识为移植后的 a16264a + tracked-dirty 修复工作树，不能充当最终提交的 BuildIdentity。提交后的 exact-SHA 验证另由下述脚本记录。

新增测试覆盖真实 Normalizer → Monitor 事件接线：startupFilter=0/1/3、计数/仅超时模式、重复卡、下一轮抢在轮询前开始、拒绝后同文件重试、跨线程关闸与同步下发、整数溢出。

扩展服务 MATLAB/DAS 回归为 4/8 通道 × 顺逆方向 × 拼接开关 × 时变低通开关 × FilteredDAS 开关，共 32 组；比较滤波后的 A-line、角度/半径/扇区 CUDA 输出、WL2 跨块历史和失败原子性。

真实 MainWindow 入口回归保留基线启动拒绝，并加入四种滤波组合的方向冻结、边界自动重启、两种滤波 accepted snapshot 保持、服务停止不错误开闸、懒创建状态恢复、非法滤波参数拒绝及回滚、窗口编辑期间延迟应用。

最终精确 SHA、tracked-clean、configure/build、56 项 CTest、真实入口回归与合成 selftest 的执行结果由 `delivery.py` 写入 `artifacts/motor-lowpass/final-*.json/txt`。交付包的 `git-receipt.txt`、`BuildIdentity.h`、依赖和文件哈希共同标识候选。历史低通性能/MATLAB证据不改称本分支重新执行的硬件或性能验收。

```powershell
# 使用机器上可用的 Python；本机使用应用随附的 Python runtime。
python CODEX_REPORTS/motor-lowpass-linkage-20261029/delivery.py build --stage final
python CODEX_REPORTS/motor-lowpass-linkage-20261029/delivery.py validate --stage final
git push -u origin codex/ring-lowpass-motor-linkage-20261029
python CODEX_REPORTS/motor-lowpass-linkage-20261029/delivery.py package --stage final
```

主工程严格使用 `build_mingw_debug.cmd` 的 configure+build；主入口测试通过本地 CMake 参数 `BUILD_MAINWINDOW_START_TEST=ON` 启用。测试临时目录设在工作区内，避免沙箱默认临时路径写入失败。交付时重新 configure，不能把移植前或提交前的二进制标成新 SHA。

软件回归、真实采集、视觉效果、T_arr 和 LabVIEW 活链路是不同证据层。本轮硬件状态保持 **UNVERIFIED，待用户联合实机验收**。操作顺序见 `hardware-operation-card.md`。
