# R6 验收：接收端口轮转和停止

2026-11-11。实施起点 fe75fd31e41dc42764a2d8e27d7b381c3ecd8075，执行代理 gpt-6.1-terra / high。R4 的时序证据与 100 ms 策略另见 R4-evidence.md。

每个可读端口每轮最多处理 256 个成功读取的数据报；反馈端口同样受限，数据端口轮换起点。每个包前检查停止；停止后跳过本轮超时处理，先 join 再关闭 socket。预算让出和停止分别用 drain flags 3、4，与 socket 错误及丢包分开。原始 ingress、START fence 和既有 demux 仍在原顺序。

处理完本轮已读取的包后，以零超时 select 检查队列。SourceCore 只对无待处理数据的卡执行既有 100 ms 组帧检查。接收线程只发布经过检查的单调空闲时间；Backend 原控制线程消费全局/分卡时间，分别推进物理边界和尾帧保存宽限，未把同步目录准备放进接收循环。启动 fence 未释放时不发布，生命周期清除旧水位。反馈端口混载控制与采样，仍可读时保守暂缓空闲判断；持续纯反馈在此情形也可能延迟封存。

独立验证：Qt 6.8.0 / MinGW 13.1 / Ninja Debug，相关 CTest 12/12（20.28 s）。包括网络与生产 gap、START race/fence/overflow、组帧/协议、HostOutput、保存绑定、短回放和两项新增测试。定向筛选：

    ctest --test-dir <audit-tests> --output-on-failure -R "^(paimage_socket_(fairness_checks|continuous_checks|short)|paimage_(core_checks|protocol_checks|network_test|production_gap_test|start_fence_regression|start_race|start_overflow|host_output_test)|count_boundary_save_binding_test)$"

真实 loopback 预算测试：卡 0 预排 301 包，预算点暂停 150 ms；观察到 1 次让出和未清 backlog，另三卡 6 包，4 帧完整，提前 Timeout=0。持续发送测试：发送 2,446，停止时已读 2,418；热端口读取 1,219，其他各 300，反馈 299，send/hard errors 均 0，stop 实测 <1 ms（断言 <1 s），发送线程在 stop 返回后才停止。持续测试不要求本机调度必然产生预算让出，已用确定性预算测试证明该分支。

四卡负载前后同参数：paimage_socket_replay.exe --seconds 3 --samples 12500 --hz 200 --timing 0 --trace 0 --looplog 0 --startup-idle-ms 0 --out <目录>。均为发送/接收 168,000/168,000，2,400 卡帧、600 sync、partial/badSamples/hardErrors 全 0；发送线程最大迟到分别 27.223 ms、67.329 ms，本轮负载下未观察到接收吞吐下降或提前封轮，未做实机网卡或更高负载专项。

最终补充：保存绑定 T3 以全局水位 0、分卡水位越过截止执行真实 HostOutput，验证尾帧进入隔离文件且没有新物理边界；重编该目标并通过（1/1，2.90 s）。主代理独立验收通过。
