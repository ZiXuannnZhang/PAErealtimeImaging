#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"
#include "PaimageAcquisition/HostOutput.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
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
                card, nullptr, displays.back().get(), nullptr, AcqConfig{}));
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

        check(until([&] { return savers[0]->savedCount() == 3 &&
                                  savers[1]->savedCount() == 3; }),
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
        check(until([&] { return savers[0]->savedCount() == 5 &&
                                  savers[1]->savedCount() == 5; }),
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
        DataProcessor processor(0, nullptr, &display, nullptr, config);
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
        check(until([&] { return saver.savedCount() == 2; }),
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
                card, nullptr, displays.back().get(), nullptr, config));
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
        check(until([&] { return savers[0]->savedCount() == 6 &&
                                  savers[1]->savedCount() == 6; }),
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
            701, 1, AutoSaveBoundaryKind::Count, QStringLiteral("test_backlog"));
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
        DataProcessor processor(0, nullptr, &display, nullptr, config);
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
        output.card(makeFrame(901, 0, 200, 20000, 0)); // timeout + control
        output.card(makeFrame(901, 0, 201, 20500, 2));
        output.card(makeFrame(901, 0, 202, 20800, 2));
        check(until([&] { return saver.savedCount() == 4; }),
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
        DataProcessor processor(0, nullptr, &display, nullptr, config);
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
        check(until([&] { return saver.savedCount() == 1; }),
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

    if (g_failures == 0) {
        std::cout << "count_boundary_save_binding_test: C1-C10 ALL PASS\n";
        return 0;
    }
    std::cout << "count_boundary_save_binding_test: " << g_failures
              << " FAILURE(S)\n";
    return 1;
}
