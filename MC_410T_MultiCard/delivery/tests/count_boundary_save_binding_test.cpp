#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"
#include "PaimageAcquisition/HostOutput.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

using namespace paimage;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

template <class F>
bool until(F predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return predicate();
}

Frame makeFrame(std::uint64_t session,
                int card,
                std::uint16_t trigger,
                std::int64_t monotonicNs,
                float value)
{
    constexpr int samples = 16;
    auto frame = std::make_shared<CardFrame>();
    frame->measurementSession = session;
    frame->card = card;
    frame->trigger = trigger;
    frame->first = monotonicNs;
    frame->closed = monotonicNs;
    frame->complete = true;
    frame->reason = Decision::Complete;
    frame->bytes.resize(samples * 8);
    const std::int32_t encoded = static_cast<std::int32_t>(value);
    for (int i = 0; i < samples; ++i) {
        std::memcpy(frame->bytes.data() + i * 8, &encoded, sizeof(encoded));
        std::memcpy(frame->bytes.data() + i * 8 + 4, &encoded, sizeof(encoded));
    }
    return frame;
}

TriggerGroupPtr makeGroup(std::uint64_t sessionGen,
                          std::uint64_t roundGeneration,
                          std::uint16_t trigger)
{
    auto group = std::make_shared<TriggerGroup>();
    group->cardId = 0;
    group->triggerSeq = trigger;
    group->sampleCount = 16;
    group->sessionGen = sessionGen;
    group->roundGeneration = roundGeneration;
    group->normalizationApplied = true;
    group->physicalDecision = PhysicalTriggerDecision::LogicalScan;
    group->freqA.assign(16, roundGeneration == 0 ? 1.0f : 2.0f);
    group->freqB.assign(16, roundGeneration == 0 ? 1.0f : 2.0f);
    return group;
}

QString dataFile(const QString& directory, int card, const QString& suffix)
{
    return QDir(directory).filePath(
        QStringLiteral("Card%1_ChA_%2_000.dat").arg(card + 1).arg(suffix));
}

bool hasSize(const QString& path, qint64 expected)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() == expected;
}

bool allHalfValues(const QString& path, std::uint16_t expected, int count)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != count * 2)
        return false;
    const QByteArray bytes = file.readAll();
    for (int i = 0; i < count; ++i) {
        std::uint16_t actual = 0;
        std::memcpy(&actual, bytes.constData() + i * 2, sizeof(actual));
        if (actual != expected)
            return false;
    }
    return true;
}

void drain(FileSaver& saver)
{
    TriggerGroupPtr group;
    while (saver.saveQueue()->try_dequeue(group))
        saver.consumeTriggerGroup(group);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // C1/C2/C4/C6: CountBoundary commits the next data binding before the
    // old final reconstruction callback is supplied.  Two cards deliver the
    // final identity at different times; both old groups resolve to S while
    // the next control + logical identity resolves to S+1.
    {
        QTemporaryDir root;
        check(root.isValid(), "C1 temporary root");
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        std::vector<AutoSaveRoundCoordinator::CommitResult> countCommits;
        std::vector<std::unique_ptr<DisplayBuffer>> displays;
        std::vector<std::unique_ptr<FileSaver>> savers;
        std::vector<std::unique_ptr<DataProcessor>> processors;
        std::vector<DataProcessor*> processorPtrs;
        std::vector<FileSaver*> saverPtrs;
        for (int card = 0; card < 2; ++card) {
            displays.push_back(std::make_unique<DisplayBuffer>());
            savers.push_back(std::make_unique<FileSaver>(card));
            savers.back()->setSessionDirResolver([&coordinator](std::uint64_t generation) {
                return coordinator.directoryFor(generation);
            });
            processors.push_back(std::make_unique<DataProcessor>(
                card, nullptr, nullptr, AcqConfig{}));
            processorPtrs.push_back(processors.back().get());
            saverPtrs.push_back(savers.back().get());
        }
        HostOutput output(
            32, 50, processorPtrs, saverPtrs, nullptr, nullptr, 3,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary)
                    countCommits.push_back(coordinator.commitBoundary(
                        event.measurementSession, event.roundGeneration,
                        AutoSaveBoundaryKind::Count,
                        QStringLiteral("test_count_boundary")));
            });
        output.setMeasurementSessionBinder([&](std::uint64_t session) {
            const auto binding = coordinator.bindMeasurementSession(session);
            return binding.failed ? AutoSaveRoundCoordinator::kFailedGeneration
                                   : binding.newSessionGen;
        });
        output.setSaveSessionResolver([&](std::uint64_t session,
                                          std::uint64_t round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(501);
        output.start();
        const auto saveGeneration = output.startSaving(initial.directory, 100,
                                                        QStringLiteral("count"));
        check(until([&] { return output.savingApplied(saveGeneration); }),
              "C1 save configuration");

        std::int64_t time = 1000;
        auto send = [&](int card, std::uint16_t trigger, float value) {
            output.card(makeFrame(501, card, trigger, time++, value));
        };
        for (int card = 0; card < 2; ++card)
            send(card, 100, 0); // one shared operational control identity
        send(0, 101, 1);
        send(0, 102, 1);
        send(0, 103, 1); // this card observes CountBoundary first
        // Card 1's same final identity arrives after CountBoundary.
        send(1, 101, 1);
        send(1, 102, 1);
        send(1, 103, 1);

        check(until([&] { return savers[0]->acceptedCount() == 3 &&
                                  savers[1]->acceptedCount() == 3; }),
              "C1 old round saved before next presentation callback");
        check(countCommits.size() == 1 && countCommits.front().committed,
              "C1 CountBoundary committed synchronously");
        const auto count = countCommits.front();
        check(count.oldSessionGen == initial.newSessionGen &&
                  count.newSessionGen == initial.newSessionGen + 1,
              "C1 CountBoundary allocates exactly the next generation");
        check(coordinator.resolveRound(501, 0).sessionGen == initial.newSessionGen &&
                  coordinator.resolveRound(501, 1).sessionGen == count.newSessionGen,
              "C2 old/new round bindings are identity based");

        // No final reconstruction callback has run yet.  The next round can
        // already write because its binding was published by CountBoundary.
        send(0, 200, 0);
        send(1, 200, 0);
        send(0, 201, 2);
        send(0, 202, 2);
        send(1, 201, 2);
        send(1, 202, 2);
        check(until([&] { return savers[0]->acceptedCount() == 5 &&
                                  savers[1]->acceptedCount() == 5; }),
              "C1 next round does not wait for old final frame");
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }),
              "C1 stop saving");

        for (int card = 0; card < 2; ++card) {
            const QString oldFile = dataFile(initial.directory, card, "count");
            const QString newFile = dataFile(count.directory, card, "count");
            check(hasSize(oldFile, 3 * 16 * 2), "C2 old directory has three groups");
            check(hasSize(newFile, 2 * 16 * 2), "C1 new directory has two groups");
            check(allHalfValues(oldFile, 0x3c00, 3 * 16),
                  "C2 old .dat content is not mixed");
            check(allHalfValues(newFile, 0x4000, 2 * 16),
                  "C1 new .dat content is not mixed");
        }

        // C6: emulate the delayed old final reconstruction.  The old PNG is
        // written to oldDirectory, then presentation applies the committed
        // target without allocating another generation or directory.
        const QString oldRecon = QDir(initial.directory).filePath("recon_png");
        check(QDir().mkpath(oldRecon), "C6 old reconstruction directory");
        QImage oldImage(2, 2, QImage::Format_RGB32);
        oldImage.fill(Qt::white);
        check(oldImage.save(QDir(oldRecon).filePath("old-final.png"), "PNG"),
              "C6 old final PNG uses old directory");
        AutoSavePresentationState presentation;
        check(presentation.apply(initial) == AutoSavePresentationState::ApplyResult::Applied,
              "C6 initial presentation target");
        check(presentation.apply(count) == AutoSavePresentationState::ApplyResult::Applied &&
                  presentation.target().directory == count.directory,
              "C6 final-frame callback applies prepared new target");
        check(coordinator.currentGeneration() == count.newSessionGen &&
                  !QDir(root.path()).exists("003"),
              "C4/C6 final frame does not allocate or skip a generation");

        output.stop();
    }

    // C3: N=1 must bind round zero before the only LogicalScan is stamped;
    // the observer can allocate round one before classify() returns.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        std::vector<AutoSaveRoundCoordinator::CommitResult> commits;
        DisplayBuffer display;
        FileSaver saver(0);
        saver.setSessionDirResolver([&coordinator](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor processor(0, nullptr, nullptr, config);
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr, 1,
                           [&](const PhysicalRoundEvent& event) {
                               if (event.kind == PhysicalRoundEvent::Kind::CountBoundary)
                                   commits.push_back(coordinator.commitBoundary(
                                       event.measurementSession, event.roundGeneration,
                                       AutoSaveBoundaryKind::Count,
                                       QStringLiteral("test_n1")));
                           });
        output.setMeasurementSessionBinder([&](std::uint64_t session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](std::uint64_t session,
                                          std::uint64_t round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(601);
        output.start();
        const auto configured = output.startSaving(initial.directory, 100,
                                                   QStringLiteral("n1"));
        check(until([&] { return output.savingApplied(configured); }),
              "C3 save configuration");
        output.card(makeFrame(601, 0, 10, 1000, 0));
        output.card(makeFrame(601, 0, 11, 1100, 3));
        output.card(makeFrame(601, 0, 20, 1200, 0));
        output.card(makeFrame(601, 0, 21, 1300, 4));
        check(until([&] { return saver.acceptedCount() == 2; }),
              "C3 both N=1 logical scans saved");
        check(commits.size() == 2 &&
                  coordinator.resolveRound(601, 0).sessionGen == initial.newSessionGen &&
                  coordinator.resolveRound(601, 1).sessionGen == commits.front().newSessionGen,
              "C3 N=1 old/new save bindings");
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }), "C3 stop saving");
        check(hasSize(dataFile(initial.directory, 0, "n1"), 16 * 2) &&
                  hasSize(dataFile(commits.front().directory, 0, "n1"), 16 * 2),
              "C3 N=1 actual files are separated");
        output.stop();
    }

    // C4: three consecutive count-complete rounds keep a stable identity to
    // directory mapping for every card.  The third boundary prepares the
    // next directory, but does not create data in it until that round starts.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(651);
        std::vector<AutoSaveRoundCoordinator::CommitResult> commits;
        std::vector<std::unique_ptr<DisplayBuffer>> displays;
        std::vector<std::unique_ptr<FileSaver>> savers;
        std::vector<std::unique_ptr<DataProcessor>> processors;
        std::vector<DataProcessor*> processorPtrs;
        std::vector<FileSaver*> saverPtrs;
        AcqConfig config;
        config.acqTimeNs = 64;
        for (int card = 0; card < 2; ++card) {
            displays.push_back(std::make_unique<DisplayBuffer>());
            savers.push_back(std::make_unique<FileSaver>(card));
            savers.back()->setSessionDirResolver([&coordinator](std::uint64_t generation) {
                return coordinator.directoryFor(generation);
            });
            processors.push_back(std::make_unique<DataProcessor>(
                card, nullptr, nullptr, config));
            processorPtrs.push_back(processors.back().get());
            saverPtrs.push_back(savers.back().get());
        }
        HostOutput output(
            32, 50, processorPtrs, saverPtrs, nullptr, nullptr, 2,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary)
                    commits.push_back(coordinator.commitBoundary(
                        event.measurementSession, event.roundGeneration,
                        AutoSaveBoundaryKind::Count,
                        QStringLiteral("test_three_count_rounds")));
            });
        output.setMeasurementSessionBinder([&](std::uint64_t session) {
            const auto binding = coordinator.bindMeasurementSession(session);
            return binding.failed ? AutoSaveRoundCoordinator::kFailedGeneration
                                   : binding.newSessionGen;
        });
        output.setSaveSessionResolver([&](std::uint64_t session,
                                          std::uint64_t round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(651);
        output.start();
        const auto configured = output.startSaving(initial.directory, 100,
                                                   QStringLiteral("c4"));
        check(until([&] { return output.savingApplied(configured); }),
              "C4 save configuration");

        std::int64_t time = 1000;
        int value = 1;
        for (int round = 0; round < 3; ++round, ++value) {
            const std::uint16_t control = static_cast<std::uint16_t>(200 + round * 10);
            for (int card = 0; card < 2; ++card)
                output.card(makeFrame(651, card, control, time++, 0));
            for (int card = 0; card < 2; ++card) {
                output.card(makeFrame(651, card,
                                      static_cast<std::uint16_t>(control + 1),
                                      time++, static_cast<float>(value)));
                output.card(makeFrame(651, card,
                                      static_cast<std::uint16_t>(control + 2),
                                      time++, static_cast<float>(value)));
            }
        }
        check(until([&] { return savers[0]->acceptedCount() == 6 &&
                                  savers[1]->acceptedCount() == 6; }),
              "C4 three rounds saved before presentation callbacks");
        check(commits.size() == 3 && commits[0].committed &&
                  commits[1].committed && commits[2].committed &&
                  commits[0].newSessionGen == initial.newSessionGen + 1 &&
                  commits[1].newSessionGen == initial.newSessionGen + 2 &&
                  commits[2].newSessionGen == initial.newSessionGen + 3,
              "C4 each CountBoundary allocates one next generation");
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }),
              "C4 stop saving");
        output.stop();

        const std::array<QString, 3> roundDirectories{
            initial.directory, commits[0].directory, commits[1].directory};
        const std::array<std::uint16_t, 3> expectedValues{0x3c00, 0x4000, 0x4200};
        for (int round = 0; round < 3; ++round) {
            for (int card = 0; card < 2; ++card) {
                const QString file = dataFile(roundDirectories[round], card, "c4");
                check(hasSize(file, 2 * 16 * 2),
                      "C4 each round has exactly two logical groups per card");
                check(allHalfValues(file, expectedValues[round], 2 * 16),
                      "C4 round file content is isolated");
            }
            const QString recon = QDir(roundDirectories[round]).filePath("recon_png");
            check(QDir().mkpath(recon), "C4 old final PNG directory prepared");
            QImage image(2, 2, QImage::Format_RGB32);
            image.fill(Qt::white);
            check(image.save(QDir(recon).filePath(
                                 QStringLiteral("round-%1.png").arg(round)), "PNG"),
                  "C4 old final PNG remains in its round directory");
        }
        check(!QDir(root.path()).exists("005"),
              "C4 no extra directory is allocated by final-frame handling");
    }

    // C5: saver backlog is explicitly FIFO-drained after the next binding is
    // committed.  Actual bytes, not only counters, prove no cross-directory
    // mixing.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        coordinator.bindMeasurementSession(701);
        FileSaver saver(0);
        saver.setSessionDirResolver([&](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        saver.startSaving(initial.directory, 100, QStringLiteral("backlog"));
        const auto oldGen = coordinator.resolveRound(701, 0).sessionGen;
        for (int i = 0; i < 3; ++i)
            saver.saveTriggerGroup(makeGroup(oldGen, 0, 1));
        const auto count = coordinator.commitBoundary(
            701, 1, AutoSaveBoundaryKind::Timeout, QStringLiteral("C9_timeout_backlog"));
        const auto newGen = coordinator.resolveRound(701, 1).sessionGen;
        for (int i = 0; i < 2; ++i)
            saver.saveTriggerGroup(makeGroup(newGen, 1, 2));
        drain(saver);
        saver.stopSaving();
        check(count.committed && oldGen != newGen, "C5 backlog boundary committed");
        check(allHalfValues(dataFile(initial.directory, 0, "backlog"), 0x3c00, 3 * 16),
              "C5 old backlog bytes stay in old directory");
        check(allHalfValues(dataFile(count.directory, 0, "backlog"), 0x4000, 2 * 16),
              "C5 new backlog bytes stay in new directory");
    }

    // C7: delayed presentation callbacks are monotonic and cannot overwrite a
    // newer applied target.  Count commits remain the only allocation points.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto first = coordinator.beginSession(0);
        coordinator.bindMeasurementSession(801);
        const auto round1 = coordinator.commitBoundary(
            801, 1, AutoSaveBoundaryKind::Count, QStringLiteral("c7-1"));
        const auto round2 = coordinator.commitBoundary(
            801, 2, AutoSaveBoundaryKind::Count, QStringLiteral("c7-2"));
        AutoSavePresentationState presentation;
        check(presentation.apply(first) == AutoSavePresentationState::ApplyResult::Applied,
              "C7 initial target");
        check(presentation.apply(round2) == AutoSavePresentationState::ApplyResult::Applied,
              "C7 newer target applies");
        check(presentation.apply(round1) == AutoSavePresentationState::ApplyResult::Stale,
              "C7 delayed old target is ignored");
        check(presentation.target().sessionGen == round2.newSessionGen &&
                  presentation.target().directory == round2.directory &&
                  coordinator.currentGeneration() == round2.newSessionGen,
              "C7 stale apply cannot roll back or allocate");
    }

    // The boundary callback runs before the final old group is enqueued.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto first = coordinator.beginSession(0);
        coordinator.setDirectoryPreparer([](const QString&) { return false; });
        FileSaver saver(0);
        saver.setSessionDirResolver([&](auto gen) { return coordinator.directoryFor(gen); });
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor processor(0, nullptr, nullptr, config);
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr, 2,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary &&
                    coordinator.enabled())
                    coordinator.commitBoundary(event.measurementSession, event.roundGeneration,
                        AutoSaveBoundaryKind::Count, QStringLiteral("test_failed_next_directory"));
            }, 0.0, 0);
        output.setMeasurementSessionBinder([&](auto session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](auto session, auto round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(900);
        output.start();
        output.startSaving(first.directory, 100, QStringLiteral("fault"));
        output.card(makeFrame(900, 0, 110, 1000, 1));
        output.card(makeFrame(900, 0, 111, 2000, 2));
        check(coordinator.faulted(), "failed next directory leaves old boundary active");
        output.card(makeFrame(900, 0, 112, 3000, 3));
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }), "fault stop drained");
        check(saver.savedCount() == 2 &&
              hasSize(dataFile(first.directory, 0, "fault"), 64),
              "fault stop saved exactly the old round including its final group");
        output.stop();
    }

    // C8: timeout uses the same round-aware binding resolver; the next round
    // is routable immediately after the source boundary.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        std::vector<AutoSaveRoundCoordinator::CommitResult> timeoutCommits;
        DisplayBuffer display;
        FileSaver saver(0);
        saver.setSessionDirResolver([&coordinator](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor processor(0, nullptr, nullptr, config);
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr, 3,
                           [&](const PhysicalRoundEvent& event) {
                               if (event.kind == PhysicalRoundEvent::Kind::TimeoutBoundary)
                                   timeoutCommits.push_back(coordinator.commitBoundary(
                                       event.measurementSession, event.roundGeneration,
                                       AutoSaveBoundaryKind::Timeout,
                                       QStringLiteral("test_timeout")));
                           }, 1.0e-6);
        output.setMeasurementSessionBinder([&](std::uint64_t session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](std::uint64_t session,
                                          std::uint64_t round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(901);
        output.start();
        const auto configured = output.startSaving(initial.directory, 100,
                                                   QStringLiteral("timeout"));
        check(until([&] { return output.savingApplied(configured); }),
              "C8 save configuration");
        output.card(makeFrame(901, 0, 100, 1000, 1));
        output.card(makeFrame(901, 0, 101, 1500, 1));
        output.card(makeFrame(901, 0, 102, 1800, 1));
        check(output.pollPhysicalRoundTimeout(20000) && timeoutCommits.size()==1 &&
                  coordinator.resolveRound(901,1).sessionGen==timeoutCommits.front().newSessionGen,
              "Session C9 active timeout publishes binding before next trigger");
        output.card(makeFrame(901, 0, 200, 20000, 0)); // new round control
        output.card(makeFrame(901, 0, 201, 20500, 2));
        output.card(makeFrame(901, 0, 202, 20800, 2));
        check(until([&] { return saver.acceptedCount() == 4; }),
              "C8 timeout next round saved immediately");
        check(timeoutCommits.size() == 1 &&
                  coordinator.resolveRound(901, 0).sessionGen == initial.newSessionGen &&
                  coordinator.resolveRound(901, 1).sessionGen == timeoutCommits.front().newSessionGen,
               "C8 timeout old/new binding");
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }), "C8 stop saving");
        check(hasSize(dataFile(initial.directory, 0, "timeout"), 2 * 16 * 2) &&
                  hasSize(dataFile(timeoutCommits.front().directory, 0, "timeout"),
                          2 * 16 * 2),
               "C8 timeout actual directories remain split");
        output.stop();
    }

    // C9: auto-save off preserves manual generation=0 semantics.
    {
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        coordinator.beginSession(0);
        coordinator.disable();
        DisplayBuffer display;
        FileSaver saver(0);
        saver.setSessionDirResolver([&coordinator](std::uint64_t generation) {
            return coordinator.directoryFor(generation);
        });
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor processor(0, nullptr, nullptr, config);
        HostOutput output(32, 50, {&processor}, {&saver}, nullptr, nullptr, 1,
                           [&](const PhysicalRoundEvent&) {});
        output.setMeasurementSessionBinder([&](std::uint64_t session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](std::uint64_t session,
                                          std::uint64_t round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.beginSession(1001);
        output.start();
        const auto configured = output.startSaving(root.path(), 100,
                                                   QStringLiteral("manual"));
        check(until([&] { return output.savingApplied(configured); }),
              "C9 manual save configuration");
        output.card(makeFrame(1001, 0, 10, 1000, 5));
        output.card(makeFrame(1001, 0, 11, 1100, 5));
        check(until([&] { return saver.acceptedCount() == 1; }),
              "C9 manual logical scan saved");
        const auto stopped = output.stopSaving();
        check(until([&] { return output.savingApplied(stopped); }), "C9 stop saving");
        check(hasSize(dataFile(root.path(), 0, "manual"), 16 * 2),
              "C9 manual data remains in base directory");
        output.stop();
    }

    // C10: measurement/source lifecycle resets the identity table.  Old
    // bindings are not usable by a new measurement, while the prepared
    // current generation remains available for the new round zero.
    {
        QTemporaryDir root;
        QTemporaryDir nextRoot;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        coordinator.bindMeasurementSession(1101);
        const auto oldRound = coordinator.commitBoundary(
            1101, 1, AutoSaveBoundaryKind::Count, QStringLiteral("c10-old"));
        check(coordinator.resolveRound(1101, 1).sessionGen == oldRound.newSessionGen,
              "C10 old measurement binding exists before restart");
        const auto newStart = coordinator.bindMeasurementSession(1102);
        check(newStart.committed &&
                  coordinator.resolveRound(1102, 0).sessionGen == newStart.newSessionGen &&
                  coordinator.resolveRound(1101, 0).failed &&
                  coordinator.resolveRound(1101, 0).sessionGen ==
                      AutoSaveRoundCoordinator::kFailedGeneration,
              "C10 new measurement cannot resolve old round binding");
        const auto activeRound = coordinator.bindMeasurementRound(
            1102, 7, QStringLiteral("c10-active-round"));
        check(activeRound.committed &&
                  coordinator.resolveRound(1102, 7).sessionGen == activeRound.newSessionGen,
              "C10 active manual-to-auto transition binds the live round");
        coordinator.bindMeasurementSession(1102); // source restart/resume reset
        check(coordinator.resolveRound(1102, 1).failed,
              "C10 restart clears stale round-one binding");
        coordinator.disable();
        check(coordinator.resolveRound(1102, 0).sessionGen == 0,
              "C10 auto-save off returns manual generation");
        coordinator.configure(nextRoot.path(), 20);
        const auto fresh = coordinator.beginSession(0);
        check(fresh.committed && fresh.newSessionGen == 21 &&
                  coordinator.directoryFor(initial.newSessionGen).isEmpty(),
              "C10 reconfigure starts an isolated directory registry");
    }

    // The GUI can request stop before the other card publishes the old frame.
    for (const bool count : {true, false}) {
        QTemporaryDir root;
        QTemporaryDir newRoot;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        coordinator.setDirectoryPreparer([](const QString&) { return false; });
        FileSaver a(0), b(1);
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor pa(0, nullptr, nullptr, config), pb(1, nullptr, nullptr, config);
        HostOutput output(32, 50, {&pa, &pb}, {&a, &b}, nullptr, nullptr, 2,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary ||
                    event.kind == PhysicalRoundEvent::Kind::TimeoutBoundary) {
                    coordinator.commitBoundary(event.measurementSession, event.roundGeneration,
                        count ? AutoSaveBoundaryKind::Count : AutoSaveBoundaryKind::Timeout, "fault-tail");
                }
            }, 1.0, 0, !count);
        output.setSaveBoundaryFaultProbe([&] { return coordinator.faulted(); });
        output.setMeasurementSessionBinder([&](auto session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](auto session, auto round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.start();
        output.beginSession(777);
        const auto started = output.startSaving(initial.directory, 100, "tail-fault",
                                                coordinator.directoryResolver());
        check(until([&] { return output.savingApplied(started); }), "fault-tail request applied");
        output.card(makeFrame(777, 0, 10, 1000000000LL, 1));
        output.card(makeFrame(777, 1, 10, 1000000000LL, 1));
        if (count)
            output.card(makeFrame(777, 0, 11, 1900000000LL, 2));
        else {
            output.assemblyStarted(1, 777, 11, 1900000000LL);
            check(output.pollPhysicalRoundTimeout(2000000000LL), "fault-tail timeout reached");
        }
        check(coordinator.faulted(), "fault-tail next folder refused");
        const auto stopped = output.stopSaving();
        check(!output.savingApplied(stopped), "old tail defers GUI stop");
        coordinator.disable();
        auto tail = std::const_pointer_cast<CardFrame>(makeFrame(777, 1, 11, 1900000000LL, 2));
        tail->firstIngressId = 11;
        tail->closed = count ? 1910000000LL : 2050000000LL;
        output.card(tail);
        check(until([&] { return output.savingApplied(stopped); }), "old tail and stop settled");
        coordinator.selectManualSave();
        const auto restarted = output.startSaving(newRoot.path(), 100, "next");
        output.card(makeFrame(777, 0, 12, 2200000000LL, 3));
        const auto stoppedAgain = output.stopSaving();
        check(until([&] { return output.savingApplied(stoppedAgain); }), "next request settled");
        check(output.savingApplied(restarted), "next request applied");
        output.stop();
        check(hasSize(dataFile(initial.directory, 0, "tail-fault"), count ? 64 : 32),
              "first card kept its old data");
        check(hasSize(dataFile(initial.directory, 1, "tail-fault"), 64),
              "delayed card kept both old columns");
        check(hasSize(dataFile(newRoot.path(), 0, "next"), 32), "new request has only new data");
    }

    // Drive packet assembly as well as classification. Each trigger spans
    // two datagrams, and the final trigger wraps from 65535 to 0.
    enum class TailCase { Complete, EarlyNext, Expired, Partial, PartialFinal, SessionEnded, AdjacentTimeout, TimeoutSplit };
    for (const auto mode : {TailCase::Complete, TailCase::EarlyNext, TailCase::Expired, TailCase::Partial,
                            TailCase::PartialFinal, TailCase::SessionEnded, TailCase::AdjacentTimeout, TailCase::TimeoutSplit}) {
        const bool adjacentTimeout = mode == TailCase::AdjacentTimeout || mode == TailCase::TimeoutSplit;
        const bool split = mode == TailCase::TimeoutSplit;
        using Outcome = HostOutput::SaveTailOutcome;
        QTemporaryDir root, newRoot;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        coordinator.setDirectoryPreparer([](const QString&) { return false; });
        FileSaver a(0), b(1);
        for (auto* saver : {&a, &b}) saver->setRawAcquisitionEvidence(32, 4, /*writeSidecar=*/true);
        AcqConfig config;
        constexpr int samples = 200;
        config.acqTimeNs = samples * 4;
        DataProcessor pa(0, nullptr, nullptr, config), pb(1, nullptr, nullptr, config);
        std::vector<std::tuple<int, std::uint16_t, Outcome>> outcomes;
        std::array<FileSaver::SaveResult, 2> settled, nextSettled;
        HostOutput output(32, 50, {&pa, &pb}, {&a, &b}, nullptr, nullptr, 3,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary)
                    coordinator.commitBoundary(event.measurementSession, event.roundGeneration,
                        AutoSaveBoundaryKind::Count, "count-tail");
            }, adjacentTimeout ? 0.1 : 0.0, 0);
        RoundIdentity frontendBarrier;
        output.setFrontendBarrierSink([&](auto session, auto generation) {
            frontendBarrier = {session, generation};
        });
        output.setSaveBoundaryFaultProbe([&] { return coordinator.faulted(); });
        output.setMeasurementSessionBinder([&](auto session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](auto session, auto round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.setSaveTailSink([&](auto round, auto trigger, int card, Outcome outcome) {
            check(round == RoundIdentity{778, 0}, "failed Count outcome stays with the old round");
            outcomes.emplace_back(card, trigger, outcome);
        });
        output.beginSession(778);
        output.start();
        const auto started = output.startSaving(initial.directory, split ? 2 : 100, "wrap", coordinator.directoryResolver(),
            [&](const auto& result) { settled.at(result.cardId) = result; });
        check(until([&] { return output.savingApplied(started); }), "source Count request ready");
        SourceCore source({2, samples, 32, 0}, [&](Frame frame) { output.card(frame); },
                          [&](auto seq, const auto& frames, bool startup) { output.sync(seq, frames, startup); });
        source.setAssemblySink([&](int card, auto session, auto ingress, Time first) {
            output.assemblyStarted(card, session, ingress, first);
        });
        source.prepareStart(778);
        source.completeStart(true, 0);
        std::uint64_t ingress = 0;
        constexpr Time ms = 1000000;
        auto send = [&](int card, std::uint16_t seq, int packet, Time time) {
            std::vector<std::uint8_t> bytes(4 + (packet == 0 ? 1440 : 160));
            bytes[0] = std::uint8_t(packet);
            bytes[2] = std::uint8_t(seq);
            bytes[3] = std::uint8_t(seq >> 8);
            const std::int32_t amplitude = 1 + (seq & 7);
            for (std::size_t offset = 4; offset < bytes.size(); offset += 4)
                std::memcpy(bytes.data() + offset, &amplitude, 4);
            return source.ingest(card, bytes.data(), bytes.size(), time * ms, ++ingress);
        };
        auto whole = [&](int card, std::uint16_t seq, Time time) {
            check(send(card, seq, 0, time) == Decision::Accepted, "first raw packet accepted");
            check(send(card, seq, 1, time + 1) == Decision::Accepted, "last raw packet accepted");
        };
        whole(0, 65534, 1000);
        whole(1, 65534, 1001);
        whole(0, 65535, 1003);
        send(1, 65535, 0, 1005);
        if (mode == TailCase::EarlyNext) {
            send(1, 65535, 1, 1006);
            send(1, 1, 0, 1007);
        }
        if (mode == TailCase::PartialFinal) {
            send(1, 65535, 1, 1006);
            check(send(1, 0, 0, 1008) == Decision::Accepted, "final assembly began with one packet");
        }
        if (adjacentTimeout) {
            send(1, 65535, 1, 1006);
            send(1, 1, 0, 1020);
        }
        whole(0, 0, adjacentTimeout ? 1039 : 1010);
        check(coordinator.faulted(), "Count directory creation refused");
        const auto stopped = output.stopSaving();
        coordinator.selectManualSave();
        const auto restarted = output.startSaving(newRoot.path(), 100, "next", {},
            [&](const auto& result) { nextSettled.at(result.cardId) = result; });
        check(!output.savingApplied(stopped), "old request drains before queued restart");

        if (mode == TailCase::Complete) {
            send(1, 65535, 1, 1012);
            whole(1, 0, 1013);
            check(send(1, 65535, 1, 1015) == Decision::RecentTrigger, "duplicate tail packet stays deduplicated");
        } else if (mode == TailCase::EarlyNext) {
            send(1, 1, 1, 1012);
            whole(1, 0, 1013);
        } else if (adjacentTimeout) {
            send(1, 1, 1, 1050);
            check(output.pollPhysicalRoundTimeout(1125 * ms), "adjacent physical Timeout reached");
            check(frontendBarrier == RoundIdentity{778, 2}, "adjacent Timeout advances the frontend");
            whole(1, 0, 1138);
        } else if (mode == TailCase::Partial) {
            whole(1, 0, 1013); // switches off the incomplete previous assembly
            check(send(1, 65535, 1, 1015) == Decision::RecentTrigger, "source rejected the missing packet");
        } else if (mode == TailCase::PartialFinal) {
            source.poll(1108 * ms);
            check(send(1, 0, 1, 1109) == Decision::RecentTrigger, "closed final cannot be admitted twice");
        } else if (mode == TailCase::SessionEnded) {
            output.endSession();
        } else {
            const std::vector<Time> stalledWatermarks{1005 * ms, 1005 * ms};
            output.pollPhysicalRoundTimeout(1005 * ms, &stalledWatermarks, 1112 * ms);
        }
        check(until([&] { return output.savingApplied(restarted); }), "bounded drain releases restart");
        if (mode == TailCase::Expired) {
            send(1, 65535, 1, 1113);
            whole(1, 0, 1114);
        }
        if (mode == TailCase::SessionEnded) {
            source.prepareStart(779);
            output.beginSession(779);
            source.completeStart(true, 0);
        }
        whole(1, 2, adjacentTimeout ? 1141 : 1116);
        const auto done = output.stopSaving();
        check(until([&] { return output.savingApplied(done); }), "new request settles");
        output.stop();

        const auto contains = [&](int card, std::uint16_t trigger, Outcome why) {
            return std::find(outcomes.begin(), outcomes.end(), std::make_tuple(card, trigger, why)) != outcomes.end();
        };
        const auto oldCount = mode == TailCase::Expired || mode == TailCase::SessionEnded ? 1 : 3;
        check(settled[0].confirmed == 3 && settled[1].confirmed == std::uint64_t(oldCount),
              "old request confirms just its own columns");
        check(settled[0].admitted == 3 && settled[0].rejected == 0 &&
              settled[1].admitted == std::uint64_t(oldCount) &&
              settled[1].rejected == (mode == TailCase::EarlyNext || adjacentTimeout ? 1u : 0u),
              "old save counts follow the frame admission decisions");
        check(nextSettled[1].admitted == 1 && nextSettled[1].rejected == 0 && nextSettled[1].confirmed == 1,
              "reopened request owns only its next-round frame");
        check(std::count_if(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
            return std::get<0>(outcome) == 1 && std::get<1>(outcome) == 0;
        }) == 1, "slow card final has exactly one receipt");
        for (const auto lane : {'A', 'B'}) {
            const auto oldPath = QDir(initial.directory).filePath(QString("Card2_Ch%1_wrap_000.dat").arg(QChar(lane)));
            const auto newPath = QDir(newRoot.path()).filePath(QString("Card2_Ch%1_next_000.dat").arg(QChar(lane)));
            check(hasSize(oldPath, (split ? 2 : oldCount) * samples * 2), "old paired lanes match expected Count tail");
            const auto finalPath = split ? QDir(initial.directory).filePath(
                QString("Card2_Ch%1_wrap_001.dat").arg(QChar(lane))) : oldPath;
            if (oldCount == 3 && mode != TailCase::PartialFinal) {
                QFile finalFile(finalPath);
                QByteArray lastColumn(samples * 2, '\0'), expected(samples * 2, '\0');
                for (int sample = 0; sample < samples; ++sample) expected[2 * sample + 1] = 0x3c;
                check(finalFile.open(QIODevice::ReadOnly) && finalFile.seek(split ? 0 : 4 * samples) &&
                      finalFile.read(lastColumn.data(), lastColumn.size()) == lastColumn.size() &&
                      lastColumn == expected, "official Count pair carries the exact last waveform");
            }
            check(allHalfValues(newPath, 0x4200, samples), "restart contains only its own next-round waveform");
        }
        QFile sidecar(dataFile(initial.directory, 1, "wrap") + ".jsonl");
        check(sidecar.open(QIODevice::ReadOnly), "old slow-card evidence sealed");
        sidecar.readLine();
        for (int col = 0; col < oldCount; ++col) {
            if (split && col == 2) {
                check(QJsonDocument::fromJson(sidecar.readLine()).object().value("confirmedColumns").toInt(-1) == 2,
                      "capacity split seals the preceding pair");
                sidecar.close();
                sidecar.setFileName(QDir(initial.directory).filePath("Card2_ChA_wrap_001.dat.jsonl"));
                check(sidecar.open(QIODevice::ReadOnly), "split pair has evidence");
                sidecar.readLine();
            }
            const auto row = QJsonDocument::fromJson(sidecar.readLine()).object();
            check(!row.value("late").toBool(true) && row.value("logicalIndex").toInt(-1) == col &&
                  row.value("triggerSeq").toInt(-1) == (65534 + col) % 65536 &&
                  row.value("complete").toBool() == !((mode == TailCase::Partial && col == 1) ||
                      (mode == TailCase::PartialFinal && col == 2)),
                  "logical indices, wrap and source completeness stay truthful");
            if (mode == TailCase::PartialFinal && col == 2)
                check(row.value("receivedSlots").toInt() == 1 && row.value("expectedSlots").toInt() == 2,
                      "sealed final retains its original missing packet evidence");
        }
        check(QJsonDocument::fromJson(sidecar.readLine()).object().value("confirmedColumns").toInt(-1) == (split ? 1 : oldCount),
              "old slow-card seal follows admitted source frames");
        if (mode == TailCase::Complete) {
            check(contains(1, 65535, Outcome::Admitted) && contains(1, 0, Outcome::Admitted),
                  "slow penultimate and final admissions reported");
        } else if (mode == TailCase::Partial) {
            check(contains(1, 65535, Outcome::SourceIncomplete), "partial source distinguished from save rejection");
        } else if (mode == TailCase::PartialFinal) {
            check(contains(1, 0, Outcome::SourceIncomplete), "incomplete final has its own admitted receipt");
        } else if (mode == TailCase::SessionEnded) {
            check(contains(1, 0, Outcome::SessionEnded) && !contains(1, 0, Outcome::Expired),
                  "session stop does not report expiry");
        } else {
            check(mode == TailCase::Expired ? contains(1, 0, Outcome::Expired) :
                  contains(1, 0, Outcome::Admitted), "old final has a single admission or expiry");
            check(!contains(1, 0, Outcome::SourceMissing), "new-round source does not prove a missing old final");
            if (mode == TailCase::EarlyNext || adjacentTimeout)
                check(contains(1, 1, Outcome::UnverifiedRound), "early next assembly cannot qualify merely by ingress time");
        }
    }

    {
        using Outcome = HostOutput::SaveTailOutcome;
        QTemporaryDir root;
        AutoSaveRoundCoordinator coordinator;
        coordinator.configure(root.path(), 0);
        const auto initial = coordinator.beginSession(0);
        coordinator.setDirectoryPreparer([](const QString&) { return false; });
        FileSaver a(0), b(1);
        AcqConfig config;
        config.acqTimeNs = 64;
        DataProcessor pa(0, nullptr, nullptr, config), pb(1, nullptr, nullptr, config);
        std::atomic<bool> writerEntered{false}, releaseWriter{false};
        bool queueFull = false;
        HostOutput output(32, 50, {&pa, &pb}, {&a, &b}, nullptr, nullptr, 401,
            [&](const PhysicalRoundEvent& event) {
                if (event.kind == PhysicalRoundEvent::Kind::CountBoundary)
                    coordinator.commitBoundary(event.measurementSession, event.roundGeneration,
                                               AutoSaveBoundaryKind::Count, "count-full");
            }, 0.0, 0);
        output.setSaveBoundaryFaultProbe([&] { return coordinator.faulted(); });
        output.setMeasurementSessionBinder([&](auto session) {
            return coordinator.bindMeasurementSession(session).newSessionGen;
        });
        output.setSaveSessionResolver([&](auto session, auto round) {
            return coordinator.resolveRound(session, round).sessionGen;
        });
        output.setSaveTailSink([&](auto, auto, int card, Outcome outcome) {
            if (card == 1 && outcome == Outcome::QueueFull) queueFull = true;
        });
        output.beginSession(779);
        output.start();
        auto directory = coordinator.directoryResolver();
        const auto started = output.startSaving(initial.directory, 500, "full", [&](auto gen) {
            writerEntered = true;
            until([&] { return releaseWriter.load(); });
            return directory(gen);
        });
        check(until([&] { return output.savingApplied(started); }), "Count queue-full request ready");
        output.card(makeFrame(779, 0, 1, 1000000000, 1));
        check(until([&] { return writerEntered.load(); }), "save worker held at directory lookup");
        for (int trigger = 1; trigger <= 400; ++trigger)
            output.card(makeFrame(779, 1, std::uint16_t(trigger), 1000000000 + trigger, 1));
        output.card(makeFrame(779, 0, 401, 1001000000, 1));
        const auto stopped = output.stopSaving();
        output.card(makeFrame(779, 1, 401, 1002000000, 1));
        check(queueFull && output.stats().saveQueueFull == 1, "tail admission reports full queue separately");
        releaseWriter = true;
        check(until([&] { return output.savingApplied(stopped); }), "queue-full tail releases stop");
        output.stop();
        check(hasSize(dataFile(initial.directory, 1, "full"), 400 * 16 * 2),
              "already queued columns drain after queue-full rejection");
    }

    // T1: only the exact pre-cutoff in-flight object may join the open tail.
    {
        QTemporaryDir root;FileSaver s0(0),s1(1);AcqConfig config;config.acqTimeNs=64;
        DataProcessor p0(0,nullptr,nullptr,config),p1(1,nullptr,nullptr,config);
        HostOutput output(32,50,{&p0,&p1},{&s0,&s1},nullptr,nullptr,8000,{},1.0,0,true);
        output.beginSession(6001);output.start();const auto gen=output.startSaving(root.path(),100,"tail");
        check(until([&]{return output.savingApplied(gen);}),"T1 configured");
        output.card(makeFrame(6001,0,10,1000000000LL,1));
        output.card(makeFrame(6001,1,10,1000000000LL,1));
        check(until([&]{return s0.acceptedCount()==1 && s1.acceptedCount()==1;}),"T1 initial files open");
        output.assemblyStarted(0,6001,11,1900000000LL);
        check(output.pollPhysicalRoundTimeout(2000000000LL),"T1 boundary");
        auto wrong=std::const_pointer_cast<CardFrame>(makeFrame(6001,0,10,2010000000LL,3));wrong->firstIngressId=22;
        output.card(wrong);
        auto tail=std::const_pointer_cast<CardFrame>(makeFrame(6001,0,11,1900000000LL,2));tail->firstIngressId=11;tail->closed=2050000000LL;
        output.card(tail);
        check(until([&]{return s0.acceptedCount()>=3;}),"T1 exact tail written");
        const auto stopped=output.stopSaving();check(until([&]{return output.savingApplied(stopped);}),"T1 stopped");output.stop();
        check(hasSize(dataFile(root.path(),0,"tail"),64),"T1 old file contains both frames");
        check(hasSize(dataFile(root.path(),1,"tail"),32),"T1 other card sealed independently");
        check(hasSize(dataFile(QDir(root.path()).filePath("_late/session-6001-round-0"),0,"tail"),32),"T1 non-token isolated");
        check(!QFile::exists(QDir(root.path()).filePath("Card1_ChA_tail_001.dat")),"T1 no split official file");
    }
    // T2: boundary triggered by a new frame still preserves an earlier
    // assembly for one card while the triggering frame reaches the new dir.
    {
        QTemporaryDir root;AutoSaveRoundCoordinator coordinator;coordinator.configure(root.path(),0);const auto initial=coordinator.beginSession(0);
        FileSaver s0(0),s1(1);s0.setSessionDirResolver([&](auto g){return coordinator.directoryFor(g);});s1.setSessionDirResolver([&](auto g){return coordinator.directoryFor(g);});
        AcqConfig config;config.acqTimeNs=64;DataProcessor p0(0,nullptr,nullptr,config),p1(1,nullptr,nullptr,config);
        AutoSaveRoundCoordinator::CommitResult next;
        HostOutput output(32,50,{&p0,&p1},{&s0,&s1},nullptr,nullptr,8000,[&](const PhysicalRoundEvent& e){if(e.kind==PhysicalRoundEvent::Kind::TimeoutBoundary)next=coordinator.commitBoundary(e.measurementSession,e.roundGeneration,AutoSaveBoundaryKind::Timeout,"tail-test");},1.0,0,true);
        output.setMeasurementSessionBinder([&](auto session){return coordinator.bindMeasurementSession(session).newSessionGen;});
        output.setSaveSessionResolver([&](auto session,auto round){return coordinator.resolveRound(session,round).sessionGen;});
        output.beginSession(6002);output.start();const auto configured=output.startSaving(initial.directory,100,"auto-tail");
        check(until([&]{return output.savingApplied(configured);}),"T2 configured");
        output.card(makeFrame(6002,0,10,1000000000LL,1));output.card(makeFrame(6002,1,10,1000000000LL,1));
        check(until([&]{return s0.acceptedCount()==1&&s1.acceptedCount()==1;}),"T2 old files open");
        output.assemblyStarted(0,6002,100,1900000000LL);
        output.assemblyStarted(1,6002,200,2010000000LL);
        auto nextFrame=std::const_pointer_cast<CardFrame>(makeFrame(6002,1,20,2010000000LL,2));nextFrame->firstIngressId=200;
        output.card(nextFrame);
        auto tail=std::const_pointer_cast<CardFrame>(makeFrame(6002,0,11,1900000000LL,3));tail->firstIngressId=100;tail->closed=2050000000LL;
        output.card(tail);
        check(until([&]{return s0.acceptedCount()>=2&&s1.acceptedCount()>=2;}),"T2 frames saved");
        const auto stopped=output.stopSaving();check(until([&]{return output.savingApplied(stopped);}),"T2 stop");output.stop();
        check(next.committed,"T2 boundary committed");
        check(hasSize(dataFile(initial.directory,0,"auto-tail"),64),"T2 eligible tail joined old file");
        check(hasSize(dataFile(next.directory,1,"auto-tail"),32),"T2 post-cutoff frame in new directory");
    }
    // T3: deadline closes the old file even if the exact assembly never
    // finishes in time; its eventual completed frame remains quarantined.
    {
        QTemporaryDir root;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;DataProcessor processor(0,nullptr,nullptr,config);
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,8000,{},1.0,0,true);
        output.beginSession(6003);output.start();const auto gen=output.startSaving(root.path(),100,"expired");
        check(until([&]{return output.savingApplied(gen);}),"T3 configured");
        output.card(makeFrame(6003,0,10,1000000000LL,1));check(until([&]{return saver.acceptedCount()==1;}),"T3 file opened");
        output.assemblyStarted(0,6003,30,1900000000LL);
        check(output.pollPhysicalRoundTimeout(2000000000LL),"T3 boundary");
        // The global queue can still be busy while this card has drained.
        // Its own watermark must seal the tail without a new round boundary.
        const std::vector<Time> cardWatermarks{2100000001LL};
        check(!output.pollPhysicalRoundTimeout(0,&cardWatermarks),"T3 card watermark seals without a new boundary");
        auto late=std::const_pointer_cast<CardFrame>(makeFrame(6003,0,11,1900000000LL,2));late->firstIngressId=30;late->closed=2200000000LL;output.card(late);
        check(until([&]{return saver.acceptedCount()>=2;}),"T3 late isolated");
        const auto stopped=output.stopSaving();check(until([&]{return output.savingApplied(stopped);}),"T3 stopped");output.stop();
        check(hasSize(dataFile(root.path(),0,"expired"),32),"T3 old file not reopened");
        check(hasSize(dataFile(QDir(root.path()).filePath("_late/session-6003-round-0"),0,"expired"),32),"T3 late exists separately");
    }
    // T4: a cached trigger after the idle deadline but before polling does
    // not reopen or append to the official old-round file.
    {
        QTemporaryDir root;FileSaver saver(0);AcqConfig config;config.acqTimeNs=64;DataProcessor processor(0,nullptr,nullptr,config);
        HostOutput output(32,50,{&processor},{&saver},nullptr,nullptr,8000,{},1.0,0,true);
        output.beginSession(6004);output.start();const auto gen=output.startSaving(root.path(),100,"cache");
        check(until([&]{return output.savingApplied(gen);}),"T4 configured");
        output.card(makeFrame(6004,0,10,1000000000LL,1));check(until([&]{return saver.acceptedCount()==1;}),"T4 file opened");
        output.card(makeFrame(6004,0,10,2010000000LL,2));
        check(until([&]{return saver.acceptedCount()>=2;}),"T4 cache hit isolated");
        const auto stopped=output.stopSaving();check(until([&]{return output.savingApplied(stopped);}),"T4 stopped");output.stop();
        check(hasSize(dataFile(root.path(),0,"cache"),32),"T4 old file unchanged");
        check(hasSize(dataFile(QDir(root.path()).filePath("_late/session-6004-round-0"),0,"cache"),32),"T4 late exists");
    }
    if (g_failures == 0) {
        std::cout << "count_boundary_save_binding_test: C1-C10 ALL PASS\n";
        return 0;
    }
    std::cout << "count_boundary_save_binding_test: " << g_failures
              << " FAILURE(S)\n";
    return 1;
}
