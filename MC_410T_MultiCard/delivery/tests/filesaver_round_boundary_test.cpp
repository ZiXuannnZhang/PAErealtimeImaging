// T1-T5, T9, T10 — Physical-round save boundary tests.
//
// Drives the real production metadata path
//   PhysicalRoundNormalizer -> FrameConverter -> TriggerGroup -> FileSaver
// through HostOutput, plus direct FileSaver fixtures for the backlog race and
// the save lifecycle. Asserts actual file contents (not just savedCount).
//
//   T1  manual save: partial timeout round forces a new file (core repro)
//   T2  manual save: two complete count rounds stay in separate files
//   T3  triggersPerFile < N and > N, including timeout partial sealing
//   T4  saver backlog: old-round groups queued behind new-round groups still
//       land in separate files (data-plane ordering proof, threaded + FIFO)
//   T5  auto save: session dir routing; roundGeneration as independent file
//       boundary within one auto session
//   T9  count-complete round followed by idle: no extra empty rollover file
//   T10 saving lifecycle: restart/suspend/resume never inherit old round state
//   T11 落盘不变量主契约：同 sessionGen 晚到帧不得截断已封存文件（bug2 回归）
//   T12 sourceIPv4 变化不得归零序号、不得抵消物理轮次的安全推进
//   T13 sessionGen 变化只有在目录真的变化时才重起编号
//   W1-W6 写盘失败必须停保存 + 告警 + A/B 双侧按记录边界回退（FILESAVER_TEST_SEAM）

#include "PaimageAcquisition/HostOutput.h"
#include "FileSaver.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace paimage;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

void require(bool condition, const char* what)
{
    if (!condition)
        throw std::runtime_error(what);
}

template <class F> bool until(F f)
{
    QElapsedTimer t; t.start();
    while (!f() && t.elapsed() < 5000)
        QThread::msleep(1);
    return f();
}

// Exact IEEE-754 binary16 for the small integer fixture values.
std::uint16_t halfOf(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 31) & 0x1;
    std::uint32_t exponent = (bits >> 23) & 0xFF;
    std::uint32_t mantissa = bits & 0x7FFFFF;
    if (exponent == 0xFF)
        return static_cast<std::uint16_t>((sign << 15) | 0x7C00 | (mantissa ? 0x0200 : 0));
    int exp16 = static_cast<int>(exponent) - 127 + 15;
    if (exp16 >= 31)
        return static_cast<std::uint16_t>((sign << 15) | 0x7C00);
    if (exp16 <= 0)
        return static_cast<std::uint16_t>(sign << 15);   // fixtures keep exact ints
    std::uint32_t m10 = mantissa >> 13;
    const std::uint32_t m13 = mantissa & 0x1FFF;
    if (m13 > 0x1000 || (m13 == 0x1000 && (m10 & 1))) {
        ++m10;
        if (m10 >= 0x400) { ++exp16; m10 = 0; }
    }
    return static_cast<std::uint16_t>((sign << 15) |
                                      (static_cast<std::uint32_t>(exp16) << 10) | m10);
}

std::vector<std::uint16_t> halvesOf(std::initializer_list<int> values)
{
    std::vector<std::uint16_t> out;
    for (int v : values)
        out.push_back(halfOf(static_cast<float>(v)));
    return out;
}

// 连号区间版本：W 系列要断言几十条记录的逐字节内容。
std::vector<std::uint16_t> halvesOfRange(int first, int count)
{
    std::vector<std::uint16_t> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        out.push_back(halfOf(static_cast<float>(first + i)));
    return out;
}

// ChB 存的是 -value（见 makeGroup），对应区间取负。
std::vector<std::uint16_t> negHalvesOfRange(int first, int count)
{
    std::vector<std::uint16_t> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        out.push_back(halfOf(static_cast<float>(-(first + i))));
    return out;
}

qint64 fileSizeOf(const QString& path)
{
    QFile f(path);
    return f.exists() ? f.size() : -1;
}

// Verify a channel file: triggersPerFile-sealed sequence of per-trigger
// constant values (fixture writes every sample of a trigger equal to it).
bool verifyFile(const QString& path, const std::vector<std::uint16_t>& expected)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = f.readAll();
    constexpr int kSamp = 16;
    if (bytes.size() != static_cast<int>(expected.size()) * kSamp * 2)
        return false;
    for (std::size_t t = 0; t < expected.size(); ++t) {
        for (int s = 0; s < kSamp; ++s) {
            std::uint16_t v;
            std::memcpy(&v, bytes.constData() + (t * kSamp + s) * 2, 2);
            if (v != expected[t])
                return false;
        }
    }
    return true;
}

constexpr int kSamples = 16;
constexpr std::int64_t kMs = 1000000;   // 1 ms in ns
constexpr double kTimeoutSec = 0.1;     // 100 ms idle timeout

struct RoundFixture {
    QCoreApplication& app;
    std::uint64_t logicalPerRound;
    std::unique_ptr<QTemporaryDir> currentRoot;
    QString lastDir;
    std::vector<PhysicalRoundEvent> events;
    std::mutex eventMutex;
    std::vector<FileRolloverInfo> rollovers;
    std::mutex rolloverMutex;

    explicit RoundFixture(QCoreApplication& a, std::uint64_t n)
        : app(a), logicalPerRound(n)
    {
    }

    ~RoundFixture() = default;   // QTemporaryDir auto-removes

    Frame makeFrame(std::uint16_t trigger, std::int64_t firstNs)
    {
        auto f = std::make_shared<CardFrame>();
        f->card = 0;
        f->trigger = trigger;
        f->measurementSession = 1;
        f->first = firstNs;
        f->complete = true;
        f->reason = Decision::Complete;
        f->bytes.resize(kSamples * 8);
        for (int i = 0; i < kSamples; ++i) {
            const std::int32_t b = -static_cast<std::int32_t>(trigger);
            const std::int32_t a = static_cast<std::int32_t>(trigger);
            std::memcpy(f->bytes.data() + i * 8, &b, 4);
            std::memcpy(f->bytes.data() + i * 8 + 4, &a, 4);
        }
        return f;
    }

    template <class F>
    void run(const QString& dirTemplate, int perFile, const QString& suffix,
             std::uint64_t sessionGen, int expectedSaves, F feed)
    {
        currentRoot = std::make_unique<QTemporaryDir>(dirTemplate);
        require(currentRoot->isValid(), "temp dir");
        lastDir = currentRoot->path();
        QString* rootPath = &lastDir;
        DisplayBuffer display;
        FileSaver saver(0);
        saver.setSessionDirResolver([rootPath](std::uint64_t g) {
            return g == 0 ? QString()
                          : QDir(*rootPath).filePath(QStringLiteral("gen%1").arg(g));
        });
        AcqConfig config;
        config.acqTimeNs = kSamples * 4;
        std::uint64_t genValue = sessionGen;
        DataProcessor processor(0, nullptr, nullptr, config, {});
        processor.setSessionGenReader([&] { return genValue; });
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr,
                          logicalPerRound,
                          [this](const PhysicalRoundEvent& e) {
                              std::lock_guard<std::mutex> lock(eventMutex);
                              events.push_back(e);
                          },
                          kTimeoutSec);
        // DirectConnection: the saver emits from the card worker thread; the
        // recorder lambda is thread-safe (mutex), and tests assert without
        // relying on the event loop.
        QObject::connect(&saver, &FileSaver::fileRolled, &app,
                         [this](int, const QString& reason,
                                quint64 oldGen, quint64 newGen,
                                int oldSeq, int newSeq,
                                int oldTriggers, bool manual) {
                             std::lock_guard<std::mutex> lock(rolloverMutex);
                             FileRolloverInfo info;
                             info.happened = true;
                             info.reason = reason;
                             info.oldRoundGeneration = oldGen;
                             info.newRoundGeneration = newGen;
                             info.oldFileSequence = oldSeq;
                             info.newFileSequence = newSeq;
                             info.oldFileTriggerCount = oldTriggers;
                             info.manualMode = manual;
                             rollovers.push_back(info);
                         },
                         Qt::DirectConnection);
        output.beginSession(1);
        output.start();
        const auto applied = output.startSaving(currentRoot->path(), perFile, suffix);
        require(until([&] { return output.savingApplied(applied); }), "saving applied");
        feed([this](std::uint16_t trigger, std::int64_t firstNs) {
                 return makeFrame(trigger, firstNs);
             },
             genValue, output);
        // Wait until the card worker has consumed everything AND the saver
        // has written it all — stopping first would drop in-flight frames.
        require(until([&] {
            return output.cardDepth(0) == 0 &&
                   saver.savedCount() == static_cast<std::uint64_t>(expectedSaves);
        }), "all fed frames saved");
        const auto stopped = output.stopSaving();
        require(until([&] { return output.savingApplied(stopped); }), "saving stopped");
        output.stop();
    }

    std::vector<PhysicalRoundEvent> snapshotEvents()
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        return events;
    }
    std::vector<FileRolloverInfo> snapshotRollovers()
    {
        std::lock_guard<std::mutex> lock(rolloverMutex);
        return rollovers;
    }
};

TriggerGroupPtr makeGroup(std::uint16_t trig, std::uint64_t roundGen, int value,
                          std::uint64_t sessionGen = 0,
                          std::uint32_t sourceIPv4 = 0x0100007f)
{
    auto g = std::make_shared<TriggerGroup>();
    g->cardId = 0;
    g->triggerSeq = trig;
    g->sampleCount = kSamples;
    g->isComplete = true;
    g->sourceIPv4 = sourceIPv4;
    g->sessionGen = sessionGen;
    g->normalizationApplied = true;
    g->physicalDecision = PhysicalTriggerDecision::LogicalScan;
    g->roundGeneration = roundGen;
    g->logicalTriggerIndex = 0;
    g->freqA.assign(kSamples, static_cast<float>(value));
    g->freqB.assign(kSamples, static_cast<float>(-value));
    return g;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── T1: manual save, partial timeout round forces a new file ────────
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t1-XXXXXX", 100, "t1", 0, 8,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   out.card(make(101, 1 * kMs));
                   out.card(make(102, 2 * kMs));
                   out.card(make(103, 3 * kMs));
                   // idle 0.5 s > 0.1 s timeout -> TimeoutBoundary, gen 0 -> 1
                   const std::int64_t base = 3 * kMs + 500 * kMs;
                   out.card(make(200, base + 1 * kMs));
                   for (int i = 1; i <= 5; ++i)
                       out.card(make(200 + i, base + (1 + i) * kMs));
               });
        const auto ev = fx.snapshotEvents();
        bool sawTimeout = false, sawCount = false;
        for (const auto& e : ev) {
            if (e.kind == PhysicalRoundEvent::Kind::TimeoutBoundary) sawTimeout = true;
            if (e.kind == PhysicalRoundEvent::Kind::CountBoundary) sawCount = true;
        }
        check(sawTimeout, "T1 TimeoutBoundary observed");
        check(sawCount, "T1 CountBoundary for the complete second round");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t1_000.dat", halvesOf({101, 102, 103})),
              "T1 old partial file holds A/B/C of round 0");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t1_001.dat",
                         halvesOf({201, 202, 203, 204, 205})),
              "T1 new file holds D..H of round 1");
        check(verifyFile(fx.lastDir + "/Card1_ChB_t1_000.dat", halvesOf({-101, -102, -103})),
              "T1 channel B old file identity");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 1 && ro[0].reason == "physical_round" &&
              ro[0].oldRoundGeneration == 0 && ro[0].newRoundGeneration == 1 &&
              ro[0].oldFileTriggerCount == 3 && ro[0].manualMode,
              "T1 exactly one physical-round rollover, old file sealed at 3");
        check(!QFile::exists(fx.lastDir + "/Card1_ChA_t1_002.dat"),
              "T1 no third file");
    } catch (const std::exception& e) {
        check(false, (QString("T1 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T2: two complete count rounds stay in separate files ──────────
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t2-XXXXXX", 100, "t2", 0, 10,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 5; ++i) out.card(make(100 + i, i * kMs));
                   out.card(make(200, 6 * kMs));
                   for (int i = 1; i <= 5; ++i) out.card(make(200 + i, (6 + i) * kMs));
               });
        check(verifyFile(fx.lastDir + "/Card1_ChA_t2_000.dat",
                         halvesOf({101, 102, 103, 104, 105})),
              "T2 round 0 file holds 5 triggers");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t2_001.dat",
                         halvesOf({201, 202, 203, 204, 205})),
              "T2 round 1 file holds 5 triggers");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 1 && ro[0].reason == "physical_round" &&
              ro[0].oldFileTriggerCount == 5,
              "T2 exactly one round rollover at the count boundary");
    } catch (const std::exception& e) {
        check(false, (QString("T2 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T3a1: triggersPerFile(2) < N(5): one complete round produces
    //         multiple capacity files, none crossing the round ──────────
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t3a1-XXXXXX", 2, "t3a1", 0, 5,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 5; ++i) out.card(make(100 + i, i * kMs));
               });
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a1_000.dat", halvesOf({101, 102})),
              "T3a1 capacity file 000 holds 2");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a1_001.dat", halvesOf({103, 104})),
              "T3a1 capacity file 001 holds 2");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a1_002.dat", halvesOf({105})),
              "T3a1 capacity file 002 holds the round tail");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 2 && ro[0].reason == "capacity" &&
              ro[1].reason == "capacity",
              "T3a1 two capacity rollovers only");
    } catch (const std::exception& e) {
        check(false, (QString("T3a1 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T3a2: perFile(2) < N(5), timeout partial round: every file holds
    //         exactly one generation; the partial round is force-sealed ─
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t3a2-XXXXXX", 2, "t3a2", 0, 7,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 3; ++i) out.card(make(100 + i, i * kMs));
                   const std::int64_t base = 3 * kMs + 500 * kMs;
                   out.card(make(200, base + 1 * kMs));
                   for (int i = 1; i <= 4; ++i)
                       out.card(make(200 + i, base + (1 + i) * kMs));
               });
        // gen0 partial (3) -> capacity split {101,102} + sealed tail {103}
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a2_000.dat", halvesOf({101, 102})),
              "T3a2 gen0 capacity file 000 holds 2");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a2_001.dat", halvesOf({103})),
              "T3a2 timeout partial round force-sealed at 1 (not filled to 5)");
        // gen1 (4) -> capacity split {201,202} {203,204}
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a2_002.dat", halvesOf({201, 202})),
              "T3a2 gen1 file 002 holds 2");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3a2_003.dat", halvesOf({203, 204})),
              "T3a2 gen1 file 003 holds 2");
        check(!QFile::exists(fx.lastDir + "/Card1_ChA_t3a2_004.dat"),
              "T3a2 no fifth file");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 3 && ro[0].reason == "capacity" &&
              ro[1].reason == "physical_round" && ro[2].reason == "capacity",
              "T3a2 rollovers ordered capacity,physical_round,capacity");
        check(ro[1].oldRoundGeneration == 0 && ro[1].newRoundGeneration == 1 &&
              ro[1].oldFileTriggerCount == 1,
              "T3a2 round rollover sealed the partial file (oldTrig=1)");
    } catch (const std::exception& e) {
        check(false, (QString("T3a2 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T3b: triggersPerFile(8) > N(5): round boundary still splits ────
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t3b-XXXXXX", 8, "t3b", 0, 8,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 5; ++i) out.card(make(100 + i, i * kMs));
                   const std::int64_t base = 5 * kMs + 500 * kMs;
                   out.card(make(200, base + 1 * kMs));
                   for (int i = 1; i <= 3; ++i) out.card(make(200 + i, base + (1 + i) * kMs));
               });
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3b_000.dat",
                         halvesOf({101, 102, 103, 104, 105})),
              "T3b full round fits one file under the capacity cap");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t3b_001.dat", halvesOf({201, 202, 203})),
              "T3b partial round forced into a new file despite capacity headroom");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 1 && ro[0].reason == "physical_round",
              "T3b only the round boundary rolls the file");
    } catch (const std::exception& e) {
        check(false, (QString("T3b exception: ") + e.what()).toUtf8().constData());
    }

    // ── T4: saver backlog — queued old-round groups never cross into the
    //        new round's file regardless of observer timing ─────────────
    try {
        // T4-threaded: real FileSaver run() queue; the boundary lands while
        // the old round is still entirely inside the queue.
        QTemporaryDir dirT(QDir::currentPath() + "/round-t4t-XXXXXX");
        require(dirT.isValid(), "T4t temp dir");
        {
            FileSaver saver(0);
            saver.startSaving(dirT.path(), 100, "t4t");
            saver.start();   // run() consumer thread
            for (int i = 1; i <= 5; ++i)
                saver.saveTriggerGroup(makeGroup(100 + i, 0, 100 + i));
            // boundary "happens" here; the queue still holds the old round
            for (int i = 1; i <= 5; ++i)
                saver.saveTriggerGroup(makeGroup(200 + i, 1, 200 + i));
            require(until([&] {
                return saver.savedCount() == 10 && saver.queueDepth() == 0;
            }), "T4t queue drained");
            saver.requestStop();
            saver.wait();
            saver.stopSaving();
            check(saver.physicalRoundRolloverCount() == 1,
                  "T4t one round rollover despite backlog");
            check(verifyFile(dirT.path() + "/Card1_ChA_t4t_000.dat",
                             halvesOf({101, 102, 103, 104, 105})),
                  "T4t old file holds only old round");
            check(verifyFile(dirT.path() + "/Card1_ChA_t4t_001.dat",
                             halvesOf({201, 202, 203, 204, 205})),
                  "T4t new file holds only new round");
        }

        // T4-fifo: deterministic drain of the same interleaving.
        QTemporaryDir dirF(QDir::currentPath() + "/round-t4f-XXXXXX");
        require(dirF.isValid(), "T4f temp dir");
        {
            FileSaver saver(0);
            saver.startSaving(dirF.path(), 100, "t4f");
            for (int i = 1; i <= 5; ++i)
                saver.saveTriggerGroup(makeGroup(100 + i, 0, 100 + i));
            for (int i = 1; i <= 5; ++i)
                saver.saveTriggerGroup(makeGroup(200 + i, 1, 200 + i));
            TriggerGroupPtr g;
            while (saver.saveQueue()->try_dequeue(g))
                saver.consumeTriggerGroup(g);
            saver.stopSaving();
            check(saver.physicalRoundRolloverCount() == 1,
                  "T4f one round rollover in FIFO drain");
            check(verifyFile(dirF.path() + "/Card1_ChA_t4f_000.dat",
                             halvesOf({101, 102, 103, 104, 105})),
                  "T4f old file holds only old round");
            check(verifyFile(dirF.path() + "/Card1_ChA_t4f_001.dat",
                             halvesOf({201, 202, 203, 204, 205})),
                  "T4f new file holds only new round");
        }
    } catch (const std::exception& e) {
        check(false, (QString("T4 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T5: auto save — session dir routing + independent round boundary ─
    try {
        // T5a: session advance at the timeout boundary (production scheme:
        // the UI advances the auto session exactly once per round).
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t5a-XXXXXX", 100, "t5a", 1, 8,
               [&](auto make, std::uint64_t& gen, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 3; ++i) out.card(make(100 + i, i * kMs));
                   const std::int64_t base = 3 * kMs + 500 * kMs;
                   gen = 2;   // coordinator generation advance at the boundary
                   out.card(make(200, base + 1 * kMs));
                   for (int i = 1; i <= 5; ++i)
                       out.card(make(200 + i, base + (1 + i) * kMs));
               });
        check(verifyFile(fx.lastDir + "/gen1/Card1_ChA_t5a_000.dat",
                         halvesOf({101, 102, 103})),
              "T5a old session dir file sealed at 3");
        check(verifyFile(fx.lastDir + "/gen2/Card1_ChA_t5a_000.dat",
                         halvesOf({201, 202, 203, 204, 205})),
              "T5a new session dir file holds the new round");
        check(!QFile::exists(fx.lastDir + "/gen1/Card1_ChA_t5a_001.dat"),
              "T5a no cross-directory write");

        // T5b: one fixed auto session; rounds split by roundGeneration alone.
        RoundFixture fx2(app, 5);
        fx2.run(QDir::currentPath() + "/round-t5b-XXXXXX", 100, "t5b", 1, 13,
                [&](auto make, std::uint64_t&, HostOutput& out) {
                    out.card(make(100, 0));
                    for (int i = 1; i <= 5; ++i) out.card(make(100 + i, i * kMs));
                    out.card(make(200, 6 * kMs));   // count boundary inside session
                    for (int i = 1; i <= 5; ++i) out.card(make(200 + i, (6 + i) * kMs));
                    const std::int64_t base = 11 * kMs + 500 * kMs;
                    out.card(make(300, base + 1 * kMs));
                    for (int i = 1; i <= 3; ++i)
                        out.card(make(300 + i, base + (1 + i) * kMs));
                });
        check(verifyFile(fx2.lastDir + "/gen1/Card1_ChA_t5b_000.dat",
                         halvesOf({101, 102, 103, 104, 105})),
              "T5b session dir file 000 round 0");
        check(verifyFile(fx2.lastDir + "/gen1/Card1_ChA_t5b_001.dat",
                         halvesOf({201, 202, 203, 204, 205})),
              "T5b session dir file 001 round 1 (count boundary)");
        check(verifyFile(fx2.lastDir + "/gen1/Card1_ChA_t5b_002.dat",
                         halvesOf({301, 302, 303})),
              "T5b session dir file 002 round 2 (timeout partial)");
        const auto ro = fx2.snapshotRollovers();
        check(ro.size() == 2 && ro[0].reason == "physical_round" &&
              ro[1].reason == "physical_round" && !ro[0].manualMode && !ro[1].manualMode,
              "T5b two auto-mode round rollovers, no capacity rollover");
    } catch (const std::exception& e) {
        check(false, (QString("T5 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T9: count-complete round followed by idle — no empty rollover ───
    try {
        RoundFixture fx(app, 5);
        fx.run(QDir::currentPath() + "/round-t9-XXXXXX", 100, "t9", 0, 10,
               [&](auto make, std::uint64_t&, HostOutput& out) {
                   out.card(make(100, 0));
                   for (int i = 1; i <= 5; ++i) out.card(make(100 + i, i * kMs));
                   // complete round, then idle beyond the timeout: the
                   // normalizer must NOT emit a second boundary for the
                   // already-idle round (accepted semantics, no rewrite).
                   const std::int64_t base = 5 * kMs + 500 * kMs;
                   out.card(make(200, base + 1 * kMs));
                   for (int i = 1; i <= 5; ++i)
                       out.card(make(200 + i, base + (1 + i) * kMs));
               });
        check(verifyFile(fx.lastDir + "/Card1_ChA_t9_000.dat",
                         halvesOf({101, 102, 103, 104, 105})),
              "T9 round 0 file intact");
        check(verifyFile(fx.lastDir + "/Card1_ChA_t9_001.dat",
                         halvesOf({201, 202, 203, 204, 205})),
              "T9 round 1 file intact");
        check(!QFile::exists(fx.lastDir + "/Card1_ChA_t9_002.dat"),
              "T9 no extra empty rollover file");
        const auto ev = fx.snapshotEvents();
        int timeouts = 0;
        for (const auto& e : ev)
            if (e.kind == PhysicalRoundEvent::Kind::TimeoutBoundary) ++timeouts;
        check(timeouts == 0, "T9 no TimeoutBoundary after a count-complete round");
        const auto ro = fx.snapshotRollovers();
        check(ro.size() == 1 && ro[0].reason == "physical_round",
              "T9 exactly one round rollover total");
    } catch (const std::exception& e) {
        check(false, (QString("T9 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T10: saving lifecycle never inherits old round file state ───────
    try {
        QTemporaryDir dir(QDir::currentPath() + "/round-t10-XXXXXX");
        require(dir.isValid(), "T10 temp dir");
        FileSaver saver(0);
        // Measurement 1: round generation 0.
        saver.startSaving(dir.path(), 100, "t10");
        saver.consumeTriggerGroup(makeGroup(11, 0, 11));
        saver.consumeTriggerGroup(makeGroup(12, 0, 12));
        saver.stopSaving();
        // Measurement 2 restarts at round generation 0: the saver must adopt
        // it as its first round, not roll over against measurement 1's state.
        saver.startSaving(dir.path(), 100, "t10");
        check(saver.physicalRoundRolloverCount() == 0,
              "T10 no rollover inherited across startSaving");
        saver.consumeTriggerGroup(makeGroup(21, 0, 21));
        saver.consumeTriggerGroup(makeGroup(22, 0, 22));
        saver.stopSaving();
        check(saver.physicalRoundRolloverCount() == 0,
              "T10 restart with same generation never rolls over");
        // 同一目录复用时落盘不变量生效：新测量落到下一个空闲序号，旧文件一个
        // 字节都不动（历史实现在此截断重写 _000，等于抹掉测量 1 的数据）。
        check(verifyFile(dir.path() + "/Card1_ChA_t10_000.dat", halvesOf({11, 12})),
              "T10 first measurement file is never rewritten by the second");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_001.dat", halvesOf({21, 22})),
              "T10 second measurement lands on the next free sequence");
        // Source restart path: suspend/resume resets round state too.
        saver.startSaving(dir.path(), 100, "t10");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_000.dat", halvesOf({11, 12})),
              "T10 third measurement leaves file 000 intact");
        saver.consumeTriggerGroup(makeGroup(31, 0, 31));
        saver.suspendForSourceRestart();
        saver.resumeAfterSourceRestart();
        saver.consumeTriggerGroup(makeGroup(41, 1, 41));
        saver.consumeTriggerGroup(makeGroup(42, 2, 42));
        saver.stopSaving();
        check(saver.physicalRoundRolloverCount() == 1,
              "T10 exactly one rollover after resume");
        const auto& info = saver.lastFileRollover();
        check(info.oldRoundGeneration == 1 && info.newRoundGeneration == 2 &&
              info.oldFileTriggerCount == 1 && info.manualMode,
              "T10 rollover metadata captured (old=1,new=2,1 trigger sealed)");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_002.dat", halvesOf({31})),
              "T10 pre-suspend file holds the third measurement's first round");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_003.dat", halvesOf({41})),
              "T10 post-resume file holds the adopted round");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_004.dat", halvesOf({42})),
              "T10 final rollover file holds the new round");
        check(!QFile::exists(dir.path() + "/Card1_ChA_t10_005.dat"),
              "T10 no extra empty file after the final rollover");
    } catch (const std::exception& e) {
        check(false, (QString("T10 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T11: 落盘不变量主契约 —— 同代残留帧不得截断已封存文件 ──────────
    // 自动保存边界提交成功后 NetworkController 会 requestCloseSavers() →
    // FileSaver::serviceCloseRequest() 刷盘关文件，但并不推进 m_fileSequence。
    // 十几分钟后任何仍带同一 sessionGen 的帧到达，就会走"句柄为空 → 重开"
    // 这条路：历史实现用 QIODevice::WriteOnly 重开同名文件 = 打开即截断，
    // 把该轮已落盘的数据换成那一小块晚到数据。这就是现场"002 里最后写的 .dat
    // 被十几分钟后写入的未知来源数据截断重写"的机制。
    try {
        QTemporaryDir dir(QDir::currentPath() + "/round-t11-XXXXXX");
        require(dir.isValid(), "T11 temp dir");
        FileSaver saver(0);
        const QString base = dir.path();
        saver.setSessionDirResolver([&base](std::uint64_t) { return base; });
        saver.startSaving(dir.path(), 100, "t11");
        saver.consumeTriggerGroup(makeGroup(1, 0, 101, 7));
        saver.consumeTriggerGroup(makeGroup(2, 0, 102, 7));
        saver.requestClose();
        saver.serviceCloseRequest();               // 边界后的落盘关闭
        check(verifyFile(dir.path() + "/Card1_ChA_t11_000.dat", halvesOf({101, 102})),
              "T11 sealed file holds the round before the late frame");
        saver.consumeTriggerGroup(makeGroup(3, 0, 201, 7));   // 同 sessionGen 晚到帧
        saver.stopSaving();
        check(verifyFile(dir.path() + "/Card1_ChA_t11_000.dat", halvesOf({101, 102})),
              "T11 committed bytes survive a late same-generation frame");
        check(verifyFile(dir.path() + "/Card1_ChB_t11_000.dat", halvesOf({-101, -102})),
              "T11 the B channel of the sealed file is intact too");
        check(verifyFile(dir.path() + "/Card1_ChA_t11_001.dat", halvesOf({201})),
              "T11 the late frame lands on a fresh sequence");
    } catch (const std::exception& e) {
        check(false, (QString("T11 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T12: sourceIPv4 变化不得归零序号、不得抵消物理轮次的安全推进 ────
    try {
        QTemporaryDir dir(QDir::currentPath() + "/round-t12-XXXXXX");
        require(dir.isValid(), "T12 temp dir");
        FileSaver saver(0);
        saver.startSaving(dir.path(), 2, "t12");
        saver.consumeTriggerGroup(makeGroup(1, 0, 101, 0, 0x0100007f));
        saver.consumeTriggerGroup(makeGroup(2, 0, 102, 0, 0x0100007f));
        // 换物理轮次 +同时换来源 IP：物理轮次轮转会 ++m_fileSequence 作"安全
        // 推进"，历史实现紧接着的 IP 归零会把它抵消并截断上一轮的 _000。
        saver.consumeTriggerGroup(makeGroup(3, 1, 201, 0, 0x0200007f));
        saver.stopSaving();
        check(verifyFile(dir.path() + "/Card1_ChA_t12_000.dat", halvesOf({101, 102})),
              "T12 previous round file survives a source change");
        check(verifyFile(dir.path() + "/Card1_ChA_t12_001.dat", halvesOf({201})),
              "T12 the physical-round advance is not cancelled by the source change");
    } catch (const std::exception& e) {
        check(false, (QString("T12 exception: ") + e.what()).toUtf8().constData());
    }

    // ── T13: sessionGen 变化只有在目录真的变化时才重起编号 ──────────────
    try {
        QTemporaryDir dir(QDir::currentPath() + "/round-t13-XXXXXX");
        require(dir.isValid(), "T13 temp dir");
        FileSaver saver(0);
        const QString shared = QDir(dir.path()).filePath(QStringLiteral("shared"));
        const QString fresh  = QDir(dir.path()).filePath(QStringLiteral("fresh"));
        QString active = shared;
        saver.setSessionDirResolver([&active](std::uint64_t) { return active; });
        saver.startSaving(shared, 100, "t13");
        saver.consumeTriggerGroup(makeGroup(1, 0, 101, 1));
        // gen 1→2 但解析到同一目录：序号不得归零，否则会回头去顶 _000。
        saver.consumeTriggerGroup(makeGroup(2, 0, 201, 2));
        // gen 2→3 且目录变化：新目录从空闲序号起。
        active = fresh;
        saver.consumeTriggerGroup(makeGroup(3, 0, 301, 3));
        saver.stopSaving();
        check(verifyFile(shared + "/Card1_ChA_t13_000.dat", halvesOf({101})),
              "T13 same-directory generation change keeps 000 intact");
        check(verifyFile(shared + "/Card1_ChA_t13_001.dat", halvesOf({201})),
              "T13 same-directory generation change continues the sequence");
        check(verifyFile(shared + "/Card1_ChB_t13_000.dat", halvesOf({-101})),
              "T13 same-directory generation change keeps B 000 intact");
        check(verifyFile(fresh + "/Card1_ChA_t13_000.dat", halvesOf({301})),
              "T13 a new directory starts at the first free sequence");
    } catch (const std::exception& e) {
        check(false, (QString("T13 exception: ") + e.what()).toUtf8().constData());
    }

    // ── W1..W6: 写盘失败必须「停保存 + 告警 + A/B 双侧按记录边界回退」────────
    // .dat 是定长记录、无文件头：一次 IO 失败/短写若留下半条记录，该文件后续
    // 所有记录的边界都会永久错位且无法离线修复。因此失败必须整段回退，且 A/B
    // 成对回退（一侧失败也撤回另一侧已写的字节），否则两通道记录数会失配。
    try {
        QTemporaryDir dir(QDir::currentPath() + "/round-w-XXXXXX");
        require(dir.isValid(), "W temp dir");
        constexpr qint64 kRec = kSamples * 2;   // 一条触发记录 = 16 点 × float16
        auto path = [&dir](const char* suffix, const char* ch) {
            return dir.path() + QString("/Card1_Ch%1_%2_000.dat").arg(ch).arg(suffix);
        };

        // 断言分两层：故障当场只断言内部记账（QIODevice::write 有内部缓冲，
        // 未落盘时外部 stat 看不到）；逐字节内容断言统一放到 stopSaving() 关文件
        // 之后——那时读到的就是最终落盘结果。
        // W5 无故障基线：内容必须与 golden 逐字节一致（防回退逻辑误伤正常写路径）
        {
            FileSaver saver(0);
            saver.setWriteFaultForTest(0);
            saver.startSaving(dir.path(), 100, "w5");
            for (int i = 0; i < 32; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 500 + i));
            saver.stopSaving();
            check(verifyFile(path("w5", "A"), halvesOfRange(500, 32)),
                  "W5 fault-free path stays byte-identical (A)");
            check(verifyFile(path("w5", "B"), negHalvesOfRange(500, 32)),
                  "W5 fault-free path stays byte-identical (B)");
            check(saver.savedCount() == 32 && fileSizeOf(path("w5", "A")) == 32 * kRec,
                  "W5 counters agree with the records actually on disk");
        }

        // W1 两通道都失败：停保存 + 恰好一次告警 + 双侧回退 + 计数同步回退；
        // W4 同一场景下失败后继续提交不得再写入、不得产生半条记录。
        {
            int errors = 0;
            FileSaver saver(0);
            QObject::connect(&saver, &FileSaver::errorOccurred, &app,
                             [&errors](const QString&) { ++errors; },
                             Qt::DirectConnection);
            saver.startSaving(dir.path(), 100, "w1");
            for (int i = 0; i < 16; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 100 + i));
            check(saver.savedCount() == 16, "W1 first segment accepted");
            saver.setWriteFaultForTest(1);
            for (int i = 16; i < 32; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 100 + i));
            check(saver.savedCount() == 16, "W1 counters roll back with the bytes");
            check(!saver.isSaving(), "W1 saving stops on a write fault");
            check(errors == 1, "W1 exactly one errorOccurred for a sustained fault");

            // W4 失败后继续提交：不再写入、不再告警、计数不动
            saver.setWriteFaultForTest(0);
            for (int i = 32; i < 48; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 100 + i));
            check(saver.savedCount() == 16 && errors == 1, "W4 counters and alert stay put");
            saver.stopSaving();
            check(fileSizeOf(path("w1", "A")) == 16 * kRec &&
                      fileSizeOf(path("w1", "B")) == 16 * kRec,
                  "W1/W4 both channels hold exactly the surviving segment");
            check(fileSizeOf(path("w1", "A")) % kRec == 0, "W4 no partial record left behind");
            check(verifyFile(path("w1", "A"), halvesOfRange(100, 16)),
                  "W1 surviving bytes are the original records, not a torn tail");
            check(verifyFile(path("w1", "B"), negHalvesOfRange(100, 16)),
                  "W1 the B channel rolled back to the same record count");
        }

        // W2 仅 A 失败 ⇒ A/B 成对回退，两侧记录数仍相等
        {
            int errors = 0;
            FileSaver saver(0);
            QObject::connect(&saver, &FileSaver::errorOccurred, &app,
                             [&errors](const QString&) { ++errors; },
                             Qt::DirectConnection);
            saver.startSaving(dir.path(), 100, "w2");
            for (int i = 0; i < 16; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 200 + i));
            saver.setWriteFaultForTest(2);   // 仅 A 失败
            for (int i = 16; i < 32; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 200 + i));
            check(saver.savedCount() == 16 && !saver.isSaving() && errors == 1,
                  "W2 stop + single alert + counter rollback");
            saver.stopSaving();
            check(fileSizeOf(path("w2", "A")) == 16 * kRec &&
                      fileSizeOf(path("w2", "B")) == 16 * kRec,
                  "W2 an A-only failure rolls BOTH channels back (pair stays equal)");
            check(verifyFile(path("w2", "A"), halvesOfRange(200, 16)) &&
                      verifyFile(path("w2", "B"), negHalvesOfRange(200, 16)),
                  "W2 both channels keep exactly the first segment's records");
        }

        // W3 仅 B 失败 ⇒ W2 对称
        {
            int errors = 0;
            FileSaver saver(0);
            QObject::connect(&saver, &FileSaver::errorOccurred, &app,
                             [&errors](const QString&) { ++errors; },
                             Qt::DirectConnection);
            saver.startSaving(dir.path(), 100, "w3");
            for (int i = 0; i < 16; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 300 + i));
            saver.setWriteFaultForTest(3);   // 仅 B 失败
            for (int i = 16; i < 32; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 300 + i));
            check(saver.savedCount() == 16 && !saver.isSaving() && errors == 1,
                  "W3 stop + single alert + counter rollback");
            saver.stopSaving();
            check(fileSizeOf(path("w3", "A")) == 16 * kRec &&
                      fileSizeOf(path("w3", "B")) == 16 * kRec,
                  "W3 a B-only failure rolls BOTH channels back (pair stays equal)");
            check(verifyFile(path("w3", "A"), halvesOfRange(300, 16)) &&
                      verifyFile(path("w3", "B"), negHalvesOfRange(300, 16)),
                  "W3 both channels keep exactly the first segment's records");
        }

        // W6 前 2 段成功、第 3 段失败 ⇒ 历史 32 条逐字节完整，只丢第 3 段
        {
            FileSaver saver(0);
            saver.startSaving(dir.path(), 100, "w6");
            for (int i = 0; i < 32; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 600 + i));
            check(saver.savedCount() == 32, "W6 first two segments accepted");
            saver.setWriteFaultForTest(1);
            for (int i = 32; i < 48; ++i)
                saver.consumeTriggerGroup(makeGroup(1, 0, 600 + i));
            check(saver.savedCount() == 32, "W6 counters drop exactly the failing segment");
            saver.stopSaving();
            check(fileSizeOf(path("w6", "A")) == 32 * kRec &&
                      fileSizeOf(path("w6", "B")) == 32 * kRec,
                  "W6 only the failing segment is dropped; earlier segments stay intact");
            check(verifyFile(path("w6", "A"), halvesOfRange(600, 32)),
                  "W6 surviving bytes are still the original records, not a torn tail");
        }
    } catch (const std::exception& e) {
        check(false, (QString("W exception: ") + e.what()).toUtf8().constData());
    }

    if (g_failures == 0) {
        std::cout << "filesaver_round_boundary_test: ALL PASS\n";
        return 0;
    }
    std::cout << "filesaver_round_boundary_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
