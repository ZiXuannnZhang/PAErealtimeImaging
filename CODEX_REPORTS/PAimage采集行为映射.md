# PAimage采集行为映射

映射版本：production-3，2026-09-10。交付版已实际替换生产采集路径；剩余界面验收由用户接管。
产品基线：e66a29bfbbaa6534911a23d48e8624fb8d552bec。
源PAimage 1.5.16.0 SHA256：2b2f4a8b82ff49fc35c2d3d96fe3f999a0dcc403144f39ef520c5af5cea9a6ec。
静态基址0x140000000；下表RVA=VA减基址。名称为恢复源码自拟名称，不是原始符号。

| VA / 调用路径 | 恢复行为 | 实现与验证 | 边界与差异 |
|---|---|---|---|
| 140150440 ← 1401415f6 | CONFIG 58字节，FA前缀；byte4=2；B延时/4大端32位放5..8，窗口/4大端24位放9..11，A延时/4大端24位放12..14 | configCommand；control_checks、protocol_checks、生产network测试 | 宿主传入ns，不改协议 |
| 1401504f0 / 140150520 | START/STOP 58字节，byte4=3，byte10=8，byte57=1/0 | startCommand/stopCommand；实际socket黄金报文 | 诊断stage6另存byte57，不将两者混算 |
| 140150580 → 140135a70，140135e56..e88 | 非空反馈仅按长度18/60分类ready/ACK，再按目标IP定位 | feedbackType、SocketReceiver、ControlState | 不检查ACK payload；没有协议事务序号，旧ACK归属无法凭空证明 |
| 140131550→140141500，140131a55，140131b91..ba8；14001387e | 仅发未ACK卡；最多4轮，每轮发送后等1秒；+58默认1，0为免等待 | ControlState；虚拟时钟及真实socket检查 | Qt 20ms管理计时器代替源等待线程，超时量化约一个tick；不走旧重试 |
| 1401375b7..62e；140137858..87f；140150600..66d | 控制socket要求具体IPv4，拒空及0.0.0.0，源端口0，发送超时500ms；逐目标58byte | ControlSocket、Backend | 宿主空IP用只读GetBestRoute2选接口后显式绑定；接受宿主扫描目标列表 |
| 140137a4e..a75；140138460 | 反馈与数据均申请64MiB，INADDR_ANY，非阻塞；先反馈后数据创建 | SocketReceiver::start；查询实际SO_RCVBUF | 源忽略setsockopt/ioctlsocket失败；恢复版设置失败拒启，明确错误路径差异 |
| 140138e90；140139310..3ac | 单select循环，1ms，接收优先级+2；先反馈再按数据socket索引逐个排空 | SocketReceiver::run；Windows四卡40Hz 28/70包10分钟 | 无每卡线程、排空预算、IOCP、绑核；不是原EXE动态对拍 |
| 140139310..3ac | 反馈端口<=64走反馈，>64按源IP走数据；数据端口按socket卡号 | SocketReceiver::run；protocol_checks | 未知、短包、反馈均先记raw；数据端口不额外拒绝不同来源IP |
| 140136166..1a3；14013620e、263..2c5 | 少于4byte拒绝；LE16 packet/trigger；接纳位+408；recent窗口先检查 | SourceCore::ingest；core_checks | 不叠加旧StartFence；诊断ID不参与接纳 |
| 14013635d..40e；140136377..393 | 首到packet为锚，16位差定位1440槽，越界拒、位图去重；重复/越界前更新末到时间 | ingest；首/中/尾缺包、乱序与重复测试 | 不修正物理slot0缺失；短payload可置源完整位，独立分析器另查长度 |
| 140136306..346、412..42f；14013922a..250 | 触发改变结旧；槽齐即结；距末到>=100ms结算 | close/poll；core_checks | 新触发无大小门控；末触发超时与Stop截断分别记录 |
| 140134dfa..e23；140134e23..edf | 完成历史16项；输出先按固定samples×位宽补零再复制槽 | close、decodeRaw、FrameConverter | 缺失槽保持缺口元信息；不因补零变成完整接收 |
| 140134fca..1350c2 | 启动卡缓存4096项/128MiB，超限关闭接纳并清缓存 | SourceCore；容量测试 | 需要停止再启动；无额外预热与自动恢复 |
| 1401342e0 | 启动同步>=4项且首同步到当前>=500ms确认；先释放卡缓存再同步缓存 | deliverSync；475/500ms边界、生产确认释放 | 不额外要求触发号连续 |
| 1400138bb；14003a9c0、03aa3d、03ad6d..80；14013623d..25e、139199..1ca | +80默认0；UI模式0..4设1000ms；未确认静默阈值到达清启动、同步、活动缓存 | Config默认0，生产显式1000；clearStartup | 生产固定选择该源模式；不宣称所有PAimage模式均相同 |
| 140132980；1401351fa、135798..7b6 | 启动过滤卡/同步分别累计；不完整计数按启动/运行分流，含卡及同步事件 | Counters及旁路Observations | 混合单位标明；不能标为包数或物理触发数 |
| 140135210..63d；140135690..776 | 完整卡才进跨卡同步；全卡到齐释放；仅不齐分支扫描age>250ms或size>16 | close/deliverSync；core_checks | 卡保存与同步显示/Ring分别接两个源出口 |
| 14012b9b0、12f340、12d398 | LE16 FNV-1a、初始8桶、load1、小桶8倍扩容、桶/链迭代顺序 | findOrInsertPending；独立碰撞/重用检查 | 显式恢复MSVC容器顺序，不用MinGW默认迭代 |
| 140134020、144172；140143e40、14409d | 单卡保存worker，单FIFO，每卡400满拒新，50ms条件等待，默认优先级 | OutputQueues/OutputWorkers；阻塞消费者测试 | worker内调用原FileSaver单组保存体；不启动旧保存QThread |
| 140133b90、133d7e、13a226；140139c90、13a0d4 | 同步worker+1；启动队列优先；普通队列满2×blockSize淘汰队首整块；50ms等待 | OutputQueues/OutputWorkers；worker_checks | 本实验源同步blockSize固定50，与规定同机测试一致 |
| 14013a2ee..2fe、13aece | 出队后及下游计算后两次同步session检查 | OutputWorkers、HostOutput转换后再查 | 同步陈旧输出单列，不给卡保存加measurement门控 |
| 140142ec0；1401430d7..0f6 | START发送前关闭并清理，成功再清组包/recent并开启；只清同步FIFO及序号，不清卡FIFO | prepareStart/completeStart；100轮生产启停 | Qt管理状态不叠加额外接纳门 |
| 140143a90..c04；140143c7b..d1d | STOP先关，失败恢复；成功清启动且confirmed，不flush活动；保存停止另清卡FIFO并进保存代 | SourceCore、OutputWorkers | stop截断旁路记录；保存代入FIFO前打标，积压跨代目录测试通过 |
| 140143780..80c | 关监听/测量/保存，唤醒并join工作线程，关闭socket、清剩余队列 | Backend requestStop/stop、NetworkController异步停止 | 源socket select退出错误由管理timer报告并停止宿主；未交付单列 |
| 140141c70、141d38→1400195e0、141e8f→141f10 | listener运行时拒改配置；复制配置后建listener并预计算slots | configurePaimage；20→50→20生产保存测试 | 宿主内部重建listener，独立run；保存翻滚续序防覆盖，明示适配 |
| 14013a450..541 | B/A有符号整数转float，源下游还乘比例 | decodeRaw/FrameConverter；逐样本黄金float16验证 | 在原始bytes边界接宿主，保留整数数值，不移植源比例39788.735772973836/(32bit?65536:32768) |

生产路径：NetworkController::start → startPaimage → Backend → SocketReceiver → SourceCore → HostOutput → 原保存/显示/Ring出口。未创建旧接收和处理QThread，也未调用旧processInputBatch或PacketAssemblyBuffer；保留旧组件只作历史测试。

46处选定机器码指令已由verify_recovery.py核对。完整函数恢复同时依赖扩展反汇编和调用/数据流核验；数量不是完整性证明。PE exception目录可能仅含函数片段，不能将片段结尾当正常路径结尾。

证据位于paimage-port-evidence：input-hashes.json、recovery-verified.json、fn-*.asm及分析脚本。没有完整原源码或原EXE动态对拍。驱动内部、物理触发沿、包在应用入口之前的具体丢失位置仍不可观测。实际窗口启动、标记和空闲导出已验证；剩余参数交互及外接网卡实验由用户验收。

