#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"
#include "FileSaver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using paimage::AutoSaveBoundaryKind;
using paimage::AutoSaveRoundCoordinator;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

TriggerGroupPtr makeGroup(std::uint64_t sessionGen,
                          std::uint64_t roundGeneration,
                          int value)
{
    constexpr int kSamples = 16;
    auto group = std::make_shared<TriggerGroup>();
    group->cardId = 0;
    group->triggerSeq = static_cast<std::uint16_t>(value);
    group->sampleCount = kSamples;
    group->sessionGen = sessionGen;
    group->normalizationApplied = true;
    group->physicalDecision = paimage::PhysicalTriggerDecision::LogicalScan;
    group->roundGeneration = roundGeneration;
    group->freqA.assign(kSamples, static_cast<float>(value));
    group->freqB.assign(kSamples, static_cast<float>(-value));
    return group;
}

void drain(FileSaver& saver)
{
    TriggerGroupPtr group;
    while (saver.saveQueue()->try_dequeue(group))
        saver.consumeTriggerGroup(group);
}

bool exists(const QString& path)
{
    return QFile::exists(path);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A1/A2/A4: the coordinator publishes a new generation before the UI
    // callback runs, while old stamped groups already queued retain the old
    // directory and the final old CountBoundary group is not split early.
    {
        QTemporaryDir root;
        check(root.isValid(), "A1 temporary root");
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto first = coordinator.beginSession(41);
        check(first.committed && first.newSessionGen == 1,
              "A1 initial generation committed");

        FileSaver saver(0);
        saver.setSessionDirResolver([&coordinator](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        saver.startSaving(root.path(), 100, QStringLiteral("coord"));

        // These groups model a saver backlog.  No UI callback is executed
        // between the source commit and the next LogicalScan stamp.
        saver.saveTriggerGroup(makeGroup(first.newSessionGen, 0, 101));
        saver.saveTriggerGroup(makeGroup(first.newSessionGen, 0, 102));
        const auto timeout = coordinator.commitBoundary(
            41, 1, AutoSaveBoundaryKind::Timeout, QStringLiteral("source_timeout"));
        check(timeout.committed && timeout.newSessionGen == 2,
              "A1 TimeoutBoundary commits before UI callback");
        const auto immediateNew = makeGroup(coordinator.currentGeneration(), 1, 201);
        check(immediateNew->sessionGen == timeout.newSessionGen,
              "A1 immediate next LogicalScan reads new generation");
        saver.saveTriggerGroup(immediateNew);
        saver.saveTriggerGroup(makeGroup(timeout.newSessionGen, 1, 202));
        drain(saver);
        saver.stopSaving();

        const QString newFile = QDir(timeout.directory).filePath(
            QStringLiteral("Card1_ChA_coord_000.dat"));
        check(exists(first.directory), "A2 old directory remains registered");
        check(exists(timeout.directory), "A1 new directory exists before UI apply");
        check(exists(QDir(first.directory).filePath(
                         QStringLiteral("Card1_ChA_coord_000.dat"))),
              "A2 queued old groups use old directory");
        check(exists(newFile), "A1/A2 new groups use new directory");
        check(QFileInfo(QDir(first.directory).filePath(
                             QStringLiteral("Card1_ChA_coord_000.dat"))).size() == 64,
              "A2 old backlog has exactly two triggers");
        check(QFileInfo(newFile).size() == 64,
              "A1 new directory has exactly two new triggers");
    }

    // A3: observer/timer/UI attempts for one physical boundary are
    // idempotent; only one directory is allocated.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        std::vector<AutoSaveRoundCoordinator::Event::Kind> events;
        coordinator.setEventSink([&events](const auto& event) {
            events.push_back(event.kind);
        });
        coordinator.configure(root.path(), 10);
        const auto first = coordinator.beginSession(7);
        const auto source = coordinator.commitBoundary(
            7, 3, AutoSaveBoundaryKind::Timeout, QStringLiteral("observer"));
        const auto timer = coordinator.commitBoundary(
            7, 3, AutoSaveBoundaryKind::Timeout, QStringLiteral("timer"));
        const auto ui = coordinator.commitBoundary(
            7, 3, AutoSaveBoundaryKind::Timeout, QStringLiteral("ui"));
        check(first.committed && source.committed, "A3 first boundary commits");
        check(timer.alreadyApplied && ui.alreadyApplied,
              "A3 timer/UI duplicate boundary is idempotent");
        check(source.newSessionGen == timer.newSessionGen &&
              timer.newSessionGen == ui.newSessionGen,
              "A3 duplicate callers observe one generation");
        check(events.size() == 6 &&
              events[0] == AutoSaveRoundCoordinator::Event::Kind::Reserved &&
              events[1] == AutoSaveRoundCoordinator::Event::Kind::Committed &&
              events[2] == AutoSaveRoundCoordinator::Event::Kind::Reserved &&
              events[3] == AutoSaveRoundCoordinator::Event::Kind::Committed &&
              events[4] == AutoSaveRoundCoordinator::Event::Kind::AlreadyApplied &&
              events[5] == AutoSaveRoundCoordinator::Event::Kind::AlreadyApplied,
              "A3 low-frequency event sequence has no skipped/extra commit");
        check(!exists(QDir(root.path()).filePath(QStringLiteral("013"))),
              "A3 duplicate callers do not create a third directory");
    }

    // A5: registration failure enters a fail-closed state.  A new group gets
    // the invalid sentinel and FileSaver drops it instead of using old dir.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto first = coordinator.beginSession(1);
        FileSaver saver(0);
        saver.setSessionDirResolver([&coordinator](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        saver.startSaving(root.path(), 100, QStringLiteral("failure"));
        saver.saveTriggerGroup(makeGroup(first.newSessionGen, 0, 301));
        coordinator.setDirectoryPreparer([](const QString&) { return false; });
        const auto failed = coordinator.commitBoundary(
            1, 1, AutoSaveBoundaryKind::Timeout, QStringLiteral("registration_failure"));
        check(failed.failed && coordinator.faulted(),
              "A5 directory registration failure is explicit");
        check(coordinator.currentGeneration() == AutoSaveRoundCoordinator::kFailedGeneration,
              "A5 failure publishes fail-closed sentinel");
        const auto unresolved = coordinator.resolveRound(1, 2);
        check(unresolved.failed &&
                  unresolved.sessionGen == AutoSaveRoundCoordinator::kFailedGeneration,
              "A5 failed coordinator keeps round lookup fail-closed");
        saver.saveTriggerGroup(makeGroup(coordinator.currentGeneration(), 1, 302));
        drain(saver);
        saver.stopSaving();
        check(saver.savedCount() == 1,
              "A5 failed generation is not saved into old directory");
        check(!exists(failed.directory),
              "A5 failed directory is not registered/created");
        check(QFileInfo(QDir(first.directory).filePath(
                             QStringLiteral("Card1_ChA_failure_000.dat"))).size() == 32,
              "A5 old directory contains only pre-failure data");
        coordinator.disable();
        check(!coordinator.enabled() &&
              coordinator.currentGeneration() == AutoSaveRoundCoordinator::kFailedGeneration,
              "A5 disable keeps fail-closed sentinel until reconfigure");
    }

    // A6: a new controller/lifecycle configuration clears old bindings and
    // starts a fresh monotonic on-disk range.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto old = coordinator.beginSession(10);
        coordinator.commitBoundary(10, 1, AutoSaveBoundaryKind::Count,
                                    QStringLiteral("count"));
        coordinator.disable();
        check(!coordinator.enabled() && coordinator.currentGeneration() == 0,
              "A6 disable clears active binding");
        QTemporaryDir nextRoot;
        coordinator.configure(nextRoot.path(), 20);
        const auto fresh = coordinator.beginSession(11);
        check(fresh.committed && fresh.newSessionGen == 21,
              "A6 lifecycle restart uses fresh directory range");
        check(coordinator.directoryFor(old.newSessionGen).isEmpty(),
              "A6 old controller binding is not inherited");
        check(coordinator.directoryFor(fresh.newSessionGen) == fresh.directory,
              "A6 fresh binding is registered");
        coordinator.configure(QString(), 0);
        check(!coordinator.enabled() && coordinator.currentGeneration() == 0,
              "A6 empty base directory disables configuration");
    }

    // A7/A8: 自动保存文件夹编号不得与磁盘上已有的会话文件夹撞号。
    // directoryPreparer_ 的默认实现是 mkpath，对已存在目录同样返回 true，只靠
    // prepared 判定会把新会话写进旧文件夹——FileSaver 随后在其中开新序号，旧
    // 轮次的命名空间从此被两个会话共用，成为"后写数据落进过往文件夹"的入口。
    {
        QTemporaryDir root;
        check(root.isValid(), "A7 temporary root");
        check(QDir().mkpath(QDir(root.path()).filePath("001")), "A7 seed 001");
        check(QDir().mkpath(QDir(root.path()).filePath("002")), "A7 seed 002");
        // 1000 号段：历史 scanMaxAutoFolder 只认恰好 3 字符的名字，会直接漏掉它，
        // 于是下次启动把编号退回去复用旧文件夹。
        check(QDir().mkpath(QDir(root.path()).filePath("1000")), "A7 seed 1000");
        check(QDir().mkpath(QDir(root.path()).filePath("notes")), "A7 seed non-numeric");

        check(AutoSaveRoundCoordinator::scanMaxDirectoryNumber(root.path()) == 1000,
              "A8 scanMaxDirectoryNumber sees the 1000 block");
        QTemporaryDir emptyRoot;
        check(AutoSaveRoundCoordinator::scanMaxDirectoryNumber(emptyRoot.path()) == 0,
              "A8 scanMaxDirectoryNumber on an empty base returns 0");

        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(
            root.path(),
            static_cast<std::uint64_t>(
                AutoSaveRoundCoordinator::scanMaxDirectoryNumber(root.path())));
        const auto first = coordinator.beginSession(71);
        check(first.committed && first.newSessionGen == 1001 &&
                  first.directory == QDir(root.path()).filePath("1001"),
              "A7 allocation continues above the on-disk maximum");

        // 即便 lastDirectoryNumber 被低估（历史扫描漏掉 1000 号段的故障模式），
        // commitBoundary 也必须跳过已存在的目录，而不是把新会话塞进去。
        AutoSaveRoundCoordinator reckless;
        reckless.configure(root.path(), 0);
        const auto second = reckless.beginSession(72);
        check(second.committed && second.newSessionGen == 3 &&
                  second.directory == QDir(root.path()).filePath("003"),
              "A7 an underestimated scan still skips directories that already exist");
    }

    if (g_failures == 0) {
        std::cout << "auto_save_round_coordinator_test: ALL PASS\n";
        return 0;
    }
    std::cout << "auto_save_round_coordinator_test: " << g_failures
              << " FAILURE(S)\n";
    return 1;
}
