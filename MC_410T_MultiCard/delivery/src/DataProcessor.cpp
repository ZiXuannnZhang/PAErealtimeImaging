#include "DataProcessor.h"
#include "FramePublisher.h"
#define _USE_MATH_DEFINES
#include <cmath>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

// ─
// 工具函数
// 
static uint64_t currentTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// 
// 构造 / 析构
// ─
DataProcessor::DataProcessor(
    int cardId,
    moodycamel::ConcurrentQueue<TriggerGroupPtr>* saveQueue,
    DisplayBuffer* displayBuf,
    FramePublisher* publisher,
    const AcqConfig& config,
    const RingFeedSink& ringFeedSink,
    QObject* parent)
    : QThread(parent)
    , m_cardId(cardId)
    , m_config(config)
    , m_saveQueue(saveQueue)
        , m_displayPoints(config.displayPoints)
    , m_displayBuffer(displayBuf)
    , m_framePublisher(publisher)
    , m_ringFeedSink(ringFeedSink)
    , m_expectedPackets(config.packetsPerTrig())
{
    m_stats.cardId = cardId;
    // Keep the worker-owned buffer large enough for the full supported packet
    // range.  The UI can reconfigure acquisition time while the thread is
    // alive; a later session must not inherit a buffer sized for the prior
    // (possibly smaller) setting.
    m_workerAssembly = std::make_unique<PacketAssemblyBuffer>(
        std::max(m_expectedPackets + 20, MAX_PKTS_PER_TRIG));
    setObjectName(QString("DataProcessor_%1").arg(cardId));
}

DataProcessor::~DataProcessor() {
    if (isRunning()) {
        requestStop();   // requestInterruption() + notify_all()
        if (!wait(500)) {
            terminate();
            wait();   // 确保线程真正结束后再析构 QThread，避免 qFatal
        }
    }
}

// 
// 热路径：接收线程调用，无锁入队 + 条件变量通知
// 
void DataProcessor::enqueuePacket(const DataPacket& pkt) {
    enqueuePacketForSession(pkt,
                            m_ingressSessionToken.load(std::memory_order_acquire));
}

void DataProcessor::enqueuePacketForSession(const DataPacket& pkt,
                                            uint64_t sessionToken) {
    DataPacket tagged = pkt;
    tagged.measurementSessionToken = sessionToken;
    m_inputQueue.enqueue(tagged);
    {
        std::lock_guard<std::mutex> lk(m_wakeMtx);
        m_hasData.store(true, std::memory_order_relaxed);
    }
    m_wakeCv.notify_one();
}

int DataProcessor::inputQueueDepth() const {
    return static_cast<int>(m_inputQueue.size_approx());
}

bool DataProcessor::postSessionCommand(SessionCommandKind kind,
                                       uint64_t sessionToken,
                                       int timeoutMs)
{
    if (!isRunning()) return false;
    auto wait = std::make_shared<SessionCommandWait>();
    {
        std::lock_guard<std::mutex> lock(m_sessionCommandMtx);
        m_sessionCommands.push_back(SessionCommand{kind, sessionToken, wait});
        m_hasSessionCommand.store(true, std::memory_order_release);
    }
    m_wakeCv.notify_one();

    std::unique_lock<std::mutex> lock(wait->mutex);
    const auto ready = [&wait] { return wait->done; };
    const bool reached = timeoutMs < 0
        ? (wait->condition.wait(lock, ready), true)
        : wait->condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    return reached && wait->success;
}

bool DataProcessor::prepareSession(uint64_t sessionToken, int timeoutMs)
{
    return sessionToken != 0
        && postSessionCommand(SessionCommandKind::Prepare, sessionToken, timeoutMs);
}

bool DataProcessor::armSession(uint64_t sessionToken, int timeoutMs)
{
    return sessionToken != 0
        && postSessionCommand(SessionCommandKind::Arm, sessionToken, timeoutMs);
}

bool DataProcessor::disarmSession(int timeoutMs)
{
    return postSessionCommand(SessionCommandKind::Disarm, 0, timeoutMs);
}

int DataProcessor::drainInputQueue()
{
    int drained = 0;
    DataPacket packet;
    while (m_inputQueue.try_dequeue(packet)) ++drained;
    return drained;
}

void DataProcessor::resetSessionState(PacketAssemblyBuffer& assemblyBuf)
{
    m_measureEnabled.store(false, std::memory_order_release);
    m_ingressSessionToken.store(0, std::memory_order_release);
    m_activeSessionToken.store(0, std::memory_order_release);
    assemblyBuf.reset();
    drainInputQueue();
    m_lastFlushedTriggerSeq = 0;
    m_hasFlushedOnce = false;
    m_consecutiveDiscards = 0;
    m_stats.resetRawSequenceAnchor();
    m_assemblyReceived.store(0, std::memory_order_release);
    m_hasFlushedOnceAtomic.store(false, std::memory_order_release);
    m_lastFlushedTriggerSeqAtomic.store(0, std::memory_order_release);
    m_consecutiveDiscardsAtomic.store(0, std::memory_order_release);
}

void DataProcessor::processSessionCommands(PacketAssemblyBuffer& assemblyBuf)
{
    std::deque<SessionCommand> commands;
    {
        std::lock_guard<std::mutex> lock(m_sessionCommandMtx);
        commands.swap(m_sessionCommands);
        m_hasSessionCommand.store(!m_sessionCommands.empty(), std::memory_order_release);
    }
    for (SessionCommand& command : commands) {
        bool success = true;
        switch (command.kind) {
        case SessionCommandKind::Prepare:
            resetSessionState(assemblyBuf);
            break;
        case SessionCommandKind::Arm:
            // A second drain immediately before the gate opens removes packets
            // that arrived after PREPARING but before ARMED.
            drainInputQueue();
            m_ingressSessionToken.store(command.sessionToken, std::memory_order_release);
            m_activeSessionToken.store(command.sessionToken, std::memory_order_release);
            m_measureEnabled.store(true, std::memory_order_release);
            break;
        case SessionCommandKind::Disarm:
            resetSessionState(assemblyBuf);
            break;
        }
        if (command.wait) {
            std::lock_guard<std::mutex> lock(command.wait->mutex);
            command.wait->success = success;
            command.wait->done = true;
            command.wait->condition.notify_one();
        }
    }
}

// 
// 处理线程主循环
// 
void DataProcessor::run() {
#ifdef _WIN32
    // 绑定到处理线程组（软绑定，允许 OS 在超线程核间调度）
    // 线程亲和性由 NetworkController 在 start() 时统一设置
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

    PacketAssemblyBuffer *assembly = m_workerAssembly.get();
    if (!assembly) return;

    while (!isInterruptionRequested()) {
        processSessionCommands(*assembly);
        // 条件变量等待，最多 1ms 超时
        {
            std::unique_lock<std::mutex> lk(m_wakeMtx);
            m_wakeCv.wait_for(lk, std::chrono::milliseconds(PROC_WAKE_TIMEOUT_MS),
                [this]{ return m_hasData.load() || m_hasSessionCommand.load()
                              || isInterruptionRequested(); });
            m_hasData.store(false, std::memory_order_relaxed);
        }

        processSessionCommands(*assembly);
        processInputBatch(*assembly);
    }
    processSessionCommands(*assembly);
    m_assemblyReceived.store(0, std::memory_order_release);
}

int DataProcessor::processInputBatch(PacketAssemblyBuffer& assemblyBuf) {
    DataPacket pkt;
    int count = 0;
    // 先检查 batch quota，再尝试 dequeue。这样第 513 个包不会被
    // try_dequeue 成功取出后又因 quota 失败而静默丢失。
    while (count < PROC_BATCH_SIZE && m_inputQueue.try_dequeue(pkt)) {
        ++count;
        m_stats.processorPacketsDequeued.fetch_add(1, std::memory_order_relaxed);
        m_stats.packetsReceived.fetch_add(1, std::memory_order_relaxed);

        // Gate 关闭期间以及旧 session token 的包都只出队、不进入组包。
        // 这样 PREPARING/ARMED 边界不会把旧队列数据算入新 session。
        const uint64_t activeToken =
            m_activeSessionToken.load(std::memory_order_acquire);
        if (!m_measureEnabled.load(std::memory_order_relaxed)
            || activeToken == 0
            || pkt.measurementSessionToken != activeToken)
            continue;

        // ══ 步骤1：丢弃旧触发迟到包（含重复包），不重复计入丢包 ──────────────────
        // 设计原则：迟到包在触发切换时已被精确统计（步骤2的missingInOld），
        //           此处只丢弃，不再重复计数，避免双重统计。
        // 首次flush前（启动阶段）用缓冲区内的triggerSeq作回退判断，
        // 防止系统启动时网络中残留的旧触发包污染第一个触发的组装缓冲区。
        if (m_hasFlushedOnce) {
            // flush后：seqDiff≤0 → 属于已flush触发（或更旧），丢弃
            if (static_cast<int16_t>(pkt.triggerSeq - m_lastFlushedTriggerSeq) <= 0) {
                // ── 新一帧/新一轮测量触发序号重置识别 ───────────────────────
                // 模拟器重发或 FPGA 重置后，首触发序号相对上一帧末触发大幅回退
                // （远大于乱序抖动窗口）。立即重置锚点并接受当前包，
                // 避免整帧首个触发被当作迟到包丢弃（触发计数少 1 且丢包为 0）。
                const int32_t backJump =
                    static_cast<int32_t>(m_lastFlushedTriggerSeq) -
                    static_cast<int32_t>(pkt.triggerSeq);
                if (backJump >= kTriggerResetBackJumpThreshold) {
                    m_hasFlushedOnce     = false;
                    m_consecutiveDiscards = 0;
                    assemblyBuf.reset();
                    m_assemblyReceived.store(0, std::memory_order_release);
                    m_hasFlushedOnceAtomic.store(false, std::memory_order_release);
                    m_consecutiveDiscardsAtomic.store(0, std::memory_order_release);
                    // fall through：把当前包当作第一个新包处理
                } else {
                    // ── 连续丢弃恢复机制（小回退=乱序迟到包）─────────────────
                    // 超过 expectedPackets+1 个连续丢弃后，强制重置锚点，
                    // 接受任意新触发序号，恢复正常处理（复位后仅丢 1 个触发）。
                    ++m_consecutiveDiscards;
                    if (m_consecutiveDiscards >= m_expectedPackets + 1) {
                        m_hasFlushedOnce     = false;
                        m_consecutiveDiscards = 0;
                        assemblyBuf.reset();
                        m_assemblyReceived.store(0, std::memory_order_release);
                        m_hasFlushedOnceAtomic.store(false, std::memory_order_release);
                        m_consecutiveDiscardsAtomic.store(0, std::memory_order_release);
                        // fall through：把当前包当作第一个新包处理
                    } else {
                        m_stats.staleTriggerPacketsDiscarded.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
            } else {
                m_consecutiveDiscards = 0;  // 收到有效包，重置计数
            }
        } else if (assemblyBuf.receivedCount() > 0) {
            // flush前（启动期）：seqDiff<0 → 属于比当前更旧的触发，丢弃
            if (static_cast<int16_t>(pkt.triggerSeq - assemblyBuf.triggerSeq()) < 0)
            {
                m_stats.staleTriggerPacketsDiscarded.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }

        // ══ 步骤2：触发切换 + 精确丢包统计 ─────────────────────────────────────
        // 核心：用 bitmask receivedCount 计算缺失包数，对乱序到达完全不敏感
        // 例：70包期望，收到68包（乱序 ok），切换时精确计2包丢失
        bool didSwitch = false;
        if (assemblyBuf.receivedCount() > 0 &&
            pkt.triggerSeq != assemblyBuf.triggerSeq())
        {
            const uint16_t oldSeq = assemblyBuf.triggerSeq();

            // ① 旧触发：期望包数 - 实收包数（bitmask去重，对乱序精确）
            const int32_t missingInOld =
                m_expectedPackets - assemblyBuf.receivedCount();
            if (missingInOld > 0) {
                m_stats.packetsDropped.fetch_add(
                    static_cast<uint32_t>(missingInOld), std::memory_order_relaxed);
                emit partialTrigger(m_cardId, oldSeq, missingInOld);
            }

            // ② 旧触发与当前包触发之间跳过的完整触发（0包到达，int16差值处理回绕）
            //    示例：oldSeq=5, pkt.triggerSeq=8 → skipGap=2 → T6+T7全部丢失
            const int16_t skipGap =
                static_cast<int16_t>(pkt.triggerSeq - oldSeq) - 1;
            if (skipGap > 0)
                m_stats.packetsDropped.fetch_add(
                    static_cast<uint32_t>(skipGap) *
                    static_cast<uint32_t>(m_expectedPackets),
                    std::memory_order_relaxed);

            m_stats.triggersPartial.fetch_add(1, std::memory_order_relaxed);
            flushAssemblyBuf(assemblyBuf);
            assemblyBuf.reset();
            m_lastFlushedTriggerSeq = oldSeq;  // 锚点移到旧触发
            m_hasFlushedOnce        = true;
            m_lastFlushedTriggerSeqAtomic.store(m_lastFlushedTriggerSeq, std::memory_order_release);
            m_hasFlushedOnceAtomic.store(true, std::memory_order_release);
            didSwitch               = true;
        }

        // ══ 步骤3：全触发丢失检测（缓冲区原本为空时的序号断层）──────────────────
        // 场景：T5完成→缓冲区清空→T7第一包到达（T6全部0包）
        //       didSwitch=false（无旧触发可切换），此处通过锚点gap检测T6的丢失
        // 注意：didSwitch=true时步骤2的skipGap已覆盖跳过的触发，此处不再执行
        if (!didSwitch && assemblyBuf.receivedCount() == 0 && m_hasFlushedOnce) {
            const int16_t gap =
                static_cast<int16_t>(pkt.triggerSeq - m_lastFlushedTriggerSeq) - 1;
            if (gap > 0)
                m_stats.packetsDropped.fetch_add(
                    static_cast<uint32_t>(gap) *
                    static_cast<uint32_t>(m_expectedPackets),
                    std::memory_order_relaxed);
        }

        // ══ 步骤4：插入包（bitmask去重 + 直接索引，乱序/重复均安全）───────────
        const PacketAssemblyBuffer::InsertResult insertResult = assemblyBuf.insertPacket(pkt);
        if (insertResult == PacketAssemblyBuffer::InsertResult::Duplicate) {
            m_stats.assemblyDuplicatePackets.fetch_add(1, std::memory_order_relaxed);
        } else if (insertResult == PacketAssemblyBuffer::InsertResult::OffsetOutOfRange) {
            m_stats.assemblyOffsetOutOfRangePackets.fetch_add(1, std::memory_order_relaxed);
        }

        // ══ 步骤5：触发完成（所有期望包均已到达，含乱序）────────────────────────
        if (assemblyBuf.isComplete(m_expectedPackets)) {
            m_lastFlushedTriggerSeq = assemblyBuf.triggerSeq();
            m_hasFlushedOnce        = true;
            m_lastFlushedTriggerSeqAtomic.store(m_lastFlushedTriggerSeq, std::memory_order_release);
            m_hasFlushedOnceAtomic.store(true, std::memory_order_release);
            flushAssemblyBuf(assemblyBuf);
            assemblyBuf.reset();
            m_assemblyReceived.store(0, std::memory_order_release);
        }
        m_assemblyReceived.store(assemblyBuf.receivedCount(), std::memory_order_release);
        m_consecutiveDiscardsAtomic.store(m_consecutiveDiscards, std::memory_order_release);
    }

    // quota 在 dequeue 前检查，因此没有“取出后因边界被丢弃”的路径；
    // batchBoundaryDiscards 作为兼容哨兵保持为 0。
    // 更新队列深度（供统计显示）
    m_stats.inputQueueDepth = static_cast<int>(m_inputQueue.size_approx());
    return count;
}

#ifdef DATA_PROCESSOR_TEST_SEAM
int DataProcessor::drainBatchForTest() {
    PacketAssemblyBuffer assemblyBuf(m_expectedPackets + 20);
    return processInputBatch(assemblyBuf);
}

DataProcessor::SessionStateSnapshot DataProcessor::sessionStateForTest() const {
    SessionStateSnapshot state;
    state.assemblyReceived = m_assemblyReceived.load(std::memory_order_acquire);
    state.hasFlushedOnce = m_hasFlushedOnceAtomic.load(std::memory_order_acquire);
    state.lastFlushedTriggerSeq = m_lastFlushedTriggerSeqAtomic.load(std::memory_order_acquire);
    state.consecutiveDiscards = m_consecutiveDiscardsAtomic.load(std::memory_order_acquire);
    state.activeSessionToken = m_activeSessionToken.load(std::memory_order_acquire);
    state.measureEnabled = m_measureEnabled.load(std::memory_order_acquire);
    return state;
}
#endif

// 
// computeFrequency
// 解调（电压→声波时域信号）已在采集卡 FPGA 完成；UDP 载荷即为已解调时域信号，
// 监听程序只做一次精度转换（int16/int32 → float32），此处保持透传，不再做
// 差分相位→频率的二次换算（历史模拟回放路径已移除）。
// 
void DataProcessor::computeFrequency(TriggerGroup& group) {
    (void)group;
}

// 
// downsample
// 均匀步进降采样：每 ratio 个点取一个（step-wise，非滤波）
// 填写 *_display 字段（displayPoints 个点）
// 
void DataProcessor::downsample(TriggerGroup& group, int displayPoints) {
    const int n = group.sampleCount;
    if (n <= 0 || displayPoints <= 0) return;

    displayPoints = std::min(displayPoints, n);  // 不超过实际采样点数

    group.freqA_display.resize(displayPoints);
    group.freqB_display.resize(displayPoints);
    group.phaseA_display.resize(displayPoints);
    group.phaseB_display.resize(displayPoints);

    // 由频率积分重建显示相位（与 bitsPerChannel 无关，系数恒等）
    constexpr float phasePerKhz = static_cast<float>(2.0 * M_PI * M_PI * 1000.0 / FPGA_ADC_FREQ_HZ);

    if (displayPoints == n) {
        // 不需要降采样，直接赋值
        group.freqA_display = group.freqA;
        group.freqB_display = group.freqB;

        float phaseA = 0.0f;
        float phaseB = 0.0f;
        for (int i = 0; i < n; ++i) {
            phaseA += group.freqA[i] * phasePerKhz;
            phaseB += group.freqB[i] * phasePerKhz;
            group.phaseA_display[i] = phaseA;
            group.phaseB_display[i] = phaseB;
        }
        return;
    }

    double step = static_cast<double>(n) / displayPoints;
    std::vector<int> sampleIndex(displayPoints);
    for (int i = 0; i < displayPoints; ++i) {
        int srcIdx = static_cast<int>(i * step);
        if (srcIdx >= n) srcIdx = n - 1;
        sampleIndex[i] = srcIdx;
    }

    int out = 0;
    int nextSample = sampleIndex[0];
    float phaseA = 0.0f;
    float phaseB = 0.0f;
    for (int i = 0; i < n && out < displayPoints; ++i) {
        phaseA += group.freqA[i] * phasePerKhz;
        phaseB += group.freqB[i] * phasePerKhz;

        if (i == nextSample) {
            group.freqA_display[out] = group.freqA[i];
            group.freqB_display[out] = group.freqB[i];
            group.phaseA_display[out] = phaseA;
            group.phaseB_display[out] = phaseB;
            ++out;
            if (out < displayPoints) nextSample = sampleIndex[out];
        }
    }
}

// ─
// flushAssemblyBuf
// 将 assemblyBuf 当前内容 export → compute → 三路分发
// 供正常完成（isComplete）和触发切换强制 flush 两种路径共用
// ─
void DataProcessor::flushAssemblyBuf(PacketAssemblyBuffer& assemblyBuf) {
    TriggerGroupPtr group;
    try {
        group = std::make_shared<TriggerGroup>();
    } catch (const std::bad_alloc&) {
        // 内存不足时跳过本帧，避免线程崩溃；统计为丢弃
        m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    group->cardId = m_cardId;
    try {
        assemblyBuf.exportTo(*group, m_config);

        if (group->isComplete)
            m_stats.triggersComplete.fetch_add(1, std::memory_order_relaxed);
        // triggersPartial 已在调用方统计，这里不重复计数

        //  频率计算：int16 Q0.15 差分相位  float32 kHz，O(N)
        computeFrequency(*group);

        deliverAssembled(group, true, true);
    } catch (...) {
        m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
    }
}

DataProcessor::DeliveryResult DataProcessor::deliverAssembled(
        const TriggerGroupPtr& group, bool save, bool displayAndRing) {
    DeliveryResult result;
    if (!group) { result.exception = true; return result; }
    try {

        //  存储（FileSaver 负责 float32→float16 转换）
        // 保存队列上限设计：
        //   FileSaver 每 WRITE_BUFFER_TRIGGERS=16 个触发调用一次 QFile::write()。
        //   在磁盘 I/O 压力高时，write() 可能阻塞长达 1000~2000ms（OS 页缓存清洗）；
        //   阻塞期间触发以 200Hz 持续入队：2000ms × 200 = 400 个触发需要缓冲。
        //   设 MAX_SAVE_QUEUE=400 可覆盖约 2 秒的磁盘卡顿，避免丢帧。
        //   内存开销：400 × ~200KB/触发 × 32卡 = ~2.5GB（峰值，TriggerGroup 共享内存）。
        static constexpr int MAX_SAVE_QUEUE = 400;
        if (save) result.save = DeliveryResult::Disabled;
        if (save && m_directSaveSink) {
            result.save = m_directSaveSink(group) ? DeliveryResult::Consumed : DeliveryResult::ConsumerFailure;
        } else if (save && m_saveEnabled.load(std::memory_order_acquire) && m_saveQueue) {
            // 自动保存会话代打标：入队前读取当前会话代（UI 线程在边界空闲期
            // 提前推进），保存器按代路由目录，实现逐触发严格分界
            group->sessionGen = m_sessionGenReader ? m_sessionGenReader() : 0;
            if (m_saveQueue->size_approx() < MAX_SAVE_QUEUE) {
                if (m_saveQueue->enqueue(group)) {
                    result.save = DeliveryResult::Queued;
                } else {
                    result.save = DeliveryResult::QueueFailure;
                    m_stats.saveQueueDiscards.fetch_add(1, std::memory_order_relaxed);
                    m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // 存储队列满：分别计入存储丢弃和总丢弃，便于 UI 区分原因
                m_stats.saveQueueDiscards.fetch_add(1, std::memory_order_relaxed);
                m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
                result.save = DeliveryResult::QueueFull;
            }
        }

        if (!displayAndRing) return result;

        //  FramePublisher（可选扩展）
        if (m_framePublisher) {
            m_framePublisher->submit(group);
            result.publisher = true;
        }

        //  降采样 + DisplayBuffer 更新
        downsample(*group, m_displayPoints.load(std::memory_order_relaxed));
        if (m_displayBuffer) {
            m_displayBuffer->update(group);
            m_displayBuffer->updateFullRes(group);  // 存储全分辨率频率供成像
            result.display = true;
        }
        // 环形实时馈送：每触发直接入队（独立工作线程消费），
        // 避免 DisplayBuffer latest-only + 主线程轮询在高触发率下丢触发
        if (m_ringFeedSink) {
            m_ringFeedSink(m_cardId, group->triggerSeq, group->freqA, group->freqB);
            result.ring = true;
        }
    } catch (const std::bad_alloc&) {
        // 显示/存储热路径仍可能因瞬时内存压力分配失败，丢弃本帧但保持线程存活。
        m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
        result.exception = true;
    } catch (...) {
        // 任意单帧处理异常都不应杀死整条卡处理线程。
        m_stats.triggersDiscarded.fetch_add(1, std::memory_order_relaxed);
        result.exception = true;
    }
    return result;
}
