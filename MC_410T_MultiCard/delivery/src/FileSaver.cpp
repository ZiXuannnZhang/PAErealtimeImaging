#include "FileSaver.h"
#include <QDir>
#include <QDateTime>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>

// F16C 快速路径：/arch:AVX2 时 MSVC 定义 __AVX__，启用 F16C 指令集
// _mm256_cvtps_ph: 8×float32 → 8×float16，一条指令，约为标量实现速度的8倍
#if defined(__AVX__)
#include <immintrin.h>
#endif

// ─
// float32  float16 转换（软件实现，符合 IEEE 754 half precision）
// 
uint16_t FileSaver::float32ToFloat16(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);

    uint32_t sign     = (bits >> 31) & 0x1;
    uint32_t exponent = (bits >> 23) & 0xFF;
    uint32_t mantissa =  bits        & 0x7FFFFF;

    //  特殊值处理 
    if (exponent == 0xFF) {
        // NaN 或 Inf
        if (mantissa != 0) return static_cast<uint16_t>((sign << 15) | 0x7C00 | 0x0200);  // NaN
        return static_cast<uint16_t>((sign << 15) | 0x7C00);  // Inf
    }

    //  指数偏置转换：float32 偏置 127  float16 偏置 15 
    int exp16 = static_cast<int>(exponent) - 127 + 15;

    if (exp16 >= 31) {
        // 溢出  Inf
        return static_cast<uint16_t>((sign << 15) | 0x7C00);
    }
    if (exp16 <= 0) {
        // 下溢或次正规数
        if (exp16 < -10) return static_cast<uint16_t>(sign << 15);  // flush to zero
        // 次正规数
        mantissa = (mantissa | 0x800000) >> (1 - exp16);
        // 四舍五入（round-to-nearest-even）
        uint32_t round = mantissa & 0xFFF;
        mantissa >>= 13;
        if (round > 0x800 || (round == 0x800 && (mantissa & 1)))
            ++mantissa;
        return static_cast<uint16_t>((sign << 15) | mantissa);
    }

    //  正规数 
    // 四舍五入尾数低13位
    uint32_t m13 = mantissa & 0x1FFF;
    uint32_t m10 = mantissa >> 13;
    if (m13 > 0x1000 || (m13 == 0x1000 && (m10 & 1))) {
        ++m10;
        if (m10 >= 0x400) { ++exp16; m10 = 0; }
    }
    if (exp16 >= 31) return static_cast<uint16_t>((sign << 15) | 0x7C00);  // 进位溢出

    return static_cast<uint16_t>((sign << 15) | (exp16 << 10) | m10);
}

void FileSaver::convertBatch(const float* src, uint16_t* dst, int count) {
#if defined(__AVX__)
    // F16C 快速路径：每次处理 8 个 float32 → float16（VCVTPS2PH 指令）
    // 需要 /arch:AVX2（MSVC）或 -mf16c（GCC/Clang）
    int i = 0;
    for (; i + 7 < count; i += 8) {
        __m256 v = _mm256_loadu_ps(src + i);
        // 0 = round to nearest even（Windows FP异常默认屏蔽，无需显式抑制）
        __m128i h = _mm256_cvtps_ph(v, 0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), h);
    }
    // 尾部余量（<8个点）用标量处理
    for (; i < count; ++i)
        dst[i] = float32ToFloat16(src[i]);
#else
    // 标量回退路径（无 AVX 时）
    for (int i = 0; i < count; ++i)
        dst[i] = float32ToFloat16(src[i]);
#endif
}

// 
// 构造 / 析构
// 
FileSaver::FileSaver(int cardId, QObject* parent)
    : QThread(parent), m_cardId(cardId) {
    setObjectName(QString("FileSaver_%1").arg(cardId));
}

FileSaver::~FileSaver() {
    if (isRunning()) {
        // requestStop() 设置 m_running=false，run() 循环会在下一轮 msleep(1) 后退出
        requestStop();
        if (!wait(500)) {
            terminate();
            wait();   // 确保线程真正结束后再析构 QThread，避免 qFatal
        }
    }
    closeFiles();
}

// 
// 控制接口
// 
void FileSaver::startSaving(const QString& directory, int triggersPerFile,
                             const QString& suffix) {
    // 关闭之前可能仍打开的文件（stopSaving 未及时关闭时）。关文件前那一次刷盘
    // 若失败，写盘链路已不可信：告警停保存并保持原目录/原参数不动，绝不带着
    // 一次已知故障去换目录开新文件。
    if (!closeFiles()) {
        handleWriteFault(QStringLiteral("开启新会话前的落盘"));
        return;
    }
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    m_accumTriggers = 0;

    m_saveDirectory     = directory;
    m_triggersPerFile   = triggersPerFile;
    m_fileSuffix        = suffix;
    m_fileSequence      = 0;
    m_currentFileTriggers = 0;
    m_currentSourceIPv4 = 0;
    m_writeFaulted = false;   // 新会话重新武装写盘故障告警
    resetPhysicalRoundState();
    // 不保证从 000 开始，只保证不覆盖：复用一个已有数据的目录时让号到空闲序号。
    resolveFreeSequence();
    m_saving.store(true, std::memory_order_release);
    emit statusMessage(QString("Card%1: 开始保存到 %2").arg(m_cardId + 1).arg(directory));
}

void FileSaver::stopSaving() {
    m_saving.store(false, std::memory_order_release);
    // 停止时立即刷盘 + 关闭文件，确保未达触发数部分也能完整写入
    const bool flushed = flushWriteBuffers();
    closeFiles();
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    m_accumTriggers = 0;
    m_currentFileTriggers = 0;
    resetPhysicalRoundState();
    if (!flushed) handleWriteFault(QStringLiteral("停止保存时的落盘"));
    emit statusMessage(QString("Card%1: 停止保存，共保存 %2 触发")
                       .arg(m_cardId + 1).arg(m_savedCount.load()));
}

void FileSaver::saveTriggerGroup(const TriggerGroupPtr& group) {
    if (!m_saving.load(std::memory_order_relaxed)) return;
    if (m_saveQueue.size_approx() < FILESAVER_QUEUE_SIZE)
        m_saveQueue.enqueue(group);
}

int FileSaver::queueDepth() const {
    return static_cast<int>(m_saveQueue.size_approx());
}

void FileSaver::resetPhysicalRoundState() {
    m_haveCurrentPhysicalRound = false;
    m_currentPhysicalRoundGeneration = 0;
}

void FileSaver::recordRollover(const FileRolloverInfo& info) {
    m_lastRollover = info;
    ++m_rolloverCount;
    if (info.reason == QStringLiteral("physical_round"))
        ++m_roundRolloverCount;
    emit fileRolled(m_cardId, info.reason,
                    info.oldRoundGeneration, info.newRoundGeneration,
                    info.oldFileSequence, info.newFileSequence,
                    info.oldFileTriggerCount, info.manualMode);
}

//
// 文件名生成
//
QString FileSaver::fileNameFor(const QString& channel, int sequence) const {
    QString n = m_fileSuffix.isEmpty()
        ? QString("Card%1_Ch%2_%3.dat")
              .arg(m_cardId + 1)
              .arg(channel)
              .arg(sequence, 3, 10, QChar('0'))
        : QString("Card%1_Ch%2_%3_%4.dat")
              .arg(m_cardId + 1)
              .arg(channel)
              .arg(m_fileSuffix)
              .arg(sequence, 3, 10, QChar('0'));
    return QDir(m_saveDirectory).filePath(n);
}

QString FileSaver::generateFileName(const QString& channel) const {
    return fileNameFor(channel, m_fileSequence);
}

// 落盘不变量第一道防线：任何一次开文件之前，先把序号让到一个 A/B 两侧都
// 不存在的名字上。截断式重开（同 sessionGen 残留帧、sourceIPv4 变化、
// startSaving 复用目录）因此不可能命中已有文件。探测从 m_fileSequence 起
// 向后进行，正常情况下 0 次跳过，只在真正撞名时代价才出现。
int FileSaver::resolveFreeSequence() {
    const int start = m_fileSequence < 0 ? 0 : m_fileSequence;
    // 有界探测：穷尽后保持原序号，交由 NewOnly 打开失败处理（绝不截断）。
    constexpr long long kMaxProbe = 1000000;
    long long candidate = start;
    for (long long probe = 0; probe < kMaxProbe; ++probe, ++candidate) {
        if (candidate > static_cast<long long>((std::numeric_limits<int>::max)()))
            break;
        const int seq = static_cast<int>(candidate);
        if (QFile::exists(fileNameFor(QStringLiteral("A"), seq)) ||
            QFile::exists(fileNameFor(QStringLiteral("B"), seq)))
            continue;
        if (seq != start) {
            FileRolloverInfo info;
            info.happened = true;
            info.reason = QStringLiteral("sequence_collision");
            info.oldRoundGeneration = m_currentPhysicalRoundGeneration;
            info.newRoundGeneration = m_currentPhysicalRoundGeneration;
            info.oldFileSequence = start;
            info.oldFileTriggerCount = m_currentFileTriggers;
            info.newFileSequence = seq;
            info.manualMode = (m_currentGen == 0);
            recordRollover(info);
        }
        m_fileSequence = seq;
        return seq;
    }
    m_fileSequence = start;
    return start;
}

//
// 文件操作
//
void FileSaver::openNewFiles(uint32_t sourceIPv4) {
    // 关旧文件时那一次刷盘若已失败，写盘链路已不可信：告警停保存，不开新文件。
    if (!closeFiles()) {
        handleWriteFault(QStringLiteral("关闭上一批文件"));
        return;
    }
    m_currentSourceIPv4 = sourceIPv4;

    QDir().mkpath(m_saveDirectory);

    // 先让号，再独占创建。两道防线缺一不可：前者给出不撞名的序号，
    // 后者保证即使并发下撞名也以失败告警，而不是静默截断已有数据。
    resolveFreeSequence();

    m_fileChannelA = new QFile(generateFileName("A"));
    m_fileChannelB = new QFile(generateFileName("B"));

    if (!m_fileChannelA->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        emit errorOccurred(QString("Card%1: 无法打开文件 %2: %3")
            .arg(m_cardId + 1).arg(m_fileChannelA->fileName())
            .arg(m_fileChannelA->errorString()));
        delete m_fileChannelA; m_fileChannelA = nullptr;
    }
    if (!m_fileChannelB->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        emit errorOccurred(QString("Card%1: 无法打开文件 %2: %3")
            .arg(m_cardId + 1).arg(m_fileChannelB->fileName())
            .arg(m_fileChannelB->errorString()));
        delete m_fileChannelB; m_fileChannelB = nullptr;
    }
    m_currentFileTriggers = 0;
}

bool FileSaver::closeFiles() {
    const bool flushed = flushWriteBuffers();   // 关闭前将尚未写盘的积累数据刷入文件
    if (m_fileChannelA) {
        m_fileChannelA->close();
        delete m_fileChannelA;
        m_fileChannelA = nullptr;
    }
    if (m_fileChannelB) {
        m_fileChannelB->close();
        delete m_fileChannelB;
        m_fileChannelB = nullptr;
    }
    return flushed;
}

//
// 写盘缓冲
//
void FileSaver::handleWriteFault(const QString& where) {
    // 一次性告警：磁盘满/介质拔出通常是持续性的，不要每段刷一条。
    if (m_writeFaulted) return;
    m_writeFaulted = true;
    m_saving.store(false, std::memory_order_release);
    emit errorOccurred(QString("Card%1: %2 写盘失败，已停止保存；未落盘的一段已按记录边界回退丢弃")
                           .arg(m_cardId + 1).arg(where));
}

bool FileSaver::flushWriteBuffers()
{
    if (m_accumTriggers == 0) return true;
    const qint64 bytesA = static_cast<qint64>(m_writeAccumA.size()) * sizeof(uint16_t);
    const qint64 bytesB = static_cast<qint64>(m_writeAccumB.size()) * sizeof(uint16_t);
    const bool openA = m_fileChannelA && m_fileChannelA->isOpen();
    const bool openB = m_fileChannelB && m_fileChannelB->isOpen();
    // 本次刷盘前的记录边界。.dat 是定长记录、无文件头，一旦留下半条记录，
    // 该文件后续所有记录的边界都会错位且无法离线修复，所以失败必须整段回退。
    const qint64 sizeA = openA ? m_fileChannelA->size() : 0;
    const qint64 sizeB = openB ? m_fileChannelB->size() : 0;

    qint64 wroteA = 0;
    qint64 wroteB = 0;
    if (openA && bytesA > 0)
        wroteA = m_fileChannelA->write(reinterpret_cast<const char*>(m_writeAccumA.data()), bytesA);
    if (openB && bytesB > 0)
        wroteB = m_fileChannelB->write(reinterpret_cast<const char*>(m_writeAccumB.data()), bytesB);

#ifdef FILESAVER_TEST_SEAM
    if (m_writeFaultForTest == 1) {
        wroteA = bytesA > 0 ? -1 : 0;
        wroteB = bytesB > 0 ? -1 : 0;
    } else if (m_writeFaultForTest == 2) {
        wroteA = bytesA > 0 ? -1 : 0;
    } else if (m_writeFaultForTest == 3) {
        wroteB = bytesB > 0 ? -1 : 0;
    }
#endif

    // 成功判据：每通道「不需要写 或 全部字节都写进去了」。短写按失败处理。
    const bool okA = !openA || bytesA == 0 || wroteA == bytesA;
    const bool okB = !openB || bytesB == 0 || wroteB == bytesB;
    if (okA && okB) {
        m_writeAccumA.clear();
        m_writeAccumB.clear();
        m_accumTriggers = 0;
        return true;
    }

    // 成对回退：一侧失败也把另一侧撤回，A/B 两侧的记录数恒相等。
    if (openA) m_fileChannelA->resize(sizeA);
    if (openB) m_fileChannelB->resize(sizeB);
    // 本段已不可恢复（不重试，避免把半条记录再写一遍）。
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    // 计数同步回退：本段 m_accumTriggers 个触发并未落盘，诊断计数必须与文件
    // 里的实际记录数保持一致，否则现场看到的"已保存触发数"会多于磁盘上的记录。
    const int dropped = m_accumTriggers;
    m_currentFileTriggers -= dropped;
    if (m_currentFileTriggers < 0) m_currentFileTriggers = 0;
    const std::uint64_t saved = m_savedCount.load(std::memory_order_relaxed);
    m_savedCount.store(saved > static_cast<std::uint64_t>(dropped)
                           ? saved - static_cast<std::uint64_t>(dropped) : 0,
                       std::memory_order_relaxed);
    m_accumTriggers = 0;
    return false;
}

//
// 存储线程主循环
//
void FileSaver::run() {
    m_running.store(true);

    while (m_running.load()) {
        TriggerGroupPtr group;
        if (!m_saveQueue.try_dequeue(group)) {
            // 队列空：处理会话主动落盘请求（方案A）——
            // 圈末/超时边界后由 UI 线程发起，本线程在队列排空后刷盘关闭，
            // 保证采集彻底停止时最后一个会话的数据也已写入
            if (m_closeRequest.exchange(false, std::memory_order_acq_rel)) {
                if (!closeFiles()) handleWriteFault(QStringLiteral("会话主动落盘"));
            }
            QThread::msleep(1);  // 队列空，短暂休眠
            continue;
        }
        consumeTriggerGroup(group);
    }

    if (!closeFiles()) handleWriteFault(QStringLiteral("保存线程退出时的落盘"));
}


bool FileSaver::consumeTriggerGroup(const TriggerGroupPtr& group) {
    if (!group) return false;
    if (!m_saving.load()) {
        // 保存已停止 → 丢弃队列中的残余数据
        // 不做刷盘/关文件，因为 stopSaving() 已处理
        return false;
    }

    // 自动保存会话代切换：边界处刷盘并关闭上一会话文件，
    // 按新会话代查询目录（gen=0 保持当前目录，即手动模式）
    const uint64_t gen = group->sessionGen;
    if (gen != m_currentGen) {
        // 关上一会话文件时那一次刷盘若失败，已由 handleWriteFault 停保存并
        // 回退；本帧不再写，避免把新数据接在一条被截断的记录边界后面。
        if (!closeFiles()) {
            handleWriteFault(QStringLiteral("会话代切换时的落盘"));
            return false;
        }
        m_currentGen = gen;
        m_currentFileTriggers = 0;
        m_dropGen = 0;
        // A new auto-save session is a new measurement: physical round file
        // state must not inherit from the previous session.
        resetPhysicalRoundState();
        // 序号只在目录真的变化时归零。gen=0（手动）或解析不到新目录时会沿用
        // 当前目录，此时归零等于让新一轮从头占用旧序号：即便 resolveFreeSequence()
        // 能兜住不致覆盖，同一目录内的编号也会失去单调性。
        bool directoryChanged = false;
        if (m_dirResolver && gen != 0) {
            const QString d = m_dirResolver(gen);
            if (!d.isEmpty()) {
                directoryChanged = (d != m_saveDirectory);
                m_saveDirectory = d;
            } else {
                // 方案C：目录未注册（正常流程不应发生）——告警并丢弃本会话数据，
                // 防止误写入上一会话目录
                emit errorOccurred(QString("Card%1: 自动保存会话代 %2 目录未注册，丢弃本会话数据")
                                       .arg(m_cardId + 1).arg(gen));
                m_dropGen = gen;
            }
        }
        if (directoryChanged)
            m_fileSequence = 0;
    }
    // 方案C：目录缺失的会话代数据直接丢弃
    if (m_dropGen != 0 && gen == m_dropGen)
        return false;

    // 物理轮次边界（数据面权威）：TriggerGroup::roundGeneration 决定保存
    // 文件边界。判定必须发生在写当前 TriggerGroup 之前；即使上一轮数据
    // 仍在 saver 队列中排队，FIFO 消费顺序也能保证旧 generation 完整写入
    // 旧文件后，首个新 generation 组才触发轮转。仅 normalizer 分类过的
    // LogicalScan 参与轮次判定，保持旧路径/旧数据行为不变。
    if (group->normalizationApplied &&
        group->physicalDecision == paimage::PhysicalTriggerDecision::LogicalScan) {
        if (!m_haveCurrentPhysicalRound) {
            m_haveCurrentPhysicalRound = true;
            m_currentPhysicalRoundGeneration = group->roundGeneration;
        } else if (group->roundGeneration != m_currentPhysicalRoundGeneration) {
            // 轮转前那次刷盘若失败，整段已回退丢弃；此时不得推进序号并开新文件，
            // 否则记录边界会带着缺口继续。停保存并让操作员介入。
            if (!closeFiles()) {
                handleWriteFault(QStringLiteral("物理轮次轮转时的落盘"));
                return false;
            }
            FileRolloverInfo info;
            info.happened = true;
            info.reason = QStringLiteral("physical_round");
            info.oldRoundGeneration = m_currentPhysicalRoundGeneration;
            info.newRoundGeneration = group->roundGeneration;
            info.oldFileSequence = m_fileSequence;
            info.oldFileTriggerCount = m_currentFileTriggers;
            ++m_fileSequence;   // 安全推进：绝不覆盖上一物理轮次的文件
            m_currentFileTriggers = 0;
            info.newFileSequence = m_fileSequence;
            info.manualMode = (m_currentGen == 0);
            m_currentPhysicalRoundGeneration = group->roundGeneration;
            recordRollover(info);
        }
    }

    // 打开文件（首次 或 IP 来源变化时）。
    // IP 来源变化不再重置序号：归零既会把上一来源的 _000 顶掉，还会就地抵消
    // 物理轮次轮转刚做的 ++m_fileSequence「安全推进」。新来源改落到下一个空闲
    // 序号，旧文件保持不动（见 FileSaver.h 的落盘不变量）。
    if (!m_fileChannelA || !m_fileChannelB ||
        group->sourceIPv4 != m_currentSourceIPv4) {
        openNewFiles(group->sourceIPv4);
    }

    // 文件序号翻滚（容量上限 triggersPerFile）
    if (m_currentFileTriggers >= m_triggersPerFile) {
        // 翻滚前先刷盘。刷盘失败则该段已按记录边界回退丢弃：此时不再推进序号
        // 并开新文件，避免把缺口带进下一批记录。
        if (!flushWriteBuffers()) {
            handleWriteFault(QStringLiteral("文件容量轮转时的落盘"));
            return false;
        }
        FileRolloverInfo info;
        info.happened = true;
        info.reason = QStringLiteral("capacity");
        info.oldRoundGeneration = m_currentPhysicalRoundGeneration;
        info.newRoundGeneration = m_currentPhysicalRoundGeneration;
        info.oldFileSequence = m_fileSequence;
        info.oldFileTriggerCount = m_currentFileTriggers;
        ++m_fileSequence;
        info.newFileSequence = m_fileSequence;
        info.manualMode = (m_currentGen == 0);
        closeFiles();
        openNewFiles(m_currentSourceIPv4);
        recordRollover(info);
    }

    if (!m_fileChannelA || !m_fileChannelB) {
        // 写盘故障已由 handleWriteFault 停止保存并报告过，这里不再重复报一条。
        if (!m_saving.load(std::memory_order_acquire)) return false;
        emit errorOccurred(QString("Card%1: 保存文件未成功打开，已自动停止保存")
                           .arg(m_cardId + 1));
        m_saving.store(false, std::memory_order_release);
        closeFiles();
        return false;
    }

    int n = group->sampleCount;
    if (n <= 0) return false;

    // float32  float16 批量转换并积累到内存缓冲区
    const size_t offset = m_writeAccumA.size();
    try {
        m_writeAccumA.resize(offset + n);
        m_writeAccumB.resize(offset + n);
        convertBatch(group->freqA.data(), m_writeAccumA.data() + offset, n);
        convertBatch(group->freqB.data(), m_writeAccumB.data() + offset, n);
    } catch (const std::bad_alloc&) {
        // 撤掉本触发刚扩出来的那段：绝不把未转换的尾巴当成记录写进 .dat
        // （无文件头，脏尾巴会污染其后所有记录的边界）。
        m_writeAccumA.resize(offset);
        m_writeAccumB.resize(offset);
        emit errorOccurred(QString("Card%1: 保存线程内存不足，已自动停止保存")
                           .arg(m_cardId + 1));
        m_saving.store(false, std::memory_order_release);
        closeFiles();
        return false;
    } catch (...) {
        m_writeAccumA.resize(offset);
        m_writeAccumB.resize(offset);
        emit errorOccurred(QString("Card%1: 保存线程发生异常，已自动停止保存")
                           .arg(m_cardId + 1));
        m_saving.store(false, std::memory_order_release);
        closeFiles();
        return false;
    }

    ++m_accumTriggers;
    // 记账在周期性 flush 之前完成：flushWriteBuffers 的失败回退按 m_accumTriggers
    // 扣减，只有当本触发也已入账时扣减才与文件里的实际记录数完全对齐。
    ++m_currentFileTriggers;
    m_savedCount.fetch_add(1, std::memory_order_relaxed);

    // 达到合并阈値时一次性写盘（WRITE_BUFFER_TRIGGERS 个触发合并为一条大 I/O）
    if (m_accumTriggers >= WRITE_BUFFER_TRIGGERS && !flushWriteBuffers()) {
        handleWriteFault(QStringLiteral("周期性落盘"));
        return false;
    }
    return m_saving.load(std::memory_order_acquire);
}

void FileSaver::serviceCloseRequest() {
    if (m_closeRequest.exchange(false, std::memory_order_acq_rel)) {
        if (!closeFiles()) handleWriteFault(QStringLiteral("会话主动落盘"));
    }
}

void FileSaver::suspendForSourceRestart() {
    const bool hadData=m_currentFileTriggers>0;
    stopSaving();
    // The immutable source listener is rebuilt when sample count changes.
    // .dat 无文件头，同一文件里混入不同 sampleCount 的记录会让离线解析失去
    // 记录边界，所以换布局必须换文件（而不是为了防覆盖——防覆盖已由
    // resolveFreeSequence() + NewOnly 负责）。Format and names unchanged.
    if(hadData)++m_fileSequence;
}

void FileSaver::resumeAfterSourceRestart() {
    // stopSaving() (via suspendForSourceRestart) already reset the physical
    // round file state; keep the invariant explicit for the resume half.
    resetPhysicalRoundState();
    m_saving.store(true,std::memory_order_release);
}
