# 主页面日志与标定旁车改进方案

2026-11-19。实现基线为 `44cc5e6ac227b1b78fb8341f0e7f7f0850dbaab8`。该版本经用户实机测试，测试期间暂未发现缺陷。本次改进针对主页面日志的可读性，以及离线 FBP 标定暂停期间的采集目录内容。

## 1. 运行日志只呈现操作结果和需要关注的事件

`MainWindow::logMessage` 同时写入日志框和诊断记录。环形成像的提交、确认、共享内存统计和保存器的日常明细持续占用日志框。正常自动保存还会逐圈记录目录。将这些明细标记为仅供诊断，复用现有记录入口，保留原有事件和文本。状态栏继续承担帧率和进度显示。 [日志入口][log] · [成像状态入口][status] · [自动保存目录][autosave]

保留监听、测量、成像和保存的操作结果，以及配置拒绝、数据丢弃、写入未确认、重建异常和 PNG 未交付。保存开始时显示操作者选择的路径或自动保存基线目录，各卡打开文件和正常轮转换目录只进诊断。停写缺失或未确认应保留，带相关卡和数量。分组、窗口几何、正常重试和性能统计等明细只进诊断。消息在产生处标记显示用途，诊断导出沿用现有数据和格式。 [保存结算][stop-save]

### 反例与处理

| 反例 | 预期处理 |
| --- | --- |
| 未开启 PNG 保存时，成像终帧丢失或轮次缺块 | 依据成像服务的实际结果独立提示，不能依赖已存在的 PNG 请求 |
| 终块被容量拒收，服务直接关闭该轮 | 对有效轮的终块拒收提示；中间块拒收由后续轮末结果确认。已撤回、过期或重复的通知留在诊断 |
| 同一轮次、同一原因同时导致成像终帧缺失和预期 PNG 未交付 | 界面只留一条能表达后果的提示，诊断保留各自事实。不同阶段的独立故障各自可见 |
| 正常通知重试、旧轮丢弃或正常预热 | 依据最终结果判断。累计拒绝计数、临时忙与已恢复的重试只进诊断 |
| 保存过程中出现故障，随后停写、回执有丢弃或未确认 | 保留保存异常及结算证据；避免将相同结果的逐卡正常计数写入日志框 |

`svcError` 会撤销成像勾选、修改就绪状态并结算待交付 PNG。仅用于提示的告警沿用日志展示路径。成像服务运行、启停、原始采集和 PNG 队列的生命周期由原有入口管理。这样可以避免为了减少 UI 日志而改变采集行为，也避免把成像缺块报成磁盘写入失败。 [成像服务事件][observations] · [错误处理][imaging-error] · [PNG 结算][png]

## 2. 暂停生产保存的标定旁车写入

停写原始波形旁边的 `Card*_ChA*.dat.jsonl`。生产保存端使用同一策略，覆盖手动保存、自动保存和 `_late` 隔离的迟到帧。旁车写入器和既有格式保留，可由定向用例显式启用。当前无需界面开关、设置迁移或文件清理。实时 `exact_fbp` 及其读取已有标定 JSON 的能力按现有行为运行。程序诊断目录的 JSONL 与原始波形旁车是独立产物。 [生产保存入口][savers] · [旁车写入][sidecar]

先将文件轮次归属从旁车创建移出。新 A/B 文件对的首列已进入缓冲时，根据现有采集身份及逻辑轮次条件登记归属。写入器只用该归属生成文件头。保留物理轮次、容量、采样布局、来源和保存代号驱动的文件边界，以及双通道批次确认、失败回退和关窗封尾。继续使用现有独占创建和序号避让，包括旧目录中同名 A/B 已移走但旁车仍存在的情况。 [文件轮转][rollover] · [序号避让][sequence]

直接关闭 `recordRawColumn()` 会丢掉 `m_rawFileRound` 的赋值。旧方案下，一对文件登记了轮次后，随后到达的未标记列会另开文件；即使它携带相同轮号也一样。关写前需保留这一行为。文件归属也不应取决于能否打开旁车。 [现有混合列用例][mixed]

停写期间的 A/B 波形仍可读取。现有严格 MATLAB 标定流程需要采集身份、逐列轮次和完整性证据，这些信息无法从波形补出。恢复标定时启用旁车，从启用之日起重新采集；旧目录的已有证据照常读取。 [MATLAB 旁车检查][matlab]

## 3. 实施与复核

1. 独立提交日志精简。复核正常高频消息不刷新文本框、诊断仍能看到原有事件、关闭 PNG 保存后终帧异常仍可见。对照重复原因、停写异常和成像就绪状态。
2. 独立提交旁车停写。用现有保存与主窗口用例检查普通和迟到 A/B 文件、混合列分段、历史旁车序号避让、双通道结算和正式 PNG。保留写入启用的证据测试作为对照。
3. 使用普通 Windows 令牌构建和运行与改动直接相关的测试。交付时注明构建身份、实际回环验证与实机验证的范围。

下列链接指向实现基线中用于复核设计的源码。

[log]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/MainWindow.cpp#L3402
[status]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/MainWindow.cpp#L550
[autosave]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/MainWindow.cpp#L2643
[stop-save]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/FileSaver.cpp#L152
[observations]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/ImagingController.cpp#L1204
[imaging-error]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/MainWindow.cpp#L5011
[png]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/MainWindow.cpp#L5549
[savers]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp#L329
[sidecar]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/FileSaver.cpp#L349
[rollover]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/FileSaver.cpp#L615
[sequence]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/src/FileSaver.cpp#L222
[mixed]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/MC_410T_MultiCard/delivery/tests/filesaver_round_boundary_test.cpp#L1076
[matlab]: https://github.com/ZiXuannnZhang/PAErealtimeImaging/blob/44cc5e6/RadiusCalibration/inspect_ring_raw_pair.m#L1
