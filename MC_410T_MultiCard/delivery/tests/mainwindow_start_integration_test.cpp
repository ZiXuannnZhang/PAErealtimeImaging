#include <winsock2.h>
#include <ws2tcpip.h>
#include <QAction>
#include <QTextDocument>
#include <QTextEdit>
#include "MainWindow.h"
#include "NetworkController.h"
#include "ImagingController.h"
#include "ImagingDisplayWindow.h"
#include "ImagingBypass.h"
#include "DiagnosticRecorder.h"
#include "RingConfigDialog.h"
#include "RingBlockAssembler.h"
#include "RingTimevarLowpassSettings.h"
#include "RingFbpSettings.h"
#include "RoundPolicySettings.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include "PaimageAcquisition/SettingsPath.h"
#include "PaimageAcquisition/SourceCore.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSharedMemory>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QJsonDocument>
#include <QSpinBox>
#include <QThread>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QUuid>
#include <qfloat16.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <future>
#include <type_traits>
#include <stdexcept>

int runReceiverForTest(int argc, char** argv, const std::function<void(MainWindow&)>& configureWindow);
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class T> static T* widget(QObject& parent, const char* name) {
    auto* result = parent.findChild<T*>(name);
    check(result, name);
    return result;
}
template<class Predicate> static void waitFor(Predicate predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    check(predicate(), message);
}
static void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

class LoopbackCard {
public:
    LoopbackCard() {
        WSADATA winsock{};
        check(WSAStartup(MAKEWORD(2,2), &winsock) == 0, "winsock startup");
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        address_.sin_family = AF_INET;
        address_.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.2", &address_.sin_addr);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address_), sizeof(address_)) != 0) {
            closesocket(socket_);
            WSACleanup();
            check(false, "bind loopback card");
        }
        inet_pton(AF_INET, "127.0.0.1", &address_.sin_addr);
    }
    ~LoopbackCard() { closesocket(socket_); WSACleanup(); }
    LoopbackCard(const LoopbackCard&) = delete;
    LoopbackCard& operator=(const LoopbackCard&) = delete;

    void expectCommand(const paimage::Command& expected) const {
        waitFor([&] {
            u_long bytes = 0;
            return ioctlsocket(socket_, FIONREAD, &bytes) == 0 && bytes > 0;
        }, "card command present");
        paimage::Command actual{};
        check(recv(socket_, reinterpret_cast<char*>(actual.data()), actual.size(), 0) ==
              static_cast<int>(actual.size()) && actual == expected, "card control command");
    }

    void send(int port, const std::vector<unsigned char>& bytes) {
        address_.sin_port = htons(port);
        check(sendto(socket_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&address_), sizeof(address_)) ==
              static_cast<int>(bytes.size()), "loopback card packet");
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in address_{};
};

using ProcessHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

static void checkOrdinaryToken(HANDLE process) {
    HANDLE rawToken = nullptr;
    check(OpenProcessToken(process, TOKEN_QUERY, &rawToken), "open process token");
    const ProcessHandle token(rawToken, CloseHandle);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    check(GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
          !elevation.TokenIsElevated, "test requires an ordinary Windows token");
}

static void writeMarker(const QDir& directory, const QString& name, const QJsonObject& values = {}) {
    QSaveFile file(directory.filePath(name));
    const auto data = QJsonDocument(values).toJson(QJsonDocument::Compact);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit(),
          "persist fixture marker");
}

class MainWindowStartIntegrationTest {
public:
    template<class F>
    static auto onWriter(ring_png::Channel& writer, F action) {
        using Result = std::invoke_result_t<F, ring_png::Channel&>;
        if constexpr (std::is_void_v<Result>) {
            check(QMetaObject::invokeMethod(&writer, [&] { action(writer); }, Qt::BlockingQueuedConnection),
                  "invoke PNG writer control");
        } else {
            Result result{};
            check(QMetaObject::invokeMethod(&writer, [&] { result = action(writer); }, Qt::BlockingQueuedConnection),
                  "observe PNG writer control");
            return result;
        }
    }

    static bool timeoutPngBusy(MainWindow& window) {
        const std::lock_guard<std::mutex> lock(window.m_pngMutex);
        return window.m_timeoutPngBusy;
    }

    static bool finishPngsWhileGuiPaused(MainWindow& window, QSemaphore& renderGate, int releaseCount,
                                         const QDir& directory, const QStringList& patterns, int files) {
        // Waiting on this observer does not pump the GUI event loop.
        return std::async(std::launch::async, [&] {
            renderGate.release(releaseCount);
            QElapsedTimer timer;
            timer.start();
            do {
                bool settled = false;
                {
                    const std::lock_guard<std::mutex> lock(window.m_pngMutex);
                    settled = window.m_pngTasks.empty() && !window.m_timeoutPngBusy &&
                              window.m_terminalPngJobs == 0 && window.m_terminalPngBytes == 0;
                }
                if (settled && directory.entryList(patterns, QDir::Files).size() == files) return true;
                QThread::msleep(5);
            } while (timer.elapsed() < 5000);
            return false;
        }).get();
    }

    static void pngCompletesWhileGuiPaused() {
        QTemporaryDir output, logs;
        check(output.isValid() && logs.isValid(), "independent PNG fixture directories");
        DiagnosticRecorder::Options options;
        options.rootDirectory = logs.path();
        auto* recorder = DiagnosticRecorder::initialize(options);
        check(recorder != nullptr, "record PNG results");
        const auto recorderLifetime = std::shared_ptr<void>(nullptr, [](void*) { DiagnosticRecorder::shutdown(); });
        MainWindow window;
        const auto renderGate = std::make_shared<QSemaphore>();
        const auto releaseOnFailure = std::shared_ptr<void>(nullptr, [renderGate](void*) { renderGate->release(2); });
        const auto entered = std::make_shared<QSemaphore>();
        const auto render = [renderGate, entered](const std::atomic_bool*) {
            entered->release();
            renderGate->acquire();
            QImage image(ring_png::kEdge, ring_png::kEdge, QImage::Format_ARGB32);
            image.fill(Qt::gray);
            return ring_png::Pair{image, image};
        };
        window.queueTerminalPng({30, 0}, {output.path(), "paused-terminal", 301}, 1, 1, 32, render);
        window.queueTimeoutPng({30, 1}, {{output.path(), "paused-timeout", 302}, {32, "_frame2", render, 2}});
        check(entered->tryAcquire(2, 3000), "both accepted renderers reached the latch");
        {
            const std::lock_guard<std::mutex> lock(window.m_pngMutex);
            check(window.m_pngTasks.size() == 2 && window.m_timeoutPngBusy && window.m_terminalPngJobs == 1,
                  "two distinct PNG jobs held their budgets before pausing the GUI");
        }
        const auto guiProcessedEvent = std::make_shared<bool>(false);
        QTimer::singleShot(0, &window, [guiProcessedEvent] { *guiProcessedEvent = true; });
        const QDir dir(output.path());
        check(finishPngsWhileGuiPaused(window, *renderGate, 2, dir, {"paused-*.png"}, 4),
              "accepted terminal and Timeout PNGs must write and release admission without GUI events");
        check(!*guiProcessedEvent &&
              dir.entryList({"paused-terminal_*.png"}, QDir::Files).size() == 2 &&
              dir.entryList({"paused-timeout_*.png"}, QDir::Files).size() == 2, "both pairs delivered during the GUI pause");
        int terminal = 0, timeout = 0;
        waitFor([&] {
            check(recorder->flush(), "flush PNG receipts");
            QFile events(QDir(recorder->runDirectory()).filePath("events.jsonl"));
            check(events.open(QIODevice::ReadOnly), "read PNG receipts");
            terminal = timeout = 0;
            while (!events.atEnd()) {
                const auto fields = QJsonDocument::fromJson(events.readLine()).object().value("fields").toObject();
                if (fields.value("requestId") == QJsonValue(QStringLiteral("301"))) {
                    check(fields.value("result").toString() == "saved", "terminal receipt is saved");
                    ++terminal;
                }
                if (fields.value("requestId") == QJsonValue(QStringLiteral("302"))) {
                    check(fields.value("result").toString() == "saved", "Timeout receipt is saved");
                    ++timeout;
                }
            }
            return *guiProcessedEvent && terminal >= 1 && timeout >= 1;
        }, "resume GUI and observe final PNG receipts");
        check(terminal == 1 && timeout == 1, "one result per job after GUI resumes");
        std::cout << "PASS both PNG pairs and budgets completed during the GUI pause" << std::endl;
    }

    static void operatorLog() {
        check(QCoreApplication::applicationDirPath().endsWith("startup-isolated"), "isolated settings");
        QTemporaryDir output, logs;
        check(output.isValid() && logs.isValid(), "operator log fixture directories");
        DiagnosticRecorder::Options options;
        options.rootDirectory = logs.path();
        auto* recorder = DiagnosticRecorder::initialize(options);
        check(recorder != nullptr, "operator log recorder");
        const auto recorderLifetime = std::shared_ptr<void>(nullptr, [](void*) { DiagnosticRecorder::shutdown(); });
        MainWindow window;
        auto* log = widget<QTextEdit>(window, "txtLog");
        auto& controller = *window.m_imagingController;
        window.prepareRingMeasurement(51);
        window.m_imagingEnabled = true;
        window.m_imagingServiceReady.store(true);
        {
            QSignalBlocker blocker(window.m_chkRealtimeImaging);
            window.m_chkRealtimeImaging->setChecked(true);
        }
        controller.m_ringAdmission.store(true);
        controller.m_ringActiveEpoch = 73;
        controller.m_ringAcceptedCount = 9;
        controller.m_ringRejectedCount = 1;
        const auto initial = log->toPlainText();
        controller.pumpRingInput();
        const auto observe = [&](QString kind, quint64 generation, QJsonObject fields = {}) {
            fields.insert("cmd", "ring_shm_observation");
            fields.insert("kind", kind);
            if (!fields.contains("measurement_session")) fields.insert("measurement_session", "51");
            fields.insert("round_generation", QString::number(generation));
            controller.processMessage(fields);
        };
        observe("periodic", 1, {{"tag", "diagnostic_probe"}});
        observe("capacity_rejected", 1, {{"input_epoch", "73"}, {"source_terminal", false}, {"rejection_applied", true}});
        observe("capacity_rejected", 1, {{"input_epoch", "73"}, {"source_terminal", true}, {"rejection_applied", false}});
        observe("capacity_rejected", 1, {{"input_epoch", "72"}, {"source_terminal", true}, {"rejection_applied", true}});
        observe("round_block_count_mismatch", 1, {{"measurement_session", "50"}});
        check(log->toPlainText() == initial, "normal counters and stale or provisional observations stay off-screen");

        observe("round_block_count_mismatch", 1);
        observe("round_block_count_mismatch", 1);
        observe("terminal_delivery_failed", 2, {{"reason", "terminal_slots_full"}});
        const auto withoutPng = log->toPlainText();
        check(withoutPng.count(QStringLiteral("环形重建缺块")) == 1 &&
              withoutPng.count(QStringLiteral("成像服务终帧槽已满")) == 1,
              "missing reconstruction or terminal is visible once without PNG saving");

        window.m_terminalPngTargets.emplace(paimage::RoundIdentity{51, 3},
            MainWindow::PendingTerminalPng{{output.path(), "operator", 543}, 0});
        const QJsonObject applied{{"input_epoch", "73"}, {"source_terminal", true}, {"rejection_applied", true}};
        observe("capacity_rejected", 3, applied);
        observe("capacity_rejected", 3, applied);
        pump(10);
        check(log->toPlainText().count(QStringLiteral("末块被拒收")) == 1 &&
              log->toPlainText().contains(QStringLiteral("环形终帧未交付")) &&
              window.m_terminalPngTargets.empty(), "actual terminal rejection settles the pending PNG with one display notice");
        check(window.m_imagingEnabled && window.m_imagingServiceReady.load() &&
              window.m_chkRealtimeImaging->isChecked(), "operator notice does not stop real-time imaging");

        check(recorder->flush(), "flush operator log diagnostics");
        QFile events(QDir(recorder->runDirectory()).filePath("events.jsonl"));
        check(events.open(QIODevice::ReadOnly), "open operator log diagnostics");
        const auto recorded = events.readAll();
        check(recorded.contains("[RingInput] enqueued_total=9 dropped_total=1") &&
              recorded.contains("diagnostic_probe") &&
              recorded.contains("round_block_count_mismatch") && recorded.contains("terminal_input_rejected"),
              "retain the raw observations and structured PNG result in diagnostics");
        std::cout << "PASS diagnostic-only counters, missing terminal with or without PNG, and one warning per affected round\n";
    }

    static void previewPressure() {
        check(QCoreApplication::applicationDirPath().endsWith("startup-isolated"), "isolated settings");
        QFile::remove(paimageSettingsPath());
        pngCompletesWhileGuiPaused();
        MainWindow window;
        auto& controller = *window.m_imagingController;
        window.m_cmbImagingMode->setCurrentIndex(1);
        auto setup = [&](int nx) {
            ImagingController::RingLayout layout{nx, 32, 4, nx * nx, {}, 0};
            check(ring_snapshot::checkedLayout(32, 4, nx * nx, layout.bytes), "test preview layout");
            const auto name = (QStringLiteral("Local\\ring-preview-") + QUuid::createUuid().toString()).toStdWString();
            check(controller.setupRingSharedMemory(layout, name.c_str()), "test preview mapping");
            controller.m_svcResourcesActive = true;
            controller.m_ringWorkerStop = false;
            controller.m_ringAdmission = true;
            window.m_ringSnapshotAdmissionBlocked.store(false);
            window.m_imagingEnabled = true;
        };
        constexpr int nx = 1024;
        setup(nx);
        const auto publish = [&](int seq) {
            const auto& layout = controller.m_ringLayout;
            auto* header = static_cast<RingImagingShmHeader*>(controller.m_ringMapping.data());
            check(controller.m_ringOutputMutex.lock(100) == ring_input::Result::Ok, "publish preview under output lock");
            header->frame_ready = 1;
            header->frame_seq = seq;
            auto* pixels = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(header) + layout.bytes.frameOffset);
            std::fill_n(pixels, layout.frameSize, float(seq));
            std::fill_n(pixels + layout.frameSize, layout.frameSize, float(seq + 50));
            controller.m_ringOutputMutex.unlock();
            controller.startRingFrameWorker(seq, seq, true, {7, 0}, false);
        };
        const auto freeSlots = [&] {
            return int(controller.m_dispConsumed[0].load()) + int(controller.m_dispConsumed[1].load());
        };
        std::vector<quint64> previewSubmits;
        QObject::connect(&controller, &ImagingController::ringSnapshotReady, &window,
            [&](int, quint64 submit, bool, quint64, quint64, bool complete, int) {
                if (!complete) previewSubmits.push_back(submit);
            });
        auto blockPreview = [&] {
            const auto gate = std::make_shared<QSemaphore>();
            auto release = std::shared_ptr<void>(nullptr, [gate](void*) { gate->release(); });
            window.m_previewRenderPool.start([gate] { gate->acquire(); });
            return release;
        };
        auto blocked = blockPreview();
        publish(1);
        waitFor([&] { return !previewSubmits.empty(); }, "first preview leased before copy");
        for (int seq = 2; seq <= 24; ++seq) { publish(seq); pump(3); }
        check(previewSubmits == std::vector<quint64>{1} && freeSlots() == 1 &&
              window.m_lastPresentedRingSubmit == 0, "render pressure kept only one full-size preview");
        blocked.reset();
        // No GUI events run while the completed render waits for presentation.
        check(window.m_previewRenderPool.waitForDone(8000), "first preview rendered");
        publish(25);
        QThread::msleep(60);
        check(freeSlots() == 1 && window.m_lastPresentedRingSubmit == 0,
              "GUI stall kept the same preview lease");
        {
            const std::lock_guard<std::mutex> lock(controller.m_ringReqMutex);
            check(controller.m_ringReqPending && controller.m_ringReqSubmitIndex == 25,
                  "GUI stall kept only the latest metadata");
        }
        waitFor([&] { return window.m_lastPresentedRingSubmit == 25 && freeSlots() == 2; },
                "released slot pulls latest preview without another block");
        check(previewSubmits == std::vector<quint64>({1,25}) && freeSlots() == 2,
              "only first and last valid previews published");
        auto* preview = window.m_imagingDisplayWindow;
        const auto* widget1 = preview->findChild<RingImageWidget*>();
        check(widget1 && qRed(widget1->image().pixel(0,0)) == 12, "last valid preview supplied the displayed pixels");

        blocked = blockPreview();
        publish(26);
        waitFor([&] { return previewSubmits.size() == 3; }, "old layout render queued");
        publish(27);
        controller.stopSvc();
        setup(512);
        publish(28);
        blocked.reset();
        waitFor([&]{ return window.m_lastPresentedRingSubmit == 28; }, "old lease released after reconfiguration");
        check(previewSubmits == std::vector<quint64>({1,25,26,28}) &&
              preview->findChild<RingImageWidget*>()->image().size() == QSize(512, 512),
              "stopped layout yielded no additional snapshot and old work cannot paste");

        QTemporaryDir output;
        check(output.isValid(), "timeout test directory");
        auto pixelsFor = [](float value, int edge) {
            return std::make_shared<std::vector<float>>(2 * std::size_t(edge) * edge, value);
        };
        auto capture = [&](quint64 round, const auto& pixels, int edge) {
            QImage view(2, 2, QImage::Format_Grayscale8);
            view.fill(0);
            preview->applyRingFrame(pixels, edge, int(round), view, view);
            window.m_timeoutPresentation.publish(9, round, preview->capturePngRender(int(round)));
            const paimage::TimeoutPresentation::Target target{output.path(), "preview", 92};
            return window.m_timeoutPresentation.close(9, round+1, target, []{});
        };
        const paimage::TimeoutPresentation::Target budgetTarget{output.path(), "budget", 91};
        for (const int grid : {3600, 3770, 3771, 4000}) {
            const auto bytes = std::size_t(grid) * grid * 2 * sizeof(float);
            std::atomic_bool rendered{false};
            window.queueTimeoutPng({8, 0}, {budgetTarget, {bytes, "_probe", [&](const std::atomic_bool*) {
                rendered.store(true);
                return ring_png::Pair{}; // Only the admission path needs to run here.
            }, grid}});
            {
                const std::lock_guard<std::mutex> lock(window.m_pngMutex);
                check(window.m_timeoutPngBusy, "each validated grid enters the Timeout renderer");
            }
            waitFor([&] { return !timeoutPngBusy(window); }, "probe render failure releases the slot");
            check(rendered.load(), "admitted layout reached the renderer");
        }
        const auto noRender = [](const std::atomic_bool*) { return ring_png::Pair{}; };
        window.queueTimeoutPng({8, 1}, {budgetTarget, {8u * 3600 * 3600, "_wrong_size", noRender, 4000}});
        window.queueTimeoutPng({8, 2}, {budgetTarget, {sizeof(float), "_overflow", noRender,
                                                     (std::numeric_limits<int>::max)()}});
        check(!timeoutPngBusy(window), "invalid layouts never enter Timeout admission");

        const auto renderGate = std::make_shared<QSemaphore>();
        auto releaseRender = std::shared_ptr<void>(nullptr, [renderGate](void*) { renderGate->release(); });
        onWriter(*window.m_timeoutPngWriter, [](auto& channel) { channel.setTestMode(ring_png::Channel::TestMode::Block); });

        constexpr int largeNx = 4000;
        auto first = pixelsFor(100, largeNx);
        std::fill(first->begin() + std::size_t(largeNx) * largeNx, first->end(), 200.f);
        preview->setGridGeometry(0.01, 40.0);
        preview->setDisplayMask(true, 1.0);
        std::weak_ptr<std::vector<float>> firstLifetime = first;
        auto job = capture(0, first, largeNx);
        check(job && job.capture.frameBytes == 8u * largeNx * largeNx, "paired capture pixel byte count");
        const auto renderEntered = std::make_shared<std::atomic_bool>(false);
        job.capture.render = [render = std::move(job.capture.render), renderGate, renderEntered]
                (const std::atomic_bool* cancel) {
            renderEntered->store(true);
            renderGate->acquire();
            return render(cancel);
        };
        window.queueTimeoutPng({9, 0}, std::move(job));
        first.reset();
        waitFor([&] { return renderEntered->load(); }, "old Timeout render is in flight");
        controller.stopSvc();
        setup(768);
        for (quint64 round = 1; round <= 6; ++round) {
            auto next = pixelsFor(150, nx);
            std::weak_ptr<std::vector<float>> lifetime = next;
            window.queueTimeoutPng({9, round}, capture(round, next, nx));
            // Replace the live display; an unadmitted screenshot must not pin it.
            QImage image(2, 2, QImage::Format_Grayscale8);
            image.fill(0);
            preview->applyRingFrame(std::make_shared<std::vector<float>>(8, 0.f), 2, 0, image, image);
            next.reset();
            check(lifetime.expired(), "each rejected Timeout releases its frame");
        }
        {
            const std::lock_guard<std::mutex> lock(window.m_pngMutex);
            check(window.m_timeoutPngBusy && !firstLifetime.expired(), "exactly one queued Timeout frame retained");
        }
        releaseRender.reset();
        waitFor([&] { return onWriter(*window.m_timeoutPngWriter, [](auto& channel) { return channel.testBlocked(); }); }, "Timeout render entered blocked PNG writer");
        check(firstLifetime.expired(), "Timeout disk wait only retained rendered pixels");
        const paimage::TimeoutPresentation::Target terminal{output.path(), "terminal", 93};
        window.queueTerminalPng({9, 7}, terminal, 1, 1, 64, [](const std::atomic_bool*) {
            QImage image(ring_png::kEdge, ring_png::kEdge, QImage::Format_ARGB32);
            image.fill(Qt::white);
            return std::array<QImage, 2>{image, image};
        });
        QDir dir(output.path());
        waitFor([&]{ return dir.entryList({"terminal_*.png"}, QDir::Files).size() == 2; },
                "formal terminal pair completes while Timeout disk is blocked");
        onWriter(*window.m_timeoutPngWriter, [](auto& channel) { channel.releaseTestGate(); });
        waitFor([&] { return dir.entryList({"preview_*.png"}, QDir::Files).size() == 2; },
                "first Timeout pair written and excess work rejected");
        const auto files = dir.entryList({"preview_*_532nm.png"}, QDir::Files);
        const auto files2 = dir.entryList({"preview_*_1064nm.png"}, QDir::Files);
        const QImage blue(dir.filePath(files.value(0))), infrared(dir.filePath(files2.value(0)));
        check(files.size() == 1 && files2.size() == 1 && qRed(blue.pixel(0,0)) == 51 &&
              qRed(infrared.pixel(0,0)) == 102 && qRed(blue.pixel(800,800)) == 0,
              "4000-square Timeout retained both original wavelength ranges and mask after layout change");
        waitFor([&] { return !timeoutPngBusy(window); }, "Timeout budget released after receipt");
        controller.stopSvc();
        std::cout << "PASS preview render and GUI stalls, restart, and bounded Timeout PNG with independent final delivery\n";
    }

    static void pngShutdownChild(MainWindow& window, const QString& directory, const QString& scenario) {
        using Mode = ring_png::Channel::TestMode;
        const bool timeout = scenario.startsWith("timeout-");
        const bool deadline = scenario.endsWith("-deadline");
        const bool graceful = scenario.endsWith("-grace") || deadline;
        const bool blocked = scenario.endsWith("-blocked") || graceful;
        const bool rendering = scenario.endsWith("-rendering");
        const bool renderFailure = scenario.endsWith("-render-failed");
        const bool realExecutable = scenario.endsWith("-production");
        const auto renderEntered = std::make_shared<std::atomic_bool>(false);
        auto& writer = *(timeout ? window.m_timeoutPngWriter : window.m_terminalPngWriter);
        if (blocked) onWriter(writer, [](auto& channel) { channel.setTestMode(Mode::Block); });
        if (scenario.endsWith("-partial")) onWriter(writer, [](auto& channel) { channel.setTestMode(Mode::ExitAfterFirst); });
        if (scenario.endsWith("-startup")) onWriter(writer, [](auto& channel) { channel.setTestMode(Mode::FailStart); });
        if (realExecutable) {
            const auto executable = qEnvironmentVariable("PAIMAGE_TEST_PNG_EXECUTABLE");
            const auto target = executable.isEmpty() ? QDir(QCoreApplication::applicationDirPath())
                .absoluteFilePath("../bin/PAimageReceiverDiagnostics.exe") : executable;
            onWriter(writer, [&](auto& channel) { channel.setTestExecutable(target); });
        }
        const paimage::TimeoutPresentation::Target target{directory, scenario, 782};
        if (timeout) {
            ImagingDisplayWindow preview;
            QImage image(2, 2, QImage::Format_ARGB32);
            image.fill(Qt::white);
            preview.applyRingFrame(std::make_shared<std::vector<float>>(8, 50.f), 2, 1, image, image);
            window.queueTimeoutPng({9, 0}, {target, preview.capturePngRender(1)});
        } else {
            window.queueTerminalPng({9, 0}, target, 31, 1, 64,
                                    [rendering, renderFailure, renderEntered](const std::atomic_bool* cancel) {
                renderEntered->store(true);
                if (rendering) while (!cancel->load()) QThread::msleep(1);
                if (rendering || renderFailure) return ring_png::Pair{};
                QImage image(ring_png::kEdge, ring_png::kEdge, QImage::Format_ARGB32);
                image.fill(Qt::white);
                return ring_png::Pair{image, image};
            });
        }
        std::shared_ptr<LoopbackCard> card;
        QString rawDirectory;
        const bool observeService = blocked && !graceful;
        constexpr int durationNs = 6000;
        if (observeService) {
            window.m_bitsPerChannel = 32;
            window.m_sampleIntervalNs = 4;
            window.m_cmbImagingMode->setCurrentIndex(1);
            widget<QLineEdit>(window, "edtDataTime")->setText(QString::number(durationNs));
            window.m_chkRealtimeImaging->setChecked(true);
            waitFor([&] { return window.m_imagingServiceReady.load(); }, "Ring service ready before close");
        }
        if (observeService && timeout) {
            card = std::make_shared<LoopbackCard>();
            window.m_localBindIP = "127.0.0.1";
            widget<QLineEdit>(window, "edtSaveDir")->setText(QDir(directory).filePath("raw"));
            widget<QLineEdit>(window, "edtTriggersPerFile")->setText("100");
            widget<QLineEdit>(window, "edtFileSuffix")->setText("closing");
            widget<QCheckBox>(window, "chkAutoSave")->setChecked(true);
            window.startListeningWithIPs({"127.0.0.2"}, QStringLiteral("explicit_target_ips"));
            check(window.m_netController != nullptr, "loopback listener started");
            auto& network = *window.m_netController;
            network.setStartupFilterTriggerCount(0);
            waitFor([&] { return network.isSaving(); }, "raw writer armed before close");
            rawDirectory = QDir(window.m_reconSaveDir).absoluteFilePath("..");
            check(network.sendConfigCommand(durationNs, 1000, 1000) && network.sendStartMeasure(),
                  "queue loopback configuration and acquisition");
            card->expectCommand(paimage::configCommand(durationNs, 1000, 1000));
            card->send(8000, std::vector<unsigned char>(60));
            card->expectCommand(paimage::startCommand());
            for (int offset = 0; offset < durationNs * 2; offset += 1440) {
                const auto size = std::min(1440, durationNs * 2 - offset);
                std::vector<unsigned char> bytes(4 + size);
                bytes[0] = offset / 1440;
                card->send(8001, bytes);
            }
            waitFor([&] { return window.m_isMeasuring &&
                network.physicalRoundSnapshot().logicalDistinctAccepted == 1; }, "one complete raw column entered saving");
        }
        auto* ready = new QTimer(&window);
        QObject::connect(ready, &QTimer::timeout, &window,
            [&window, &writer, ready, directory = QDir(directory), blocked, graceful, deadline,
             rendering, realExecutable, renderEntered, card, rawDirectory, announced = false]() mutable {
            const auto* svc = window.m_imagingController->m_svcProcess;
            if (blocked && !onWriter(writer, [](auto& channel) { return channel.testBlocked(); })) return;
            if (rendering && !renderEntered->load()) return;
            if (realExecutable) {
                const std::lock_guard<std::mutex> lock(window.m_pngMutex);
                if (!window.m_pngTasks.empty()) return;
            }
            if (blocked && !graceful && (!svc || svc->state() != QProcess::Running)) return;
            if (!announced) {
                announced = true;
                const auto* recorder = DiagnosticRecorder::instance();
                writeMarker(directory, "closing.json", {
                    {"runDirectory", recorder ? recorder->runDirectory() : QString()},
                    {"writerPid", double(onWriter(writer, [](auto& channel) { return channel.testProcessId(); }))}, {"svcPid", svc ? double(svc->processId()) : 0.0},
                    {"rawDirectory", rawDirectory}
                });
            }
            if (!directory.exists("observe.ready")) return;
            ready->stop();
            if (deadline) onWriter(writer, [](auto& channel) { channel.deferTestReceiptNotification(); });
            window.close();
            const int repeatAfterMs = blocked && (!graceful || deadline) ? 2000 : 100;
            QTimer::singleShot(repeatAfterMs, Qt::PreciseTimer, &window, [&window] { window.close(); });
            if (deadline) {
                QTimer::singleShot(ring_png::kShutdownGraceMs - 800, Qt::PreciseTimer, &window,
                                   [&writer] { onWriter(writer, [](auto& channel) { channel.releaseTestGate(); }); });
                QTimer::singleShot(ring_png::kShutdownGraceMs - 100, Qt::PreciseTimer, &window,
                                   [&writer, directory] {
                    if (onWriter(writer, [](auto& channel) { return channel.testReceiptReady(); })) writeMarker(directory, "reply.ready");
                });
            } else if (graceful) {
                QTimer::singleShot(150, &window, [&writer] { onWriter(writer, [](auto& channel) { channel.releaseTestGate(); }); });
            }
        });
        ready->start(10);
    }

    static void pngShutdown(bool productionOnly = false) {
        checkOrdinaryToken(GetCurrentProcess());
        const std::array<std::pair<QString, QString>, 9> scenarios{{
            {"timeout-blocked", "shutdown_unconfirmed"}, {"terminal-blocked", "shutdown_unconfirmed"},
            {"timeout-grace", "saved"}, {"terminal-deadline", "saved"}, {"terminal-production", "saved"},
            {"terminal-partial", "writer_exited_unconfirmed"}, {"terminal-startup", "writer_start_failed"},
            {"terminal-rendering", "application_closed"}, {"terminal-render-failed", "render_failed"}
        }};
        for (const auto& [scenario, expected] : scenarios) {
            if (productionOnly && scenario != "terminal-production") continue;
            std::cout << "Checking PNG shutdown " << scenario.toStdString() << std::endl;
            QTemporaryDir output;
            check(output.isValid(), "shutdown fixture directory");
            QFile::remove(paimageSettingsPath());
            {
                QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
                settings.setValue("RingConfigDialog/Defaults/block", 4);
                settings.setValue("RingConfigDialog/Defaults/totalAlines", 8);
                settings.setValue("RingConfigDialog/Defaults/fovMm", 4.0);
                settings.setValue("RingConfigDialog/Defaults/gridMm", 0.1);
                for (int ch = 0; ch < 8; ++ch)
                    settings.setValue(QString("RingConfigDialog/Defaults/ch%1").arg(ch), ch == 0);
            }
            QProcess host;
            host.setProgram(QCoreApplication::applicationFilePath());
            host.setArguments({"--png-shutdown-child", output.path(), scenario});
            host.start();
            check(host.waitForStarted(5000), "start receiver shutdown child");
            const auto closing = output.filePath("closing.json");
            waitFor([&] { return QFile::exists(closing) || host.state() == QProcess::NotRunning; },
                    "receiver entered close");
            QFile marker(closing);
            check(marker.open(QIODevice::ReadOnly), "receiver wrote closing marker");
            const auto info = QJsonDocument::fromJson(marker.readAll()).object();
            const auto observe = [&](qint64 pid) {
                check(pid > 0, "observed process has a real PID");
                ProcessHandle handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                                  DWORD(pid)), CloseHandle);
                if (!handle) {
                    const auto error = GetLastError();
                    throw std::runtime_error("cannot observe live process, Win32 error " + std::to_string(error));
                }
                check(WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT, "process was alive before close");
                checkOrdinaryToken(handle.get());
                return handle;
            };
            const bool blocked = scenario.endsWith("-blocked");
            const bool deadline = scenario.endsWith("-deadline");
            const auto hostHandle = observe(host.processId());
            ProcessHandle writer(nullptr, CloseHandle), svc(nullptr, CloseHandle);
            if (blocked || deadline || scenario.endsWith("-grace")) {
                writer = observe(info.value("writerPid").toInteger());
                const QDir dir(output.path());
                const auto temps = dir.entryList({"*.png.*"}, QDir::Files);
                check(temps.size() == 1 && QFileInfo(dir.filePath(temps.front())).size() > 8,
                      "writer is paused after bytes reached the first temporary PNG");
            }
            if (blocked) svc = observe(info.value("svcPid").toInteger());
            QElapsedTimer elapsed;
            elapsed.start();
            writeMarker(QDir(output.path()), "observe.ready");
            check(host.state() == QProcess::NotRunning || host.waitForFinished(7500),
                  "receiver main and Qt shutdown finished within the PNG grace plus cleanup allowance");
            check(host.exitStatus() == QProcess::NormalExit && host.exitCode() == 0 &&
                  WaitForSingleObject(hostHandle.get(), 0) == WAIT_OBJECT_0, "receiver exited normally");
            const auto hostMs = elapsed.elapsed();
            for (const auto* handle : {&writer, &svc})
                if (*handle) check(WaitForSingleObject(handle->get(), 1000) == WAIT_OBJECT_0,
                                   "observed child actually exited");
            if (blocked || deadline) {
                check(hostMs >= ring_png::kShutdownGraceMs - 100 && hostMs < ring_png::kShutdownGraceMs + 1800,
                      "production grace expires once without an early exit or renewal on repeat close");
                if (blocked) {
                    DWORD code = 0;
                    check(GetExitCodeProcess(writer.get(), &code) && code == 1,
                          "writer stopped by the deadline while its gate was unreleased");
                }
            }
            if (deadline) check(QFile::exists(output.filePath("reply.ready")),
                                "reply was signaled before the production timer handled it");
            if (scenario == "timeout-blocked") {
                const QDir raw(info.value("rawDirectory").toString());
                const auto pair = raw.entryList({"Card1_*.dat"}, QDir::Files);
                check(pair.size() == 2 && QFileInfo(raw.filePath(pair[0])).size() == 3000 &&
                      QFileInfo(raw.filePath(pair[1])).size() == 3000, "close drains one A/B waveform column");
                check(raw.entryList({"*.dat.jsonl"}, QDir::Files).isEmpty(),
                      "closing with auto-save active does not create a raw sidecar");
            }

            QFile events(QDir(info.value("runDirectory").toString()).filePath("events.jsonl"));
            check(events.open(QIODevice::ReadOnly), "closed receiver flushed diagnostic events");
            int matched = 0;
            int rawConfirmations = 0;
            while (!events.atEnd()) {
                const auto event = QJsonDocument::fromJson(events.readLine()).object();
                const auto fields = event.value("fields").toObject();
                if (event.value("message") == QStringLiteral("save_request_settled") &&
                    fields.value("confirmed") == QStringLiteral("1") &&
                    fields.value("failed") == QStringLiteral("0") &&
                    fields.value("discarded") == QStringLiteral("0"))
                    ++rawConfirmations;
                if (fields.value("requestId").toString() != "782") continue;
                ++matched;
                check(fields.value("result").toString() == expected, "PNG has expected terminal outcome");
                check(QDir(fields.value("directory").toString()) == QDir(output.path()), "PNG keeps original directory");
                if (expected != "application_closed") {
                    check(fields.value("path532nm").toString().endsWith("_532nm.png") &&
                          fields.value("path1064nm").toString().endsWith("_1064nm.png"), "receipt records both planned paths");
                }
            }
            check(matched == 1, "one PNG settlement despite competing callbacks and repeat close");
            if (scenario == "timeout-blocked")
                check(rawConfirmations == 1, "close confirms the A/B pair in the save receipt");
            const auto pngs = QDir(output.path()).entryList({"*.png"}, QDir::Files);
            check(pngs.size() == (expected == "saved" ? 2 : expected == "writer_exited_unconfirmed" ? 1 : 0),
                  "published files match the receipt");
            std::cout << "PASS " << scenario.toStdString() << " [" << expected.toStdString()
                      << ", close " << hostMs << " ms]" << std::endl;
        }
        QFile::remove(paimageSettingsPath());
    }

    static void listenerRestartPng() {
        check(QCoreApplication::applicationDirPath().endsWith("startup-isolated"), "isolated settings");
        QFile::remove(paimageSettingsPath());
        {
            QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
            settings.setValue("RingConfigDialog/Defaults/block", 4);
            settings.setValue("RingConfigDialog/Defaults/totalAlines", 8);
            settings.setValue("RingConfigDialog/Defaults/fovMm", 4.0);
            settings.setValue("RingConfigDialog/Defaults/gridMm", 0.1);
            RoundPolicySettings::save(settings, {0, false});
            for (int ch = 0; ch < 8; ++ch)
                settings.setValue(QString("RingConfigDialog/Defaults/ch%1").arg(ch), ch == 0);
        }
        QTemporaryDir save(QDir::currentPath()+"/restart-save-XXXXXX");
        check(save.isValid(), "temporary save root");
        LoopbackCard card;
        {
            MainWindow window;
            auto& controller = *window.m_imagingController;
            window.m_localBindIP = "127.0.0.1";
            window.m_bitsPerChannel = 32;
            window.m_sampleIntervalNs = 4;
            window.m_cmbImagingMode->setCurrentIndex(1);
            constexpr int durationNs = 6000;
            widget<QLineEdit>(window, "edtDataTime")->setText(QString::number(durationNs));
            window.m_chkRealtimeImaging->setChecked(true);
            waitFor([&]{ return window.m_imagingServiceReady.load(); }, "Ring service ready");
            const auto cfg = controller.ringConfig();
            const int triggers = cfg.alinesPerFrame / cfg.enabledChannelCount;
            check(triggers > 0 && cfg.sampDepth == durationNs/4, "one-round fixture geometry");
            widget<QLineEdit>(window, "edtSaveDir")->setText(save.filePath("next"));
            widget<QLineEdit>(window, "edtTriggersPerFile")->setText("100");
            widget<QLineEdit>(window, "edtFileSuffix")->setText("restart");
            widget<QCheckBox>(window, "chkAutoSave")->setChecked(true);

            {
                QTemporaryDir queueTarget;
                onWriter(*window.m_terminalPngWriter, [](auto& channel) { channel.setTestMode(ring_png::Channel::TestMode::Block); });
                const paimage::TimeoutPresentation::Target target{queueTarget.path(), "bounded", 91};
                const auto render = [](const std::atomic_bool*) {
                    QImage image(ring_png::kEdge, ring_png::kEdge, QImage::Format_ARGB32);
                    image.fill(Qt::white);
                    return std::array<QImage, 2>{image, image};
                };
                for (quint64 round = 0; round < 3; ++round)
                    window.queueTerminalPng({999, round}, target, round + 1, int(round), 64, render);
                {
                    const std::lock_guard<std::mutex> lock(window.m_pngMutex);
                    check(window.m_terminalPngJobs == 2, "slow PNG writer bounds the number of pending jobs");
                }
                waitFor([&] { return onWriter(*window.m_terminalPngWriter, [](auto& channel) { return channel.testBlocked(); }); }, "admitted PNG reached child gate");
                onWriter(*window.m_terminalPngWriter, [](auto& channel) { channel.releaseTestGate(); });
                waitFor([&] { return QDir(queueTarget.path()).entryList({"*.png"}, QDir::Files).size() == 4; },
                    "admitted PNG pairs finish after the writer resumes");
            }

            std::vector<paimage::RoundIdentity> completed;
            const std::array<int, 5> samplePoints{0, 400, 800, 1200, 1599};
            std::array<std::vector<int>, 2> expectedGray;
            QObject::connect(&controller, &ImagingController::ringSnapshotReady, &window,
                [&](int, quint64, bool, quint64 session, quint64 generation, bool complete, int buffer) {
                    if (!complete) return;
                    completed.push_back({session,generation});
                    const auto frame = controller.terminalSnapshotBuffer(buffer);
                    const int nx = controller.ringDisplayNx();
                    if (!window.m_imagingDisplayWindow || !frame || nx <= 0 || nx >= 1600) return;
                    const auto ranges = std::array{window.m_imagingDisplayWindow->range1(),
                                                   window.m_imagingDisplayWindow->range2()};
                    for (int wl = 0; wl < 2; ++wl) {
                        expectedGray[wl].clear();
                        for (int y : samplePoints) for (int x : samplePoints) {
                            const float value = (*frame)[std::size_t(wl)*nx*nx + (x*nx/1600)*nx + y*nx/1600];
                            const auto range = ranges[wl];
                            const double scaled = std::isfinite(value)
                                ? (value-range.lower)/(range.upper-range.lower) : 0.0;
                            expectedGray[wl].push_back(int(qBound(0.0, scaled, 1.0)*255+0.5));
                        }
                    }
                });
            QTemporaryDir timeoutWhileSaving;
            onWriter(*window.m_timeoutPngWriter, [](auto& channel) { channel.setTestMode(ring_png::Channel::TestMode::Block); });
            {
                ImagingDisplayWindow preview;
                QImage view(2, 2, QImage::Format_ARGB32);
                view.fill(Qt::black);
                preview.applyRingFrame(std::make_shared<std::vector<float>>(8, 1.f), 2, 1, view, view);
                window.queueTimeoutPng({997, 0},
                    {{timeoutWhileSaving.path(), "blocked", 97}, preview.capturePngRender(1)});
            }
            waitFor([&] { return onWriter(*window.m_timeoutPngWriter, [](auto& channel) { return channel.testBlocked(); }); }, "Timeout writer blocked during acquisition");
            QString previousDirectory;
            for (int pass = 0; pass < 2; ++pass) {
                window.startListeningWithIPs({"127.0.0.2"}, QStringLiteral("explicit_target_ips"));
                check(window.m_isListening && window.m_netController, "start listener on the same window");
                auto& network = *window.m_netController;
                network.setStartupFilterTriggerCount(0);
                waitFor([&]{ return network.isSaving(); }, "auto-save armed");
                QDir pngDir(window.m_reconSaveDir);
                check(pngDir.absolutePath() != previousDirectory, "new listener has a new save directory");
                previousDirectory = pngDir.absolutePath();
                // A stale display request and the old UI save flag must not own a Count target.
                window.m_reconSaveEnabled = false;
                window.syncTimeoutPngSettings();
                window.m_lastPresentedRingSubmit = pass == 0 ? (std::numeric_limits<quint64>::max)() : 0;
                check(network.sendConfigCommand(durationNs,1000,1000) && network.sendStartMeasure(), "queue CONFIG and START");
                card.expectCommand(paimage::configCommand(durationNs,1000,1000));
                card.send(8000,std::vector<unsigned char>(60));
                waitFor([&]{ return window.m_isMeasuring; }, "card START accepted");
                card.expectCommand(paimage::startCommand());

                const auto previewGate = std::make_shared<QSemaphore>();
                auto releasePreview = std::shared_ptr<void>(nullptr, [previewGate](void*) { previewGate->release(); });
                window.m_previewRenderPool.start([previewGate] { previewGate->acquire(); });
                if (pass) window.m_imagingServiceReady.store(false);
                const int requestedRounds = pass ? 2 : 1;
                for (int trigger = 0; trigger < triggers * requestedRounds; ++trigger) {
                    for (int offset = 0; offset < durationNs*2; offset += 1440) {
                        const int size = std::min(1440,durationNs*2-offset);
                        std::vector<unsigned char> bytes(size+4);
                        bytes[0] = offset/1440;
                        bytes[2] = trigger;
                        for (int i = 0; i < size; i += 8) {
                            const int sample = (offset+i)/8;
                            const std::int32_t a = (((sample/19)+trigger*3)%31-15)*(pass+1)*30;
                            const std::int32_t b = -a;
                            std::memcpy(bytes.data()+4+i,&b,sizeof(b));
                            std::memcpy(bytes.data()+8+i,&a,sizeof(a));
                        }
                        card.send(8001,bytes);
                    }
                    pump(8);
                    if (pass && trigger == triggers - 1) {
                        const QString skippedDirectory = pngDir.absolutePath();
                        waitFor([&] { return window.m_reconSaveDir != skippedDirectory; },
                            "Count boundary proceeds while imaging is unready");
                        check(completed.size() == 1 && pngDir.entryList({"*.png"}, QDir::Files).isEmpty(),
                              "unready imaging skips the first physical round");
                        pngDir = QDir(window.m_reconSaveDir);
                        window.m_imagingServiceReady.store(true);
                    }
                }
                waitFor([&]{ return completed.size() == std::size_t(pass+1); }, "first complete round after listening");
                check(completed.back().roundGeneration == quint64(pass), "first eligible round generation");
                if (pass) check(completed[0] < completed[1], "round identity advances across listeners");
                const std::array<const char*, 2> wavelengths{"532","1064"};
                for (int wl = 0; wl < 2; ++wl) {
                    const QString filter = QStringLiteral("*_session%1_round%2_frame*_%3nm.png")
                        .arg(completed.back().measurementSession).arg(completed.back().roundGeneration)
                        .arg(QString::fromLatin1(wavelengths[wl]));
                    QImage png;
                    waitFor([&]{
                        const auto files = pngDir.entryList({filter},QDir::Files);
                        if (files.size() != 1) return false;
                        png = QImage(pngDir.filePath(files.front()));
                        return png.size() == QSize(1600,1600);
                    }, "decode saved wavelength PNG in the round directory");
                    check(expectedGray[wl].size() == samplePoints.size()*samplePoints.size(),
                          "observe terminal pixels for this wavelength");
                    std::size_t pixel = 0;
                    for (int y : samplePoints) for (int x : samplePoints)
                        check(qRed(png.pixel(x,y)) == expectedGray[wl][pixel++], "PNG belongs to terminal wavelength pixels");
                }
                check(pngDir.entryList({"*.png"},QDir::Files).size() == 2, "exactly one PNG pair per round");
                if (!pass) {
                    const auto state = window.m_roundUi.snapshot();
                    check(state.frameCount == 0 && state.frameEndEvents == 0 &&
                          !window.m_lblImagingStatus->text().contains(QStringLiteral("软件覆盖完整")),
                          "a stale terminal cannot change the current display state");
                }
                window.onStartListenClicked();
                card.expectCommand(paimage::stopCommand());
                waitFor([&]{ return !window.m_isListening && !window.m_netController; }, "listener stopped");
                check(controller.isRunning(), "same Ring process stays resident between listeners");
                QDir roundDir(pngDir.absolutePath());
                check(roundDir.cdUp(), "resolve raw data directory");
                for (const auto* channel : {"A","B"}) {
                    const auto files = roundDir.entryList({QStringLiteral("Card1_Ch%1_*.dat")
                        .arg(QString::fromLatin1(channel))}, QDir::Files);
                    check(files.size() == 1 &&
                          QFileInfo(roundDir.filePath(files.front())).size() == qint64(triggers)*cfg.sampDepth*2,
                          "raw file trigger count in the same round directory");
                }
                releasePreview.reset();
            }

            check(onWriter(*window.m_timeoutPngWriter, [](auto& channel) { return channel.testBlocked(); }) &&
                  QDir(timeoutWhileSaving.path()).entryList({"*.png"}, QDir::Files).isEmpty(),
                  "terminal delivery, preview and raw saving advanced with Timeout writer still blocked");
            onWriter(*window.m_timeoutPngWriter, [](auto& channel) { channel.releaseTestGate(); });
            waitFor([&] { return !timeoutPngBusy(window); }, "original Timeout pair settled after gate release");
            check(QDir(timeoutWhileSaving.path()).entryList({"*.png"}, QDir::Files).size() == 2,
                  "Timeout pair kept its original target through listener changes");

            auto& dialog = *window.m_ringConfigDialog;
            auto* calibrationInput = widget<QLineEdit>(dialog, "ringFbpCalibrationFile");
            widget<QComboBox>(dialog, "ringReconAlgorithm")->setCurrentIndex(1);
            const auto calibrationFile = save.filePath("fbp-calibration.json");
            RingFbpSettings fileOffsets;
            fileOffsets.angleOffset[0] = 1.5;
            fileOffsets.delayOffset[0][1] = .25;
            auto calibration = ring_fbp_settings::toJson(fileOffsets);
            calibration.insert("formatVersion", 1);
            calibration.insert("geometry", "concentric-ring");
            calibration.insert("daqHz", 250e6);
            writeMarker(QDir(save.path()), "fbp-calibration.json", calibration);

            QStringList fallback, configurationErrors;
            QObject::connect(&controller, &ImagingController::svcStatus, &window,
                [&](const QString& message, float) {
                    if (message.contains(calibrationFile)) fallback.push_back(message);
                });
            QObject::connect(&controller, &ImagingController::svcError, &window,
                [&](const QString& message) { configurationErrors.push_back(message); });
            const auto stage = [&](const QString& requestedPath) {
                dialog.setAcquisitionParams(4, durationNs);
                dialog.setMeasurementConfigFrozen(true);
                calibrationInput->setText(requestedPath);
                QString error;
                check(dialog.applyConfig(&error), "stage the requested FBP settings");
                dialog.setMeasurementConfigFrozen(false);
            };
            const auto start = [&](int acquisitionNs) {
                widget<QLineEdit>(window, "edtDataTime")->setText(QString::number(acquisitionNs));
                window.startListeningWithIPs({"127.0.0.2"}, QStringLiteral("explicit_target_ips"));
                if (!window.m_isListening || !window.m_netController || !window.m_chkRealtimeImaging->isChecked()) {
                    std::cerr << "Listener setup at " << acquisitionNs << " ns, running=" << window.m_isListening
                              << ", imaging checked=" << window.m_chkRealtimeImaging->isChecked() << '\n'
                              << widget<QTextEdit>(window, "txtLog")->toPlainText().right(6500).toStdString() << '\n';
                    throw std::runtime_error("loopback listener starts with an FBP request");
                }
                window.m_netController->setStartupFilterTriggerCount(0);
                if (!window.m_autoSaveEnabled && !window.m_netController->isSaving())
                    window.onToggleSaveClicked();
                waitFor([&] { return window.m_netController->isSaving(); }, "raw writer armed");
                const QDir pngDirectory(window.m_reconSaveDir);
                const auto delayA = widget<QLineEdit>(window, "edtADelay")->text().toInt();
                const auto delayB = widget<QLineEdit>(window, "edtBDelay")->text().toInt();
                window.onStartMeasureClicked();
                if (acquisitionNs == durationNs)
                    check(window.m_chkRealtimeImaging->isChecked() && !window.m_btnImagingConfig->isEnabled(),
                          "measurement freeze must not unlock the imaging parameters during service restart");
                card.expectCommand(paimage::configCommand(acquisitionNs, delayA, delayB));
                card.send(8000, std::vector<unsigned char>(60));
                waitFor([&] { return window.m_isMeasuring; }, "measurement really started");
                card.expectCommand(paimage::startCommand());
                return pngDirectory;
            };
            const auto waveform = [](int sample, int trigger, int fixture) {
                return ((sample / 19 + trigger * 3) % 31 - 15) * 30 * fixture;
            };
            const auto deliver = [&](int acquisitionNs, int columns, int fixture) {
                for (int trigger = 0; trigger < columns; ++trigger) {
                    for (int offset = 0; offset < acquisitionNs * 2; offset += 1440) {
                        const auto size = std::min(1440, acquisitionNs * 2 - offset);
                        std::vector<unsigned char> bytes(size + 4);
                        bytes[0] = offset / 1440;
                        bytes[2] = trigger;
                        for (int i = 0; i < size; i += 8) {
                            const int sample = (offset + i) / 8;
                            const std::int32_t a = waveform(sample, trigger, fixture);
                            const std::int32_t b = -a;
                            std::memcpy(bytes.data() + 4 + i, &b, sizeof(b));
                            std::memcpy(bytes.data() + 8 + i, &a, sizeof(a));
                        }
                        card.send(8001, bytes);
                    }
                    pump(8);
                }
            };
            const auto stopAndCheckRaw = [&](QDir directory, int acquisitionNs, int columns, int fixture,
                                             paimage::RoundIdentity expectedRound = {}) {
                if (expectedRound.valid()) {
                    const std::lock_guard<std::mutex> lock(window.m_ringAssemblerMutex);
                    check(expectedRound.measurementSession == window.m_ringAdmissionSession,
                          "the formal PNG belongs to the measurement writing this raw directory");
                }
                window.onStartListenClicked();
                card.expectCommand(paimage::stopCommand());
                waitFor([&] { return !window.m_netController; }, "real listener and raw writer stopped");
                check(directory.cdUp(), "resolve raw output directory");
                const int samples = acquisitionNs / 4;
                static_assert(sizeof(qfloat16) == 2);
                for (int channel : {1, -1}) {
                    const auto pattern = channel == 1 ? "Card1_ChA_*.dat" : "Card1_ChB_*.dat";
                    const auto files = directory.entryList({QString::fromLatin1(pattern)}, QDir::Files);
                    check(files.size() == 1, "one file per raw channel");
                    QFile data(directory.filePath(files.front()));
                    check(data.open(QIODevice::ReadOnly), "read confirmed waveform");
                    const auto bytes = data.readAll();
                    check(bytes.size() == qint64(columns) * samples * 2, "raw A/B sizes match capture");
                    for (int trigger = 0; trigger < columns; ++trigger) {
                        for (int sample = 0; sample < samples; ++sample) {
                            qfloat16 value;
                            std::memcpy(&value, bytes.constData() + 2 * (trigger * samples + sample), 2);
                            check(float(value) == channel * waveform(sample, trigger, fixture),
                                  "raw A/B contents match the packet fixture and column order");
                        }
                    }
                }
                check(directory.entryList({"*.dat.jsonl"}, QDir::Files).isEmpty(),
                      "no raw sidecar is created by the production save path");
            };

            // Empty, valid and then deleted calibration on real START entries.
            for (int pass = 0; pass < 3; ++pass) {
                const auto requestedPath = pass ? calibrationFile : QString();
                stage(requestedPath);
                if (pass == 2) {
                    check(QFile::remove(calibrationFile) && controller.m_fbpSettings.angleOffset[0] == 1.5,
                          "resident service keeps the loaded snapshot until applying again");
                    const auto buttons = dialog.findChildren<QPushButton*>();
                    const auto saveDefault = std::find_if(buttons.cbegin(), buttons.cend(), [](const auto* button) {
                        return button->text() == QStringLiteral("设为默认");
                    });
                    check(saveDefault != buttons.cend(), "find the save defaults button");
                    (*saveDefault)->click();
                    const QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
                    check(settings.value("RingConfigDialog/Defaults/exactFbpCalibration").toString() == calibrationFile,
                          "defaults preserve the operator's unavailable calibration path");
                }
                const int starts = controller.m_testSvcStarts;
                const auto oldRounds = completed.size();
                const auto pngDirectory = start(durationNs);
                check(window.m_measurementRingImagingAvailable.load(), "optional calibration leaves FBP available");
                waitFor([&] { return controller.m_testSvcStarts > starts && window.m_imagingServiceReady.load(); },
                        "reconfigured FBP service becomes ready before the next round");
                check(window.m_chkRealtimeImaging->isChecked() && !window.m_btnImagingConfig->isEnabled(),
                      "imaging configuration stays locked after service readiness");
                const auto& effective = controller.m_fbpSettings;
                check(effective.enabled && effective.angleOffset[0] == (pass == 1 ? 1.5 : 0.) &&
                      effective.delayOffset[0][1] == (pass == 1 ? .25 : 0.) && effective.artifact.isEmpty(),
                      "the effective FBP snapshot uses only offsets from the accepted configuration");
                check(calibrationInput->text() == requestedPath &&
                      fallback.size() == (pass == 2 ? 1 : 0) && configurationErrors.isEmpty(),
                      "requested path stays visible and optional fallback is reported once");
                const auto renderGate = std::make_shared<QSemaphore>();
                auto releaseOnFailure = std::shared_ptr<void>(nullptr, [renderGate](void*) { renderGate->release(2); });
                if (pass == 2) {
                    // Hold both terminal render threads until the actual Count
                    // terminal snapshot has reached MainWindow admission.
                    for (int i = 0; i < 2; ++i)
                        window.m_terminalRenderPool.start([renderGate] { renderGate->acquire(); });
                }
                controller.m_testLastRingAlgorithm.clear();
                deliver(durationNs, triggers, pass + 1);
                try {
                    waitFor([&] { return completed.size() == oldRounds + 1 &&
                                          controller.m_testLastRingAlgorithm == "exact_fbp"; },
                            "actual service returned an exact_fbp terminal for the new round");
                } catch (...) {
                    std::cerr << "FBP fixture pass " << pass << ", completed " << completed.size() - oldRounds
                              << ", algorithm " << controller.m_testLastRingAlgorithm.toStdString() << '\n'
                              << widget<QTextEdit>(window, "txtLog")->toPlainText().right(6500).toStdString() << '\n';
                    throw;
                }
                if (pass == 2) {
                    {
                        const std::lock_guard<std::mutex> lock(window.m_pngMutex);
                        check(window.m_terminalPngJobs == 1, "real fallback terminal was accepted before pausing GUI");
                    }
                    check(finishPngsWhileGuiPaused(window, *renderGate, 2, pngDirectory, {"*.png"}, 2),
                          "real fallback FBP terminal writes and settles while GUI is paused");
                } else {
                    waitFor([&] { return pngDirectory.entryList({"*.png"}, QDir::Files).size() == 2; },
                            "exact FBP delivered a terminal PNG pair");
                }
                const auto round = completed.back();
                for (const auto* wavelength : {"532", "1064"}) {
                    const auto pattern = QStringLiteral("*_session%1_round%2_frame*_%3nm.png")
                        .arg(round.measurementSession).arg(round.roundGeneration).arg(QString::fromLatin1(wavelength));
                    const auto png = pngDirectory.entryList({pattern}, QDir::Files);
                    check(png.size() == 1 && QImage(pngDirectory.filePath(png.front())).size() ==
                          QSize(ring_png::kEdge, ring_png::kEdge), "one decodable terminal PNG per wavelength in this FBP round");
                }
                stopAndCheckRaw(pngDirectory, durationNs, triggers, pass + 1, round);
                check(window.m_chkRealtimeImaging->isChecked() && !window.m_btnImagingConfig->isEnabled(),
                      "stopping acquisition alone must not unlock resident real-time imaging");
                constexpr const char* scenarios[]{"empty", "valid with offsets", "removed before START"};
                std::cout << "PASS " << scenarios[pass] << ": exact_fbp, raw A/B without sidecars and PNG pair\n";
            }

            // Repeat with manual saving and an invalid base FBP window.
            widget<QCheckBox>(window, "chkAutoSave")->setChecked(false);
            widget<QLineEdit>(window, "edtSaveDir")->setText(save.filePath("manual"));
            stage(calibrationFile);
            const auto oldRounds = completed.size();
            // 400 acquired samples permit raw saving and DelayCut, but
            // the retained FBP window is too short for either wavelength.
            constexpr int shortAcquisition = 1600;
            const auto rawOnly = start(shortAcquisition);
            check(rawOnly == QDir(save.filePath("manual/recon_png")) && !window.m_autoSaveEnabled,
                  "the final capture uses the selected manual save directory");
            check(!window.m_measurementRingImagingAvailable.load() && fallback.size() == 1 &&
                  configurationErrors.size() == 1 &&
                  configurationErrors.front().contains("FBP effective samples"),
                  "invalid base is reported and does not claim fallback success");
            deliver(shortAcquisition, 1, 4);
            waitFor([&] { return window.m_netController->physicalRoundSnapshot().logicalDistinctAccepted == 1; },
                    "raw acquisition continues on invalid base FBP");
            stopAndCheckRaw(rawOnly, shortAcquisition, 1, 4);
            check(completed.size() == oldRounds, "invalid base never produced a formal FBP terminal");
            const auto operatorLog = widget<QTextEdit>(window, "txtLog")->toPlainText();
            check(!operatorLog.contains("[RingInput]") && !operatorLog.contains("[RingSHMObs]") &&
                  !operatorLog.contains(QStringLiteral("自动保存会话目录")),
                  "real listener, save and reconstruction counters stay out of the operator log");
        }
        QFile::remove(paimageSettingsPath());
        std::cout << "PASS listener restart, exact FBP with optional calibration and raw/PNG delivery during GUI pause\n";
    }

    static void run() {
        check(QCoreApplication::applicationDirPath().endsWith("startup-isolated"),
              "test must use isolated INI directory");
        QFile::remove(paimageSettingsPath());
        const QString motorFile = QCoreApplication::applicationDirPath() + "/motor-test.json";
        const QString selectedMotorFile = QCoreApplication::applicationDirPath() + "/motor-selected.json";
        QFile::remove(motorFile);
        {
            QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
            settings.setValue("RingConfigDialog/Defaults/block", 4);
            settings.setValue("RingConfigDialog/Defaults/totalAlines", 8);
            settings.setValue("RingConfigDialog/Defaults/fovMm", 4.0);
            settings.setValue("RingConfigDialog/Defaults/gridMm", 0.1);
            settings.setValue("RingConfigDialog/Defaults/sectorStart", 25.0);
            settings.setValue("MotorScanLinkage/ParamsFile", motorFile);
            // Legacy settings must not restore the retired step-count linkage.
            settings.setValue("MotorScanLinkage/TotalAlinesRule", "perEnabledChannel");
            for (int channel = 0; channel < 8; ++channel)
                settings.setValue(QString("RingConfigDialog/Defaults/ch%1").arg(channel), channel == 0);
            settings.sync();
        }
        {
            MainWindow window;
            auto *logView=widget<QTextEdit>(window,"txtLog");
            auto *exportLog=widget<QPushButton>(window,"btnExportDiagnostic");
            auto *diagnosticMode=widget<QCheckBox>(window,"chkDiagnosticMode");
            check(logView->minimumHeight()==112&&logView->maximumHeight()==112
                &&logView->document()->maximumBlockCount()==500,"log view must stay readable and bounded");
            check(exportLog->text()==QStringLiteral("导出日志")&&exportLog->maximumWidth()<=104
                &&diagnosticMode->text()==QStringLiteral("诊断模式")&&!diagnosticMode->isChecked(),
                "daily diagnostics controls changed");
            check(!window.findChild<QAction*>("prepareSystemCaptureAction"),"retired capture action is still present");

            auto& controller = *window.m_imagingController;
            window.m_cmbImagingMode->setCurrentIndex(1);
            window.m_motorScanMonitor->tick();
            const auto earlyStatus = window.m_motorScanMonitor->lastStatus();
            window.ensureRingConfigDialog();
            auto& dialog = *window.m_ringConfigDialog;
            check(dialog.config().sectorStartDeg == 205.0, "manual sector start was not preserved");
            check(widget<QLabel>(dialog, "motorScanLinkageStatus")->text() == earlyStatus,
                  "lazy dialog did not recover the monitor status");
            auto* depth = widget<QLineEdit>(window, "edtDataTime");
            auto* checkbox = window.m_chkRealtimeImaging;
            int toggles = 0;
            QObject::connect(checkbox, &QCheckBox::toggled, &window, [&](bool) { ++toggles; });
            widget<QCheckBox>(dialog, "timevarEnabled")->setChecked(true);
            depth->setText("20000"); // N=5000, p2=2000: valid enabled configuration.
            auto controlsAvailable = [&] {
                return checkbox->isEnabled() && window.m_btnImagingConfig->isEnabled()
                    && depth->isEnabled() && widget<QLineEdit>(window,"edtADelay")->isEnabled()
                    && widget<QLineEdit>(window,"edtBDelay")->isEnabled();
            };
            auto start = [&] {
                // Actual UI signal -> production MainWindow::onRealtimeImagingToggled.
                checkbox->setChecked(true);
                check(!window.m_btnImagingConfig->isEnabled(), "imaging parameters lock on the initial check");
                try {
                    waitFor([&] { return controller.isRunning() && window.m_imagingEnabled
                        && window.m_imagingServiceReady.load() && checkbox->isEnabled(); }, "valid startup did not become ready");
                } catch (...) {
                    std::cerr << logView->toPlainText().toStdString() << '\n';
                    throw;
                }
                check(checkbox->isChecked() && !depth->isEnabled() && !window.m_btnImagingConfig->isEnabled(),
                      "running UI not locked");
            };
            auto stop = [&] {
                checkbox->setChecked(false);
                if (controller.hasActiveService()) {
                    window.freezeMeasurementConfig(false);
                    check(!window.m_btnImagingConfig->isEnabled(), "imaging parameters unlock only after service stops");
                }
                waitFor([&] { return !controller.isRunning() && !window.m_imagingEnabled
                    && controlsAvailable() && !controller.m_ringMapping.data(); }, "normal stop failed");
                check(!checkbox->isChecked(), "stop left checkbox checked");
            };
            check(controlsAvailable(), "imaging parameters are editable before enabling imaging");
            window.freezeMeasurementConfig(true);
            check(window.m_btnImagingConfig->isEnabled() && !depth->isEnabled(),
                  "raw-only measurement may stage Ring settings for the next measurement");
            window.freezeMeasurementConfig(false);
            start();
            check(controller.ringTimevarConfig().enabled && controller.ringConfig().sampDepth == 5000,
                  "enabled configuration not accepted through MainWindow");
            stop();
            std::cout << "PASS valid enabled startup -> stop\n";

            auto reject = [&](bool resident, bool fbp=false) {
                const auto acceptedFbp=ring_fbp_settings::toJson(controller.m_fbpSettings);
                const auto accepted = ring_timevar::toJson(controller.ringTimevarConfig());
                const QByteArray oldConfig(reinterpret_cast<const char*>(&controller.ringConfig()),
                                           sizeof(RingReconCudaConfig));
                const QByteArray oldDelays(reinterpret_cast<const char*>(controller.m_ringSysDelayCh),
                                           sizeof(controller.m_ringSysDelayCh));
                auto* const shm = controller.m_ringMapping.data();
                auto* const process = controller.m_svcProcess;
                const auto pid = process ? process->processId() : 0;
                const auto running = controller.m_running.load();
                const int configs = window.m_testAssemblerConfigs, feeds = window.m_testFeedStarts;
                const int starts = controller.m_testSvcStarts, stops = controller.m_testSvcStops;
                const int oldToggles = toggles;
                const bool configured = window.m_ringAssemblerConfigured;
                const bool ready = window.m_imagingServiceReady.load();
                const bool restart = window.m_restartRingOnSvcStop;
                const bool imaging = window.m_imagingEnabled;
                const auto assembler = window.m_ringAssembler->snapshot();
                const auto status = window.m_lblImagingStatus->text();
                int messages = 0;
                QTimer dismiss;
                QObject::connect(&dismiss, &QTimer::timeout, [&] {
                    for (auto* top : QApplication::topLevelWidgets())
                        if (auto* box = qobject_cast<QMessageBox*>(top)) { ++messages; box->accept(); }
                });
                dismiss.start(10);
                checkbox->setChecked(true);
                dismiss.stop();
                check(messages == 1, "validation warning missing");
                // Also let the old startup timeout elapse: rejection must install no delayed work.
                pump(5200);
                check(toggles == oldToggles + 1, "rollback emitted an extra toggled signal");
                check(!checkbox->isChecked() && controlsAvailable(), "rejected UI cannot retry");
                check(window.m_testAssemblerConfigs == configs && window.m_testFeedStarts == feeds,
                      "rejection entered assembler/feed startup");
                check(controller.m_testSvcStarts == starts && controller.m_testSvcStops == stops,
                      "rejection entered service start/stop");
                check(controller.m_ringMapping.data() == shm && controller.m_svcProcess == process
                    && (!process || process->processId() == pid) && controller.m_running.load() == running,
                      "rejection changed process/shared-memory lifecycle");
                check(controller.isRunning() == resident, "resident service state changed");
                check(QByteArray(reinterpret_cast<const char*>(&controller.ringConfig()),
                                sizeof(RingReconCudaConfig)) == oldConfig
                    && QByteArray(reinterpret_cast<const char*>(controller.m_ringSysDelayCh),
                                  sizeof(controller.m_ringSysDelayCh)) == oldDelays
                    && ring_timevar::toJson(controller.ringTimevarConfig()) == accepted
                    && ring_fbp_settings::toJson(controller.m_fbpSettings) == acceptedFbp,
                      "rejection changed accepted configuration");
                check(fbp ? dialog.fbpSettings().enabled && dialog.fbpSettings().densities[0]==0
                      : dialog.timevarConfig().enabled && dialog.timevarConfig().p2 == 2000,
                      "invalid draft silently disabled or clamped");
                const auto after = window.m_ringAssembler->snapshot();
                check(after.hasActiveRound == assembler.hasActiveRound
                    && after.activeRound == assembler.activeRound && after.blockTriggers == assembler.blockTriggers,
                      "rejection reset assembler state");
                check(window.m_ringAssemblerConfigured == configured, "rejection changed assembler configured state");
                check(window.m_imagingServiceReady.load() == ready, "rejection changed service ready state");
                check(window.m_restartRingOnSvcStop == restart && window.m_imagingEnabled == imaging,
                      "rejection changed restart/imaging state");
                if(window.m_lblImagingStatus->text()!=status)
                    std::cerr<<"status before="<<status.toStdString()<<" after="<<window.m_lblImagingStatus->text().toStdString()
                             <<" resident="<<resident<<" fbp="<<fbp<<'\n';
                check(window.m_lblImagingStatus->text() == status, "rejection changed status text");
                check(!window.m_imagingTimer->isActive(), "rejection started display timer");
            };

            // Non-empty assembler sentinel detects a reset as well as reconfiguration.
            std::vector<float> line(5000, 1.0f);
            window.m_ringAssembler->pushChannelLine(0, 0, {17, 3}, line.data(), int(line.size()));
            check(window.m_ringAssembler->snapshot().blockTriggers == 1, "assembler sentinel missing");
            depth->setText("6000"); // N=1500, existing p2=2000 must be rejected.
            reject(false);
            std::cout << "PASS shorter depth rejected; no assembler/feed/service/SHM startup; UI/config retained\n";
            widget<QSpinBox>(dialog, "timevarP1")->setValue(500);
            widget<QSpinBox>(dialog, "timevarP2")->setValue(1400);
            start();
            check(controller.ringConfig().sampDepth == 1500 && controller.ringTimevarConfig().p2 == 1400,
                  "corrected parameters not accepted");
            stop();
            std::cout << "PASS corrected parameters -> normal startup/stop\n";

            depth->setText("20000");
            widget<QSpinBox>(dialog, "timevarP1")->setValue(780);
            widget<QSpinBox>(dialog, "timevarP2")->setValue(2000);
            start();
            // Prepare the supported resident-service case with the real process still alive.
            // Only the UI/feed request is inactive; do not invoke stopSvc or replace the controller.
            { QSignalBlocker blocker(checkbox); checkbox->setChecked(false); }
            window.stopRingFeedWorker();
            window.m_imagingEnabled = false;
            window.setImagingParamControlsEnabled(true);
            depth->setText("6000");
            reject(true);
            std::cout << "PASS resident real ImagingSvc PID/SHM/running/ready state unchanged\n";
            controller.stopSvc();
            waitFor([&] { return !controller.isRunning() && !controller.m_ringMapping.data(); },
                    "resident cleanup failed");
            depth->setText("20000");
            start();
            stop();
            std::cout << "PASS valid restart/start/stop regression\n";
            widget<QComboBox>(dialog,"ringReconAlgorithm")->setCurrentIndex(1);
            widget<QLineEdit>(dialog,"fbpDensities")->setText("0");
            reject(false,true);
            widget<QLineEdit>(dialog,"fbpDensities")->setText("1000,1100,1100,1100,1100,1100,1100,1100");
            start();
            check(controller.m_fbpSettings.enabled && controller.m_fbpSettings.densities[0]==1000,
                  "FBP snapshot missing from real startup");
            {QSignalBlocker blocker(checkbox);checkbox->setChecked(false);}
            window.stopRingFeedWorker();window.m_imagingEnabled=false;window.setImagingParamControlsEnabled(true);
            widget<QComboBox>(dialog,"ringReconAlgorithm")->setCurrentIndex(1);
            widget<QLineEdit>(dialog,"fbpDensities")->setText("0");
            reject(true,true);
            controller.stopSvc();waitFor([&]{return !controller.isRunning()&&!controller.m_ringMapping.data();},"FBP resident cleanup");
            widget<QLineEdit>(dialog,"fbpDensities")->setText("1000,1100,1100,1100,1100,1100,1100,1100");
            std::uint64_t motorTestSession = 0;
            for(bool tv:{false,true})for(bool fbp:{false,true}) {
                widget<QCheckBox>(dialog,"timevarEnabled")->setChecked(tv);
                widget<QComboBox>(dialog,"ringReconAlgorithm")->setCurrentIndex(fbp?1:0);
                check(dialog.applyConfig(), "joint configuration rejected");
                const auto acceptedTv = ring_timevar::toJson(controller.ringTimevarConfig());
                const auto acceptedFbp = ring_fbp_settings::toJson(controller.m_fbpSettings);
                auto writeMotor = [&](const char *direction, const QJsonValue &steps = 2000) {
                    QSaveFile file(motorFile);
                    check(file.open(QIODevice::WriteOnly), "open motor file");
                    QJsonObject body{{"formatVersion",1},{"xStepsNum",steps},
                                     {"xStepsPerPeriod",QJsonObject{}},{"speed",2500}};
                    if (direction) body["direction"] = QString::fromLatin1(direction);
                    file.write(QJsonDocument(body).toJson());
                    check(file.commit(), "atomic motor file commit");
                };
                writeMotor("CCW"); window.m_motorScanMonitor->tick();
                start();check(controller.ringTimevarConfig().enabled==tv&&controller.m_fbpSettings.enabled==fbp,
                              "four-combination startup snapshot");
                paimage::PhysicalRoundNormalizer normalizer(8, [&](const paimage::PhysicalRoundEvent& event) {
                    window.m_motorScanMonitor->onPhysicalRoundEvent(event);
                    if (event.kind == paimage::PhysicalRoundEvent::Kind::RoundStarted)
                        window.bindRingRoundDirection({event.measurementSession, event.roundGeneration});
                });
                const auto session = ++motorTestSession;
                normalizer.setStartupFilterTriggerCount(0); normalizer.beginSession(session);
                const auto first = normalizer.classify(session, 100);
                const int startsBeforeMotor = controller.m_testSvcStarts;
                const int stopsBeforeMotor = controller.m_testSvcStops;
                const auto pid = controller.m_svcProcess->processId();
                const auto* shm = controller.m_ringMapping.data();
                const auto feedStarts = window.m_testFeedStarts;
                const auto assemblerConfigs = window.m_testAssemblerConfigs;
                const auto totalAlines = controller.ringConfig().alinesPerFrame;
                auto feedBlock = [&](const paimage::PhysicalRoundClassification &round) {
                    for (int j = 0; j < controller.ringConfig().alinesPerChannelPerBlock; ++j) {
                        auto group = std::make_shared<TriggerGroup>();
                        group->cardId = 0;
                        group->triggerSeq = uint16_t(round.triggerSeq + j);
                        group->measurementSession = round.measurementSession;
                        group->roundGeneration = round.roundGeneration;
                        group->logicalTriggerIndex = j;
                        group->normalizationApplied = group->isComplete = true;
                        group->physicalDecision = paimage::PhysicalTriggerDecision::LogicalScan;
                        group->sampleCount = controller.ringConfig().sampDepth;
                        group->freqA.assign(group->sampleCount, 0.f);
                        group->freqB.assign(group->sampleCount, 0.f);
                        group->freqA[1000] = 100.f + j;
                        check(window.ringFeedSink(group) == ImagingSubmitResult::Accepted,
                              "direction switch gated the imaging FIFO");
                    }
                };
                writeMotor("CW", QStringLiteral("unused")); window.m_motorScanMonitor->tick();
                check(controller.ringConfig().sectorCcw == 0 && controller.ringConfig().sectorStartDeg == 180
                    && controller.m_testSvcStarts == startsBeforeMotor,
                      "zero-filter active round changed direction/service");
                normalizer.timeoutBoundary(session); window.m_motorScanMonitor->tick();
                check(controller.ringConfig().sectorCcw == 1 && controller.ringConfig().sectorStartDeg == 180
                    && ring_timevar::toJson(controller.ringTimevarConfig()) == acceptedTv
                    && ring_fbp_settings::toJson(controller.m_fbpSettings) == acceptedFbp,
                      "live motor switch moved the nine-o'clock start or lost a filter snapshot");
                check(window.m_lblImagingStatus->text().contains("等待下一轮"),
                      "direction request not reflected in the status label");
                feedBlock(first); // Late frontend work must retain its old direction.
                waitFor([&]{ return window.m_roundUi.frameCount() == 1; }, "old round did not finish in resident service");
                check(window.m_lblImagingStatus->text().contains("等待下一轮"),
                      "late old-direction snapshot cleared pending switch");
                const auto second = normalizer.classify(session, 101);
                waitFor([&]{return window.m_lblImagingStatus->text().contains("等待首帧");},
                        "first trigger did not advance direction status");
                feedBlock(second);
                waitFor([&]{return window.m_lblImagingStatus->text().contains("输出");},
                        "new-direction snapshot did not restore normal status");
                check(controller.isRunning() && window.m_imagingServiceReady.load()
                    && controller.m_svcProcess->processId() == pid && controller.m_ringMapping.data() == shm
                    && controller.m_testSvcStarts == startsBeforeMotor && controller.m_testSvcStops == stopsBeforeMotor
                    && window.m_testFeedStarts == feedStarts && window.m_testAssemblerConfigs == assemblerConfigs,
                      "direction switch restarted or reconfigured the resident imaging chain");
                normalizer.timeoutBoundary(session);
                writeMotor("CW", 4000); window.m_motorScanMonitor->tick();
                writeMotor(nullptr, 6000); window.m_motorScanMonitor->tick();
                check(controller.ringConfig().alinesPerFrame == totalAlines
                    && controller.ringConfig().sectorCcw == 1
                    && controller.m_testSvcStarts == startsBeforeMotor && controller.m_testSvcStops == stopsBeforeMotor
                    && controller.m_ringMapping.data() == shm && window.m_pendingRingDirectionRevision == 0,
                      "unused motor fields changed imaging configuration");
                // A service stopping inside another active round must not reopen the motor gate.
                normalizer.classify(session, 102);
                stop();
                check(!window.m_motorScanMonitor->applyGateOpen(), "service stop reopened an active physical round");
                normalizer.timeoutBoundary(session);
            }
            std::cout<<"PASS FBP real rejection, four resident DAS/FBP + STFT combinations, direction status and service-stop latch\n";
            QString motorError;
            const auto oldDirection = controller.ringConfig().sectorCcw;
            const auto oldStart = controller.ringConfig().sectorStartDeg;
            widget<QComboBox>(dialog,"ringReconAlgorithm")->setCurrentIndex(1);
            widget<QLineEdit>(dialog,"fbpDensities")->setText("0");
            check(!dialog.applyMotorScanDirection(true, &motorError) && !motorError.isEmpty(),
                  "invalid filter draft did not reject motor update without a modal dialog");
            check(controller.ringConfig().sectorCcw == oldDirection && dialog.config().sectorCcw == oldDirection
                && controller.ringConfig().sectorStartDeg == oldStart && dialog.config().sectorStartDeg == oldStart,
                  "rejected motor update left hidden direction/anchor changes");
            dialog.reject();
            dialog.show();
            check(!dialog.applyMotorScanDirection(true, &motorError)
                && motorError.contains(QStringLiteral("正在编辑")), "motor update consumed a visible filter draft");
            dialog.reject();

            // Exercise the real browser without bypassing the physical-round gate.
            { QSaveFile file(selectedMotorFile); check(file.open(QIODevice::WriteOnly), "selected motor file open");
              file.write("{\"formatVersion\":1,\"direction\":\"CCW\"}"); check(file.commit(), "selected motor file commit"); }
            paimage::PhysicalRoundNormalizer selectionRound(8, [&](const paimage::PhysicalRoundEvent& event) {
                window.m_motorScanMonitor->onPhysicalRoundEvent(event);
            });
            selectionRound.setStartupFilterTriggerCount(0); selectionRound.beginSession(77);
            selectionRound.classify(77, 10);
            auto* fileButton = widget<QPushButton>(dialog, "motorScanParamsFileButton");
            check(fileButton->text() == QStringLiteral("参数文件选择"), "file picker button label");
            dialog.show();
            bool pickerOpened = false;
            QTimer::singleShot(0, [&] {
                for (auto* top : QApplication::topLevelWidgets())
                    if (auto* picker = qobject_cast<QFileDialog*>(top)) {
                        pickerOpened = true; picker->selectFile(selectedMotorFile);
                        QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
                    }
            });
            fileButton->click();
            check(pickerOpened && window.m_motorScanMonitor->paramsFile() == selectedMotorFile,
                  "browse selection did not switch the live monitor");
            check(QSettings(paimageSettingsPath(), QSettings::IniFormat)
                .value("MotorScanLinkage/ParamsFile").toString() == selectedMotorFile,
                "selected parameter path not persisted");
            check(!window.m_motorScanMonitor->applyGateOpen() && controller.ringConfig().sectorCcw == 1,
                  "file selection bypassed an active round latch");
            QTimer::singleShot(0, [&] {
                for (auto* top : QApplication::topLevelWidgets())
                    if (auto* picker = qobject_cast<QFileDialog*>(top)) picker->reject();
            });
            fileButton->click();
            check(window.m_motorScanMonitor->paramsFile() == selectedMotorFile,
                  "cancel changed the parameter file");
            dialog.reject(); selectionRound.timeoutBoundary(77); window.m_motorScanMonitor->tick();
            check(controller.ringConfig().sectorCcw == 0 && controller.ringConfig().sectorStartDeg == 180,
                  "selected file was not consumed at the next idle boundary");
            dialog.show(); QApplication::processEvents();
            auto* linkageLabel = widget<QLabel>(dialog, "motorScanLinkageStatus");
            const auto row = linkageLabel->geometry().united(fileButton->geometry());
            linkageLabel->parentWidget()->grab(QRect(0, row.y()-30,
                linkageLabel->parentWidget()->width(), row.height()+60))
                .save(QCoreApplication::applicationDirPath()+"/motor-file-picker.png");
            dialog.reject();
            std::cout << "PASS parameter file browser selection/cancel/persistence/active-round latch\n";
        }
        { MainWindow restarted;
          check(restarted.m_motorScanMonitor->paramsFile() == selectedMotorFile,
                "application restart did not restore the selected file"); }
        QFile::remove(paimageSettingsPath());
        QFile::remove(motorFile);
        QFile::remove(selectedMotorFile);
    }
};

int main(int argc, char** argv) {
    if (const auto result = ring_png::runWriterMode(argc, argv)) return *result;
    if (argc == 4 && std::strcmp(argv[1], "--png-shutdown-child") == 0) {
        const QString directory = QString::fromLocal8Bit(argv[2]), scenario = QString::fromLatin1(argv[3]);
        int receiverArgc = 1;
        char* receiverArgs[]{argv[0], nullptr};
        try {
            return runReceiverForTest(receiverArgc, receiverArgs, [&](MainWindow& window) {
                MainWindowStartIntegrationTest::pngShutdownChild(window, directory, scenario);
            });
        } catch (const std::exception& e) {
            std::cerr << "FAIL shutdown child: " << e.what() << '\n';
            return 1;
        }
    }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    try {
        if (app.arguments().contains("--png-production-smoke"))
            MainWindowStartIntegrationTest::pngShutdown(true);
        else if (app.arguments().contains("--png-shutdown"))
            MainWindowStartIntegrationTest::pngShutdown();
        else if (app.arguments().contains("--listener-restart-png"))
            MainWindowStartIntegrationTest::listenerRestartPng();
        else if (app.arguments().contains("--preview-pressure"))
            MainWindowStartIntegrationTest::previewPressure();
        else if (app.arguments().contains("--operator-log"))
            MainWindowStartIntegrationTest::operatorLog();
        else
            MainWindowStartIntegrationTest::run();
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
