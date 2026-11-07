#include "MainWindow.h"
#include "ImagingController.h"
#include "ImagingBypass.h"
#include "RingConfigDialog.h"
#include "RingBlockAssembler.h"
#include "RingTimevarLowpassSettings.h"
#include "RingFbpSettings.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include "PaimageAcquisition/SettingsPath.h"
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
                waitFor([&] { return controller.isRunning() && window.m_imagingEnabled
                    && window.m_imagingServiceReady.load() && checkbox->isEnabled(); }, "valid startup did not become ready");
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

            auto reject = [&](bool resident, bool fbp=false) {
                const auto acceptedFbp=ring_fbp_settings::toJson(controller.m_fbpSettings);
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
            waitFor([&] { return !controller.isRunning() && !controller.m_ringSharedMemory; },
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
            controller.stopSvc();waitFor([&]{return !controller.isRunning()&&!controller.m_ringSharedMemory;},"FBP resident cleanup");
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
                const auto* shm = controller.m_ringSharedMemory;
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
                    && controller.m_svcProcess->processId() == pid && controller.m_ringSharedMemory == shm
                    && controller.m_testSvcStarts == startsBeforeMotor && controller.m_testSvcStops == stopsBeforeMotor
                    && window.m_testFeedStarts == feedStarts && window.m_testAssemblerConfigs == assemblerConfigs,
                      "direction switch restarted or reconfigured the resident imaging chain");
                normalizer.timeoutBoundary(session);
                writeMotor("CW", 4000); window.m_motorScanMonitor->tick();
                writeMotor(nullptr, 6000); window.m_motorScanMonitor->tick();
                check(controller.ringConfig().alinesPerFrame == totalAlines
                    && controller.ringConfig().sectorCcw == 1
                    && controller.m_testSvcStarts == startsBeforeMotor && controller.m_testSvcStops == stopsBeforeMotor
                    && controller.m_ringSharedMemory == shm && window.m_pendingRingDirectionRevision == 0,
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
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    try { MainWindowStartIntegrationTest::run(); return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
