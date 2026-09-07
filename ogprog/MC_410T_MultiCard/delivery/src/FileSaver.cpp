#include "FileSaver.h"
#include <QDir>
#include <QDateTime>
#include <cstring>
#include <cmath>
#include <algorithm>

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
        if (!wait(500)) terminate();
    }
    closeFiles();
}

// 
// 控制接口
// 
void FileSaver::startSaving(const QString& directory, int triggersPerFile,
                             const QString& suffix) {
    // 关闭之前可能仍打开的文件（stopSaving 未及时关闭时）
    flushWriteBuffers();
    closeFiles();
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    m_accumTriggers = 0;

    m_saveDirectory     = directory;
    m_triggersPerFile   = triggersPerFile;
    m_fileSuffix        = suffix;
    m_fileSequence      = 0;
    m_currentFileTriggers = 0;
    m_currentSourceIPv4 = 0;
    m_saving.store(true, std::memory_order_release);
    emit statusMessage(QString("Card%1: 开始保存到 %2").arg(m_cardId + 1).arg(directory));
}

void FileSaver::stopSaving() {
    m_saving.store(false, std::memory_order_release);
    // 停止时立即刷盘 + 关闭文件，确保未达触发数部分也能完整写入
    flushWriteBuffers();
    closeFiles();
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    m_accumTriggers = 0;
    m_currentFileTriggers = 0;
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

// 
// 文件名生成
// 
QString FileSaver::generateFileName(const QString& channel) const {
    QString n = m_fileSuffix.isEmpty()
        ? QString("Card%1_Ch%2_%3.dat")
              .arg(m_cardId + 1)
              .arg(channel)
              .arg(m_fileSequence, 3, 10, QChar('0'))
        : QString("Card%1_Ch%2_%3_%4.dat")
              .arg(m_cardId + 1)
              .arg(channel)
              .arg(m_fileSuffix)
              .arg(m_fileSequence, 3, 10, QChar('0'));
    return QDir(m_saveDirectory).filePath(n);
}

// 
// 文件操作
// 
void FileSaver::openNewFiles(uint32_t sourceIPv4) {
    closeFiles();
    m_currentSourceIPv4 = sourceIPv4;

    QDir().mkpath(m_saveDirectory);

    m_fileChannelA = new QFile(generateFileName("A"));
    m_fileChannelB = new QFile(generateFileName("B"));

    if (!m_fileChannelA->open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("Card%1: 无法打开文件 %2: %3")
            .arg(m_cardId + 1).arg(m_fileChannelA->fileName())
            .arg(m_fileChannelA->errorString()));
        delete m_fileChannelA; m_fileChannelA = nullptr;
    }
    if (!m_fileChannelB->open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("Card%1: 无法打开文件 %2: %3")
            .arg(m_cardId + 1).arg(m_fileChannelB->fileName())
            .arg(m_fileChannelB->errorString()));
        delete m_fileChannelB; m_fileChannelB = nullptr;
    }
    m_currentFileTriggers = 0;
}

void FileSaver::closeFiles() {
    flushWriteBuffers();    // 关题前将尚未写盘的积累数据刷入文件
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
}

// 
// 存储线程主循环
// 
void FileSaver::flushWriteBuffers()
{
    if (m_accumTriggers == 0) return;
    const qint64 bytesA = static_cast<qint64>(m_writeAccumA.size()) * sizeof(uint16_t);
    const qint64 bytesB = static_cast<qint64>(m_writeAccumB.size()) * sizeof(uint16_t);
    if (m_fileChannelA && m_fileChannelA->isOpen() && bytesA > 0)
        m_fileChannelA->write(reinterpret_cast<const char*>(m_writeAccumA.data()), bytesA);
    if (m_fileChannelB && m_fileChannelB->isOpen() && bytesB > 0)
        m_fileChannelB->write(reinterpret_cast<const char*>(m_writeAccumB.data()), bytesB);
    m_writeAccumA.clear();
    m_writeAccumB.clear();
    m_accumTriggers = 0;
}

// 
// 存储线程主循环
// 
void FileSaver::run() {
    m_running.store(true);

    while (m_running.load()) {
        TriggerGroupPtr group;
        if (!m_saveQueue.try_dequeue(group)) {
            QThread::msleep(1);  // 队列空，短暂休眠
            continue;
        }
        if (!m_saving.load()) {
            // 保存已停止 → 丢弃队列中的残余数据
            // 不做刷盘/关文件，因为 stopSaving() 已处理
            continue;
        }

        // 打开文件（首次 或 IP 来源变化时）
        if (!m_fileChannelA || !m_fileChannelB ||
            group->sourceIPv4 != m_currentSourceIPv4) {
            if (group->sourceIPv4 != m_currentSourceIPv4)
                m_fileSequence = 0;  // 新 IP 来源，文件序号重置
            openNewFiles(group->sourceIPv4);
        }

        // 文件序号翻滚
        if (m_currentFileTriggers >= m_triggersPerFile) {
            flushWriteBuffers();   // 翻滚前先刷盘
            ++m_fileSequence;
            closeFiles();
            openNewFiles(m_currentSourceIPv4);
        }

        if (!m_fileChannelA || !m_fileChannelB) {
            emit errorOccurred(QString("Card%1: 保存文件未成功打开，已自动停止保存")
                               .arg(m_cardId + 1));
            m_saving.store(false, std::memory_order_release);
            closeFiles();
            continue;
        }

        int n = group->sampleCount;
        if (n <= 0) continue;

        try {
            // float32  float16 批量转换并积累到内存缓冲区
            size_t offset = m_writeAccumA.size();
            m_writeAccumA.resize(offset + n);
            m_writeAccumB.resize(offset + n);
            convertBatch(group->freqA.data(), m_writeAccumA.data() + offset, n);
            convertBatch(group->freqB.data(), m_writeAccumB.data() + offset, n);
            ++m_accumTriggers;

            // 达到合并阈値时一次性写盘（WRITE_BUFFER_TRIGGERS 个触发合并为一条大 I/O）
            if (m_accumTriggers >= WRITE_BUFFER_TRIGGERS)
                flushWriteBuffers();

        } catch (const std::bad_alloc&) {
            emit errorOccurred(QString("Card%1: 保存线程内存不足，已自动停止保存")
                               .arg(m_cardId + 1));
            m_saving.store(false, std::memory_order_release);
            closeFiles();
            continue;
        } catch (...) {
            emit errorOccurred(QString("Card%1: 保存线程发生异常，已自动停止保存")
                               .arg(m_cardId + 1));
            m_saving.store(false, std::memory_order_release);
            closeFiles();
            continue;
        }

        ++m_currentFileTriggers;
        m_savedCount.fetch_add(1, std::memory_order_relaxed);
    }

    closeFiles();
}
