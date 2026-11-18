#include "FileSaver.h"
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>

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
    if (!closeFiles()) qWarning().noquote() << "FileSaver destructor:" << m_faultDetail;
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
    m_currentSamples=0;
    m_currentIdentity={};m_sealedThrough={};
    m_writeFaulted = false;   // 新会话重新武装写盘故障告警
    m_faultDetail.clear();
    resetPhysicalRoundState();
    // 不保证从 000 开始，只保证不覆盖：复用一个已有数据的目录时让号到空闲序号。
    resolveFreeSequence();
    m_saving.store(true, std::memory_order_release);
    emit statusMessage(QString("Card%1: 开始保存到 %2").arg(m_cardId + 1).arg(directory));
}

void FileSaver::stopSaving() {
    m_saving.store(false, std::memory_order_release);
    // 停止时立即刷盘 + 关闭文件，确保未达触发数部分也能完整写入
    const bool flushed = closeFiles();
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
    m_fileRound.reset();
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
            info.oldRoundGeneration = currentPhysicalRoundGeneration();
            info.newRoundGeneration = currentPhysicalRoundGeneration();
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

    auto a = std::make_unique<QFile>(generateFileName("A"));
    auto b = std::make_unique<QFile>(generateFileName("B"));
#ifdef FILESAVER_TEST_SEAM
    if (m_openRaceForTest) m_openRaceForTest(a->fileName(), b->fileName());
#endif
    const bool openedA = a->open(QIODevice::WriteOnly | QIODevice::NewOnly);
    const bool openedB = b->open(QIODevice::WriteOnly | QIODevice::NewOnly);
    if (!openedA || !openedB) {
        auto noteOpenFailure = [&](const QFile &file) {
            m_faultDetail += QStringLiteral("; 无法打开 %1: %2").arg(file.fileName(), file.errorString());
        };
        if (!openedA) noteOpenFailure(*a);
        if (!openedB) noteOpenFailure(*b);
        // NewOnly proves ownership only on success. Neither local handle has
        // entered the write path, so it is safe to roll back that creation.
        auto removeCreated = [&](QFile &file) {
#ifdef FILESAVER_TEST_SEAM
            if (m_removeFaultForTest) {
                m_faultDetail += QStringLiteral("; 未能回收残留文件 %1: injected").arg(file.fileName());
                return;
            }
#endif
            if (!file.remove())
                m_faultDetail += QStringLiteral("; 未能回收残留文件 %1: %2").arg(file.fileName(), file.errorString());
        };
        if (openedA) removeCreated(*a);
        if (openedB) removeCreated(*b);
        a.reset(); b.reset();
        handleWriteFault(QStringLiteral("创建 A/B 文件对"));
        return;
    }
    m_fileChannelA = a.release();
    m_fileChannelB = b.release();
    m_currentFileTriggers = 0;
    m_confirmedA = m_confirmedB = 0;
}

bool FileSaver::closeFiles() {
    bool ok = flushWriteBuffers();
    auto closeOne = [&](QFile*& file) {
        if (!file) return;
        file->close();
        if (file->error() != QFileDevice::NoError) {
            m_faultDetail += QStringLiteral("; 关闭 %1: %2（文件完整性未知）")
                                 .arg(file->fileName(), file->errorString());
            ok = false;
        }
        delete file;
        file = nullptr;
    };
    closeOne(m_fileChannelA);
    closeOne(m_fileChannelB);
    return ok;
}

void FileSaver::handleWriteFault(const QString& where) {
    m_saving.store(false, std::memory_order_release);
    if (m_writeFaulted) return;
    m_writeFaulted = true;
    emit errorOccurred(QString("Card%1: %2 写盘失败，已停止保存%3")
                           .arg(m_cardId + 1).arg(where, m_faultDetail));
}

bool FileSaver::flushWriteBuffers()
{
    if (m_accumTriggers == 0) return true;
    const qint64 bytesA = static_cast<qint64>(m_writeAccumA.size()) * sizeof(uint16_t);
    const qint64 bytesB = static_cast<qint64>(m_writeAccumB.size()) * sizeof(uint16_t);
    auto writeOne = [&](QFile* f, const std::vector<uint16_t>& data, qint64 bytes, int side) {
        if (!f || !f->isOpen()) return qint64(-1);
#ifdef FILESAVER_TEST_SEAM
        if (m_writeFaultForTest == 1 || m_writeFaultForTest == side + 1)
            return f->write(reinterpret_cast<const char*>(data.data()), (bytes > 1 ? bytes / 2 - (bytes / 2 % 2 == 0 ? 1 : 0) : 0));
#else
        (void)side;
#endif
        return f->write(reinterpret_cast<const char*>(data.data()), bytes);
    };
    const qint64 wroteA = writeOne(m_fileChannelA, m_writeAccumA, bytesA, 1);
    const qint64 wroteB = writeOne(m_fileChannelB, m_writeAccumB, bytesB, 2);
    auto flushOne = [&](QFile* f, int side) {
        if (!f || !f->isOpen()) return false;
#ifdef FILESAVER_TEST_SEAM
        if (m_flushFaultForTest == side) return false;
#else
        (void)side;
#endif
        return f->flush();
    };
    // Flush both even when a write failed, then close both before rollback. No
    // buffered bytes may be written back after truncation on the fresh handles.
    const bool flushA = flushOne(m_fileChannelA, 1);
    const bool flushB = flushOne(m_fileChannelB, 2);
    const bool ok = wroteA == bytesA && wroteB == bytesB && flushA && flushB;
    if (ok) {
        m_confirmedA += bytesA; m_confirmedB += bytesB;
        m_savedCount.fetch_add(static_cast<uint64_t>(m_accumTriggers), std::memory_order_relaxed);
    } else {
        auto description = [](QFile* f) { return f ? f->fileName() : QStringLiteral("<未打开>"); };
        const QString pathA = description(m_fileChannelA), pathB = description(m_fileChannelB);
        m_faultDetail = QStringLiteral("；写入/flush A %1 (%2/%3, flush=%4, %5)，B %6 (%7/%8, flush=%9, %10)")
            .arg(pathA).arg(wroteA).arg(bytesA).arg(flushA).arg(m_fileChannelA ? m_fileChannelA->errorString() : QStringLiteral("未打开"))
            .arg(pathB).arg(wroteB).arg(bytesB).arg(flushB).arg(m_fileChannelB ? m_fileChannelB->errorString() : QStringLiteral("未打开"));
        auto discardHandle = [&](QFile*& f) {
            if (!f) return;
            f->close();
            if (f->error() != QFileDevice::NoError)
                m_faultDetail += QStringLiteral("；关闭 %1: %2").arg(f->fileName(), f->errorString());
            delete f; f = nullptr;
        };
        discardHandle(m_fileChannelA); discardHandle(m_fileChannelB);
        auto rollback = [&](const QString& path, qint64 boundary, int side) {
            QFile f(path);
            bool success = f.open(QIODevice::ReadWrite | QIODevice::ExistingOnly);
            if (success && f.size() < boundary) {
                m_faultDetail += QStringLiteral("；回退 %1: 文件短于已确认边界（文件完整性未知）").arg(path);
                f.close();
                return false;
            }
#ifdef FILESAVER_TEST_SEAM
            if (m_rollbackFaultForTest == side) {
                m_faultDetail += QStringLiteral("；回退 %1: 注入 resize 失败（文件完整性未知）").arg(path);
                if (success) f.close();
                return false;
            }
#else
            (void)side;
#endif
            if (success) success = f.resize(boundary);
            if (!success) m_faultDetail += QStringLiteral("；回退 %1: %2（文件完整性未知）").arg(path, f.errorString());
            if (f.isOpen()) {
                f.close();
                if (f.error() != QFileDevice::NoError) {
                    m_faultDetail += QStringLiteral("；回退后关闭 %1: %2（文件完整性未知）").arg(path, f.errorString());
                    success = false;
                }
            }
            return success;
        };
        const bool rolledA = rollback(pathA, m_confirmedA, 1);
        const bool rolledB = rollback(pathB, m_confirmedB, 2);
        if (rolledA && rolledB) m_faultDetail += QStringLiteral("；两侧已回退至此前确认的边界");
        m_currentFileTriggers -= m_accumTriggers;
    }
    m_writeAccumA.clear(); m_writeAccumB.clear(); m_accumTriggers = 0;
    return ok;
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


void FileSaver::sealRound(paimage::RoundIdentity identity){
    if(!identity.valid() || (m_sealedThrough.valid() && identity<=m_sealedThrough))return;
    m_sealedThrough=identity;
    if(m_currentIdentity.valid() && m_currentIdentity<=identity && !closeFiles())
        handleWriteFault(QStringLiteral("轮次封存"));
}

bool FileSaver::consumeTriggerGroup(const TriggerGroupPtr& group) {
    if (!group) return false;
    if (!m_saving.load()) {
        // 保存已停止 → 丢弃队列中的残余数据
        // 不做刷盘/关文件，因为 stopSaving() 已处理
        return false;
    }

    const paimage::RoundIdentity identity{group->measurementSession,group->roundGeneration};
    if(!m_isLateWriter && group->normalizationApplied && identity.valid() &&
       (group->saveForceLate || (group->saveTailCandidate && (m_currentIdentity!=identity || m_currentGen!=group->sessionGen || !m_fileChannelA || !m_fileChannelB || m_currentSamples!=group->sampleCount || m_currentSourceIPv4!=group->sourceIPv4)) || (m_sealedThrough.valid() && identity<=m_sealedThrough) ||
        (m_currentIdentity.valid() && identity<m_currentIdentity))){
        const QString root=group->sessionGen ? (m_dirResolver ? m_dirResolver(group->sessionGen) : QString()) : m_saveDirectory;
        if(root.isEmpty()){emit errorOccurred(QStringLiteral("迟到帧保存目录不可用"));return false;}
        FileSaver late(m_cardId);late.m_isLateWriter=true;
        const QString dir=QDir(root).filePath(QStringLiteral("_late/session-%1-round-%2").arg(identity.measurementSession).arg(identity.roundGeneration));
        late.startSaving(dir,1,m_fileSuffix);
        auto copy=std::make_shared<TriggerGroup>(*group);copy->sessionGen=0;
        QObject::connect(&late, &FileSaver::errorOccurred, this, [this](const QString& error) {
            m_faultDetail = QStringLiteral("；迟到文件：") + error;
        }, Qt::DirectConnection);
#ifdef FILESAVER_TEST_SEAM
        late.m_writeFaultForTest=m_lateWriteFaultForTest; late.m_flushFaultForTest=m_flushFaultForTest; late.m_rollbackFaultForTest=m_rollbackFaultForTest;
#endif
        const bool accepted=late.consumeTriggerGroup(copy);
        const bool flushed=late.closeFiles();
        m_acceptedCount.fetch_add(late.acceptedCount(), std::memory_order_relaxed);
        if(!accepted || !flushed){
            const QString lateDetail = m_faultDetail.isEmpty() ? late.m_faultDetail : m_faultDetail;
            const bool mainClosed = closeFiles();
            const QString mainDetail = mainClosed ? QString() : m_faultDetail;
            m_faultDetail = lateDetail + (mainClosed ? QString() : QStringLiteral("；主文件关闭失败：") + mainDetail);
            handleWriteFault(QStringLiteral("迟到帧保存")); return false;
        }
        m_savedCount.fetch_add(1,std::memory_order_relaxed);
        emit statusMessage(QStringLiteral("Card%1: 隔离迟到帧 session=%2 round=%3 trigger=%4").arg(m_cardId+1).arg(identity.measurementSession).arg(identity.roundGeneration).arg(group->triggerSeq));
        return true;
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

    // The consumer owns the boundary: queued old groups finish first, and
    // late groups have already been routed above. A save-generation change
    // has already closed the pair and reset m_fileRound, so do not advance
    // the file sequence again when adopting the first identity there.
    if (group->normalizationApplied &&
        group->physicalDecision == paimage::PhysicalTriggerDecision::LogicalScan) {
        const bool roundChanged = m_fileRound &&
            (identity.valid() && m_fileRound->valid()
                ? *m_fileRound != identity
                : m_fileRound->roundGeneration != identity.roundGeneration);
        FileRolloverInfo info;
        if (roundChanged) {
            if (!closeFiles()) {
                handleWriteFault(QStringLiteral("物理轮次轮转时的落盘"));
                return false;
            }
            info.happened = true;
            info.reason = QStringLiteral("physical_round");
            info.oldRoundGeneration = m_fileRound->roundGeneration;
            info.newRoundGeneration = identity.roundGeneration;
            info.oldFileSequence = m_fileSequence;
            info.oldFileTriggerCount = m_currentFileTriggers;
            ++m_fileSequence;
            m_currentFileTriggers = 0;
            info.newFileSequence = m_fileSequence;
            info.manualMode = (m_currentGen == 0);
        }
        if (!m_fileRound || roundChanged || identity.valid())
            m_fileRound = identity;
        if (identity.valid()) {
            if (m_currentIdentity.valid() && m_currentIdentity < identity &&
                (!m_sealedThrough.valid() || m_sealedThrough < m_currentIdentity))
                m_sealedThrough = m_currentIdentity;
            m_currentIdentity = identity;
        }
        if (roundChanged)
            recordRollover(info);
    }

    // 打开文件（首次 或 IP 来源变化时）。
    // IP 来源变化不再重置序号：归零既会把上一来源的 _000 顶掉，还会就地抵消
    // 物理轮次轮转刚做的 ++m_fileSequence「安全推进」。新来源改落到下一个空闲
    // 序号，旧文件保持不动（见 FileSaver.h 的落盘不变量）。
    if (!m_fileChannelA || !m_fileChannelB ||
        group->sourceIPv4 != m_currentSourceIPv4) {
        openNewFiles(group->sourceIPv4);
        if (!m_fileChannelA || !m_fileChannelB) return false;
    }
    m_currentSamples=group->sampleCount;

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
        info.oldRoundGeneration = currentPhysicalRoundGeneration();
        info.newRoundGeneration = currentPhysicalRoundGeneration();
        info.oldFileSequence = m_fileSequence;
        info.oldFileTriggerCount = m_currentFileTriggers;
        ++m_fileSequence;
        info.newFileSequence = m_fileSequence;
        info.manualMode = (m_currentGen == 0);
        if (!closeFiles()) { handleWriteFault(QStringLiteral("容量轮转关闭文件")); return false; }
        openNewFiles(m_currentSourceIPv4);
        if (!m_fileChannelA || !m_fileChannelB) return false;
        recordRollover(info);
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
        if (!closeFiles()) handleWriteFault(QStringLiteral("转换失败后关闭文件"));
        return false;
    } catch (...) {
        m_writeAccumA.resize(offset);
        m_writeAccumB.resize(offset);
        emit errorOccurred(QString("Card%1: 保存线程发生异常，已自动停止保存")
                           .arg(m_cardId + 1));
        m_saving.store(false, std::memory_order_release);
        if (!closeFiles()) handleWriteFault(QStringLiteral("转换失败后关闭文件"));
        return false;
    }

    ++m_accumTriggers;
    // 记账在周期性 flush 之前完成：flushWriteBuffers 的失败回退按 m_accumTriggers
    // 扣减，只有当本触发也已入账时扣减才与文件里的实际记录数完全对齐。
    ++m_currentFileTriggers;
    m_acceptedCount.fetch_add(1, std::memory_order_relaxed);
    if(group->saveTailCandidate)emit statusMessage(QStringLiteral("Card%1: 旧轮尾帧已接收 session=%2 round=%3 trigger=%4").arg(m_cardId+1).arg(identity.measurementSession).arg(identity.roundGeneration).arg(group->triggerSeq));

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
    if (!m_writeFaulted) m_saving.store(true,std::memory_order_release);
}
