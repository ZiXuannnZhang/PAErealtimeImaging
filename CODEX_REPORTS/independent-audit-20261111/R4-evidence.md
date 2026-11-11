# R4 回执：单卡 100 ms 组帧策略

结论：策略暂缓，未证明缺陷。仍保留单卡 100 ms、recent 去重和 partial 只进入卡帧路径的策略。该超时独立于物理轮超时和 a284bf3 的按卡保存宽限。R6 对排队可读性的处理只决定何时有证据执行既有超时检查。

现有诊断可以直接看到：用户态 recv 完成后的 ingress 单调时刻、原始头部和本地端口；SourceCore 的 TriggerSwitch、Timeout、RecentTrigger 决策；详细 timing 的 recv、核心锁等待和处理耗时；looplog 的 select 就绪、drain 批次及循环间隔。可用这些字段定位触发切换、recent 窗口内被抑制的重复，并判断接收线程有无长停顿或服务机会不足。RecentTrigger 只证明匹配了近期触发缓存；超出缓存的同号数据不能据此认定重复。

尚缺能够证明“同一触发的有效尾包在接收层实际间隔超过 100 ms”的现场证据。SocketReceiver 的可选 WSARecvMsg 路径只统计 timestamp 控制消息是否存在，未保存每包 kernel timestamp 值；默认关闭。用户态 recv/ingest 时间只能测已经交给本程序的间隔，不能单凭它区分 NIC/内核排队和到达端间隔。若需重新评估阈值，应同一现场运行对齐独立的包到达时间和程序 trace，并验证对应帧确有可接纳尾包。当前未运行现场采集，不据此调整阈值。
