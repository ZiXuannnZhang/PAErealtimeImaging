#include "MainWindow.h"
#include "ImagingController.h"
#include "RingConfigDialog.h"
#include "RingBlockAssembler.h"
#include "RingTimevarLowpassSettings.h"
#include "FilteredDasSettings.h"
#include <QDoubleSpinBox>
#include "PaimageAcquisition/SettingsPath.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSharedMemory>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QJsonDocument>
#include <QSpinBox>
#include <QThread>
#include <iostream>
#include <stdexcept>

QStringList g_targetIPs;
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

class MainWindowStartIntegrationTest {
public:
    static void run() {
        check(QCoreApplication::applicationDirPath().endsWith("startup-isolated"),
              "test must use isolated INI directory");
        QFile::remove(paimageSettingsPath());
        const QString motorFile = QCoreApplication::applicationDirPath() + "/motor-test.json";
        QFile::remove(motorFile);
        {
            QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
            settings.setValue("RingConfigDialog/Defaults/block", 4);
            settings.setValue("RingConfigDialog/Defaults/totalAlines", 8);
            settings.setValue("RingConfigDialog/Defaults/fovMm", 4.0);
            settings.setValue("RingConfigDialog/Defaults/gridMm", 0.1);
            settings.setValue("MotorScanLinkage/ParamsFile", motorFile);
            settings.setValue("MotorScanLinkage/TotalAlinesRule", "directionOnly");
            for (int channel = 0; channel < 8; ++channel)
                settings.setValue(QString("RingConfigDialog/Defaults/ch%1").arg(channel), channel == 0);
            settings.sync();
        }
        {
            MainWindow window;
            auto& controller = *window.m_imagingController;
            window.m_cmbImagingMode->setCurrentIndex(1);
            window.m_motorScanMonitor->tick();
            const auto earlyStatus = window.m_motorScanMonitor->lastStatus();
            window.ensureRingConfigDialog();
            auto& dialog = *window.m_ringConfigDialog;
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
                waitFor([&] { return controller.isRunning() && window.m_imagingEnabled
                    && checkbox->isEnabled(); }, "valid startup did not become ready");
                check(checkbox->isChecked() && !depth->isEnabled(), "running UI not locked");
            };
            auto stop = [&] {
                checkbox->setChecked(false);
                waitFor([&] { return !controller.isRunning() && !window.m_imagingEnabled
                    && controlsAvailable() && !controller.m_ringSharedMemory; }, "normal stop failed");
                check(!checkbox->isChecked(), "stop left checkbox checked");
            };
            start();
            check(controller.ringTimevarConfig().enabled && controller.ringConfig().sampDepth == 5000,
                  "enabled configuration not accepted through MainWindow");
            stop();
            std::cout << "PASS valid enabled startup -> stop\n";

            auto reject = [&](bool resident, bool filtered=false) {
                const auto acceptedFiltered=filtered_das::toJson(controller.filteredDasConfig());
                const auto accepted = ring_timevar::toJson(controller.ringTimevarConfig());
                const QByteArray oldConfig(reinterpret_cast<const char*>(&controller.ringConfig()),
                                           sizeof(RingReconCudaConfig));
                const QByteArray oldDelays(reinterpret_cast<const char*>(controller.m_ringSysDelayCh),
                                           sizeof(controller.m_ringSysDelayCh));
                auto* const shm = controller.m_ringSharedMemory;
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
                check(controller.m_ringSharedMemory == shm && controller.m_svcProcess == process
                    && (!process || process->processId() == pid) && controller.m_running.load() == running,
                      "rejection changed process/shared-memory lifecycle");
                check(controller.isRunning() == resident, "resident service state changed");
                check(QByteArray(reinterpret_cast<const char*>(&controller.ringConfig()),
                                sizeof(RingReconCudaConfig)) == oldConfig
                    && QByteArray(reinterpret_cast<const char*>(controller.m_ringSysDelayCh),
                                  sizeof(controller.m_ringSysDelayCh)) == oldDelays
                    && ring_timevar::toJson(controller.ringTimevarConfig()) == accepted
                    && filtered_das::toJson(controller.filteredDasConfig()) == acceptedFiltered,
                      "rejection changed accepted configuration");
                check(filtered ? dialog.filteredDasConfig().enabled && dialog.filteredDasConfig().stopMhz==125
                      : dialog.timevarConfig().enabled && dialog.timevarConfig().p2 == 2000,
                      "invalid draft silently disabled or clamped");
                const auto after = window.m_ringAssembler->snapshot();
                check(after.hasActiveRound == assembler.hasActiveRound
                    && after.activeRound == assembler.activeRound && after.blockTriggers == assembler.blockTriggers,
                      "rejection reset assembler state");
                check(window.m_ringAssemblerConfigured == configured
                    && window.m_imagingServiceReady.load() == ready
                    && window.m_restartRingOnSvcStop == restart && window.m_imagingEnabled == imaging
                    && window.m_lblImagingStatus->text() == status && !window.m_imagingTimer->isActive(),
                      "rejection changed prior startup/presentation state");
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
            waitFor([&] { return !controller.isRunning() && !controller.m_ringSharedMemory; },
                    "resident cleanup failed");
            depth->setText("20000");
            start();
            stop();
            std::cout << "PASS valid restart/start/stop regression\n";
            widget<QDoubleSpinBox>(dialog,"filteredDasStopMhz")->setValue(125);
            reject(false,true);
            widget<QDoubleSpinBox>(dialog,"filteredDasStopMhz")->setValue(60);
            start();
            check(controller.filteredDasConfig().enabled && controller.filteredDasConfig().referenceMhz==5,
                  "FilteredDAS snapshot missing from real startup");
            {QSignalBlocker blocker(checkbox);checkbox->setChecked(false);}
            window.stopRingFeedWorker();window.m_imagingEnabled=false;window.setImagingParamControlsEnabled(true);
            widget<QDoubleSpinBox>(dialog,"filteredDasStopMhz")->setValue(125);
            reject(true,true);
            controller.stopSvc();waitFor([&]{return !controller.isRunning()&&!controller.m_ringSharedMemory;},"filtered resident cleanup");
            widget<QDoubleSpinBox>(dialog,"filteredDasStopMhz")->setValue(60);
            for(bool tv:{false,true})for(bool fd:{false,true}) {
                widget<QCheckBox>(dialog,"timevarEnabled")->setChecked(tv);
                widget<QCheckBox>(dialog,"filteredDasEnabled")->setChecked(fd);
                check(dialog.applyConfig(), "joint configuration rejected");
                const auto acceptedTv = ring_timevar::toJson(controller.ringTimevarConfig());
                const auto acceptedFd = filtered_das::toJson(controller.filteredDasConfig());
                auto writeMotor = [&](const char *direction) {
                    QSaveFile file(motorFile);
                    check(file.open(QIODevice::WriteOnly), "open motor file");
                    file.write(QJsonDocument(QJsonObject{{"formatVersion",1},{"direction",direction},
                        {"xStepsNum",2000}}).toJson());
                    check(file.commit(), "atomic motor file commit");
                };
                writeMotor("CCW"); window.m_motorScanMonitor->tick();
                start();check(controller.ringTimevarConfig().enabled==tv&&controller.filteredDasConfig().enabled==fd,
                              "four-combination startup snapshot");
                paimage::PhysicalRoundNormalizer normalizer(8, [&](const paimage::PhysicalRoundEvent& event) {
                    window.m_motorScanMonitor->onPhysicalRoundEvent(event);
                });
                normalizer.setStartupFilterTriggerCount(0); normalizer.beginSession(1);
                normalizer.classify(1, 100);
                const int startsBeforeMotor = controller.m_testSvcStarts;
                writeMotor("CW"); window.m_motorScanMonitor->tick();
                check(controller.ringConfig().sectorCcw == 1 && controller.m_testSvcStarts == startsBeforeMotor,
                      "zero-filter active round changed direction/service");
                normalizer.timeoutBoundary(1); window.m_motorScanMonitor->tick();
                waitFor([&]{return controller.isRunning() && window.m_imagingServiceReady.load()
                    && controller.m_testSvcStarts > startsBeforeMotor;}, "motor boundary restart failed");
                check(controller.ringConfig().sectorCcw == 0 && controller.ringConfig().sectorStartDeg == 0
                    && ring_timevar::toJson(controller.ringTimevarConfig()) == acceptedTv
                    && filtered_das::toJson(controller.filteredDasConfig()) == acceptedFd,
                      "motor direction restart lost either lowpass/FilteredDAS snapshot");
                // A service stopping inside another active round must not reopen the motor gate.
                normalizer.classify(1, 101); stop();
                check(!window.m_motorScanMonitor->applyGateOpen(), "service stop reopened an active physical round");
                normalizer.timeoutBoundary(1);
            }
            std::cout<<"PASS FilteredDAS real rejection, four-combination motor CW restart with both filter snapshots, service-stop latch\n";
            QString motorError;
            const auto oldDirection = controller.ringConfig().sectorCcw;
            widget<QDoubleSpinBox>(dialog,"filteredDasStopMhz")->setValue(125);
            check(!dialog.applyMotorScanParams(true, true, 0, &motorError) && !motorError.isEmpty(),
                  "invalid filter draft did not reject motor update without a modal dialog");
            check(controller.ringConfig().sectorCcw == oldDirection && dialog.config().sectorCcw == oldDirection,
                  "rejected motor update left hidden direction/anchor changes");
            dialog.reject();
            dialog.show();
            check(!dialog.applyMotorScanParams(true, true, 0, &motorError)
                && motorError.contains(QStringLiteral("正在编辑")), "motor update consumed a visible filter draft");
            dialog.reject();

        }
        QFile::remove(paimageSettingsPath());
        QFile::remove(motorFile);
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try { MainWindowStartIntegrationTest::run(); return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
