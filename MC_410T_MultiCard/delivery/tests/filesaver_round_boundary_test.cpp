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

TriggerGroupPtr makeGroup(std::uint16_t trig, std::uint64_t roundGen, int value)
{
    auto g = std::make_shared<TriggerGroup>();
    g->cardId = 0;
    g->triggerSeq = trig;
    g->sampleCount = kSamples;
    g->isComplete = true;
    g->sourceIPv4 = 0x0100007f;
    g->sessionGen = 0;
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
        check(verifyFile(dir.path() + "/Card1_ChA_t10_000.dat", halvesOf({21, 22})),
              "T10 second measurement owns file 000 fresh");
        // Source restart path: suspend/resume resets round state too.
        saver.startSaving(dir.path(), 100, "t10");
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
        check(verifyFile(dir.path() + "/Card1_ChA_t10_001.dat", halvesOf({41})),
              "T10 post-resume file holds the adopted round");
        check(verifyFile(dir.path() + "/Card1_ChA_t10_002.dat", halvesOf({42})),
              "T10 final rollover file holds the new round");
    } catch (const std::exception& e) {
        check(false, (QString("T10 exception: ") + e.what()).toUtf8().constData());
    }

    if (g_failures == 0) {
        std::cout << "filesaver_round_boundary_test: ALL PASS\n";
        return 0;
    }
    std::cout << "filesaver_round_boundary_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
