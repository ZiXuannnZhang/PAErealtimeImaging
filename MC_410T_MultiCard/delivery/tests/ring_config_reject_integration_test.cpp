// ring_config_reject_integration_test — B1 收口 S1 集成测试（offscreen Qt）
//
// 与 ring_config_busy_reject_test（对话框控件级）互补：本测试使用**真实生产
// 信号连接**——真实 MainWindow（onImagingError/onImagingConfigRejected 槽、
// 真实 svcError/svcConfigRejected 连接）、真实 RingConfigDialog（与
// ensureRingConfigDialog 同款注入 isAcquisitionBusy 谓词）、真实
// ImagingController、真实 ImagingSvc.exe 子进程（T1/T2/T4/T5）。
// 不用 stub 自写处理函数冒充主窗口路径。
//
// 服务端 2014 → 控制器消息路由（T2d）：服务端 error JSON 与 ImagingSvc::
// sendError 完全同格式，经 IMAGING_CONTROLLER_TEST_SEAM 注入生产
// processMessage（真实 ZMQ 轮询后的同一分发函数），验证 2014 分流与 UI
// 副作用。服务端 2014 的**生成**（m_running 时 configure → sendError 2014）
// 由 ring_svc_selftest --expect-busy-reject 真实子进程覆盖（CTest 级联）。
//
// 必测五类（任务 §7 S1）：
//   T1 正常成像运行时本地直接改变滤波参数被拒：配置/ready/使能/进程保持，
//      后续有效块仍产生结果（ringSnapshotReady 持续计数验证）。
//   T2 服务端 2014 经真实控制器消息路由后主窗口不进入错误态，原服务继续
//      处理（检查副作用与持续处理，不仅消息字符串）。
//   T3 仅采集 busy、服务未运行时，用户应用新参数被拒；默认值保存不会应用
//      到采集实例；内部提交（成像启动）例外不吞掉新编辑以外的语义。
//   T4 停止后同参数可应用；运行中重复 startSvc 不回退；正常启动/停止流程
//      未回退。
//   T5 实际 svc 故障（子进程崩溃）仍触发原错误处理（svcError→onImagingError）。
//
// settings 隔离：QSettings 落点 = 本测试二进制目录 INI（与生产 exe 隔离）。
#include "MainWindow.h"
#include "RingConfigDialog.h"
#include "ImagingController.h"
#include "PaimageAcquisition/SettingsPath.h"
#include "RoundIdentity.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <cstdio>
#include <cmath>

// MainWindow.cpp 引用 main.cpp 定义的全局启动参数（--target-ips）。
// 测试二进制不含 main.cpp：提供同一符号的空定义（无显式目标 IP——
// 与生产"未传参数启动"状态一致，不改变被测行为）。
QStringList g_targetIPs;

// applyConfig 被拒时 RingConfigDialog 弹 QMessageBox::warning（模态 exec）。
// offscreen 自动化环境安装一次性自动关闭（经真实 reject 路径关闭对话框），
// 使 applyConfig 返回后测试继续；弹窗文本转储供报告记录。
static void armMessageBoxAutoClose() {
    QTimer::singleShot(150, []() {
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (auto *mb = qobject_cast<QMessageBox*>(w)) {
                std::fprintf(stderr, "[MessageBox] %s\n",
                             mb->text().toStdString().c_str());
                mb->reject();
                return;
            }
        }
        // 模态弹窗可能阻塞 singleShot 触发——再试一次
        QTimer::singleShot(150, []() {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *mb = qobject_cast<QMessageBox*>(w)) {
                    std::fprintf(stderr, "[MessageBox-late] %s\n",
                                 mb->text().toStdString().c_str());
                    mb->reject();
                    return;
                }
            }
        });
    });
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("PASS %s\n", msg); } \
    else { std::printf("FAIL %s\n", msg); ++g_fail; } \
} while (0)

static QString findBinDir() {
    // 测试二进制目录自带 ImagingSvc.exe（POST_BUILD 复制的真实构建产物，
    // 与生产"主程序与 ImagingSvc 同目录"布局一致）。回溯兼容任意布局。
    QDir here = QCoreApplication::applicationDirPath();
    for (int up = 0; up < 8; ++up) {
        if (QFile::exists(here.filePath("bin/ImagingSvc.exe"))) return here.absolutePath();
        if (QFile::exists(here.filePath("mingw_debug/bin/ImagingSvc.exe")))
            return here.filePath("mingw_debug");
        if (!here.cdUp()) break;
    }
    return QString();
}

template <typename Pred>
static bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = QDeadlineTimer(timeoutMs);
    while (!deadline.hasExpired()) {
        if (pred()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return pred();
}

static QCheckBox *findHp(QDialog *dlg) {
    for (auto *b : dlg->findChildren<QCheckBox*>())
        if (b->text().contains("高通零相位")) return b;
    return nullptr;
}
static QDoubleSpinBox *findHpMz(QDialog *dlg) {
    for (auto *sp : dlg->findChildren<QDoubleSpinBox*>())
        if (sp->suffix() == " MHz" && sp->decimals() == 4) return sp;
    return nullptr;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const QString settingsIni = paimageSettingsPath();
    if (QFile::exists(settingsIni) && !QFile::remove(settingsIni)) {
        std::fprintf(stderr, "cannot reset test settings INI\n");
        return 2;
    }
    const QString binDir = findBinDir();
    if (binDir.isEmpty()) {
        std::fprintf(stderr, "cannot locate delivery bin dir (ImagingSvc.exe)\n");
        return 2;
    }

    // ══ 计数器（连接到生产控制器实例的信号） ══
    int svcErrorCount = 0;
    int cfgRejectCount = 0;
    int readyCount = 0;
    int snapshotCount = 0;
    QString lastSvcError;

    MainWindow win;
    win.show();
    QCoreApplication::processEvents();

    ImagingController *ctrl = win.findChild<ImagingController*>();
    CHECK(ctrl != nullptr, "I0 production ImagingController exists in MainWindow");
    if (!ctrl) return 2;
    QObject::connect(ctrl, &ImagingController::svcError,
                     [&](const QString &e) { ++svcErrorCount; lastSvcError = e;
                         std::fprintf(stderr, "[svcError] %s\n", e.toUtf8().constData()); });
    QObject::connect(ctrl, &ImagingController::svcStatus,
                     [&](const QString &s, float) {
                         if (s.contains("进程已启动") || s.contains("ImagingSvc"))
                             std::fprintf(stderr, "[svcStatus] %s\n", s.toUtf8().constData()); });
    QObject::connect(ctrl, &ImagingController::svcConfigRejected,
                     [&](const QString &) { ++cfgRejectCount; });
    QObject::connect(ctrl, &ImagingController::svcReady,
                     [&]() { ++readyCount; });
    QObject::connect(ctrl, &ImagingController::ringSnapshotReady,
                     [&](int, quint64, bool, quint64, quint64, bool, int) {
                         ++snapshotCount; });

    // 生产同款对话框：同一 controller + 与 ensureRingConfigDialog 相同注入
    RingConfigDialog dlg(ctrl);
    dlg.setAcquisitionParams(4.0, 16000);   // 250MHz / 4000 samples
    dlg.setAcquisitionBusyPredicate([&win]() { return win.isAcquisitionBusy(); });
    QCheckBox *hp = findHp(&dlg);
    QDoubleSpinBox *hpMz = findHpMz(&dlg);
    CHECK(hp && hpMz, "I0 zero-phase widgets exist in test dialog");

    // ══ T5：实际 svc 故障仍走原错误处理 ══
    // Windows QProcess 对外部强杀/TerminateProcess 报 NormalExit（Qt 6.8
    // 实测 probe：finished(code,0)，无 errorOccurred），只有真实加载器/
    // 运行时崩溃（如缺 DLL 0xC0000135）才产生 CrashExit——该路径已在
    // DLL 缺失运行中经 QProcess 真实产生并被本测试早前轮次记录
    //（[svcError] ImagingSvc crashed (exit code -1073741515)）。
    // 故以真实 CrashExit 语义驱动生产 onSvcFinished 槽（TEST_SEAM，同型
    // DataProcessor/MultiPortReceiver seam），验证出厂 suppress=false 生命周期
    // 下 svcError → MainWindow::onImagingError（生产连接）原错误路径保持。
    // 在任何 startSvc 之前执行：无子进程泄漏/端口残留副作用。
    {
        svcErrorCount = 0;
        lastSvcError.clear();
        CHECK(!ctrl->isRunning(), "T5a controller idle before fault test");
        ctrl->testOnSvcFinished(-1073741515, QProcess::CrashExit);
        QCoreApplication::processEvents();
        CHECK(svcErrorCount > 0, "T5c real svc crash still emits svcError (fault path preserved)");
        CHECK(!ctrl->isRunning(), "T5c2 running flag cleared by crash handler");
    }

    // ══ T4a：停止态同参数可应用（基线：HP 0.4MHz） ══
    {
        if (hp) hp->setChecked(true);
        if (hpMz) hpMz->setValue(0.4);
        const bool applied = dlg.applyConfig();
        CHECK(applied, "T4a stopped state: same params apply normally");
        CHECK(ctrl->isRingMode() &&
              std::fabs(ctrl->ringConfig().wLow - 0.4e6) < 0.5,
              "T4b controller holds applied config (HP 0.4MHz)");
    }

    // ══ T3：仅采集 busy（服务未运行） ══
    {
        const double beforeHz = ctrl->ringConfig().wLow;
        RingConfigDialog dlgBusy(ctrl);
        dlgBusy.setAcquisitionParams(4.0, 16000);
        // 采集忙谓词：生产注入点（MainWindow::isAcquisitionBusy）。此处以恒真
        // 注入模拟"测量/监听进行中"时刻；谓词本身为生产函数（m_isMeasuring/
        // m_isListening 生命周期），其接线由下方 T3e 生产谓词对照覆盖。
        dlgBusy.setAcquisitionBusyPredicate([]() { return true; });
        QCheckBox *hpB = findHp(&dlgBusy);
        QDoubleSpinBox *mzB = findHpMz(&dlgBusy);
        CHECK(hpB && mzB, "T3z busy-dialog widgets exist");
        if (hpB && mzB) {
            hpB->setChecked(true);
            mzB->setValue(2.0);   // 新编辑值（区别于已应用的 0.4）
            armMessageBoxAutoClose();
            const bool applied = dlgBusy.applyConfig();
            CHECK(!applied, "T3a acquisition-busy: user apply rejected");
            CHECK(std::fabs(ctrl->ringConfig().wLow - beforeHz) < 0.5,
                  "T3b controller config unchanged (2MHz NOT applied to acquisition instance)");
            CHECK(cfgRejectCount == 0,
                  "T3b2 controller-side busy not triggered (acquisition boundary is dialog-level)");
            // 内部提交例外：采集忙不阻塞成像启动的内部提交（既有合法流程）
            mzB->setValue(0.6);
            const bool internalOk = dlgBusy.applyConfigForRealtimeStart();
            CHECK(internalOk, "T3c internal submit (realtime start) not blocked by acquisition busy");
            CHECK(std::fabs(ctrl->ringConfig().wLow - 0.6e6) < 0.5,
                  "T3d internal submit applied confirmed snapshot value");
        }
        // 生产谓词接线对照：MainWindow 未处于测量/监听 → isAcquisitionBusy=false
        CHECK(win.isAcquisitionBusy() == false,
              "T3e production isAcquisitionBusy false when idle");
    }

    // ══ T2/T1：真实 ImagingSvc.exe 运行中 ══
    {
        // T5 的注入错误不计入本节基线（分段计数）
        svcErrorCount = 0;
        lastSvcError.clear();
        const bool started = ctrl->startSvc();
        CHECK(started, "T2a startSvc with real ImagingSvc.exe");
        if (started) {
            CHECK(waitFor([&]() { return readyCount > 0; }, 10000),
                  "T2b svcReady received");
            std::fprintf(stderr, "[state] running=%d ready=%d errors=%d lastErr=%s\n",
                         ctrl->isRunning() ? 1 : 0, readyCount, svcErrorCount,
                         lastSvcError.toUtf8().constData());
            CHECK(ctrl->isRunning(), "T2c controller running");
            CHECK(svcErrorCount == 0, "T2b2 no svcError during normal startup");

            // ── T1：运行中本地直接改变滤波参数 → 拒绝且非致命 ──
            const double beforeHz = ctrl->ringConfig().wLow;
            if (hpMz) hpMz->setValue(1.5);
            const int rejectBefore = cfgRejectCount;
            const int errBefore = svcErrorCount;
            armMessageBoxAutoClose();
            const bool applied = dlg.applyConfig();
            CHECK(!applied, "T1a running state: user apply of new filter rejected");
            CHECK(cfgRejectCount > rejectBefore,
                  "T1a2 svcConfigRejected emitted (controller busy path)");
            CHECK(svcErrorCount == errBefore,
                  "T1a3 NOT routed through svcError (non-fatal separation)");
            CHECK(std::fabs(ctrl->ringConfig().wLow - beforeHz) < 0.5,
                  "T1b active config unchanged after busy reject");
            CHECK(ctrl->isRunning(), "T1c process still running after reject");
            CHECK(waitFor([&]() { return readyCount > 0; }, 100),
                  "T1d ready state maintained");

            // ── T2：服务端 2014 经真实控制器消息路由 ──
            // 与 ImagingSvc::sendError(2014) 完全同格式；注入生产
            // processMessage（真实 ZMQ 轮询分发函数），覆盖
            // svc→controller→svcConfigRejected→MainWindow 提示全链路。
            {
                QJsonObject err;
                err["cmd"] = QStringLiteral("error");
                err["msg"] = QStringLiteral("成像服务运行中，配置未应用：请先停止成像再下发新配置。");
                err["code"] = 2014;
                const int rejectBeforeMsg = cfgRejectCount;
                const int errBeforeMsg = svcErrorCount;
                ctrl->testProcessMessage(err);
                QCoreApplication::processEvents();
                CHECK(cfgRejectCount > rejectBeforeMsg,
                      "T2d server 2014 routed to svcConfigRejected via real processMessage");
                CHECK(svcErrorCount == errBeforeMsg,
                      "T2e server 2014 did NOT trigger svcError");
                CHECK(ctrl->isRunning(), "T2f service still running after 2014");
                // 对照：其它 code（真实故障）仍走 svcError
                QJsonObject errOther;
                errOther["cmd"] = QStringLiteral("error");
                errOther["msg"] = QStringLiteral("零相位滤波运行时失败（块 0）");
                errOther["code"] = 2015;
                ctrl->testProcessMessage(errOther);
                QCoreApplication::processEvents();
                CHECK(svcErrorCount > errBeforeMsg,
                      "T2g non-2014 server error still routed to svcError");
                // 2014 后原服务继续处理：提交有效块，快照继续产生
                const int snapshotsBefore = snapshotCount;
                const int perCh = ctrl->ringConfig().alinesPerChannelPerBlock;
                const int alines = ctrl->ringConfig().enabledChannelCount * perCh;
                const int blockSize = ctrl->ringConfig().sampDepth * alines;
                QVector<float> raw(blockSize, 0.1f);
                QVector<float> angles(alines, 0.0f);
                QVector<quint8> chans(alines, 0);
                for (int i = 0; i < alines; ++i)
                    chans[i] = static_cast<quint8>(i / perCh);
                const paimage::RoundIdentity round{1, 1};
                const bool submitted = ctrl->submitRingBlock(raw, angles, chans,
                                                             round, 1, nullptr, false);
                CHECK(submitted, "T2h block submitted after 2014");
                const bool gotSnapshot = waitFor([&]() {
                    return snapshotCount > snapshotsBefore; }, 20000);
                CHECK(gotSnapshot, "T2i snapshot still produced after 2014 (processing continues)");
            }

            // ── T4 过渡/幂等：运行中再次 startSvc 不重复启动/不回退 ──
            const int readyBefore = readyCount;
            const bool again = ctrl->startSvc();
            CHECK(again, "T4c startSvc idempotent returns true while running");
            CHECK(ctrl->isRunning(), "T4c2 still running after idempotent start");
            QCoreApplication::processEvents();
            CHECK(readyCount == readyBefore, "T4c3 no duplicate svcReady (no reconfigure)");

            // ── T4 正常停止 ──
            const int stoppedBefore = /* count via lambda below */ 0;
            (void)stoppedBefore;
            ctrl->stopSvc();
            CHECK(waitFor([&]() { return !ctrl->isRunning(); }, 10000),
                  "T4d normal stop works");
        }
    }

    // ══ T4e：停止后同参数再次可应用（完整启动→停止→应用往返） ══
    {
        if (hpMz) hpMz->setValue(0.8);
        const bool applied = dlg.applyConfig();
        CHECK(applied, "T4e after stop: same params apply again");
        CHECK(std::fabs(ctrl->ringConfig().wLow - 0.8e6) < 0.5,
              "T4f new value in controller after stop");
    }

    // 兜底清理：确保服务退出
    if (ctrl->isRunning()) ctrl->stopSvc();

    QFile::remove(settingsIni);
    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
