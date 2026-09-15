#include "PaimageAcquisition/SettingsPath.h"
#include <QInputDialog>
#include <QMenuBar>
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "NetworkController.h"
#include "ImagingController.h"
#include "RingConfigDialog.h"
#include "ImagingDisplayWindow.h"
#include "RingBlockAssembler.h"
#include "ImagingBypass.h"
#include "DiagnosticRecorder.h"
#include "DiagnosticExportDialog.h"
#include "Constants.h"
#include "AcqConfig.h"
#include "StartupPolicy.h"
#include "SystemCaptureStatus.h"
#include "PaimageAcquisition/CardDiscovery.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QMenu>
#include <QApplication>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QDialog>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUuid>
#include <QApplication>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <mutex>
#include <condition_variable>
#include <deque>
#ifdef _WIN32
#include <windows.h>
#endif
#include <QComboBox>
#include <QFrame>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <thread>
#include <cmath>
#include <numeric>
#include <chrono>
#include <algorithm>
#include <fstream>

// ══ 成像测试数据参数（文件路径运行时拼接 exe 目录）══
static constexpr int kTestCardNum = 8;   // F8 模式 bin 文件固定有 8 路逻辑卡
static constexpr int kTestDepth = 12500; // 示例程序 F8 depth=12500（250M v3）

// 读取二进制 float 文件到 vector
// ══ 辅助：QVector<int> ↔ 逗号分隔字符串（用于 QSettings 持久化）══
static QVector<int> strToVec(const QString &s) {
    QVector<int> v;
    for (const QString &tok : s.split(',', Qt::SkipEmptyParts))
        v.append(tok.trimmed().toInt());
    return v;
}
static QString vecToStr(const QVector<int> &v) {
    QStringList sl; for (int x : v) sl.append(QString::number(x));
    return sl.join(",");
}

static bool readBinaryFloats(const char *path, std::vector<float> &data)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    std::streamsize bytes = ifs.tellg();
    if (bytes < 0 || (bytes % static_cast<std::streamsize>(sizeof(float))) != 0)
        return false;
    ifs.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(bytes) / sizeof(float));
    if (!data.empty())
        ifs.read(reinterpret_cast<char *>(data.data()), bytes);
    return ifs.good() || ifs.eof();
}

// ══ 辅助：设置按钮文本时同步更新 Accessibility 名称（供 windows-mcp UIA 识别）══
static void setBtnText(QPushButton *btn, const QString &text)
{
    btn->setText(text);
    btn->setAccessibleName(text);
}

static QString uniqueDiagnosticZipPath(const QString &requestedPath)
{
    const QFileInfo requested(requestedPath);
    const QString absolute = requested.absoluteFilePath();
    if (!QFileInfo::exists(absolute)) return absolute;

    const QString directory = requested.absolutePath();
    const QString baseName = requested.completeBaseName();
    const QString suffix = requested.suffix().isEmpty()
        ? QStringLiteral("zip") : requested.suffix();
    for (int index = 1; index < 100000; ++index) {
        const QString candidate = QDir(directory).filePath(
            QStringLiteral("%1 (%2).%3").arg(baseName).arg(index).arg(suffix));
        if (!QFileInfo::exists(candidate)) return candidate;
    }
    return absolute;
}

// ══ 频域显示辅助：radix-2 FFT 幅值谱（Hann 窗）══
static int nextPow2Ceil(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// 自动重标定坐标轴的节流：每隔 N 次实际显示刷新才 rescale 一次，
// 避免每帧 rescaleAxes 触发全量重排与重绘（信号抖动时尤甚）
constexpr int kAutoRescaleEveryTick = 3;

// 峰峰值统计范围（显示采样点，与 X 轴显示一致）→ 显示裁切区间 [start,end)。
// 无效输入（end<=start 等）返回全文区间，不裁切。仅作用于显示层，
// 不影响实时成像与保存的原始数据（两者均使用全分辨率 freqA/freqB）。
static void computePkRange(int nPts, int pkStartInput, int pkEndInput,
                           int *start, int *end)
{
    *start = 0;
    *end = nPts;
    if (nPts <= 0 || pkEndInput <= pkStartInput) return;
    if (pkStartInput >= nPts) return;   // 起点超出显示范围：不裁切（保持全文）
    const int s = qBound(0, pkStartInput, nPts - 1);
    const int e = qMin(pkEndInput, nPts);
    if (e <= s) return;
    *start = s;
    *end = e;
}

static QVector<double> computeMagnitudeSpectrum(const QVector<double> &x) {
    if (x.size() < 2) return {};
    const int n = nextPow2Ceil(x.size());
    QVector<double> re(n, 0.0), im(n, 0.0);
    const double denom = static_cast<double>(x.size() - 1);
    for (int i = 0; i < x.size(); ++i) {
        // Hann 窗降低频谱泄漏
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / denom);
        re[i] = x[i] * w;
    }
    // 位反转置换
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    // 迭代 radix-2 FFT
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const double ur = re[i + k + half] * cwr - im[i + k + half] * cwi;
                const double ui = re[i + k + half] * cwi + im[i + k + half] * cwr;
                re[i + k + half] = re[i + k] - ur;
                im[i + k + half] = im[i + k] - ui;
                re[i + k] += ur;
                im[i + k] += ui;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    QVector<double> mag(n >> 1);
    const double scale = 2.0 / n;
    for (int k = 0; k < (n >> 1); ++k)
        mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * scale;
    return mag;
}

static QVector<double> makeSpectrumBins(int halfN, int origSamples,
                                        int dispSamples) {
    QVector<double> bins(halfN);
    // 采样率固定为真实采集 250 MHz（采样间隔 4 ns）。
    // 显示段经均匀抽取后，有效采样率 = 250 MHz × 显示点数 / 原始点数。
    const double fsOrigHz = FPGA_ADC_FREQ_HZ;
    const double decim = (origSamples > 0 && dispSamples > 0)
        ? static_cast<double>(origSamples) / dispSamples : 1.0;
    const double fsEffHz = fsOrigHz / decim;
    const double nfft = static_cast<double>(halfN) * 2.0;
    for (int k = 0; k < halfN; ++k)
        bins[k] = static_cast<double>(k) * fsEffHz / nfft / 1e6;  // MHz
    return bins;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_nCards(4)
    , m_netController(nullptr)
    , m_statsTimer(new QTimer(this))
    , m_diagnosticStatusTimer(new QTimer(this))
    , m_systemCaptureStatusTimer(new QTimer(this))
    , m_displayTimer(new QTimer(this))
    , m_ringTimeoutTimer(new QTimer(this))
    , m_isListening(false)
    , m_isMeasuring(false)
    , m_highDataRateWarningShown(false)
        , m_pendingAutoSave(false)
    , m_saveWarnCooldown(0)
    , m_displayEnabled(true)
    , m_enableDownsampling(false)
    , m_autoRescaleAxes(true)
    , m_autoRescalePlot{}
    , m_displayInterval(10)
    , m_openglInitialized(false)
    , m_isTileView(false)
    , m_groupTabBar(nullptr)
    , m_placeholderPage(nullptr)
    , m_currentGroup(0)
    , m_numGroups(1)
    , m_refreshCounter(0)
    , m_displayTickCount(0)
    , m_loadingSettings(false)
    , m_useTestImagingData(false)
    , m_imagingController(nullptr)
    , m_imagingConfigDialog(nullptr)
    , m_imagingEnabled(false)
    , m_imagingPulseCount(0)
    , m_freqColorRange(0, 500)
    , m_pixelColorRange(0, 500)
    , m_freqColorInited(false)
    , m_pixelColorInited(false)
    , m_btnImagingConfig(nullptr)
    , m_cmbImagingMode(nullptr)
    , m_chkRealtimeImaging(nullptr)
    , m_grpImaging(nullptr)
{
    // 0xFFFF 保证触发序号从 0 开始时首触发不被去重跳过
    for (int i = 0; i < MAX_CARDS; ++i) m_lastFeedSeq[i] = 0xFFFF;
    for (int c = 0; c < CARDS_PER_DISPLAY_GROUP; ++c)
        for (int h = 0; h < 2; ++h) {
            m_autoRescalePlot[c][h] = true;
            m_autoRescaleSpectrum[c][h] = true;
        }

    ui->setupUi(this);
    setWindowTitle(QStringLiteral("PAimage接收诊断版 — receiver-diagnostics"));
    auto* trialAction=menuBar()->addAction(QStringLiteral("实验标记"));
    connect(trialAction,&QAction::triggered,this,[this]{
        bool ok=false;const QString marker=QInputDialog::getText(this,QStringLiteral("实验轮次标记"),
            QStringLiteral("trialId / roundId（例如 trialA / 1；记录操作时刻，不代表触发沿）"),QLineEdit::Normal,QString(),&ok);
        if(!ok||marker.trimmed().isEmpty())return;
        const auto parts=marker.split('/');
        recordDiagnosticAction(QStringLiteral("experiment_marker"),
            {{"trialId",parts.value(0).trimmed()},{"roundId",parts.value(1).trimmed()},
             {"markerText",marker},{"dataTimeNs",ui->edtDataTime->text().toInt()},
             {"saving",m_netController&&m_netController->isSaving()},{"measuring",m_isMeasuring},
             {"operatorTimestampNotTriggerEdge",true}});
        recordAcquisitionSnapshot({},QStringLiteral("experiment_marker"));
        logMessage(QStringLiteral("实验标记：")+marker);
    });
    auto* systemCaptureAction=menuBar()->addAction(QStringLiteral("准备系统抓取"));
    systemCaptureAction->setObjectName(QStringLiteral("prepareSystemCaptureAction"));
    connect(systemCaptureAction,&QAction::triggered,this,&MainWindow::onPrepareSystemCaptureClicked);
    m_systemCaptureStatusLabel=new QLabel(QStringLiteral("系统抓取：未准备"),this);
    m_systemCaptureStatusLabel->setAccessibleName(QStringLiteral("系统抓取状态"));
    m_systemCaptureStatusLabel->setMinimumWidth(180);
    statusBar()->addPermanentWidget(m_systemCaptureStatusLabel);

    // 工作七：主窗口可自由缩小，内容超出时自动出现滚动条
    {
        QScrollArea *scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidget(ui->centralwidget);
        setCentralWidget(scrollArea);
    }

    // 重设显示类型：差分相位(0) / 瞬时频率(1)
    // 成像方式（线性/环形扫描）由“实时成像”分组框中的 cmbImagingMode 选择
    ui->cmbDisplayType->clear();
    ui->cmbDisplayType->addItem("  差分相位");
    ui->cmbDisplayType->addItem("  瞬时频率");

    // ══ 为所有控件设置 Accessibility 名称（供 windows-mcp UIA 识别）══
    // 注意：btnStartListen / btnStartMeasure / btnToggleSave 的 accessible name
    // 通过 setBtnText() 随按钮文字自动同步，此处不再硬编码。
    ui->btnConfig->setAccessibleName("配置参数");
    ui->lblNetInfoIndicator->setAccessibleName("网络信息");
    ui->edtDataTime->setAccessibleName("采集时间(ns)");
    ui->edtADelay->setAccessibleName("A延时(ns)");
    ui->edtBDelay->setAccessibleName("B延时(ns)");
    // 显示控制组
    ui->chkEnableDisplay->setAccessibleName("实时显示");
    ui->chkRealtimeImaging->setAccessibleName("实时成像");
    ui->cmbImagingMode->setAccessibleName("成像方式");
    ui->chkTileView->setAccessibleName("平铺显示");
    ui->cmbDisplayType->setAccessibleName("显示类型");
    ui->spnRefreshRate->setAccessibleName("刷新间隔");
    ui->spnDownsampleRatio->setAccessibleName("显示最大点数");
    ui->edtPkStart->setAccessibleName("峰峰值统计起点");
    ui->edtPkEnd->setAccessibleName("峰峰值统计终点");
    // 数据保存组
    ui->btnSelectDir->setAccessibleName("选择目录");
    ui->edtSaveDir->setAccessibleName("保存目录");
    ui->edtTriggersPerFile->setAccessibleName("每文件触发数");
    ui->edtFileSuffix->setAccessibleName("文件后缀");
    ui->chkAutoSave->setAccessibleName("自动保存");
    ui->btnExportDiagnostic->setAccessibleName("导出诊断日志");

    m_lblStats[0] = ui->lblStats1;
    m_lblStats[1] = ui->lblStats2;
    m_lblStats[2] = ui->lblStats3;
    m_lblStats[3] = ui->lblStats4;
    // 状态栏长文本（触发/丢失/速率/队列）不得撑大 centralwidget 最小宽度：
    // 否则开始监听后状态文本变长，布局最小宽度超过窗口，出现横向滚动条，
    // 右侧时域/频域图框也被连带撑宽超出可视区域。
    for (int i = 0; i < CARDS_PER_BOX; ++i) {
        if (!m_lblStats[i]) continue;
        m_lblStats[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_lblStats[i]->setMinimumSize(0, 0);
        // 状态文本显式两行（卡号/触发/丢失/速率 + 处队/存队），标签自动按两行取高，
        // 避免长文本在窄格内被横向截断或与相邻标签互相遮挡
        m_lblStats[i]->setWordWrap(true);
    }

    m_colorMapPlot = nullptr;
    m_colorMap     = nullptr;

    memset(m_prevSaveDiscards, 0, sizeof(m_prevSaveDiscards));
    memset(m_displayCounter,   0, sizeof(m_displayCounter));
    m_colorScale   = nullptr;

    for (int i = 0; i < MAX_CARDS; ++i) {
        m_displayCounter[i] = 0;
        for (int j = 0; j < 2; ++j) {
            m_stackedWidgets[i][j] = nullptr;
            m_plotsPhase[i][j]     = nullptr;
            m_plotsFrequency[i][j] = nullptr;
            m_plotsSpectrum[i][j]  = nullptr;
            m_pkpkLabels[i][j]     = nullptr;
            m_tileContainers[i][j] = nullptr;
            m_firstPlot[i][j]      = true;
        }
    }

    setupUI();        // 创建 groupTabBar、colorMapPlot 等静态部件
    createConnections();
    loadStyleSheet();
    loadSettings();   // 会读取 nCards 并触发 rebuildDynamicUI()

    // ══ 成像控制器初始化 ════════════════════════════════════════
    m_peizhunLoaded = false;
    m_peizhunFilePath.clear();

    m_imagingController = new ImagingController(this);
    connect(m_imagingController, &ImagingController::imageReady,
            this, &MainWindow::onImagingImageReady);
    // 每块显示快照：固定双缓冲 → 显示窗口（方案A：全分辨率 3600²）
    connect(m_imagingController, &ImagingController::ringSnapshotReady,
            this, [this](int seq, quint64 submitIndex, bool hasSubmitIndex,
                         int bufferIndex) {
        if (!m_imagingEnabled ||
            m_ringSnapshotAdmissionBlocked.load(std::memory_order_acquire)) {
            // 停止/故障后的迟到帧：不弹窗、不计数，但必须释放缓冲。
            if (m_imagingController)
                m_imagingController->releaseSnapshotBuffer(bufferIndex);
            return;
        }
        if (!hasSubmitIndex || submitIndex == 0) {
            recordDiagnosticAction(QStringLiteral("ring_snapshot_identity_missing"),
                {{QStringLiteral("seq"), seq}});
            if (m_imagingController)
                m_imagingController->releaseSnapshotBuffer(bufferIndex);
            return;
        }
        // 物理轮次准入：TimeoutBoundary 复位前已重建完成的旧轮快照视为
        // stale——不得推进新轮帧计数、不得重新污染已清空图像，直接释放。
        if (!m_roundUi.admitSnapshot(submitIndex)) {
            m_roundUi.noteStaleSnapshot();
            const auto state = m_roundUi.snapshot();
            recordDiagnosticAction(QStringLiteral("physical_round_stale_frame_dropped"),
                {{QStringLiteral("seq"), seq},
                 {QStringLiteral("submitIndex"), qint64(submitIndex)},
                 {QStringLiteral("staleCutoffSubmitIndex"),
                  qint64(state.staleCutoffSubmitIndex)},
                 {QStringLiteral("uiEpoch"), qint64(state.epoch)}});
            if (m_imagingController)
                m_imagingController->releaseSnapshotBuffer(bufferIndex);
            return;
        }
        m_roundUi.onFrame();   // 环形重建输出帧计数（反馈显示用）
        const int frameIdx = int(m_roundUi.frameCount());   // 本帧序号（圈末重置前捕获）
        updateRingImagingStatus();   // 帧计数变化后即时刷新状态栏
        // 帧末检测：本轮已准入快照数达到 blocksPerFrame 整数倍即一帧完成。
        // 采用本轮计数而非 svc 全局 seq 取模——TimeoutBoundary 后 RingBlockAssembler
        // 块序号清零而 svc 的 shm frame_seq 不归零，全局取模会在超时后永久失准。
        const int bpf = m_imagingController->ringBlocksPerFrame();
        const bool frameEnd = m_roundUi.noteSnapshot(bpf);
        if (frameEnd)
            m_roundUi.resetFrameCount();   // 圈末：判定为下一圈图像，帧计数重新计算
        // CountBoundary has already prepared and published the next data
        // binding on the source thread.  Capture the old presentation target
        // before applying that committed target to the UI.  The final old
        // frame therefore keeps its old directory even when its render/save
        // callback is delayed.
        AutoSaveCommit countPresentation;
        bool hasCountPresentation = false;
        if (frameEnd) {
            std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
            for (auto it = m_pendingCountPresentation.begin();
                 it != m_pendingCountPresentation.end(); ++it) {
                if (it->measurementSession == m_ringBoundarySession &&
                    it->roundGeneration == m_ringBoundaryGeneration) {
                    countPresentation = *it;
                    m_pendingCountPresentation.erase(it);
                    hasCountPresentation = true;
                    break;
                }
            }
        }
        QString saveDir = m_reconSaveDir;
        if (hasCountPresentation && !countPresentation.oldDirectory.isEmpty())
            saveDir = QDir(countPresentation.oldDirectory).filePath("recon_png");
        const bool saveNow = frameEnd && m_reconSaveEnabled && !saveDir.isEmpty();
        if (saveNow)
            m_ringTimeoutSaveDone = true;
        const QString saveSuffix = m_reconSaveSuffix;
        if (hasCountPresentation) {
            if (countPresentation.failed)
                queueAutoSaveFailure(countPresentation);
            else
                // This call only applies the already committed presentation
                // target; it never allocates or increments a generation.
                applyAutoSaveCommitToUi(countPresentation);
        }
        if (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1) {
            if (!m_imagingDisplayWindow) {
                m_imagingDisplayWindow = new ImagingDisplayWindow();
            }
            if (!m_imagingDisplayWindow->isVisible()) {
                m_imagingDisplayWindow->show();
                m_imagingDisplayWindow->raise();
                m_imagingDisplayWindow->activateWindow();
            }
            const int nx = m_imagingController->ringDisplayNx();
            const float *buf = m_imagingController->snapshotBuffer(bufferIndex);
            if (buf && nx > 0 && m_imagingDisplayWindow) {
                // 解耦成像刷新与信号显示：数据拷贝（~103MB）与 float→QImage 灰度
                // 转换放到线程池，UI 线程只做贴图。原同步路径每次快照阻塞 UI
                // 数十毫秒，导致时域频域信号显示周期性卡顿。
                const size_t n = static_cast<size_t>(nx) * nx;
                auto frame = std::make_shared<std::vector<float>>(n * 2);
                const RingImageWidget::Range r1 = m_imagingDisplayWindow->range1();
                const RingImageWidget::Range r2 = m_imagingDisplayWindow->range2();
                QPointer<ImagingDisplayWindow> win(m_imagingDisplayWindow);
                QPointer<ImagingController> ctrl(m_imagingController);
                const int idx = bufferIndex;
                // 圈末已保存：本次成像的“空闲超时到点保存”不再重复保存同一张图
                // （两种保存条件互斥；新触发到来时由进度回调复位该标志）
                QThreadPool::globalInstance()->start([ctrl, idx, frame, n, nx, seq, frameIdx, r1, r2, win,
                                   saveNow, saveDir, saveSuffix]() {
                    const float *src = ctrl ? ctrl->snapshotBuffer(idx) : nullptr;
                    if (!src) {
                        if (ctrl) ctrl->releaseSnapshotBuffer(idx);
                        return;
                    }
                    std::memcpy(frame->data(), src, frame->size() * sizeof(float));
                    if (ctrl) ctrl->releaseSnapshotBuffer(idx);
                    const QImage img1 = ImagingDisplayWindow::renderFrame(
                        frame->data(), nx, r1.lower, r1.upper);
                    const QImage img2 = ImagingDisplayWindow::renderFrame(
                        frame->data() + n, nx, r2.lower, r2.upper);
                    if (!win) return;
                    // 回投 UI 线程：仅贴图与（圈末）保存
                    QMetaObject::invokeMethod(qApp, [win, frame, nx, frameIdx, img1, img2,
                                                     saveNow, saveDir, saveSuffix]() {
                        if (!win) return;
                        win->applyRingFrame(frame, nx, frameIdx, img1, img2);
                        if (saveNow)
                            win->saveWindowPngs(saveDir, saveSuffix, win->lastSeq());
                    }, Qt::QueuedConnection);
                });
            } else {
                m_imagingController->releaseSnapshotBuffer(bufferIndex);
            }
        } else {
            m_imagingController->releaseSnapshotBuffer(bufferIndex);
        }
    });
    connect(m_imagingController, &ImagingController::svcError,
            this, &MainWindow::onImagingError);
    connect(m_imagingController, &ImagingController::svcStatus,
            this, [this](const QString &status, float fps) {
        if (status == "running") {
            // 更新进度指示显示帧率（线性/环形只读反馈格式一致）
            if (m_lblImagingStatus && m_imagingEnabled) {
                if (m_imagingController && m_imagingController->isRingMode()) {
                    // 环形模式：按组包块进度实时反馈（帧周期较长，不能等整帧）
                    updateRingImagingStatus();
                } else {
                    int total = m_imagingScanParams.move_aline;
                    int progress = m_imagingPulseCount % total;
                    if (progress == 0) progress = total;
                    m_lblImagingStatus->setText(
                        QString("采集脉冲 %1/%2 · 输出 %3 帧 · %4 fps")
                        .arg(progress).arg(total)
                        .arg(m_roundUi.frameCount())
                        .arg(fps, 0, 'f', 1));
                }
            }
        } else {
            logMessage(QString("[成像] %1").arg(status));
        }
    });

    // 阶段B：环形真实采集组包器（UDP 频率数据 → 环形块 → 重建）
    m_imagingBypass = std::make_unique<ImagingBypass>(256);
    m_imagingBypass->start();
    m_ringAssembler = new RingBlockAssembler();
    // PhysicalRoundNormalizer owns the production physical-idle boundary.
    // Keep the assembler's direct timeout path available for its standalone
    // tests, but prevent a second production reset for the same idle gap.
    m_ringAssembler->setTimeoutManagedExternally(true);
    m_ringAssembler->setBlockCallback(
        [this](std::vector<float> &&raw, std::vector<float> &&angles,
               std::vector<uint8_t> &&channels, int blockSeq) {
            // 回调运行在独立成像 worker；只提交成像服务并更新 per-round 统计。
            m_roundUi.onBlock();
            const int alines = static_cast<int>(channels.size());
            QVector<float> rawQ(raw.begin(), raw.end());
            QVector<float> angQ(angles.begin(), angles.end());
            QVector<quint8> chQ(channels.begin(), channels.end());
            bool submitted = false;
            std::uint64_t submitIndex = 0;
            try {
                submitted = m_imagingController && m_imagingServiceReady.load(std::memory_order_acquire)
                    && m_imagingController->submitRingBlock(
                        rawQ, angQ, chQ, blockSeq, &submitIndex);
                // submit_index is the producer identity domain. Record it
                // even when dontwait send returned false: the missing
                // notification is a gap, never an attempt-count alias.
                if (submitIndex != 0)
                    m_roundUi.recordSubmitIndex(submitIndex);
                if (m_imagingBypass) m_imagingBypass->observeBlockResult(submitted);
            } catch (...) {
                if (m_imagingBypass) m_imagingBypass->observeBlockException();
            }
            Q_UNUSED(alines);
        });
    m_ringAssembler->setProgressCallback([this]() {
        m_ringTimeoutSaveDone.store(false, std::memory_order_release);
    });
    // PhysicalRoundNormalizer is the production source of physical idle
    // boundaries. This callback clears queued pre-boundary frames and resets
    // Ring/reconstruction exactly once for each session/generation.
    m_ringAssembler->setTimeoutCallback([this]() {
        bool sent = false;
        if (m_imagingController && m_imagingServiceReady.load(std::memory_order_acquire))
            sent = m_imagingController->sendRingReset();
        m_ringResetCommandSent.store(sent, std::memory_order_release);
    });
    m_imagingBypass->setConsumer([this](const TriggerGroupConstPtr& frame,
                                        const std::array<bool, 8>& enabled) {
        if (!frame || !m_ringAssembler || !m_ringAssemblerConfigured) return false;
        std::lock_guard<std::mutex> assemblerLock(m_ringAssemblerMutex);
        if (!m_ringAssemblerConfigured) return false;
        const int chA = frame->cardId * 2;
        const int chB = chA + 1;
        bool fed = false;
        if (enabled[chA])
            m_ringAssembler->pushChannelLine(chA, frame->triggerSeq, frame->freqA.data(),
                                              static_cast<int>(frame->freqA.size()));
        fed = fed || enabled[chA];
        if (enabled[chB])
            m_ringAssembler->pushChannelLine(chB, frame->triggerSeq, frame->freqB.data(),
                                             static_cast<int>(frame->freqB.size()));
        fed = fed || enabled[chB];
        const bool pendingForFrame =
            m_ringBoundaryPending &&
            frame->measurementSession == m_ringBoundarySession &&
            frame->triggerSeq == m_ringBoundaryTrigger &&
            (m_ringBoundaryGeneration == 0 ||
             frame->roundGeneration + 1 == m_ringBoundaryGeneration);
        if (fed && frame->roundComplete && frame->normalizationApplied &&
            frame->physicalDecision == paimage::PhysicalTriggerDecision::LogicalScan &&
            !pendingForFrame &&
            (!m_ringBoundaryApplied ||
             frame->measurementSession != m_lastRingBoundarySession ||
             frame->roundGeneration + 1 != m_lastRingBoundaryGeneration ||
             frame->triggerSeq != m_lastRingBoundaryTrigger)) {
            // Fallback for an adapter that supplies the group marker without
            // the boundary observer. Production normally arrives here with
            // the observer-created pending boundary already installed.
            m_ringBoundaryPending = true;
            m_ringBoundarySession = frame->measurementSession;
            m_ringBoundaryGeneration = frame->roundGeneration + 1;
            m_ringBoundaryTrigger = frame->triggerSeq;
            m_ringBoundaryCards = 0;
        }
        if (fed && (pendingForFrame ||
                    (m_ringBoundaryPending &&
                     frame->measurementSession == m_ringBoundarySession &&
                     frame->triggerSeq == m_ringBoundaryTrigger &&
                     (m_ringBoundaryGeneration == 0 ||
                      frame->roundGeneration + 1 == m_ringBoundaryGeneration)))) {
            const uint32_t expectedCards =
                (enabled[0] || enabled[1] ? 1u : 0u) |
                (enabled[2] || enabled[3] ? 2u : 0u) |
                (enabled[4] || enabled[5] ? 4u : 0u) |
                (enabled[6] || enabled[7] ? 8u : 0u);
            m_ringBoundaryCards |= 1u << frame->cardId;
            if (expectedCards != 0 &&
                (m_ringBoundaryCards & expectedCards) == expectedCards &&
                m_ringAssembler->completeLogicalRound()) {
                m_ringBoundaryPending = false;
                m_ringBoundaryCards = 0;
                m_ringBoundaryApplied = true;
                m_lastRingBoundarySession = frame->measurementSession;
                m_lastRingBoundaryGeneration = m_ringBoundaryGeneration;
                m_lastRingBoundaryTrigger = frame->triggerSeq;
            }
        }
        return true;
    });
    connect(m_imagingController, &ImagingController::svcStopped, this, [this]() {
        m_imagingServiceReady.store(false, std::memory_order_release);
        m_ringSnapshotAdmissionBlocked.store(true, std::memory_order_release);
        m_ringResetCommandSent.store(false, std::memory_order_release);
        if (m_imagingBypass) m_imagingBypass->setServiceReady(false);
        m_ringAssemblerConfigured = false;
        { std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
          if (m_ringAssembler) m_ringAssembler->reset(); }
        if (m_imagingController && m_imagingController->isRingMode())
            m_imagingTimer->stop();

        if (m_restartRingOnSvcStop) {
            // 运行中修改了环形参数：停止完成后用新参数自动重启，保持实时成像开启
            m_restartRingOnSvcStop = false;
            if (m_imagingController) m_imagingController->startSvc();
            return;
        }

        if (m_chkRealtimeImaging) {
            QSignalBlocker blocker(m_chkRealtimeImaging);
            m_chkRealtimeImaging->setChecked(false);
        }
        setImagingParamControlsEnabled(true);   // 服务已完全停止：恢复成像参数/采集控制
    });
    connect(m_imagingController, &ImagingController::ringConfigChangedWhileRunning,
            this, [this]() {
        if (!m_imagingController || !m_imagingController->isRingMode()) return;
        m_restartRingOnSvcStop = true;
        // 先停馈送线程并清空组包器，避免重启期间继续提交旧尺寸的块
        stopRingFeedWorker();
        m_ringAssemblerConfigured = false;
        { std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
          if (m_ringAssembler) m_ringAssembler->reset(); }
        m_imagingController->stopSvc();
    });
    connect(m_imagingController, &ImagingController::svcReady, this, [this]() {
        m_imagingServiceReady.store(true, std::memory_order_release);
        m_ringSnapshotAdmissionBlocked.store(false, std::memory_order_release);
        if (m_imagingBypass) m_imagingBypass->setServiceReady(true);
        // 环形模式：配置组包器并启动独立馈送工作线程（不再依赖主线程 5ms 定时器）
        if (m_imagingController && m_imagingController->isRingMode()) {
            configureRingAssembler();
            startRingFeedWorker();
        }
        if (m_chkRealtimeImaging) {
            QSignalBlocker blocker(m_chkRealtimeImaging);
            m_chkRealtimeImaging->setChecked(m_imagingController->isRunning());
        }
    });

    connect(m_statsTimer, &QTimer::timeout, this, &MainWindow::onUpdateStatistics);
    m_statsTimer->start(2000);
    m_diagnosticStatusTimer->setInterval(1000);
    connect(m_diagnosticStatusTimer, &QTimer::timeout,
            this, &MainWindow::onDiagnosticStatusTick);
    m_diagnosticStatusTimer->start();
    m_systemCaptureStatusTimer->setInterval(500);
    connect(m_systemCaptureStatusTimer,&QTimer::timeout,this,&MainWindow::onSystemCaptureStatusTick);
    m_systemCaptureStatusTimer->start();

    // 超时到点检测只负责在没有下一触发时落盘；边界分类和 Ring reset
    // 仍由 PhysicalRoundNormalizer 的生产链路负责。
    m_ringTimeoutTimer->setInterval(250);
    connect(m_ringTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_imagingEnabled || !m_netController ||
            !m_reconSaveEnabled || m_reconSaveDir.isEmpty())
            return;
        const auto round = m_netController->physicalRoundSnapshot();
        if (round.timeoutResetNs <= 0 ||
            round.state != paimage::PhysicalRoundState::CollectingScan ||
            !round.hasLastDistinctTrigger || round.lastDistinctTriggerTimeNs <= 0)
            return;
        const auto nowNs = paimage::SocketReceiver::now();
        if (nowNs < round.lastDistinctTriggerTimeNs ||
            nowNs - round.lastDistinctTriggerTimeNs < round.timeoutResetNs)
            return;
        if (!m_ringTimeoutSaveDone.exchange(true, std::memory_order_acq_rel)) {
            if (m_imagingDisplayWindow)
                m_imagingDisplayWindow->saveWindowPngs(
                    m_reconSaveDir, m_reconSaveSuffix,
                    m_imagingDisplayWindow->lastSeq());
        }
    });
    m_ringTimeoutTimer->start();

    // ══ 成像馈送定时器（5ms，独立于 33ms 显示刷新，确保捕获 100Hz 触发）══
    m_imagingTimer = new QTimer(this);
    m_imagingTimer->setInterval(5);
    connect(m_imagingTimer, &QTimer::timeout, this, &MainWindow::feedImagingPulse);

    connect(m_displayTimer, &QTimer::timeout, this, &MainWindow::onDisplayRefresh);
    // displayTimer 延迟到 showEvent 中启动，确保OpenGL初始化完成后再开始数据刷新

    logMessage("系统初始化完成");

    // 窗口几何尺寸恢复
    {
        QSettings s(paimageSettingsPath(), QSettings::IniFormat);
        if (s.contains("Window/X")) {
            int wx = s.value("Window/X", 100).toInt();
            int wy = s.value("Window/Y", 100).toInt();
            int ww = s.value("Window/W", 1600).toInt();
            int wh = s.value("Window/H", 900).toInt();
            setGeometry(wx, wy, ww, wh);
            logMessage(QString("恢复窗口几何: %1,%2 %3x%4").arg(wx).arg(wy).arg(ww).arg(wh));
        } else {
            resize(1600, 900);
            logMessage("首次运行，使用默认窗口尺寸 1600x900");
        }
    }
}

MainWindow::~MainWindow()
{
    invalidateSystemCaptureRequest();
    if (m_imagingBypass) m_imagingBypass->stop();

    // 关闭成像子进程
    if (m_imagingController) {
        m_imagingController->stopSvc();
        delete m_imagingController;
        m_imagingController = nullptr;
    }

    if (m_ringConfigDialog) {
        delete m_ringConfigDialog;
        m_ringConfigDialog = nullptr;
    }
    if (m_imagingDisplayWindow) {
        delete m_imagingDisplayWindow;
        m_imagingDisplayWindow = nullptr;
    }
    delete m_ringAssembler;
    m_ringAssembler = nullptr;

    // saveSettings() 已在 closeEvent 中调用，析构时不重复保存
    if (m_netController) {
        m_netController->setParent(nullptr);
        m_netController->deleteLater();
        m_netController = nullptr;
    }
    delete ui;
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);

    // 图表统一软件渲染：视口由 QCustomPlot::resizeEvent 自动维护、画面随 paintEvent 刷新，
    // 无需逐图 processEvents/msleep 同步（旧版 OpenGL 初始化曾在每次显示时阻塞 UI 线程）；
    // 仅在第一次显示时启动显示定时器
    if (!m_openglInitialized) {
        m_openglInitialized = true;
        m_displayTimer->start(DISPLAY_REFRESH_MS);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 窗口大小变化时同步图表几何布局，确保OpenGL视口正确更新
    QTimer::singleShot(0, this, [this]() {
        syncVisiblePlotsGeometry();
    });
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    
    // 处理窗口状态变化（最小化/恢复）
    if (event->type() == QEvent::WindowStateChange) {
        QWindowStateChangeEvent *stateEvent = static_cast<QWindowStateChangeEvent*>(event);
        
        // 如果窗口从最小化恢复
        if (!(stateEvent->oldState() & Qt::WindowMinimized) && 
            (windowState() & Qt::WindowMinimized)) {
            // 窗口正在最小化，不需要处理
        } else if ((stateEvent->oldState() & Qt::WindowMinimized) && 
                   !(windowState() & Qt::WindowMinimized)) {
            // 窗口从最小化恢复，强制同步所有图表的几何布局
            QTimer::singleShot(0, this, [this]() {
                QTimer::singleShot(0, this, [this]() {
                    syncVisiblePlotsGeometry();
                });
            });
        }
    }
}

// =====================================================================
// setupUI — 只负责不随 nCards 变化的静态部件（GroupTabBar、colorMap容器等）
// =====================================================================
void MainWindow::setupUI()
{
    // 颜色图页
    m_colorMapPlot = new QCustomPlot();
    m_colorMapPlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_colorMapPlot->setMinimumSize(0, 0);
    ui->pageColorMap->layout()->addWidget(m_colorMapPlot);

    // ══ 成像控制：两个独立按钮 + 状态 label，各占一行 ════════════════
    m_btnImagingConfig = new QPushButton("成像参数");
    m_btnImagingConfig->setObjectName("btnImagingConfig");
    m_btnImagingConfig->setMinimumWidth(100);
    m_btnImagingConfig->setAccessibleName("成像参数");

    m_lblImagingStatus = new QLabel("成像就绪");
    m_lblImagingStatus->setObjectName("lblImagingStatus");
    m_lblImagingStatus->setAccessibleName("成像状态");
    m_lblImagingStatus->setStyleSheet(
        "color: #888888; background: transparent; padding: 2px 8px;"
        "font-size: 12px;");

    // 实时成像分组框：成像方式 + 实时成像勾选框 + 成像参数按钮 + 状态
    m_cmbImagingMode = ui->cmbImagingMode;
    m_cmbImagingMode->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_cmbImagingMode->setMinimumWidth(96);
    m_chkRealtimeImaging = ui->chkRealtimeImaging;

    // 将动态创建的成像控件加入“实时成像”分组框
    if (ui->grpImaging) {
        QGridLayout *gl = qobject_cast<QGridLayout*>(ui->grpImaging->layout());
        if (gl) {
            int nextRow = gl->rowCount();   // 0=成像方式行, 1=实时成像行
            gl->addWidget(m_btnImagingConfig, nextRow - 1, 1, 1, 1);
            gl->addWidget(m_lblImagingStatus, nextRow,     0, 1, 2);
        }
    }
    m_grpImaging = ui->grpImaging;

    // 连接按钮/勾选框/成像方式信号
    connect(m_btnImagingConfig, &QPushButton::clicked,
            this, &MainWindow::onImagingConfigClicked);
    connect(m_chkRealtimeImaging, &QCheckBox::toggled,
            this, &MainWindow::onRealtimeImagingToggled);
    connect(m_cmbImagingMode, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onImagingModeChanged);

    // 工作八：运行日志框可拖拽调整高度，其余分组框整体随动适应
    if (ui->leftPanelLayout) {
        QWidget *topContainer = new QWidget(ui->leftPanel);
        topContainer->setObjectName("leftTopContainer");
        QVBoxLayout *topLayout = new QVBoxLayout(topContainer);
        topLayout->setContentsMargins(0, 0, 0, 0);
        QList<QWidget*> topWidgets;
        while (ui->leftPanelLayout->count() > 0) {
            QLayoutItem *item = ui->leftPanelLayout->takeAt(0);
            if (QWidget *w = item->widget()) {
                if (w != ui->grpLog) topWidgets.append(w);
            }
            delete item;
        }
        for (QWidget *w : topWidgets) topLayout->addWidget(w);
        // 其余分组框保持自然高度，多余空间由底部空白吸收，不随日志框伸缩
        topLayout->addStretch(1);
        QSplitter *logSplitter = new QSplitter(Qt::Vertical, ui->leftPanel);
        logSplitter->setObjectName("logSplitter");
        logSplitter->addWidget(topContainer);
        logSplitter->addWidget(ui->grpLog);
        logSplitter->setStretchFactor(0, 0);
        logSplitter->setStretchFactor(1, 1);
        logSplitter->setSizes({ 520, 200 });
        ui->leftPanelLayout->addWidget(logSplitter);
    }
    // 日志区域最小高度：确保默认显示至少6行日志
    if (ui->grpLog) {
        ui->grpLog->setMinimumHeight(100);
    }

    // 工作七优化：左侧交互面板独立滚动，右侧信号显示区自适应窗口
    if (ui->centralwidget && ui->leftPanel) {
        QHBoxLayout *hl = qobject_cast<QHBoxLayout*>(ui->centralwidget->layout());
        QScrollArea *leftScroll = new QScrollArea(ui->centralwidget);
        leftScroll->setObjectName("leftScrollArea");
        leftScroll->setWidgetResizable(true);
        leftScroll->setFrameShape(QFrame::NoFrame);
        leftScroll->setMinimumWidth(300);
        leftScroll->setMaximumWidth(300);
        // 左栏宽度固定，无需横向滚动
        leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        int idx = hl ? hl->indexOf(ui->leftPanel) : -1;
        if (hl && idx >= 0) {
            hl->removeWidget(ui->leftPanel);
            hl->insertWidget(idx, leftScroll);
        }
        leftScroll->setWidget(ui->leftPanel);
    }

    ui->spnRefreshRate->setAlignment(Qt::AlignLeft);
    ui->spnDownsampleRatio->setAlignment(Qt::AlignLeft);

    // GroupTabBar 占位（rebuildDynamicUI 会重建 tab 数量）
    m_groupTabBar = new QTabBar();
    m_groupTabBar->setExpanding(false);
    m_groupTabBar->setDrawBase(false);
    m_groupTabBar->setMinimumHeight(28);  // QTabBar 默认高度约 28-30px
    QHBoxLayout *groupBarLayout = qobject_cast<QHBoxLayout*>(ui->groupTabBarContainer->layout());
    if (groupBarLayout) {
        groupBarLayout->addWidget(m_groupTabBar);
    }
    m_placeholderPage = nullptr;
}

// =====================================================================
// rebuildDynamicUI — 根据 m_nCards 重建分组 Tab、图表、统计栏
// 监听期间不允许调用（isListening 时 spnNCards 已被禁用）
// =====================================================================
void MainWindow::rebuildDynamicUI()
{
    // ── 1. 清理旧的 Tab 和图表 ────────────────────────────────
    while (ui->tabWidget->count() > 0)
        ui->tabWidget->removeTab(0);

    // 清理旧 tile containers（从 grid 布局移除）
    for (int i = 0; i < MAX_CARDS; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (m_tileContainers[i][j]) {
                ui->gridTileLayout->removeWidget(m_tileContainers[i][j]);
                m_tileContainers[i][j]->deleteLater();
                m_tileContainers[i][j] = nullptr;
            }
            // StackedWidget 若还在 tabWidget 里会被 removeTab 删除，这里只重置指针
            m_stackedWidgets[i][j] = nullptr;
            m_plotsPhase[i][j]     = nullptr;
            m_plotsFrequency[i][j] = nullptr;
            m_plotsSpectrum[i][j]  = nullptr;
            m_pkpkLabels[i][j]     = nullptr;
        }
    }

    // ── 2. 清理旧的 GroupTabBar ────────────────────────────────
    while (m_groupTabBar->count() > 0)
        m_groupTabBar->removeTab(0);

    // ── 3. 计算分组参数 ────────────────────────────────────────
    m_numGroups = (m_nCards + CARDS_PER_DISPLAY_GROUP - 1) / CARDS_PER_DISPLAY_GROUP;

    // 只有超过 1 个分组时才显示分组 Tab 栏
    if (ui->groupTabBarContainer) {
        bool showGroups = (m_numGroups > 1);
        ui->groupTabBarContainer->setVisible(showGroups);
        // 显式调用 show/hide 确保布局立即生效（setVisible 可能被布局缓存延迟应用）
        if (showGroups) {
            ui->groupTabBarContainer->show();
            m_groupTabBar->show();
        } else {
            ui->groupTabBarContainer->hide();
        }
        logMessage(QString("[分组] nCards=%1 groups=%2 show=%3")
                   .arg(m_nCards).arg(m_numGroups).arg(showGroups));
    }

    // ── 4. 为当前组的 4 张（或更少）卡创建 Tab + 图表 ────────────
    // 这里只为当前可见组（m_currentGroup）创建 Tab 和图表
    // 切组时由 updateGroupDisplay() 重新绑定（复用同一批图表，只更新标题）
    // 注意：为了简化内存管理，始终为最多 CARDS_PER_DISPLAY_GROUP 张卡创建图表槽位
    for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
        for (int ch = 0; ch < 2; ++ch) {
            int globalCard = m_currentGroup * CARDS_PER_DISPLAY_GROUP + card;
            bool cardExists = (globalCard < m_nCards);
            QString tabName = cardExists
                ? QString("卡%1-通道%2").arg(globalCard + 1).arg(ch == 0 ? 'A' : 'B')
                : QString("--");

            // Tab 页
            QWidget *tabPage = new QWidget();
            QVBoxLayout *tabLayout = new QVBoxLayout(tabPage);
            tabLayout->setContentsMargins(0, 0, 0, 0);

            QStackedWidget *stack = new QStackedWidget();
            stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            m_stackedWidgets[card][ch] = stack;

            // pk-pk 标签（放 stacked 外部，相位/频率模式均可见）
            m_pkpkLabels[card][ch] = new QLabel(QString("峰峰值: --"));
            m_pkpkLabels[card][ch]->setAlignment(Qt::AlignCenter);
            m_pkpkLabels[card][ch]->setFixedHeight(24);
            m_pkpkLabels[card][ch]->setProperty("pkpkLabel", true);

            // 第0页：相位图
            m_plotsPhase[card][ch] = new QCustomPlot();
            stack->addWidget(m_plotsPhase[card][ch]);

            // 第1页：瞬时频率
            QWidget *freqPage = new QWidget();
            freqPage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            QVBoxLayout *freqLayout = new QVBoxLayout(freqPage);
            freqLayout->setContentsMargins(2, 2, 2, 2);
            freqLayout->setSpacing(2);
            m_plotsFrequency[card][ch] = new QCustomPlot();
            freqLayout->addWidget(m_plotsFrequency[card][ch], 1);
            // 瞬时频率页：上方时域瞬时频率，下方频域幅值谱
            m_plotsSpectrum[card][ch] = new QCustomPlot();
            freqLayout->addWidget(m_plotsSpectrum[card][ch], 1);
            stack->addWidget(freqPage);

            tabLayout->addWidget(m_pkpkLabels[card][ch]);
            tabLayout->addWidget(stack, 1);
            ui->tabWidget->addTab(tabPage, tabName);

            // 平铺容器
            QWidget *tileContainer = new QWidget();
            QVBoxLayout *tileLayout = new QVBoxLayout(tileContainer);
            tileLayout->setSpacing(0);
            tileLayout->setContentsMargins(1, 1, 1, 1);

            QLabel *titleLabel = new QLabel(tabName);
            titleLabel->setAlignment(Qt::AlignCenter);
            titleLabel->setProperty("tileTitle", true);
            tileLayout->addWidget(titleLabel);
            tileLayout->addStretch();

            int row = (card * 2 + ch) / 4;
            int col = (card * 2 + ch) % 4;
            ui->gridTileLayout->addWidget(tileContainer, row, col);
            m_tileContainers[card][ch] = tileContainer;

            // 若该卡不存在（当前组不足4张），隐藏 Tab
            if (!cardExists) {
                ui->tabWidget->setTabVisible(card * 2 + ch, false);
                tileContainer->setVisible(false);
            }

            m_firstPlot[card][ch] = true;
        }
    }

    // ── 5. 重建 GroupTabBar ──────────────────────────────────
    for (int g = 0; g < m_numGroups; ++g) {
        int firstCard = g * CARDS_PER_DISPLAY_GROUP + 1;
        int lastCard  = std::min(firstCard + CARDS_PER_DISPLAY_GROUP - 1, m_nCards);
        m_groupTabBar->addTab(QString("第%1组").arg(g + 1));
        m_groupTabBar->setTabToolTip(g,
            QString("第%1组：卡%2 ~ 卡%3").arg(g + 1).arg(firstCard).arg(lastCard));
    }
    // 确保 GroupTabBar 本身可见
    m_groupTabBar->setVisible(m_numGroups > 0);

    // 窗口显示后修复 QTabBar 高度（容器可见但 tab 栏可能高度为 0）
    if (m_numGroups > 1 && ui->groupTabBarContainer) {
        QTimer::singleShot(100, this, [this]() {
            if (m_groupTabBar->height() < 10) {
                m_groupTabBar->setMinimumHeight(28);
                m_groupTabBar->updateGeometry();
                m_groupTabBar->adjustSize();
            }
            ui->groupTabBarContainer->setVisible(true);
        });
    }

    // ── 6. 初始化图表样式（添加 graph、设置右键菜单等）──────────
    setupPlots();

    // ── 7. 重置当前组（防越界）并刷新显示 ──────────────────────
    if (m_currentGroup >= m_numGroups) m_currentGroup = 0;
    m_groupTabBar->setCurrentIndex(m_currentGroup);
    updateGroupDisplay(m_currentGroup);

    // ── 7.5 同步当前显示模式到新建的 stackedWidget ──────────────
    // loadSettings() 调用 onDisplayTypeChanged() 时 stackedWidget 还未创建，
    // 重建 UI 后必须重新应用一次，否则频率模式下 stack 停在相位页导致图表不可见
    onDisplayTypeChanged(ui->cmbDisplayType->currentIndex());
    onImagingModeChanged(ui->cmbImagingMode->currentIndex());

    // 平铺模式激活时：重建后重新应用平铺布局（新建 stack 需挂入 tile 容器，
    // 否则平铺页只剩空容器），并同步频域信号可见性（频域仅单通道窗口显示）
    if (m_isTileView)
        onTileViewToggled(true);

    // ── 8. 更新网络信息标签 ────────────────────────────────────
    updateNetworkInfoLabels();

    // ── 9. 重置显示计数器 ─────────────────────────────────────
    for (int i = 0; i < MAX_CARDS; ++i) {
        m_displayCounter[i] = 0;
        for (int j = 0; j < 2; ++j)
            m_firstPlot[i][j] = true;
    }
}

// =====================================================================
// setupPlots — 初始化当前可见的 CARDS_PER_DISPLAY_GROUP 个图表样式
// =====================================================================
void MainWindow::setupPlots()
{
    // 应用当前图表的轴范围到所有同类图表，同时同步自适应坐标轴标志
    auto applyRangeToAll = [this](QCustomPlot *src, bool isFreq) {
        if (!src || !src->xAxis || !src->yAxis) return;
        QCPRange xr = src->xAxis->range();
        QCPRange yr = src->yAxis->range();
        // 查找当前图的 card/ch 索引
        int srcCard = -1, srcCh = -1;
        for (int c = 0; c < CARDS_PER_DISPLAY_GROUP && srcCard < 0; ++c) {
            for (int h = 0; h < 2; ++h) {
                QCustomPlot *p = isFreq ? m_plotsFrequency[c][h] : m_plotsPhase[c][h];
                if (p == src) { srcCard = c; srcCh = h; break; }
            }
        }
        for (int c = 0; c < CARDS_PER_DISPLAY_GROUP; ++c) {
            for (int h = 0; h < 2; ++h) {
                QCustomPlot *p = isFreq ? m_plotsFrequency[c][h] : m_plotsPhase[c][h];
                if (p && p != src && p->xAxis && p->yAxis) {
                    p->xAxis->setRange(xr);
                    p->yAxis->setRange(yr);
                    // 同步自适应坐标轴标志
                    if (srcCard >= 0 && srcCh >= 0)
                        m_autoRescalePlot[c][h] = m_autoRescalePlot[srcCard][srcCh];
                    p->replot(QCustomPlot::rpQueuedReplot);
                }
            }
        }
    };

    // 频率图/频域图共用右键菜单（与时域窗口一致：还原/自适应/设置范围/复制/应用到全部）
    auto attachPlotContextMenu = [this](QCustomPlot *plot, int card, int ch, bool isFreq) {
        plot->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(plot, &QWidget::customContextMenuRequested, this,
                [this, plot, card, ch, isFreq](const QPoint &pos) {
            bool &autoFlag = isFreq
                ? m_autoRescalePlot[card][ch]
                : m_autoRescaleSpectrum[card][ch];
            QCPRange &savedX = isFreq
                ? m_savedXRange[card][ch]
                : m_savedSpectrumXRange[card][ch];
            QCPRange &savedY = isFreq
                ? m_savedYRange[card][ch]
                : m_savedSpectrumYRange[card][ch];
            QMenu menu(plot);
            QAction *actRestore = menu.addAction("还原视图");
            QAction *actAutoRescale = menu.addAction("自适应坐标轴");
            actAutoRescale->setCheckable(true);
            actAutoRescale->setChecked(autoFlag);
            menu.addSeparator();
            QAction *actSetRange = menu.addAction("设置坐标范围...");
            menu.addSeparator();
            QAction *actCopyClip = menu.addAction("复制到剪贴板");
            menu.addSeparator();
            QAction *actApplyAll = menu.addAction(isFreq ? "应用到所有频率图" : "应用到所有频域图");
            QAction *selected = menu.exec(plot->mapToGlobal(pos));
            if (selected == actRestore) { plot->rescaleAxes(); plot->replot(); }
            else if (selected == actAutoRescale) {
                bool wasAuto = autoFlag;
                autoFlag = actAutoRescale->isChecked();
                if (autoFlag) { plot->rescaleAxes(); plot->replot(); }
                else if (wasAuto) {
                    plot->xAxis->setRange(savedX);
                    plot->yAxis->setRange(savedY);
                    plot->replot();
                }
            }
            else if (selected == actCopyClip) { QApplication::clipboard()->setPixmap(plot->toPixmap()); }
            else if (selected == actSetRange) {
                if (autoFlag) { autoFlag = false; actAutoRescale->setChecked(false); }
                QDialog dlg(plot);
                dlg.setWindowTitle("设置坐标范围");
                QFormLayout *fl = new QFormLayout(&dlg);
                QDoubleSpinBox *xMin = new QDoubleSpinBox(); xMin->setRange(-1e9, 1e9);
                xMin->setValue(plot->xAxis->range().lower);
                QDoubleSpinBox *xMax = new QDoubleSpinBox(); xMax->setRange(-1e9, 1e9);
                xMax->setValue(plot->xAxis->range().upper);
                QDoubleSpinBox *yMin = new QDoubleSpinBox(); yMin->setRange(-1e9, 1e9);
                yMin->setValue(plot->yAxis->range().lower);
                QDoubleSpinBox *yMax = new QDoubleSpinBox(); yMax->setRange(-1e9, 1e9);
                yMax->setValue(plot->yAxis->range().upper);
                fl->addRow("X 轴最小值:", xMin);
                fl->addRow("X 轴最大值:", xMax);
                fl->addRow("Y 轴最小值:", yMin);
                fl->addRow("Y 轴最大值:", yMax);
                QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
                connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                fl->addRow(bb);
                if (dlg.exec() == QDialog::Accepted) {
                    plot->xAxis->setRange(xMin->value(), xMax->value());
                    plot->yAxis->setRange(yMin->value(), yMax->value());
                    plot->replot();
                    savedX = plot->xAxis->range();
                    savedY = plot->yAxis->range();
                }
            }
            else if (selected == actApplyAll) {
                if (QMessageBox::question(plot, "确认",
                    QString("将当前 X/Y 范围应用到当前组的%1，是否继续？")
                        .arg(isFreq ? "所有频率图" : "所有频域图"),
                    QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                    QCPRange xr = plot->xAxis->range();
                    QCPRange yr = plot->yAxis->range();
                    for (int c = 0; c < CARDS_PER_DISPLAY_GROUP; ++c) {
                        for (int h = 0; h < 2; ++h) {
                            QCustomPlot *p = isFreq ? m_plotsFrequency[c][h] : m_plotsSpectrum[c][h];
                            if (!p || p == plot) continue;
                            p->xAxis->setRange(xr);
                            p->yAxis->setRange(yr);
                            if (isFreq) m_autoRescalePlot[c][h] = autoFlag;
                            else m_autoRescaleSpectrum[c][h] = autoFlag;
                            p->replot(QCustomPlot::rpQueuedReplot);
                        }
                    }
                }
            }
        });
    };

    for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
        for (int ch = 0; ch < 2; ++ch) {
            if (!m_plotsPhase[card][ch] || !m_plotsFrequency[card][ch] ||
                !m_plotsSpectrum[card][ch]) continue;

            // 相位图 — Keysight 风格：深色背景 + 亮青色曲线
            QCustomPlot *phasePlot = m_plotsPhase[card][ch];
            phasePlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            phasePlot->setMinimumSize(0, 0);
            phasePlot->setOpenGl(false);
            phasePlot->addGraph();
            // 曲线关闭抗锯齿 + 1px 画笔：信号铺满图框时（scalebar 小、长线段密集覆盖）
            // 抗锯齿逐像素混合的成本随绘制像素量暴涨，关闭后卡顿显著缓解
            phasePlot->graph(0)->setPen(QPen(QColor(0, 188, 255), 1.0));    // 亮青色 1px
            phasePlot->graph(0)->setAdaptiveSampling(true);
            phasePlot->graph(0)->setAntialiased(false);
            phasePlot->setNotAntialiasedElements(QCP::aeAxes | QCP::aeGrid | QCP::aePlottables);
            phasePlot->xAxis->setLabel("采样点");
            phasePlot->yAxis->setLabel("相位 (rad)");
            phasePlot->xAxis->setLabelColor(QColor(180, 180, 180));
            phasePlot->yAxis->setLabelColor(QColor(180, 180, 180));
            phasePlot->xAxis->setTickLabelColor(QColor(160, 160, 160));
            phasePlot->yAxis->setTickLabelColor(QColor(160, 160, 160));
            phasePlot->xAxis->setBasePen(QPen(QColor(80, 80, 80)));
            phasePlot->yAxis->setBasePen(QPen(QColor(80, 80, 80)));
            phasePlot->xAxis->setTickPen(QPen(QColor(60, 60, 60)));
            phasePlot->yAxis->setTickPen(QPen(QColor(60, 60, 60)));
            phasePlot->xAxis->setSubTickPen(QPen(QColor(90, 90, 90)));
            phasePlot->yAxis->setSubTickPen(QPen(QColor(90, 90, 90)));
            phasePlot->xAxis->setRange(0, 1024);
            phasePlot->yAxis->setRange(-M_PI, M_PI);
            phasePlot->setBackground(QBrush(QColor(30, 30, 30)));
            phasePlot->axisRect()->setBackground(QBrush(QColor(38, 38, 38)));
            // 多层次网格 —— Keysight/R&S 风格
            phasePlot->xAxis->grid()->setVisible(true);
            phasePlot->yAxis->grid()->setVisible(true);
            phasePlot->xAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
            phasePlot->yAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
            phasePlot->xAxis->grid()->setSubGridVisible(false);
            phasePlot->yAxis->grid()->setSubGridVisible(false);
            phasePlot->xAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
            phasePlot->yAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
            phasePlot->xAxis->grid()->setZeroLinePen(QPen(QColor(140, 140, 140), 1.0, Qt::DashLine));
            phasePlot->yAxis->grid()->setZeroLinePen(QPen(QColor(140, 140, 140), 1.0, Qt::DashLine));
            phasePlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
            phasePlot->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(phasePlot, &QWidget::customContextMenuRequested, this, [this, phasePlot, card, ch, applyRangeToAll](const QPoint &pos) {
                QMenu menu(phasePlot);
                QAction *actRestore = menu.addAction("还原视图");
                QAction *actAutoRescale = menu.addAction("自适应坐标轴");
                actAutoRescale->setCheckable(true);
                actAutoRescale->setChecked(m_autoRescalePlot[card][ch]);
                menu.addSeparator();
                QAction *actSetRange = menu.addAction("设置坐标范围...");
                menu.addSeparator();
                QAction *actCopyClip = menu.addAction("复制到剪贴板");
                menu.addSeparator();
                QAction *actApplyAll = menu.addAction("应用到所有相位图");
                QAction *selected = menu.exec(phasePlot->mapToGlobal(pos));
                if (selected == actRestore) { phasePlot->rescaleAxes(); phasePlot->replot(); }
                else if (selected == actAutoRescale) {
                    bool wasAuto = m_autoRescalePlot[card][ch];
                    m_autoRescalePlot[card][ch] = actAutoRescale->isChecked();
                    if (m_autoRescalePlot[card][ch]) {
                        phasePlot->rescaleAxes(); phasePlot->replot();
                    } else if (wasAuto) {
                        phasePlot->xAxis->setRange(m_savedXRange[card][ch]);
                        phasePlot->yAxis->setRange(m_savedYRange[card][ch]);
                        phasePlot->replot();
                    }
                }
                else if (selected == actCopyClip) { QApplication::clipboard()->setPixmap(phasePlot->toPixmap()); }
                else if (selected == actSetRange) {
                    // 自动取消自适应（如果开启），一步弹出对话框
                    if (m_autoRescalePlot[card][ch]) {
                        m_autoRescalePlot[card][ch] = false;
                        actAutoRescale->setChecked(false);
                    }
                    // 设置坐标范围对话框（同时设 X/Y）
                    QDialog dlg(phasePlot);
                    dlg.setWindowTitle("设置坐标范围");
                    QFormLayout *fl = new QFormLayout(&dlg);
                    QDoubleSpinBox *xMin = new QDoubleSpinBox(); xMin->setRange(-1e9, 1e9);
                    xMin->setValue(phasePlot->xAxis->range().lower);
                    QDoubleSpinBox *xMax = new QDoubleSpinBox(); xMax->setRange(-1e9, 1e9);
                    xMax->setValue(phasePlot->xAxis->range().upper);
                    QDoubleSpinBox *yMin = new QDoubleSpinBox(); yMin->setRange(-1e9, 1e9);
                    yMin->setValue(phasePlot->yAxis->range().lower);
                    QDoubleSpinBox *yMax = new QDoubleSpinBox(); yMax->setRange(-1e9, 1e9);
                    yMax->setValue(phasePlot->yAxis->range().upper);
                    fl->addRow("X 轴最小值:", xMin);
                    fl->addRow("X 轴最大值:", xMax);
                    fl->addRow("Y 轴最小值:", yMin);
                    fl->addRow("Y 轴最大值:", yMax);
                    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
                    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                    fl->addRow(bb);
                    if (dlg.exec() == QDialog::Accepted) {
                        phasePlot->xAxis->setRange(xMin->value(), xMax->value());
                        phasePlot->yAxis->setRange(yMin->value(), yMax->value());
                        phasePlot->replot();
                        m_savedXRange[card][ch] = phasePlot->xAxis->range();
                        m_savedYRange[card][ch] = phasePlot->yAxis->range();
                    }
                }
                else if (selected == actApplyAll) {
                    if (QMessageBox::question(phasePlot, "确认",
                        "将当前 X/Y 范围应用到当前组的所有相位图，是否继续？",
                        QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                        applyRangeToAll(phasePlot, false);
                    }
                }
            });
            phasePlot->replot();
            attachAxisEdit(phasePlot, card, ch, false);   // 坐标轴数字可编辑

            // 时域/频域双窗口统一样式（深色背景 + 统一坐标轴/网格配色）。
            // 两图均强制软件渲染：瞬时频率图若启用 OpenGL，其缓冲可能超出
            // 控件区域、延伸到下方频域窗口并被其遮挡，导致时域窗口下半不可见。
            auto applySignalWindowStyle = [](QCustomPlot *plot, const QColor &curveColor) {
                plot->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                plot->setMinimumSize(0, 0);
                plot->setOpenGl(false);
                plot->addGraph();
                // 曲线关闭抗锯齿 + 1px 画笔：绘制成本随被绘制像素量线性下降，
                // scalebar 小时信号铺满图框的密集绘制不再拖慢刷新
                plot->graph(0)->setPen(QPen(curveColor, 1.0));
                plot->graph(0)->setAdaptiveSampling(true);
                plot->graph(0)->setAntialiased(false);
                plot->setNotAntialiasedElements(QCP::aeAxes | QCP::aeGrid | QCP::aePlottables);
                plot->xAxis->setLabelColor(QColor(180, 180, 180));
                plot->yAxis->setLabelColor(QColor(180, 180, 180));
                plot->xAxis->setTickLabelColor(QColor(160, 160, 160));
                plot->yAxis->setTickLabelColor(QColor(160, 160, 160));
                plot->xAxis->setBasePen(QPen(QColor(80, 80, 80)));
                plot->yAxis->setBasePen(QPen(QColor(80, 80, 80)));
                plot->xAxis->setTickPen(QPen(QColor(60, 60, 60)));
                plot->yAxis->setTickPen(QPen(QColor(60, 60, 60)));
                plot->xAxis->setSubTickPen(QPen(QColor(90, 90, 90)));
                plot->yAxis->setSubTickPen(QPen(QColor(90, 90, 90)));
                plot->setBackground(QBrush(QColor(30, 30, 30)));
                plot->axisRect()->setBackground(QBrush(QColor(38, 38, 38)));
                plot->xAxis->grid()->setVisible(true);
                plot->yAxis->grid()->setVisible(true);
                plot->xAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
                plot->yAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
                plot->xAxis->grid()->setSubGridVisible(false);
                plot->yAxis->grid()->setSubGridVisible(false);
                plot->xAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
                plot->yAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
                plot->xAxis->grid()->setZeroLinePen(QPen(QColor(140, 140, 140), 1.0, Qt::DashLine));
                plot->yAxis->grid()->setZeroLinePen(QPen(QColor(140, 140, 140), 1.0, Qt::DashLine));
                plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
            };

            // 上方时域瞬时频率图 — 暖橙曲线
            QCustomPlot *freqPlot = m_plotsFrequency[card][ch];
            applySignalWindowStyle(freqPlot, QColor(255, 170, 0));
            freqPlot->xAxis->setLabel("采样点");
            freqPlot->yAxis->setLabel("瞬时频率 (kHz)");
            freqPlot->xAxis->setRange(0, 1024);
            freqPlot->yAxis->setRange(0, 500);
            attachPlotContextMenu(freqPlot, card, ch, true);
            freqPlot->replot();
            attachAxisEdit(freqPlot, card, ch, false);     // 坐标轴数字可编辑

            // 下方频域幅值谱图 — 亮绿色曲线，样式与上方完全统一
            QCustomPlot *specPlot = m_plotsSpectrum[card][ch];
            applySignalWindowStyle(specPlot, QColor(120, 220, 120));
            specPlot->xAxis->setLabel("频率 (MHz)");
            specPlot->yAxis->setLabel("幅值");
            specPlot->xAxis->setRange(0, 1);
            specPlot->yAxis->setRange(0, 1);
            attachPlotContextMenu(specPlot, card, ch, false);
            specPlot->replot();
            attachAxisEdit(specPlot, card, ch, true);      // 坐标轴数字可编辑

            // 时域/频域两图 y 轴对齐：共享左边距组，坐标范围数字位数不同时
            // 信号区（轴矩形）仍保持相同的左边界，纵向排列的曲线区域对齐
            QCPMarginGroup *yMargin = new QCPMarginGroup(freqPlot);
            freqPlot->axisRect()->setMarginGroup(QCP::msLeft, yMargin);
            specPlot->axisRect()->setMarginGroup(QCP::msLeft, yMargin);
        }
    }

    // 颜色图（通道数随 nCards 变化）
    int numChannels = m_nCards * 2;  // 每卡2通道
    int numSamples  = 1024;
    m_colorMapPlot->setOpenGl(false); // 延迟到 showEvent 布局完成后开启
    m_colorMapPlot->setNotAntialiasedElements(QCP::aeAll);
    if (!m_colorMap) {
        m_colorMap = new QCPColorMap(m_colorMapPlot->xAxis, m_colorMapPlot->yAxis);
    }
    m_colorMap->data()->setSize(numSamples, numChannels);
    m_colorMap->data()->setRange(QCPRange(0, numSamples-1), QCPRange(0, numChannels-1));
    for (int x = 0; x < numSamples; ++x)
        for (int y = 0; y < numChannels; ++y)
            m_colorMap->data()->setCell(x, y, 0.0);
    QCPColorGradient gradient;
    gradient.setColorStopAt(0.0, QColor(0, 0, 0));
    gradient.setColorStopAt(1.0, QColor(255, 255, 255));
    // NaN/Inf 帧数据不参与色值映射（避免 QCPColorGradient::colorize 越界）
    gradient.setNanHandling(QCPColorGradient::nhLowestColor);
    m_colorMap->setGradient(gradient);
    m_settingColorRange = true;
    m_colorMap->setDataRange(QCPRange(0, 500));
    m_settingColorRange = false;
    m_colorMap->setInterpolate(true);
    m_colorMap->setTightBoundary(false);
    if (!m_colorScale) {
        m_colorScale = new QCPColorScale(m_colorMapPlot);
        m_colorMapPlot->plotLayout()->addElement(0, 1, m_colorScale);
        m_colorScale->setType(QCPAxis::atRight);
        m_colorScale->setLabel("频率 (kHz)");
        m_colorMap->setColorScale(m_colorScale);
        // setColorScale 会把地图渐变覆盖为色标默认 gpCold，重新应用项目灰度渐变
        m_colorScale->setGradient(gradient);
        m_colorMap->setGradient(gradient);
        // 用户拖拽色条后自动持久化
        connect(m_colorScale->axis(), qOverload<const QCPRange &>(&QCPAxis::rangeChanged),
                this, [this](const QCPRange &newRange) {
            if (m_settingColorRange) return;  // 程序设置，忽略
            if (m_imagingEnabled) {
                m_pixelColorRange = newRange;
            } else {
                m_freqColorRange = newRange;
            }
            saveSettings();
        });
    }
    m_colorMapPlot->xAxis->setLabel("采样点");
    m_colorMapPlot->yAxis->setLabel("通道编号");
    m_colorMapPlot->xAxis->setLabelColor(QColor(180, 180, 180));
    m_colorMapPlot->yAxis->setLabelColor(QColor(180, 180, 180));
    m_colorMapPlot->xAxis->setTickLabelColor(QColor(160, 160, 160));
    m_colorMapPlot->yAxis->setTickLabelColor(QColor(160, 160, 160));
    m_colorMapPlot->xAxis->setBasePen(QPen(QColor(80, 80, 80)));
    m_colorMapPlot->yAxis->setBasePen(QPen(QColor(80, 80, 80)));
    m_colorMapPlot->xAxis->setTickPen(QPen(QColor(60, 60, 60)));
    m_colorMapPlot->yAxis->setTickPen(QPen(QColor(60, 60, 60)));
    m_colorMapPlot->xAxis->setRange(0, numSamples-1);
    m_colorMapPlot->yAxis->setRange(-0.5, numChannels-0.5);
    m_colorScale->axis()->setTickLabelColor(QColor(180, 180, 180));
    m_colorScale->axis()->setBasePen(QPen(QColor(80, 80, 80)));
    m_colorScale->axis()->setLabelColor(QColor(180, 180, 180));
    QVector<double> yTicks;
    QVector<QString> yLabels;
    for (int i = 0; i < numChannels; ++i) {
        yTicks << i;
        yLabels << QString("卡%1-%2").arg(i/2+1).arg(i%2==0?'A':'B');
    }
    QSharedPointer<QCPAxisTickerText> textTicker(new QCPAxisTickerText);
    textTicker->addTicks(yTicks, yLabels);
    m_colorMapPlot->yAxis->setTicker(textTicker);
    m_colorMapPlot->setBackground(QBrush(QColor(30, 30, 30)));
    // 颜色图多层次网格
    m_colorMapPlot->xAxis->grid()->setVisible(true);
    m_colorMapPlot->yAxis->grid()->setVisible(true);
    m_colorMapPlot->xAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
    m_colorMapPlot->yAxis->grid()->setPen(QPen(QColor(85, 85, 85), 0.5, Qt::SolidLine));
    m_colorMapPlot->xAxis->grid()->setSubGridVisible(true);
    m_colorMapPlot->yAxis->grid()->setSubGridVisible(true);
    m_colorMapPlot->xAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
    m_colorMapPlot->yAxis->grid()->setSubGridPen(QPen(QColor(68, 68, 68), 0.5, Qt::DotLine));
    m_colorMapPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // ══ 颜色图右键菜单（Keysight/R&S 风格）══════════════════════════
    m_colorMapPlot->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_colorMapPlot, &QWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) {
        QMenu menu(m_colorMapPlot);

        QAction *actSave = menu.addAction("保存图片...");
        menu.addSeparator();
        QAction *actCopyClip = menu.addAction("复制到剪贴板");
        menu.addSeparator();
        QAction *actSetColorRange = menu.addAction("设置色条范围...");
        QAction *actAutoRange = menu.addAction("自适应数据范围");
        QAction *actResetView = menu.addAction("还原视图");

        QAction *selected = menu.exec(m_colorMapPlot->mapToGlobal(pos));
        if (selected == actSave) {
            QString fn = QFileDialog::getSaveFileName(m_colorMapPlot, "保存图片",
                "imaging.png", "PNG (*.png);;BMP (*.bmp);;JPG (*.jpg)");
            if (!fn.isEmpty()) {
                if (fn.endsWith(".png", Qt::CaseInsensitive)) m_colorMapPlot->savePng(fn);
                else if (fn.endsWith(".bmp", Qt::CaseInsensitive)) m_colorMapPlot->saveBmp(fn);
                else if (fn.endsWith(".jpg", Qt::CaseInsensitive)) m_colorMapPlot->saveJpg(fn);
            }
        } else if (selected == actCopyClip) {
            QApplication::clipboard()->setPixmap(m_colorMapPlot->toPixmap());
        } else if (selected == actSetColorRange) {
            // 设置色条范围对话框
            QDialog dlg(m_colorMapPlot);
            dlg.setWindowTitle("设置色条范围");
            QFormLayout *fl = new QFormLayout(&dlg);
            QDoubleSpinBox *minSp = new QDoubleSpinBox(); minSp->setRange(-1e9, 1e9);
            minSp->setValue(m_colorMap->dataRange().lower);
            QDoubleSpinBox *maxSp = new QDoubleSpinBox(); maxSp->setRange(-1e9, 1e9);
            maxSp->setValue(m_colorMap->dataRange().upper);
            fl->addRow("最小值:", minSp);
            fl->addRow("最大值:", maxSp);
            QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            fl->addRow(bb);
            if (dlg.exec() == QDialog::Accepted) {
                QCPRange manualRng(minSp->value(), maxSp->value());
                m_settingColorRange = true;
                m_colorMap->setDataRange(manualRng);
                m_colorScale->axis()->setRange(manualRng);
                m_settingColorRange = false;
                m_colorMapPlot->replot(QCustomPlot::rpQueuedReplot);
                // 按实际数据显示类型保存（成像中=像素，停止=频率）
                if (m_imagingEnabled) {
                    m_pixelColorRange = manualRng;
                } else {
                    m_freqColorRange = manualRng;
                }
                saveSettings();
            }
        } else if (selected == actAutoRange) {
            if (m_colorMap && m_colorMap->data()) {
                double minVal = std::numeric_limits<double>::max();
                double maxVal = std::numeric_limits<double>::lowest();
                int ksz = m_colorMap->data()->keySize();
                int vsz = m_colorMap->data()->valueSize();
                bool hasFinite = false;
                for (int ky = 0; ky < ksz; ++ky)
                    for (int vz = 0; vz < vsz; ++vz) {
                        double v = m_colorMap->data()->cell(ky, vz);
                        if (std::isfinite(v)) {
                            if (v < minVal) minVal = v;
                            if (v > maxVal) maxVal = v;
                            hasFinite = true;
                        }
                    }
                if (hasFinite && maxVal > minVal) {
                    QCPRange autoRng(minVal, maxVal);
                    m_settingColorRange = true;
                    m_colorMap->setDataRange(autoRng);
                    m_colorScale->axis()->setRange(autoRng);
                    m_settingColorRange = false;
                    m_colorMapPlot->replot(QCustomPlot::rpQueuedReplot);
                    // 按实际数据显示类型保存（成像中=像素，停止=频率）
                    if (m_imagingEnabled) {
                        m_pixelColorRange = autoRng;
                    } else {
                        m_freqColorRange = autoRng;
                    }
                    saveSettings();
                } else {
                    logMessage(QString("[色条] 自适应跳过: hasFinite=%1 min=%2 max=%3")
                               .arg(hasFinite).arg(minVal, 0, 'g', 4).arg(maxVal, 0, 'g', 4));
                }
            }
        } else if (selected == actResetView) {
            if (m_colorMap) {
                int w = m_colorMap->data()->keySize();
                int h = m_colorMap->data()->valueSize();
                m_colorMapPlot->xAxis->setRange(0, w - 1);
                m_colorMapPlot->yAxis->setRange(-0.5, h - 0.5);
                m_colorMapPlot->replot(QCustomPlot::rpQueuedReplot);
            }
        }
    });

    m_colorMapPlot->replot();

    syncVisiblePlotsGeometry();
}

void MainWindow::recreatePlots()
{
    // 删除所有现有的图表
    for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
        for (int ch = 0; ch < 2; ++ch) {
            if (m_plotsPhase[card][ch]) {
                delete m_plotsPhase[card][ch];
                m_plotsPhase[card][ch] = nullptr;
            }
            if (m_plotsFrequency[card][ch]) {
                delete m_plotsFrequency[card][ch];
                m_plotsFrequency[card][ch] = nullptr;
            }
            if (m_plotsSpectrum[card][ch]) {
                delete m_plotsSpectrum[card][ch];
                m_plotsSpectrum[card][ch] = nullptr;
            }
        }
    }
    
    // 删除颜色图
    if (m_colorMapPlot) {
        delete m_colorMapPlot;
        m_colorMapPlot = nullptr;
    }
    
    // 重新创建所有图表
    setupPlots();
    
    // 同步几何布局
    syncVisiblePlotsGeometry();
}

void MainWindow::syncVisiblePlotsGeometry()
{
    auto syncPlot = [](QCustomPlot *plot) {
        if (!plot) return;
        // 隐藏态（后台 Tab / 被隐藏的频域图）不参与重绘：省去无谓的同步 replot，
        // 待其重新可见时由 paintEvent 依据脏标记自动重绘
        if (!plot->isVisible()) return;
        
        // 确保widget已完成布局
        plot->ensurePolished();
        plot->updateGeometry();
        
        // 获取当前widget的实际大小
        QSize currentSize = plot->size();
        
        // 如果是OpenGL模式，强制设置正确的viewport大小
        if (plot->openGl()) {
            // 强制设置viewport为当前widget大小
            plot->setViewport(QRect(0, 0, currentSize.width(), currentSize.height()));
            // 使用立即刷新确保OpenGL缓冲区正确更新
            plot->replot(QCustomPlot::rpImmediateRefresh);
        } else {
            plot->replot(QCustomPlot::rpQueuedReplot);
        }
    };

    syncPlot(m_colorMapPlot);
    for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
        for (int ch = 0; ch < 2; ++ch) {
            syncPlot(m_plotsPhase[card][ch]);
            syncPlot(m_plotsFrequency[card][ch]);
            syncPlot(m_plotsSpectrum[card][ch]);
        }
    }
}

// =====================================================================
// createConnections
// =====================================================================
void MainWindow::createConnections()
{
    connect(ui->btnStartListen,  &QPushButton::clicked, this, &MainWindow::onStartListenClicked);
    connect(ui->btnConfig,       &QPushButton::clicked, this,
            [this]() { onConfigParamsClicked(false); });
    connect(ui->btnStartMeasure, &QPushButton::clicked, this, &MainWindow::onStartMeasureClicked);
    connect(ui->btnExportDiagnostic, &QPushButton::clicked,
            this, &MainWindow::onExportDiagnosticClicked);
    connect(ui->btnSelectDir,    &QPushButton::clicked, this, &MainWindow::onSelectDirClicked);
    connect(ui->btnToggleSave,   &QPushButton::clicked, this, &MainWindow::onToggleSaveClicked);
    connect(ui->chkAutoSave,     &QCheckBox::toggled,   this, &MainWindow::onAutoSaveToggled);
    connect(ui->chkEnableDisplay, &QCheckBox::toggled,  this, &MainWindow::onEnableDisplayToggled);
    QCheckBox *chkTileView = ui->grpDisplay->findChild<QCheckBox*>("chkTileView");
    if (chkTileView)
        connect(chkTileView, &QCheckBox::toggled, this, &MainWindow::onTileViewToggled);
    // 单通道模式切通道 tab：立即刷新新前台通道，不等下一个刷新周期，
    // 避免后台转前台时信号长时间停留在旧数据
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int) {
        if (m_isTileView) return;
        m_forceRefreshOnce = true;
        QTimer::singleShot(0, this, [this]() { onDisplayRefresh(); });
    });
    connect(ui->cmbDisplayType, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDisplayTypeChanged);
    connect(ui->spnRefreshRate, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int value) {
                onRefreshRateChanged(value);
                saveSettings();
            });
    connect(ui->spnDownsampleRatio, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int value) {
                saveSettings();
                logMessage(QString("显示最大点数已设置为 %1 点").arg(value));
                if (m_netController) m_netController->setDisplayPoints(value);
            });
    connect(m_groupTabBar, &QTabBar::currentChanged, this, &MainWindow::onGroupTabChanged);
}

void MainWindow::onExportDiagnosticClicked()
{
    auto *recorder = DiagnosticRecorder::instance();
    if (!recorder) {
        QMessageBox::warning(this, QStringLiteral("导出诊断日志"),
                             QStringLiteral("诊断日志服务尚未初始化。"));
        return;
    }

    DiagnosticExportDialog dialog(recorder->runId(), recorder->runDirectory(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const DiagnosticExportDialog::Selection selected = dialog.selection();
    QString targetPath = uniqueDiagnosticZipPath(selected.targetPath);
    if (targetPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导出诊断日志"),
                             QStringLiteral("请选择有效的 ZIP 保存位置。"));
        return;
    }

    recordDiagnosticAction(QStringLiteral("diagnostic_export"),
                           {{QStringLiteral("targetPath"), targetPath},
                            {QStringLiteral("requestedStartTime"), selected.startTime.toString(Qt::ISODateWithMs)},
                            {QStringLiteral("requestedEndTime"), selected.endTime.toString(Qt::ISODateWithMs)}});

    const DiagnosticRecorder::TimeWindowRequest request =
        recorder->captureTimeWindowRequest(selected.startTime, selected.endTime, selected.note);

    statusBar()->showMessage(QStringLiteral("正在后台导出诊断日志…"), 3000);
    const QPointer<MainWindow> guard(this);
    auto *watcher = new QFutureWatcher<DiagnosticRecorder::ExportResult>();
    connect(watcher,
            &QFutureWatcher<DiagnosticRecorder::ExportResult>::finished,
            [watcher, guard]() {
                const DiagnosticRecorder::ExportResult result = watcher->result();
                watcher->deleteLater();
                if (!guard) return;

                if (!result.success) {
                    QMessageBox::warning(guard,
                                         QStringLiteral("导出诊断日志失败"),
                                         result.error.isEmpty()
                                             ? QStringLiteral("未知导出错误")
                                             : result.error);
                    return;
                }

                QString details = QStringLiteral("诊断日志已导出：\n%1")
                                      .arg(result.targetPath);
                if (result.noRecords)
                    details += QStringLiteral("\n\n该时间段没有日志。");
                if (!result.truncationReasons.isEmpty()) {
                    details += QStringLiteral("\n\n提示：导出包含以下记录完整性说明：\n• ")
                               + result.truncationReasons.join(QStringLiteral("\n• "));
                }
                QMessageBox box(QMessageBox::Information,
                                QStringLiteral("导出诊断日志"), details,
                                QMessageBox::NoButton, guard);
                QPushButton *openButton = box.addButton(QStringLiteral("打开目录"),
                                                        QMessageBox::AcceptRole);
                box.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
                box.exec();
                if (box.clickedButton() == openButton)
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(QFileInfo(result.targetPath).absolutePath()));
                guard->statusBar()->showMessage(
                    QStringLiteral("诊断日志导出完成：%1").arg(result.targetPath), 8000);
            });
    watcher->setFuture(QtConcurrent::run(
        [request, targetPath]() mutable {
            return DiagnosticRecorder::exportTimeWindow(request, targetPath);
    }));
}

void MainWindow::onPrepareSystemCaptureClicked()
{
    if (!m_isListening || !m_netController) {
        updateSystemCaptureStatus(QStringLiteral("app_only（请先开始监听）"),
                                  {{QStringLiteral("reason"), QStringLiteral("listener_not_running")}});
        QMessageBox::information(this, QStringLiteral("准备系统抓取"),
                                 QStringLiteral("请先开始网络监听，再准备本次监听会话的系统抓取请求。"));
        return;
    }
    const QString runId=m_netController->diagnosticRunId();
    const QString runDirectory=m_netController->diagnosticRunDirectory();
    if (runId.isEmpty()||runDirectory.isEmpty()) {
        updateSystemCaptureStatus(QStringLiteral("app_only（应用运行目录未就绪）"),
                                  {{QStringLiteral("reason"), QStringLiteral("diagnostic_run_not_ready")}});
        return;
    }
    QVector<QString> targetIPs=m_netController->diagnosticTargetIPs();
    if (targetIPs.isEmpty()) {
        for (const std::string &ip : m_netController->config().targetIPs)
            targetIPs.append(QString::fromStdString(ip));
    }
    if (targetIPs.isEmpty()) {
        updateSystemCaptureStatus(QStringLiteral("app_only（目标 IP 为空）"),
                                  {{QStringLiteral("reason"), QStringLiteral("empty_card_ips")}});
        return;
    }

    if (!m_systemCaptureRequestPath.isEmpty()) {
        QFile previous(QDir(m_systemCaptureOutputDirectory).filePath(QStringLiteral("capture-state.json")));
        if (previous.open(QIODevice::ReadOnly)) {
            const auto state=QJsonDocument::fromJson(previous.readAll()).object();
            const QString phase=state.value("status").toString();
            const auto resources=state.value("resources").toObject();
            const bool ownsResources=resources.value("pktmonStarted").toBool()
                || resources.value("wprStarted").toBool() || !resources.value("filterNames").toArray().isEmpty();
            if (QStringList{"preflight","starting","ready","collecting","stopping","validating"}.contains(phase)
                || ownsResources) {
                logMessage(QStringLiteral("本轮系统抓取尚未结束或资源尚未清理，请先处理当前状态，不能覆盖请求。"));
                onSystemCaptureStatusTick();
                return;
            }
        }
        if (!invalidateSystemCaptureRequest()) return;
    }
    QString channel=QString::fromStdString(systemCaptureChannelDir());
    if (channel.isEmpty()) channel=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("system-capture-channel"));
    m_systemCaptureTrialId=QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QStringLiteral("-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_systemCaptureOutputDirectory=QDir(runDirectory).filePath(QStringLiteral("system-capture/")+m_systemCaptureTrialId);
    m_systemCaptureSessionToken=QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_systemCaptureRequestPath=QDir(channel).filePath(QStringLiteral("capture-request-%1.json").arg(runId));
    if (!QDir().mkpath(channel)||!QDir().mkpath(m_systemCaptureOutputDirectory)) {
        updateSystemCaptureStatus(QStringLiteral("failed（无法创建请求目录）"),
                                  {{QStringLiteral("reason"), QStringLiteral("create_request_directory_failed")}});
        return;
    }
    QJsonArray cards,ports;
    for (const QString &ip : targetIPs) cards.append(ip);
    for (int port=8000;port<=8004;++port) ports.append(port);
    const QJsonObject request{
        {QStringLiteral("schemaVersion"),2},
        {QStringLiteral("kind"),QStringLiteral("system-capture-request")},
        {QStringLiteral("active"),true},
        {QStringLiteral("applicationExecutable"),QCoreApplication::applicationFilePath()},
        {QStringLiteral("trialId"),m_systemCaptureTrialId},
        {QStringLiteral("captureSessionToken"),m_systemCaptureSessionToken},
        {QStringLiteral("runId"),runId},
        {QStringLiteral("listenId"),m_diagnosticListenId},
        {QStringLiteral("measurementSessionId"),m_netController->measurementSessionId()},
        {QStringLiteral("cardIPs"),cards},
        {QStringLiteral("ports"),ports},
        {QStringLiteral("outputDirectory"),m_systemCaptureOutputDirectory},
        {QStringLiteral("channelDirectory"),channel},
        {QStringLiteral("createdUtc"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("applicationPid"),static_cast<double>(QCoreApplication::applicationPid())}
    };
    QSaveFile file(m_systemCaptureRequestPath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(request).toJson(QJsonDocument::Indented))<0
        || !file.commit()) {
        updateSystemCaptureStatus(QStringLiteral("failed（请求写入失败）"),
                                  {{QStringLiteral("reason"),file.errorString()}});
        return;
    }
    m_systemCaptureLastState.clear();
    recordDiagnosticAction(QStringLiteral("system_capture_prepare"),
                           {{QStringLiteral("trialId"),m_systemCaptureTrialId},
                            {QStringLiteral("captureSessionToken"),m_systemCaptureSessionToken},
                            {QStringLiteral("runId"),runId},
                            {QStringLiteral("listenId"),m_diagnosticListenId},
                            {QStringLiteral("outputDirectory"),m_systemCaptureOutputDirectory},
                            {QStringLiteral("channelDirectory"),channel},
                            {QStringLiteral("cardIPs"),cards},
                            {QStringLiteral("ports"),ports},
                            {QStringLiteral("autoElevation"),false}});
    updateSystemCaptureStatus(QStringLiteral("尚未启动（请运行管理员入口）"),
                              {{QStringLiteral("trialId"),m_systemCaptureTrialId},
                               {QStringLiteral("requestPath"),m_systemCaptureRequestPath},
                               {QStringLiteral("outputDirectory"),m_systemCaptureOutputDirectory}});
    logMessage(QStringLiteral("已准备系统抓取请求；请在管理员终端运行 Open-AdminCapture.cmd，应用不会自动提权。"));
    logMessage(QStringLiteral("本轮请求：%1；输出：%2").arg(m_systemCaptureRequestPath,m_systemCaptureOutputDirectory));
    logMessage(QStringLiteral("如同时运行多个程序，请指定本轮：Open-AdminCapture.cmd -ApplicationRequestPath \"%1\"").arg(m_systemCaptureRequestPath));
}

bool MainWindow::invalidateSystemCaptureRequest()
{
    if (m_systemCaptureRequestPath.isEmpty()) return true;
    QFile input(m_systemCaptureRequestPath);
    if (!input.exists()) return true;
    if (!input.open(QIODevice::ReadOnly)) return false;
    QJsonObject request=QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    if (request.value("captureSessionToken").toString()!=m_systemCaptureSessionToken) return false;
    request.insert("active",false);
    request.insert("invalidatedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QSaveFile output(m_systemCaptureRequestPath);
    const QByteArray bytes=QJsonDocument(request).toJson();
    const bool ok=output.open(QIODevice::WriteOnly) && output.write(bytes)==bytes.size() && output.commit();
    if (!ok) logMessage(QStringLiteral("系统抓取旧请求失效标记写入失败：%1").arg(m_systemCaptureRequestPath));
    return ok;
}

void MainWindow::updateSystemCaptureStatus(const QString &status,const QJsonObject &fields)
{
    if (m_systemCaptureStatusLabel) {
        m_systemCaptureStatusLabel->setText(QStringLiteral("系统抓取：")+status.left(90));
        m_systemCaptureStatusLabel->setMaximumWidth(620);
        m_systemCaptureStatusLabel->setToolTip(status+QStringLiteral("\n请求：")+m_systemCaptureRequestPath
            +QStringLiteral("\n输出：")+m_systemCaptureOutputDirectory);
    }
    if (m_systemCaptureLastState==status) return;
    m_systemCaptureLastState=status;
    logMessage(QStringLiteral("系统抓取：%1；输出：%2").arg(status,m_systemCaptureOutputDirectory));
    QJsonObject eventFields=fields;
    eventFields.insert(QStringLiteral("status"),status);
    eventFields.insert(QStringLiteral("requestPath"),m_systemCaptureRequestPath);
    eventFields.insert(QStringLiteral("outputDirectory"),m_systemCaptureOutputDirectory);
    recordDiagnosticAction(QStringLiteral("system_capture_status"),eventFields);
}

void MainWindow::onSystemCaptureStatusTick()
{
    if (m_systemCaptureRequestPath.isEmpty()) {
        updateSystemCaptureStatus(m_isListening ? QStringLiteral("本次监听未准备（请点“准备系统抓取”）")
                                                : QStringLiteral("未启动（请先开始监听）"));
        return;
    }
    const QString statePath=QDir(m_systemCaptureOutputDirectory).filePath(QStringLiteral("capture-state.json"));
    QFile stateFile(statePath);
    if (!stateFile.open(QIODevice::ReadOnly)) {
        updateSystemCaptureStatus(QFileInfo::exists(m_systemCaptureRequestPath)
                                  ? QStringLiteral("尚未启动（请运行管理员入口）")
                                  : QStringLiteral("启动失败（本轮请求文件缺失，请重新准备）"),
                                  {{QStringLiteral("reason"),QStringLiteral("capture_state_missing")},
                                   {QStringLiteral("requestExists"),QFileInfo::exists(m_systemCaptureRequestPath)}});
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document=QJsonDocument::fromJson(stateFile.readAll(),&parseError);
    if (!document.isObject()) {
        updateSystemCaptureStatus(QStringLiteral("failed（状态文件不可解析）"),
                                  {{QStringLiteral("reason"),parseError.errorString()}});
        return;
    }
    const QJsonObject state=document.object();
    if (!SystemCaptureStatus::matches(state,m_systemCaptureTrialId,m_systemCaptureSessionToken,
            m_netController?m_netController->diagnosticRunId():QString(),m_diagnosticListenId,m_systemCaptureOutputDirectory)) {
        updateSystemCaptureStatus(QStringLiteral("状态不匹配（不是本轮抓取，未确认启动）"));
        return;
    }
    const QString raw=state.value(QStringLiteral("status")).toString();
    const QString display=SystemCaptureStatus::describe(state);
    QJsonObject fields{{QStringLiteral("rawState"),raw},
                       {QStringLiteral("phase"),state.value(QStringLiteral("phase")).toString()},
                       {QStringLiteral("trialId"),state.value(QStringLiteral("trialId")).toString()},
                       {QStringLiteral("runId"),state.value(QStringLiteral("runId")).toString()},
                       {QStringLiteral("failureReasons"),state.value(QStringLiteral("failureReasons"))},
                       {QStringLiteral("analysisReady"),state.value(QStringLiteral("analysisReady"))},
                       {QStringLiteral("targetPacketsPresent"),state.value(QStringLiteral("targetPacketsPresent"))},
                       {QStringLiteral("manifestPath"),state.value(QStringLiteral("manifestPath"))}};
    updateSystemCaptureStatus(display,fields);
}

// =====================================================================
// 开始 / 停止监听
// =====================================================================
void MainWindow::onStartListenClicked()
{
    if (!m_isListening) {
        // 扫描进行中，忽略重复点击
        if (m_scanning) return;
        m_diagnosticListenId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        recordDiagnosticAction(QStringLiteral("listen_button"),
                               {{QStringLiteral("phase"), QStringLiteral("start")},
                                {QStringLiteral("scanBaseIP"), m_scanBaseIP},
                                {QStringLiteral("scanIPCount"), m_scanIPCount}});
        recordAcquisitionSnapshot({}, QStringLiteral("listen_button"));
        logMessage("开始网络监听...");

        // ══ 固定目标 IP（--target-ips）：跳过网段扫描直接监听 ══
        if (!g_targetIPs.isEmpty()) {
            QVector<QString> ips;
            for (const QString &ip : g_targetIPs) ips << ip;
            logMessage(QString("使用固定目标 IP 启动监听：%1").arg(ips.join(", ")));
            startListeningWithIPs(ips, QStringLiteral("explicit_target_ips"));
            return;
        }

        // ══ CONFIG-ACK 动态发现（后台线程；只有本次会话内返回 60 字节
        // CONFIG 确认的候选 IP 才能成为采集卡，ICMP/ARP 仅作诊断证据）══
        logMessage(QString("开始动态发现：候选 %1 起 %2 个地址（ICMP/ARP 可达仅作诊断，采集卡身份以 60 字节 CONFIG 确认为准）")
                   .arg(m_scanBaseIP).arg(m_scanIPCount));
        m_scanning = true;
        ui->btnStartListen->setEnabled(false);
        setBtnText(ui->btnStartListen, "正在发现采集卡...");
        // 发现参数在点击时取一次不可变快照；发现期间锁定相关参数控件
        ui->edtDataTime->setEnabled(false);
        ui->edtADelay->setEnabled(false);
        ui->edtBDelay->setEnabled(false);

        paimage::DiscoveryOptions options;
        options.baseIP = m_scanBaseIP;
        options.candidateCount = m_scanIPCount;
        options.localBindIP = m_localBindIP;
        options.configDurationNs = ui->edtDataTime->text().toInt();
        options.delayANs = ui->edtADelay->text().toInt();
        options.delayBNs = ui->edtBDelay->text().toInt();
        options.discoveryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSettings discoverySettings(paimageSettingsPath(), QSettings::IniFormat);
            options.ackWindowMs = discoverySettings.value("Discovery/AckWindowMs", 2000).toInt();
            options.maxAttempts = discoverySettings.value("Discovery/MaxAttempts", 5).toInt();
            options.finalGraceMs = discoverySettings.value("Discovery/FinalGraceMs", 500).toInt();
            options.icmpTimeoutMs = discoverySettings.value("Discovery/IcmpTimeoutMs", 150).toInt();
        }
        QString normalizationNote;
        options = paimage::sanitizeDiscoveryOptions(options, &normalizationNote);
        if (!normalizationNote.isEmpty())
            logMessage(QString("发现参数已规范化：%1").arg(normalizationNote));
        logMessage(QString("发现参数：ACK 窗口 %1ms × 最多 %2 轮，末轮宽限 %3ms（总等待上限约 %4 s）")
                   .arg(options.ackWindowMs).arg(options.maxAttempts)
                   .arg(options.finalGraceMs)
                   .arg((options.ackWindowMs * options.maxAttempts + options.finalGraceMs) / 1000.0,
                        0, 'f', 1));

        // 后台发现只接收值快照与取消标志；完成后由 QFutureWatcher 回到 UI。
        // QPointer 使窗口在发现完成前关闭时直接丢弃回调，避免访问悬空 this。
        m_activeDiscoveryId = options.discoveryId;
        m_discoveryCancel = std::make_shared<std::atomic<bool>>(false);
        const paimage::DiscoveryOptions optionsSnapshot = options;
        const auto cancelFlag = m_discoveryCancel;
        const QString activeDiscoveryId = m_activeDiscoveryId;
        const QPointer<MainWindow> guard(this);
        auto *watcher = new QFutureWatcher<paimage::DiscoveryResult>(this);
        connect(watcher,
                &QFutureWatcher<paimage::DiscoveryResult>::finished,
                this,
                [guard, watcher, activeDiscoveryId, cancelFlag]() {
                    const paimage::DiscoveryResult result = watcher->result();
                    watcher->deleteLater();
                    if (!guard) return;
                    MainWindow *window = guard.data();
                    window->m_scanning = false;
                    window->ui->edtDataTime->setEnabled(true);
                    window->ui->edtADelay->setEnabled(true);
                    window->ui->edtBDelay->setEnabled(true);
                    const auto restoreButton = [window]() {
                        setBtnText(window->ui->btnStartListen, "开始监听");
                        window->ui->btnStartListen->setProperty("state", QVariant());
                        window->ui->btnStartListen->style()->unpolish(window->ui->btnStartListen);
                        window->ui->btnStartListen->style()->polish(window->ui->btnStartListen);
                        window->ui->btnStartListen->setEnabled(true);
                    };
                    // 旧发现会话的迟到结果不得覆盖新会话状态
                    if (!paimage::canApplyDiscoveryResult(window->isEnabled(),
                                                          activeDiscoveryId, result)) {
                        if (window->isEnabled()) restoreButton();
                        return;
                    }
                    window->m_activeDiscoveryId.clear();
                    if (!result.error.isEmpty()) {
                        window->logMessage(QString("❌ 动态发现失败：%1（未修改卡数）")
                                               .arg(result.error));
                        restoreButton();
                        return;
                    }
                    if (result.cancelled) {
                        restoreButton();
                        return;
                    }
                    int excludedLocal = 0, timedOut = 0, sendFailed = 0;
                    for (const auto &candidate : result.candidates) {
                        if (candidate.state == paimage::DiscoveryCandidateState::ExcludedLocal)
                            ++excludedLocal;
                        else if (candidate.state == paimage::DiscoveryCandidateState::TimedOut)
                            ++timedOut;
                        else if (candidate.state == paimage::DiscoveryCandidateState::SendFailed)
                            ++sendFailed;
                    }
                    window->logMessage(
                        QString("发现结果：候选 %1 个，本机地址排除 %2 个，CONFIG 已确认采集卡 %3 张，"
                                "超时 %4 个，发送失败 %5 个")
                            .arg(result.candidates.size())
                            .arg(excludedLocal)
                            .arg(result.verifiedIPs.size())
                            .arg(timedOut)
                            .arg(sendFailed));
                    if (result.verifiedIPs.isEmpty()) {
                        window->logMessage("❌ 未发现任何返回 60 字节 CONFIG 确认的采集卡，"
                                           "请检查网线/交换机/采集卡上电状态");
                        restoreButton();
                        return;
                    }
                    for (const auto &candidate : result.candidates) {
                        if (candidate.state != paimage::DiscoveryCandidateState::Verified)
                            continue;
                        window->logMessage(
                            QString("✅ 采集卡确认：%1（第 %2 轮确认，首次发送到确认 %3 ms，重复确认 %4 次）")
                                .arg(candidate.ip)
                                .arg(candidate.verifiedRound)
                                .arg((candidate.firstAckNs - candidate.firstSendNs + 500000) / 1000000)
                                .arg(candidate.duplicateAck60Count));
                    }
                    window->startListeningWithIPs(result.verifiedIPs,
                                                  QStringLiteral("config_ack_discovery"));
                });
        watcher->setFuture(QtConcurrent::run(
            [optionsSnapshot, cancelFlag]() {
                return paimage::runDiscovery(optionsSnapshot, *cancelFlag);
            }));

    } else {
        recordDiagnosticAction(QStringLiteral("listen_button"),
                               {{QStringLiteral("phase"), QStringLiteral("stop")}});
        logMessage("正在停止网络监听...");
        invalidateSystemCaptureRequest();
        ui->btnStartListen->setEnabled(false);
        setBtnText(ui->btnStartListen, "正在停止...");

        // 简洁的停止提示标签（不用模态弹窗，避免潜在事件循环问题）
        QLabel *stopLabel = new QLabel("⏳ 正在停止所有模块，请稍候...", this);
        stopLabel->setAlignment(Qt::AlignCenter);
        stopLabel->setStyleSheet("QLabel { background: #2c3e50; color: white; "
                                 "padding: 8px 20px; border-radius: 6px; font-size: 13px; }");
        stopLabel->adjustSize();
        stopLabel->move((width() - stopLabel->width()) / 2,
                        (height() - stopLabel->height()) / 2);
        stopLabel->show();
        stopLabel->raise();

        if (m_isMeasuring) onStartMeasureClicked();

        // 记录是否在保存中，重新监听后自动恢复
        m_pendingAutoSave = m_netController && m_netController->isSaving();
        if (m_netController) {
            // 捕获本次停止会话的控制器：快速重启监听时若旧会话的 stopped 迟到，
            // 不能误删新会话的控制器（m_netController 已指向新对象）
            NetworkController *ctrl = m_netController;
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(m_netController, &NetworkController::stopped, this,
                            [this, ctrl, conn, stopLabel]() {
                disconnect(*conn);
                stopLabel->hide();
                stopLabel->deleteLater();

                if (m_netController != ctrl) {
                    // 期间已启动新的监听会话：只回收旧控制器，不触碰新会话 UI
                    if (ctrl) { ctrl->setParent(nullptr); ctrl->deleteLater(); }
                    return;
                }

                // 先更新 UI，再异步删除 controller（避免在 slot 里同步 delete QObject）
                m_isListening = false;
                setBtnText(ui->btnStartListen, "开始监听");
                ui->btnStartListen->setProperty("state", QVariant());
                ui->btnStartListen->style()->unpolish(ui->btnStartListen);
                ui->btnStartListen->style()->polish(ui->btnStartListen);
                ui->btnStartListen->setEnabled(true);
                ui->btnConfig->setEnabled(false);
                ui->btnStartMeasure->setEnabled(false);
                ui->btnToggleSave->setEnabled(false);
                logMessage("网络监听已停止");

                // 同步保存按钮状态：停止监听时一定停止了保存，无论按钮之前是什么状态
                setBtnText(ui->btnToggleSave, "开始保存");
                ui->btnToggleSave->setProperty("state", QVariant());
                ui->btnToggleSave->style()->unpolish(ui->btnToggleSave);
                ui->btnToggleSave->style()->polish(ui->btnToggleSave);
                m_reconSaveEnabled = false;   // 重建图像保存随监听停止而关闭
                m_autoSavePresentation.reset();
                {
                    std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
                    m_pendingCountPresentation.clear();
                }

                if (m_netController) {
                    m_netController->setParent(nullptr);
                    m_netController->deleteLater();
                    m_netController = nullptr;
                }
            }, Qt::QueuedConnection);

            m_netController->stop();
        }
    }
}

// =====================================================================
// startListeningWithIPs — 显式目标或 CONFIG-ACK 发现完成后：创建 controller 并启动监听
// 仅在主线程（异步发现完成回调）调用；onlineIPs 为按序排列的已确认卡 IP
// =====================================================================
void MainWindow::startListeningWithIPs(const QVector<QString>& onlineIPs,
                                       const QString& targetSourceKind)
{
    // 新监听会话：重置各卡触发去重序号，避免把首触发当作旧数据跳过
    for (int i = 0; i < MAX_CARDS; ++i) m_lastFeedSeq[i] = 0xFFFF;
    invalidateSystemCaptureRequest();
    m_systemCaptureRequestPath.clear();
    m_systemCaptureOutputDirectory.clear();
    m_systemCaptureTrialId.clear();
    m_systemCaptureSessionToken.clear();
    m_systemCaptureLastState.clear();
    updateSystemCaptureStatus(QStringLiteral("未准备"));

    const bool explicitTargets = targetSourceKind == QStringLiteral("explicit_target_ips");
    const QString targetSourceLabel = explicitTargets
        ? QStringLiteral("显式 --target-ips")
        : QStringLiteral("CONFIG-ACK 动态发现");
    m_activeTargetSource = targetSourceKind;
    logMessage(QString("监听启动目标来源：%1（%2）；物理采集卡数=%3（每卡 2 个通道）")
               .arg(targetSourceLabel, targetSourceKind).arg(onlineIPs.size()));
    for (int i = 0; i < onlineIPs.size(); ++i) {
        logMessage(QString("监听目标映射：物理卡%1 -> %2（通道 A/B）")
                   .arg(i + 1).arg(onlineIPs.at(i)));
    }

    if (onlineIPs.size() != m_nCards) {
        // 自动识别卡数并重建动态 UI（监听前，符合"监听期间禁止调用"约束）
        m_nCards = static_cast<int>(onlineIPs.size());
        rebuildDynamicUI();
    }
    m_targetIPRangeText = QString("%1 ~ %2").arg(onlineIPs.first()).arg(onlineIPs.last());
    saveSettings();  // 持久化自动识别的卡数

    m_netController = new NetworkController(this);
    m_netController->setDiagnosticContext(
        m_diagnosticListenId,
        targetSourceKind);
    // Preserve the ring dialog's canonical timeout even if the ImagingSvc
    // is currently stopped. The PAimage normalizer is created with this
    // value when the listener starts.
    if (m_imagingController && m_imagingController->isRingMode())
        m_netController->setPhysicalRoundTimeout(
            m_imagingController->ringConfig().timeoutResetSec);
    recordAcquisitionSnapshot(onlineIPs, QStringLiteral("listen_starting"));
    // 环形实时馈送回调：DataProcessor 每触发直连入队（须在 start() 之前设置）
    m_netController->setRingFeedSink(
        [this](const TriggerGroupConstPtr& frame) {
            return ringFeedSink(frame);
        });
    m_netController->setPhysicalRoundBoundarySink(
        [this](const paimage::PhysicalRoundEvent& event) {
            if (event.kind == paimage::PhysicalRoundEvent::Kind::CountBoundary) {
                // Allocate and publish the next data binding synchronously on
                // the source boundary.  UI work below only applies the
                // presentation target after the old final frame completes.
                AutoSaveCommit autoCommit;
                bool hasAutoCommit = false;
                if (m_netController && m_netController->autoSaveEnabled()) {
                    autoCommit = m_netController->commitAutoSaveBoundary(
                        event.measurementSession,
                        event.roundGeneration,
                        paimage::AutoSaveBoundaryKind::Count,
                        QStringLiteral("normalizer_count_boundary"));
                    hasAutoCommit = true;
                }
                if (hasAutoCommit && autoCommit.failed)
                    queueAutoSaveFailure(autoCommit);
                if (hasAutoCommit &&
                    (autoCommit.committed || autoCommit.alreadyApplied)) {
                    QMetaObject::invokeMethod(this, [this, autoCommit]() {
                        recordDiagnosticAction(
                            QStringLiteral("auto_save_count_presentation_transition_pending"),
                            {{QStringLiteral("measurementSession"),
                              qint64(autoCommit.measurementSession)},
                             {QStringLiteral("roundGeneration"),
                              qint64(autoCommit.roundGeneration)},
                             {QStringLiteral("oldSessionGen"),
                              qint64(autoCommit.oldSessionGen)},
                             {QStringLiteral("newSessionGen"),
                              qint64(autoCommit.newSessionGen)},
                             {QStringLiteral("oldDirectory"), autoCommit.oldDirectory},
                             {QStringLiteral("newDirectory"), autoCommit.directory},
                             {QStringLiteral("boundaryKind"),
                              QString::fromLatin1(paimage::autoSaveBoundaryKindName(
                                  autoCommit.boundaryKind))},
                             {QStringLiteral("phase"), autoCommit.phase}});
                    }, Qt::QueuedConnection);
                }
                {
                    std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
                    if (hasAutoCommit &&
                        (autoCommit.committed || autoCommit.alreadyApplied))
                        m_pendingCountPresentation.push_back(autoCommit);
                }
                // 低频业务事件：count 边界不在此清零帧计数（保留帧末驱动清零）。
                // 边界 sink 运行在网络线程；诊断记录 marshal 到 UI 线程，
                // 避免跨线程读 UI 成员。
                QMetaObject::invokeMethod(this, [this, event]() {
                    const auto ui = m_roundUi.snapshot();
                    recordDiagnosticAction(QStringLiteral("physical_round_boundary"),
                        {{QStringLiteral("measurementSession"), qint64(event.measurementSession)},
                         {QStringLiteral("roundGeneration"), qint64(event.roundGeneration)},
                         {QStringLiteral("boundaryKind"), QStringLiteral("count")},
                         {QStringLiteral("uiFrameCountBefore"), qint64(ui.frameCount)},
                         {QStringLiteral("uiFrameCountAfter"), qint64(ui.frameCount)},
                         {QStringLiteral("ringBlockCounterBefore"), qint64(ui.blockCount)},
                         {QStringLiteral("ringBlockCounterAfter"), qint64(ui.blockCount)},
                         {QStringLiteral("frameReset"), QStringLiteral("deferred_to_final_frame")}});
                }, Qt::QueuedConnection);
                if (!m_ringAssembler || !m_ringAssemblerConfigured) return;
                std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
                if (!m_ringAssemblerConfigured) return;
                m_ringTimeoutSaveDone.store(false, std::memory_order_release);
                m_ringBoundaryPending = true;
                m_ringBoundarySession = event.measurementSession;
                m_ringBoundaryGeneration = event.roundGeneration;
                m_ringBoundaryTrigger = event.triggerSeq;
                m_ringBoundaryCards = 0;
                return;
            }
            if (event.kind != paimage::PhysicalRoundEvent::Kind::TimeoutBoundary)
                return;
            // TimeoutBoundary is the source-side save authority.  Commit the
            // directory mapping before this callback returns; UI work below
            // is deliberately queued and cannot determine the generation.
            AutoSaveCommit autoCommit;
            bool hasAutoCommit = false;
            if (m_netController && m_netController->autoSaveEnabled()) {
                autoCommit = m_netController->commitAutoSaveBoundary(
                    event.measurementSession,
                    event.roundGeneration,
                    paimage::AutoSaveBoundaryKind::Timeout,
                    QStringLiteral("normalizer_timeout_boundary"));
                hasAutoCommit = true;
            }
            // Capture the old presentation directory from the committed
            // transition, not from the mutable UI target.  A delayed timeout
            // callback must not save an old reconstruction into a directory
            // that a later boundary has already applied.
            const QString oldReconDirectory =
                hasAutoCommit && !autoCommit.oldDirectory.isEmpty()
                    ? QDir(autoCommit.oldDirectory).filePath("recon_png")
                    : QString();
            // Claim the physical boundary before touching UI state.  The
            // coordinator makes directory publication idempotent; this gate
            // makes the corresponding Ring/UI reset idempotent as well.
            {
                std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
                if (m_lastRingTimeoutBoundarySession == event.measurementSession &&
                    m_lastRingTimeoutBoundaryGeneration == event.roundGeneration)
                    return;
                m_lastRingTimeoutBoundarySession = event.measurementSession;
                m_lastRingTimeoutBoundaryGeneration = event.roundGeneration;
            }
            const auto uiBefore = m_roundUi.snapshot();
            // Keep stale admission and per-round counters safe even if the UI
            // event loop is delayed by rendering or PNG work.
            m_roundUi.onTimeoutBoundary();
            QMetaObject::invokeMethod(this, [this, event, uiBefore,
                                             autoCommit, hasAutoCommit,
                                             oldReconDirectory]() {
                updateRingImagingStatus();
                if (!m_ringTimeoutSaveDone.exchange(true, std::memory_order_acq_rel) &&
                    m_reconSaveEnabled &&
                    !(oldReconDirectory.isEmpty() && m_reconSaveDir.isEmpty()) &&
                    m_imagingDisplayWindow) {
                    const QString target = oldReconDirectory.isEmpty()
                        ? m_reconSaveDir : oldReconDirectory;
                    m_imagingDisplayWindow->saveWindowPngs(
                        target, m_reconSaveSuffix,
                        m_imagingDisplayWindow->lastSeq());
                }
                if (hasAutoCommit) {
                    if (autoCommit.failed)
                        queueAutoSaveFailure(autoCommit);
                    else
                        applyAutoSaveCommitToUi(autoCommit);
                }
                const auto uiAfter = m_roundUi.snapshot();
                recordDiagnosticAction(QStringLiteral("physical_round_boundary"),
                    {{QStringLiteral("measurementSession"), qint64(event.measurementSession)},
                     {QStringLiteral("roundGeneration"), qint64(event.roundGeneration)},
                     {QStringLiteral("boundaryKind"), QStringLiteral("timeout")},
                     {QStringLiteral("uiFrameCountBefore"), qint64(uiBefore.frameCount)},
                     {QStringLiteral("uiFrameCountAfter"), qint64(uiAfter.frameCount)},
                     {QStringLiteral("ringBlockCounterBefore"), qint64(uiBefore.blockCount)},
                     {QStringLiteral("ringBlockCounterAfter"), qint64(uiAfter.blockCount)},
                     {QStringLiteral("autoSessionGenBefore"),
                      hasAutoCommit ? qint64(autoCommit.oldSessionGen) : 0},
                     {QStringLiteral("autoSessionGenAfter"),
                      hasAutoCommit ? qint64(autoCommit.publishedSessionGen) : 0},
                     {QStringLiteral("saveRoundRollover"), QStringLiteral("data_plane_roundGeneration")}});
            }, Qt::QueuedConnection);

            // Remove queued pre-boundary groups first, then reset the
            // assembler under its existing mutex so no partial block survives
            // that boundary. Deduplicate the reset by session/generation.
            if (m_imagingBypass)
                m_imagingBypass->clear(ImagingSubmitResult::StaleSession);
            if (!m_ringAssembler || !m_ringAssemblerConfigured)
                return;
            std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
            if (!m_ringAssemblerConfigured) return;
            m_ringBoundaryPending = false;
            m_ringBoundaryCards = 0;
            m_ringBoundaryApplied = false;
            m_ringResetCommandSent.store(false, std::memory_order_release);
            m_ringAssembler->resetAfterPhysicalTimeout();
            // ring_reset and ring_block_ready share ImagingController's ZMQ
            // mutex.  The explicit producer submit_index cutoff is captured
            // only after the reset command has been handed to that FIFO.
            if (m_ringResetCommandSent.exchange(false, std::memory_order_acq_rel)) {
                const auto cutoff = m_imagingController
                    ? m_imagingController->ringLastSubmitIndex() : 0;
                m_roundUi.armStaleCutoff(cutoff);
            } else {
                // Without a reset acknowledgement, accepting a later
                // snapshot could mix old CUDA state into the new UI round.
                // Fail closed until the service lifecycle is re-established.
                m_ringSnapshotAdmissionBlocked.store(true, std::memory_order_release);
                m_roundUi.disarmStaleCutoff();
                recordDiagnosticAction(QStringLiteral("ring_reset_submit_failed"),
                    {{QStringLiteral("measurementSession"), qint64(event.measurementSession)},
                     {QStringLiteral("roundGeneration"), qint64(event.roundGeneration)}});
            }
        });
    connect(m_netController, &NetworkController::statusMessage, this, &MainWindow::logMessage);
    connect(m_netController, &NetworkController::errorOccurred, this, &MainWindow::logMessage);
    connect(m_netController, &NetworkController::fileSaverRollover, this,
            [this](int cardId, const QString& reason,
                   quint64 oldRoundGeneration, quint64 newRoundGeneration,
                   int oldFileSequence, int newFileSequence,
                   int oldFileTriggerCount, bool manualMode) {
        // 低频：仅在文件翻滚时记录；可区分容量翻滚与物理轮次强制翻滚。
        recordDiagnosticAction(QStringLiteral("save.file_rollover"),
            {{QStringLiteral("card"), cardId},
             {QStringLiteral("boundary"), reason},
             {QStringLiteral("oldRoundGeneration"), static_cast<qint64>(oldRoundGeneration)},
             {QStringLiteral("newRoundGeneration"), static_cast<qint64>(newRoundGeneration)},
             {QStringLiteral("oldFileSequence"), oldFileSequence},
             {QStringLiteral("newFileSequence"), newFileSequence},
             {QStringLiteral("oldFileTriggerCount"), oldFileTriggerCount},
             {QStringLiteral("saveMode"), manualMode ? QStringLiteral("manual") : QStringLiteral("auto")}});
    });
    // ══ 卡片就绪信号：更新网络信息标签 ════════════════════════════
    connect(m_netController, &NetworkController::cardReady, this, [this](int cardIdx) {
        int ready = m_netController ? m_netController->readyCardCount() : 0;
        int total = m_nCards;
        ui->lblTargetIPs->setText(
            QString("目标IP: %1  [%2/%3 卡就绪]")
            .arg(m_targetIPRangeText).arg(ready).arg(total));
        ui->lblTargetIPs->setStyleSheet(
            ready == total ? "color: #00FF88;" : "color: #FFAA00;");
        updateNetworkInfoIndicator();
    });
    connect(m_netController, &NetworkController::allCardsReady, this, [this]() {
        logMessage("✅ 所有采集卡均已就绪，可以发送控制命令");
        ui->lblTargetIPs->setText(
            QString("目标IP: %1  [全部%2卡就绪 ✓]")
            .arg(m_targetIPRangeText).arg(m_nCards));
        ui->lblTargetIPs->setStyleSheet("color: #00FF88;");
        updateNetworkInfoIndicator();
    });
    // 配置参数确认（60 字节反馈）信号
    connect(m_netController, &NetworkController::configConfirmed, this, [this]() {
        logMessage("✅ 所有采集卡配置参数均已确认，可以开始测量");
    });
    connect(m_netController, &NetworkController::configAckFailed, this, [this](int cardIdx) {
        logMessage(QString("⚠️ 卡%1 配置确认失败，请检查链路后重新下发配置").arg(cardIdx + 1));
    });
    connect(m_netController, &NetworkController::measurementStarted,
            this, [this](const QString& sessionId) {
        // 会话建立时先清掉旧成像旁路；首个新帧携带的源 session 会成为新锚点。
        if (m_imagingBypass) m_imagingBypass->beginSession(0);
        m_isMeasuring = true;
        setBtnText(ui->btnStartMeasure, "停止测量");
        ui->btnStartMeasure->setProperty("state", "measuring");
        ui->btnStartMeasure->style()->unpolish(ui->btnStartMeasure);
        ui->btnStartMeasure->style()->polish(ui->btnStartMeasure);
        m_highDataRateWarningShown = false;
        logMessage(QString("测量会话已启动：%1").arg(sessionId));
    });
    connect(m_netController, &NetworkController::measurementStartFailed,
            this, [this](const QString& sessionId, const QString& reason) {
        logMessage(QString("❌ 测量会话启动失败并已回滚：%1（%2）")
                   .arg(sessionId, reason));
    });
    connect(m_netController, &NetworkController::measurementStopped,
            this, [this](const QString& sessionId, bool) {
        m_isMeasuring = false;
        setBtnText(ui->btnStartMeasure, "开始测量");
        ui->btnStartMeasure->setProperty("state", QVariant());
        ui->btnStartMeasure->style()->unpolish(ui->btnStartMeasure);
        ui->btnStartMeasure->style()->polish(ui->btnStartMeasure);
        logMessage(QString("测量会话已停止：%1").arg(sessionId));
    });
    connect(m_netController, &NetworkController::measurementStopFailed,
            this, [this](const QString& sessionId, const QString& reason) {
        logMessage(QString("⚠️ 测量停止失败，已 disarm 处理器但硬件状态需复核：%1（%2）")
                   .arg(sessionId, reason));
    });

    // 初始状态：等待卡片就绪
    ui->lblTargetIPs->setText(
        QString("目标IP: %1  [等待%2卡就绪...]")
        .arg(m_targetIPRangeText).arg(m_nCards));
    ui->lblTargetIPs->setStyleSheet("color: #FFAA00;");
    updateNetworkInfoIndicator();

    AcqConfig cfg;
    cfg.nCards        = onlineIPs.size();
    cfg.localBindIP   = m_localBindIP.toStdString();  // 控制 socket 绑定到指定本地接口
    cfg.targetIPs.clear();
    for (const QString& ip : onlineIPs)
        cfg.targetIPs.push_back(ip.toStdString());    // 网段扫描识别到的目标卡 IP
    cfg.acqTimeNs     = ui->edtDataTime->text().toInt();
    cfg.delayA        = ui->edtADelay->text().toInt();
    cfg.delayB        = ui->edtBDelay->text().toInt();
    cfg.displayPoints = ui->spnDownsampleRatio->value();
    // 数据格式参数：与线性实例相同来源（注册表，默认 250 MSa/s 满速率 32bit Q16.16）
    cfg.bitsPerChannel = m_bitsPerChannel;
    cfg.sampleIntervalNs = m_sampleIntervalNs;
    cfg.logicalTriggersPerRound = m_logicalTriggersPerRound;
    {
        QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
        cfg.diagnosticLevel = qBound(0, settings.value("Diagnostics/Level", 1).toInt(), 2);
        cfg.diagnosticTraceEnabled = settings.value("Diagnostics/RawIngressTrace", true).toBool();
        cfg.socketTimestampMode = qBound(0, settings.value("Diagnostics/SocketTimestampMode", 0).toInt(), 3);
    }
    // Startup admission policy from the command line: --startup-policy=legacy
    // restores the 1000 ms startup filter; the diagnostic default bypasses it.
    cfg.startupIdleMs = startupIdleMsFor(startupPolicy());

    // ── 路线A（WinSock）：start() 同步完成，直接在调用后更新按钮 ──
    if (!m_netController->start(cfg)) {
        // 接收端口启动失败：不进入监听状态，恢复按钮与标签
        // （controller 内部已同步回滚，这里仅清理 UI 状态）
        m_netController->deleteLater();
        m_netController = nullptr;
        setBtnText(ui->btnStartListen, "开始监听");
        ui->btnStartListen->setProperty("state", QVariant());
        ui->btnStartListen->style()->unpolish(ui->btnStartListen);
        ui->btnStartListen->style()->polish(ui->btnStartListen);
        ui->btnStartListen->setEnabled(true);
        ui->btnConfig->setEnabled(false);
        ui->btnStartMeasure->setEnabled(false);
        ui->btnToggleSave->setEnabled(false);
        ui->lblTargetIPs->setText(
            QString("目标IP: %1  [启动失败]").arg(m_targetIPRangeText));
        ui->lblTargetIPs->setStyleSheet("color: #FF5555;");
        updateNetworkInfoIndicator();
        logMessage("❌ 网络监听启动失败，请查看上方错误日志");
        return;
    }

    // start() 已完全返回，所有线程已启动，直接设置状态
    m_isListening = true;
    setBtnText(ui->btnStartListen, "停止监听");
    ui->btnStartListen->setProperty("state", "stopping");
    ui->btnStartListen->style()->unpolish(ui->btnStartListen);
    ui->btnStartListen->style()->polish(ui->btnStartListen);
    ui->btnStartListen->setEnabled(true);
    ui->btnConfig->setEnabled(true);
    ui->btnStartMeasure->setEnabled(true);
    ui->btnToggleSave->setEnabled(true);
    logMessage(QString("网络监听已启动，共 %1 张卡，端口 %2~%3")
               .arg(m_nCards).arg(BASE_PORT).arg(BASE_PORT + m_nCards - 1));
    QJsonArray startedTargets;
    for (const QString &ip : onlineIPs) startedTargets.append(ip);
    recordDiagnosticAction(QStringLiteral("listen_started"),
                           {{QStringLiteral("cardCount"), m_nCards},
                            {QStringLiteral("targetIPs"), startedTargets}});
    recordAcquisitionSnapshot(onlineIPs, QStringLiteral("listen_started"));

    // 保存状态恢复（方案B）：自动保存勾选优先——在新控制器上重建自动保存
    // （会话代/目录注册表随旧控制器销毁，需重新初始化，避免保存器停摆丢数据）；
    // 否则若之前处于手动保存状态则恢复手动保存
    if (ui->chkAutoSave->isChecked()) {
        m_pendingAutoSave = false;      // 自动保存期间记录的手动恢复标志作废
        initAutoSaveSavers();           // 新控制器重建自动保存（重扫编号、注册会话代、开启保存器）
        logMessage("自动保存已在新监听会话上恢复");
    } else if (m_pendingAutoSave) {
        m_pendingAutoSave = false;
        onToggleSaveClicked();  // 重新调用开始保存逻辑（手动）
    }

    // 重置首次绘图标志
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i) {
        m_displayCounter[i] = 0;
        m_firstPlot[i][0] = true;
        m_firstPlot[i][1] = true;
    }
}

// =====================================================================
// 配置参数
// =====================================================================
void MainWindow::onConfigParamsClicked(bool fromStartMeasure)
{
    int dataTime = ui->edtDataTime->text().toInt();
    int aDelay   = ui->edtADelay->text().toInt();
    int bDelay   = ui->edtBDelay->text().toInt();
    const QString trigger = fromStartMeasure
        ? QStringLiteral("start_measure") : QStringLiteral("config_button");
    recordDiagnosticAction(trigger,
                           {{QStringLiteral("dataTimeNs"), dataTime},
                            {QStringLiteral("delayA"), aDelay},
                            {QStringLiteral("delayB"), bDelay}});
    recordAcquisitionSnapshot({}, trigger);
    saveSettings();
    logMessage(QString("发送配置: 采集=%1ns, A延时=%2ns, B延时=%3ns").arg(dataTime).arg(aDelay).arg(bDelay));
    if (m_netController) {
        if (m_netController->sendConfigCommand(dataTime, aDelay, bDelay, trigger)) {
            // 同步更新 DataProcessor 的采集参数（包数、采样点数等）
            AcqConfig cfg = m_netController->config();
            cfg.acqTimeNs = dataTime;
            cfg.delayA    = aDelay;
            cfg.delayB    = bDelay;
            cfg.displayPoints = ui->spnDownsampleRatio->value();
            cfg.sampleIntervalNs = m_sampleIntervalNs;
            m_netController->reconfigure(cfg);
            logMessage("配置命令发送成功");
        } else {
            logMessage("配置命令发送失败，请检查网络连接和采集卡 IP");
        }
    }
}

// =====================================================================
// 开始 / 停止测量
// =====================================================================
void MainWindow::onStartMeasureClicked()
{
    if (!m_isMeasuring) {
        recordDiagnosticAction(QStringLiteral("measure_button"),
                               {{QStringLiteral("phase"), QStringLiteral("start")}});
        // 功能整合：开始测量前先下发配置参数，再发送开始测量命令
        logMessage("开始测量：先下发配置参数");
        onConfigParamsClicked(true);
        if (m_netController) {
            if (m_netController->sendStartMeasure())
                logMessage("开始测量请求已提交，等待 PAimage CONFIG 确认及 START 发送结果");
            else
                logMessage("开始测量请求失败，未进入测量状态");
        }
    } else {
        recordDiagnosticAction(QStringLiteral("measure_button"),
                               {{QStringLiteral("phase"), QStringLiteral("stop")}});
        if (m_netController) {
            if (m_netController->sendStopMeasure())
                logMessage("停止测量请求已提交，等待 PAimage STOP 发送结果");
            else
                logMessage("停止测量请求失败，仍保留当前测量状态供复核");
        }
    }
}

// =====================================================================
// 选择目录
// =====================================================================
void MainWindow::onSelectDirClicked()
{
    QString dir = QFileDialog::getExistingDirectory(this, "选择保存目录",
                                                     ui->edtSaveDir->text(),
                                                     QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty()) {
        ui->edtSaveDir->setText(dir);
        logMessage(QString("保存目录设置为: %1").arg(dir));
    }
}

// =====================================================================
// 开始 / 停止保存
// =====================================================================
void MainWindow::onToggleSaveClicked()
{
    if (m_netController && m_netController->isSaving()) {
        // ── 停止保存 ──
        m_netController->stopSaving();
        setBtnText(ui->btnToggleSave, "开始保存");
        ui->btnToggleSave->setProperty("state", QVariant());
        ui->btnToggleSave->style()->unpolish(ui->btnToggleSave);
        ui->btnToggleSave->style()->polish(ui->btnToggleSave);
        // 解锁保存参数
        ui->edtSaveDir->setEnabled(true);
        ui->btnSelectDir->setEnabled(true);
        ui->edtTriggersPerFile->setEnabled(true);
        ui->edtFileSuffix->setEnabled(true);
        m_reconSaveEnabled = false;   // 停止保存时同时停止重建图像保存
        logMessage("停止保存数据");
    } else {
        // ── 开始保存 ──
        QString saveDir = ui->edtSaveDir->text();
        QDir dir(saveDir);
        if (!dir.exists() && !dir.mkpath(".")) {
            logMessage(QString("错误: 无法创建目录 %1").arg(saveDir));
            return;
        }
        bool ok;
        int triggersPerFile = ui->edtTriggersPerFile->text().toInt(&ok);
        if (!ok || triggersPerFile <= 0) {
            logMessage("错误: 每文件触发数必须是大于0的整数");
            return;
        }
        QString fileSuffix = ui->edtFileSuffix->text().trimmed();
        if (m_netController) {
            m_netController->startSaving(saveDir, triggersPerFile, fileSuffix);
        }
        // 实时重建图像随数据保存联动：整圈最后一帧更新完后保存最终重建图到 保存目录/recon_png
        m_reconSaveEnabled = true;
        // 开始保存时不立即对“已空闲”画面触发超时到点保存（下一触发到来时复位）
        m_ringTimeoutSaveDone = true;
        m_reconSaveDir = QDir(saveDir).filePath("recon_png");
        m_reconSaveSuffix = fileSuffix;
        QDir().mkpath(m_reconSaveDir);
        // 锁定保存参数（主流仪表设计：采集/保存中不可修改配置）
        ui->edtSaveDir->setEnabled(false);
        ui->btnSelectDir->setEnabled(false);
        ui->edtTriggersPerFile->setEnabled(false);
        ui->edtFileSuffix->setEnabled(false);

        setBtnText(ui->btnToggleSave, "停止保存");
        ui->btnToggleSave->setProperty("state", "saving");
        ui->btnToggleSave->style()->unpolish(ui->btnToggleSave);
        ui->btnToggleSave->style()->polish(ui->btnToggleSave);
        QString suffixInfo = fileSuffix.isEmpty() ? "(none)" : fileSuffix;
        logMessage(QString("开始保存到: %1 (每文件%2触发, 后缀: %3)").arg(saveDir).arg(triggersPerFile).arg(suffixInfo));
        logMessage(QString("环形重建图像将在每整圈最后一帧更新完后保存至: %1").arg(m_reconSaveDir));
    }
}

// =====================================================================
// 自动保存（环形模式，物理轮次协调器精确分界）：
//   基线目录 = 保存目录输入路径忽略最后一层（如 D:\zzx\data\run\01 → D:\zzx\data\run）
//   勾选时扫描基线目录下三位数命名文件夹的最大编号，从最大编号+1 起分配。
//   目录准备与 generation 发布由 AutoSaveRoundCoordinator 完成；TimeoutBoundary
//   与 CountBoundary 都在源线程同步提交，最终旧帧只负责应用已准备的展示目标。
//   DataProcessor 在数据入队前按 measurementSession+roundGeneration 解析，
//   FileSaver 按已解析 generation 路由目录。
// =====================================================================
static int scanMaxAutoFolder(const QString &base)
{
    int mx = 0;
    QDir d(base);
    const QFileInfoList list = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : list) {
        if (fi.fileName().size() != 3) continue;
        bool ok = false;
        const int v = fi.fileName().toInt(&ok);
        if (ok && v >= 0) mx = qMax(mx, v);
    }
    return mx;
}

void MainWindow::applyAutoSaveCommitToUi(const AutoSaveCommit& commit)
{
    if (!m_autoSaveEnabled || !m_isListening)
        return;
    if (commit.failed) {
        queueAutoSaveFailure(commit);
        return;
    }
    if ((!commit.committed && !commit.alreadyApplied) ||
        commit.newSessionGen == 0 || commit.directory.isEmpty())
        return;

    const auto presentationResult = m_autoSavePresentation.apply(commit);
    if (presentationResult == paimage::AutoSavePresentationState::ApplyResult::Stale) {
        recordDiagnosticAction(QStringLiteral("auto_save_presentation_transition_stale_ignored"),
            {{QStringLiteral("measurementSession"), qint64(commit.measurementSession)},
             {QStringLiteral("roundGeneration"), qint64(commit.roundGeneration)},
             {QStringLiteral("oldSessionGen"), qint64(commit.oldSessionGen)},
             {QStringLiteral("newSessionGen"), qint64(commit.newSessionGen)},
             {QStringLiteral("oldDirectory"), commit.oldDirectory},
             {QStringLiteral("newDirectory"), commit.directory},
             {QStringLiteral("boundaryKind"),
              QString::fromLatin1(paimage::autoSaveBoundaryKindName(commit.boundaryKind))},
             {QStringLiteral("phase"), commit.phase}});
        return;
    }

    m_reconSaveEnabled = true;
    m_reconSaveDir = QDir(commit.directory).filePath("recon_png");
    m_reconSaveSuffix = ui->edtFileSuffix->text().trimmed();
    if (!QDir().mkpath(m_reconSaveDir)) {
        m_reconSaveEnabled = false;
        logMessage(QString("错误: 无法创建自动保存重建目录 %1；数据目录仍按已提交代号保存")
                   .arg(m_reconSaveDir));
    }
    if (commit.committed)
        logMessage(QString("自动保存会话目录：%1").arg(commit.directory));
    recordDiagnosticAction(QStringLiteral("auto_save_round_generation_applied_ui"),
        {{QStringLiteral("measurementSession"), qint64(commit.measurementSession)},
         {QStringLiteral("roundGeneration"), qint64(commit.roundGeneration)},
         {QStringLiteral("boundaryKind"),
          QString::fromLatin1(paimage::autoSaveBoundaryKindName(commit.boundaryKind))},
         {QStringLiteral("oldSessionGen"), qint64(commit.oldSessionGen)},
         {QStringLiteral("newSessionGen"), qint64(commit.newSessionGen)},
         {QStringLiteral("oldDirectory"), commit.oldDirectory},
         {QStringLiteral("directory"), commit.directory},
         {QStringLiteral("phase"), commit.phase},
         {QStringLiteral("idempotent"), commit.alreadyApplied},
         {QStringLiteral("presentationTransition"),
           commit.boundaryKind == paimage::AutoSaveBoundaryKind::Count
               ? QStringLiteral("count_presentation_applied")
               : QStringLiteral("round_generation_applied")},
         {QStringLiteral("presentationApply"),
           presentationResult == paimage::AutoSavePresentationState::ApplyResult::AlreadyCurrent
               ? QStringLiteral("already_current") : QStringLiteral("applied")}});
}

void MainWindow::queueAutoSaveFailure(const AutoSaveCommit& commit)
{
    const auto apply = [this, commit]() {
        if (m_netController) {
            // Stop the producer/consumer path before clearing the active
            // controller state.  The coordinator itself also retains its
            // failed sentinel until the next configure().
            m_netController->stopSaving();
            m_netController->disableAutoSave();
        }
        m_autoSaveEnabled = false;
        m_reconSaveEnabled = false;
        if (ui && ui->chkAutoSave) {
            QSignalBlocker blocker(ui->chkAutoSave);
            ui->chkAutoSave->setChecked(false);
        }
        if (ui) {
            ui->btnToggleSave->setEnabled(m_isListening);
            ui->edtSaveDir->setEnabled(true);
            ui->btnSelectDir->setEnabled(true);
            ui->edtTriggersPerFile->setEnabled(true);
            ui->edtFileSuffix->setEnabled(true);
        }
        recordDiagnosticAction(QStringLiteral("auto_save_round_generation_failed"),
            {{QStringLiteral("measurementSession"), qint64(commit.measurementSession)},
             {QStringLiteral("roundGeneration"), qint64(commit.roundGeneration)},
             {QStringLiteral("boundaryKind"),
              QString::fromLatin1(paimage::autoSaveBoundaryKindName(commit.boundaryKind))},
             {QStringLiteral("oldSessionGen"), qint64(commit.oldSessionGen)},
             {QStringLiteral("error"), commit.error},
             {QStringLiteral("behavior"), QStringLiteral("save_disabled_fail_closed")}});
        logMessage(QString("错误: 自动保存已停用（目录未注册，数据不会回退到旧目录）：%1")
                   .arg(commit.error));
    };
    if (QThread::currentThread() == thread())
        apply();
    else
        QMetaObject::invokeMethod(this, apply, Qt::QueuedConnection);
}

// 方案B：在当前控制器上（重）建自动保存（勾选时与监听重启恢复共用）。
// 重扫基线目录最大编号防止覆盖，分配下一目录并持续开启保存器。
void MainWindow::initAutoSaveSavers()
{
    if (!m_netController || !m_autoSaveEnabled) return;
    m_autoSavePresentation.reset();
    {
        std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
        m_pendingCountPresentation.clear();
    }
    const QFileInfo fi(ui->edtSaveDir->text().trimmed());
    const QString base = fi.absolutePath();
    if (base.isEmpty()) return;
    m_netController->configureAutoSave(base,
                                       static_cast<std::uint64_t>(scanMaxAutoFolder(base)));
    const auto first = m_netController->beginAutoSaveSession(
        0, QStringLiteral("ui_session_start"));
    if (!first.failed) {
        applyAutoSaveCommitToUi(first);
        int tpf = ui->edtTriggersPerFile->text().toInt();
        if (tpf <= 0) tpf = 1000;
        m_netController->startSaving(first.directory, tpf,
                                     ui->edtFileSuffix->text().trimmed());
    } else {
        queueAutoSaveFailure(first);
    }
}

void MainWindow::onAutoSaveToggled(bool checked)
{
    if (checked) {
        const QString in = ui->edtSaveDir->text().trimmed();
        const QFileInfo fi(in);
        const QString base = fi.absolutePath();   // 忽略输入路径最后一层
        if (in.isEmpty() || base.isEmpty()) {
            logMessage("错误: 自动保存需要有效的保存目录");
            QSignalBlocker blocker(ui->chkAutoSave);
            ui->chkAutoSave->setChecked(false);
            return;
        }
        m_autoSaveEnabled = true;
        // 自动保存期间禁用手动保存按钮，避免两者争用同一保存器/目录
        ui->btnToggleSave->setEnabled(false);
        // 与手动“开始保存”一致：锁定保存参数（目录/每文件触发数/文件后缀）
        ui->edtSaveDir->setEnabled(false);
        ui->btnSelectDir->setEnabled(false);
        ui->edtTriggersPerFile->setEnabled(false);
        ui->edtFileSuffix->setEnabled(false);
        // 分配首个会话并持续开启保存器（此后按会话代路由目录，不再逐会话启停，
        // 首触发到达时读到的已是会话代 1，不丢触发）
        initAutoSaveSavers();
        logMessage(QString("自动保存已开启：基线目录 %1").arg(base));
    } else {
        m_autoSaveEnabled = false;
        // 关闭自动保存：先停写并清理队列，再清除活动会话代，避免异步
        // 停写窗口把新组误标为手动模式并回落到旧目录。
        if (m_netController) {
            m_netController->stopSaving();
            m_netController->disableAutoSave();
        }
        m_autoSavePresentation.reset();
        {
            std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
            m_pendingCountPresentation.clear();
        }
        m_reconSaveEnabled = false;   // PNG 保存随自动保存关闭
        ui->btnToggleSave->setEnabled(m_isListening);
        // 恢复保存参数可写
        ui->edtSaveDir->setEnabled(true);
        ui->btnSelectDir->setEnabled(true);
        ui->edtTriggersPerFile->setEnabled(true);
        ui->edtFileSuffix->setEnabled(true);
        logMessage("自动保存已关闭");
    }
}

// =====================================================================
// 显示控制
// =====================================================================
void MainWindow::onEnableDisplayToggled(bool checked)
{
    m_displayEnabled = checked;
    logMessage(checked ? "实时显示已启用" : "实时显示已禁用");
}

void MainWindow::onRealtimeImagingToggled(bool checked)
{
    if (!m_imagingController) return;
    const auto imagingToggleNs = paimage::SocketReceiver::now();
    recordDiagnosticAction(checked ? QStringLiteral("imaging_start_requested")
                                   : QStringLiteral("imaging_stop_requested"),
        {{"monotonicNs", QString::number(imagingToggleNs)},
         {"ringMode", m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1},
         {"serviceRunning", m_imagingController->isRunning()}});
    if (checked) {
        // 启动成像：线性扫描启动线性重建，环形扫描先下发环形参数再启动环形重建
        m_chkRealtimeImaging->setEnabled(false);
        // 实时成像期间锁定成像参数与采集控制，避免运行中修改参数引发重启竞态/尺寸不匹配
        setImagingParamControlsEnabled(false);
        if (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1) {
            if (!m_ringConfigDialog) {
                m_ringConfigDialog = new RingConfigDialog(m_imagingController, this);
            }
            m_ringConfigDialog->setAcquisitionParams(
                m_sampleIntervalNs, ui->edtDataTime->text().toInt());
            m_ringConfigDialog->applyConfig();
            // 请求成像即打开旁路门控；服务连接完成前帧明确记为 ServiceNotReady。
            configureRingAssembler();
            startRingFeedWorker();
        }
        const bool alreadyRunning = m_imagingController->isRunning();
        if (m_imagingController->startSvc()) {
            if (alreadyRunning) {
                // 服务已在运行（例如停止监听未停服务、或取消后快速再勾选）：
                // 此时 svcReady 不会再发出，直接恢复馈送与 UI 状态，
                // 避免环形模式无成像弹窗/线性模式定时器不启动
                m_imagingEnabled = true;
                if (m_imagingController->isRingMode()) {
                    configureRingAssembler();
                    startRingFeedWorker();
                } else {
                    m_imagingTimer->start();
                }
                if (m_chkRealtimeImaging) m_chkRealtimeImaging->setEnabled(true);
                onImagingStarted();
                updateRingImagingStatus();   // 环形模式：服务就绪后先显示“成像中，等待重建数据…”
                return;
            }
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(m_imagingController, &ImagingController::svcReady,
                            this, [this, conn]() {
                disconnect(*conn);
                recordDiagnosticAction(QStringLiteral("imaging_service_ready"),
                    {{"monotonicNs", QString::number(paimage::SocketReceiver::now())}});
                m_imagingEnabled = true;
                if (!(m_imagingController && m_imagingController->isRingMode()))
                    m_imagingTimer->start();   // 环形模式由独立工作线程馈送
                if (m_chkRealtimeImaging) m_chkRealtimeImaging->setEnabled(true);
                onImagingStarted();
                updateRingImagingStatus();   // 环形模式：服务就绪后先显示“成像中，等待重建数据…”
            });
            QTimer::singleShot(5000, this, [this, conn]() {
                if (m_chkRealtimeImaging && !m_chkRealtimeImaging->isEnabled()) {
                    disconnect(*conn);
                    m_imagingEnabled = m_imagingController->isRunning();
                    recordDiagnosticAction(QStringLiteral("imaging_service_ready_timeout"),
                        {{"monotonicNs", QString::number(paimage::SocketReceiver::now())},
                         {"serviceRunning", m_imagingEnabled}});
                    m_chkRealtimeImaging->setEnabled(true);
                }
            });
        } else {
            recordDiagnosticAction(QStringLiteral("imaging_start_failed"),
                {{"monotonicNs", QString::number(paimage::SocketReceiver::now())}});
            stopRingFeedWorker();
            m_chkRealtimeImaging->setEnabled(true);
            setImagingParamControlsEnabled(true);   // 启动失败：立即恢复
        }
    } else {
        // 停止成像
        m_chkRealtimeImaging->setEnabled(false);
        stopRingFeedWorker();
        // 取消勾选时清除参数变更的自动重启意图，避免停止过程中服务又自动重启、
        // 导致控件无法恢复（R2 风险）
        m_restartRingOnSvcStop = false;
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(m_imagingController, &ImagingController::svcStopped,
                        this, [this, conn]() {
            disconnect(*conn);
            recordDiagnosticAction(QStringLiteral("imaging_service_stopped"),
                {{"monotonicNs", QString::number(paimage::SocketReceiver::now())}});
            m_imagingTimer->stop();
            m_imagingEnabled = false;
            setImagingParamControlsEnabled(true);   // svcStopped 后恢复（停止完成前保持锁定）
            if (m_chkRealtimeImaging) m_chkRealtimeImaging->setEnabled(true);
            onImagingStopped();
        });
        m_imagingController->stopSvc();
    }
}

// =====================================================================
// setImagingParamControlsEnabled — 实时成像运行期间锁定参数修改入口
// 勾选时禁用：成像参数按钮 + 采集控制框的采集时间/A延时/B延时；
// 恢复统一放在 svcStopped（或启动失败/成像错误）之后，避免停止中途可改参数。
// =====================================================================
void MainWindow::setImagingParamControlsEnabled(bool enable)
{
    if (m_btnImagingConfig) m_btnImagingConfig->setEnabled(enable);
    if (ui->edtDataTime)    ui->edtDataTime->setEnabled(enable);
    if (ui->edtADelay)      ui->edtADelay->setEnabled(enable);
    if (ui->edtBDelay)      ui->edtBDelay->setEnabled(enable);
}

void MainWindow::onDisplayTypeChanged(int index)
{
    // 显示类型仅控制通道信号视图（差分相位/瞬时频率）；
    // 成像方式（线性/环形扫描）由“实时成像”分组框中的 cmbImagingMode 控制
    ui->stackedMainDisplay->setCurrentIndex(0);
    ui->stackedDisplayMode->setCurrentIndex(m_isTileView ? 1 : 0);
    logMessage(index == DM_PHASE ? "切换到差分相位显示" : "切换到瞬时频率显示");
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i)
        for (int j = 0; j < 2; ++j)
            if (m_stackedWidgets[i][j])
                m_stackedWidgets[i][j]->setCurrentIndex(index);

    if (!m_loadingSettings) {
        saveSettings();
    }
}

void MainWindow::onImagingModeChanged(int index)
{
    // 成像方式：0=线性扫描（重建图像在独立弹窗显示，主窗口继续显示时域信号），
    //          1=环形扫描（主窗口保留通道监视，重建图像在独立弹窗显示）
    ui->stackedMainDisplay->setCurrentIndex(0);
    ui->stackedDisplayMode->setCurrentIndex(m_isTileView ? 1 : 0);
    if (index == 0) {
        for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i)
            for (int j = 0; j < 2; ++j)
                if (m_stackedWidgets[i][j])
                    m_stackedWidgets[i][j]->setCurrentIndex(ui->cmbDisplayType->currentIndex());
        // 自动保存仅环形模式有效（依赖圈末/超时重置）；切回线性时关闭
        if (ui->chkAutoSave->isChecked()) {
            QSignalBlocker blocker(ui->chkAutoSave);
            ui->chkAutoSave->setChecked(false);
            onAutoSaveToggled(false);
        }
        ui->chkAutoSave->setEnabled(false);
        logMessage("切换到线性扫描成像模式（重建图像在独立窗口显示）");
    } else {
        for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i)
            for (int j = 0; j < 2; ++j)
                if (m_stackedWidgets[i][j])
                    m_stackedWidgets[i][j]->setCurrentIndex(DM_FREQ);
        ui->chkAutoSave->setEnabled(true);
        logMessage("切换到环形扫描成像模式（重建图像在独立窗口显示）");
    }
    if (!m_loadingSettings) {
        saveSettings();
    }
}

void MainWindow::onRefreshRateChanged(int value)
{
    m_displayInterval = value;
    logMessage(QString("显示刷新间隔已设置为每 %1 个触发更新一次").arg(value));
}

// =====================================================================
// 30fps pull 模式刷新（替代旧项目的 push 信号）
// =====================================================================
void MainWindow::onDisplayRefresh()
{
    if (!m_displayEnabled || !m_netController) return;

    ++m_refreshCounter;
    if (!m_forceRefreshOnce && (m_refreshCounter % m_displayInterval != 0)) return;
    m_forceRefreshOnce = false;
    ++m_displayTickCount;   // 实际显示刷新计数（rescaleAxes 节流用）

    const int displayMode = ui->cmbDisplayType->currentIndex();
    // 环形扫描在主窗口按瞬时频率监视通道信号（重建图像在控制台显示）
    const bool ringMode = (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1);
    const int effMode = ringMode ? DM_FREQ : displayMode;

    // 相位 / 频率 / 环形监视模式
    const bool isTileView = m_isTileView;

    // Tab 模式：预先确定要刷新的槽位和通道（不对其他卡调用 tryRead，保留 m_hasNew）
    int tabSlot = -1, tabCh = -1;
    if (!isTileView) {
        int tabIdx = ui->tabWidget->currentIndex();
        tabSlot = tabIdx / 2;
        tabCh   = tabIdx % 2;
    }

    auto toQVec = [](const std::vector<double> &v) {
        QVector<double> r(static_cast<int>(v.size()));
        for (int k = 0; k < static_cast<int>(v.size()); ++k) r[k] = v[k];
        return r;
    };

    bool anyUpdated = false;

    for (int slotIdx = 0; slotIdx < CARDS_PER_DISPLAY_GROUP; ++slotIdx) {
        int globalCard = m_currentGroup * CARDS_PER_DISPLAY_GROUP + slotIdx;
        if (globalCard >= m_nCards) break;

        // Tab 模式：只处理当前显示的卡，跳过其他卡（不消耗其 m_hasNew 标志）
        if (!isTileView && slotIdx != tabSlot) continue;

        DisplayBuffer *db = m_netController->displayBuffer(globalCard);
        if (!db) continue;
        DisplayBuffer::Snapshot snap;
        if (!db->tryRead(snap) || !snap.valid) continue;

        // 诊断：打印首次读到数据的信息
        if (slotIdx == 0) {
            static int diagCount = 0;
            if (++diagCount <= 5) {
                logMessage(QString("[显示] 卡%1 读到%2点 phaseA[0]=%3 freqA[0]=%4")
                    .arg(globalCard).arg(snap.freqA.size())
                    .arg(snap.phaseA.empty() ? 0 : snap.phaseA[0], 0, 'f', 2)
                    .arg(snap.freqA.empty() ? 0 : snap.freqA[0], 0, 'f', 2));
            }
        }

        QVector<double> freqA  = toQVec(snap.freqA);
        QVector<double> freqB  = toQVec(snap.freqB);
        QVector<double> phaseA = toQVec(snap.phaseA);
        QVector<double> phaseB = toQVec(snap.phaseB);

        // x 轴以频率数组长度为准（phaseA/freqA 在 downsample 中同步 resize，通常相同）
        const int nPts = freqA.size();
        QVector<double> xAxis(nPts);
        for (int k = 0; k < nPts; ++k) xAxis[k] = k;

        if (!isTileView) {
            // Tab 单通道模式
            if (tabCh == 0)
                updatePlot(slotIdx, 0, effMode == DM_PHASE ? phaseA : freqA, freqA, xAxis);
            else
                updatePlot(slotIdx, 1, effMode == DM_PHASE ? phaseB : freqB, freqB, xAxis);
            // 瞬时频率模式：仅当前台可见的通道计算/显示频域幅值谱（资源优化）
            QCustomPlot *specPlot = m_plotsSpectrum[slotIdx][tabCh];
            if (effMode == DM_FREQ && specPlot && specPlot->isVisible()) {
                const QVector<double> &trace = (tabCh == 0) ? freqA : freqB;
                // 与 updatePlot 相同的显示裁切：频域幅值谱仅对应时域显示段
                const int pkStartInput = ui->edtPkStart->text().toInt();
                const int pkEndInput   = ui->edtPkEnd->text().toInt();
                int pkStart = 0, pkEnd = trace.size();
                computePkRange(trace.size(), pkStartInput, pkEndInput, &pkStart, &pkEnd);
                const int len = pkEnd - pkStart;
                QVector<double> seg(len);
                for (int i = 0; i < len; ++i) seg[i] = trace[pkStart + i];
                QVector<double> mag = computeMagnitudeSpectrum(seg);
                if (!mag.isEmpty()) {
                    // 原始采样点数按显示段比例换算，保持有效采样率与频点计算正确
                    const int origSeg = (len > 0 && nPts > 0)
                        ? static_cast<int>(static_cast<long long>(snap.sampleCount) * len / nPts)
                        : 0;
                    QVector<double> bins =
                        makeSpectrumBins(mag.size(), origSeg, len);
                    updateSpectrumPlot(slotIdx, tabCh, bins, mag);
                }
            }
            anyUpdated = true;
        } else {
            // 平铺模式：写数据，延迟 replot（统一批量）
            updatePlot(slotIdx, 0, effMode == DM_PHASE ? phaseA : freqA, freqA, xAxis, false);
            updatePlot(slotIdx, 1, effMode == DM_PHASE ? phaseB : freqB, freqB, xAxis, false);
            anyUpdated = true;
        }
    }

    // 平铺模式：统一触发全部图表重绘（无可见性过滤，确保隐藏态下数据也就绪）
    if (isTileView && anyUpdated) {
        for (int s = 0; s < CARDS_PER_DISPLAY_GROUP; ++s) {
            int gc = m_currentGroup * CARDS_PER_DISPLAY_GROUP + s;
            if (gc >= m_nCards) break;
            QCustomPlot *plt0 = (effMode == DM_PHASE) ? m_plotsPhase[s][0] : m_plotsFrequency[s][0];
            QCustomPlot *plt1 = (effMode == DM_PHASE) ? m_plotsPhase[s][1] : m_plotsFrequency[s][1];
            if (plt0) plt0->replot(QCustomPlot::rpQueuedReplot);
            if (plt1) plt1->replot(QCustomPlot::rpQueuedReplot);
        }
    }
}

// =====================================================================
// updatePlot（完整保留旧项目逻辑）
// =====================================================================
void MainWindow::updatePlot(int cardId, int channel,
                             const QVector<double> &data,
                             const QVector<double> &frequency,
                             const QVector<double> &xAxis,
                             bool doReplot)
{
    if (cardId < 0 || cardId >= CARDS_PER_DISPLAY_GROUP || channel < 0 || channel >= 2) return;
    if (data.isEmpty()) return;

    // 确保视口与控件尺寸同步；通道图统一软件渲染，避免 OpenGL 缓冲覆盖相邻图窗
    auto syncViewport = [](QCustomPlot *plot) {
        if (!plot) return;
        // 通道图统一软件渲染：OpenGL 缓冲可能超出控件区域并覆盖相邻
        // 频域窗口（时域窗口下半不可见问题），因此不再启用 OpenGL；
        // 软件渲染下视口由 QCustomPlot::resizeEvent 自动维护，无需手动同步
        if (plot->openGl()) plot->setOpenGl(false);
    };

    try {
        int displayMode = ui->cmbDisplayType->currentIndex();
        if (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1)
            displayMode = DM_FREQ;  // 环形模式按瞬时频率监视
        QVector<double> x;
        if (!xAxis.isEmpty()) {
            x = xAxis;
        } else {
            x.resize(data.size());
            for (int i = 0; i < data.size(); ++i) x[i] = i;
        }

        if (displayMode == DM_PHASE) {
            QCustomPlot *plotPhase = m_plotsPhase[cardId][channel];
            if (!plotPhase || plotPhase->graphCount() == 0) return;
            
            syncViewport(plotPhase);

            // 峰峰值统计范围 → 显示裁切（仅显示层裁切 Aline 段，不动成像/保存数据）
            const int pkStartInput = ui->edtPkStart->text().toInt();
            const int pkEndInput   = ui->edtPkEnd->text().toInt();
            int pkStart = 0, pkEnd = x.size();
            computePkRange(x.size(), pkStartInput, pkEndInput, &pkStart, &pkEnd);
            const int len = pkEnd - pkStart;

            plotPhase->graph(0)->setData(x.mid(pkStart, len), data.mid(pkStart, len));
            if (m_firstPlot[cardId][channel]) { plotPhase->rescaleAxes(); m_firstPlot[cardId][channel] = false; }
            else if (m_autoRescalePlot[cardId][channel] && (m_displayTickCount % kAutoRescaleEveryTick == 0))
                plotPhase->rescaleAxes();

            // 相位图也显示峰峰值（与显示裁切段一致）
            if (m_pkpkLabels[cardId][channel]) {
                double mn = data[pkStart], mx = data[pkStart];
                for (int i = pkStart + 1; i < pkEnd; ++i) {
                    if (data[i] < mn) mn = data[i];
                    if (data[i] > mx) mx = data[i];
                }
                m_pkpkLabels[cardId][channel]->setText(
                    QString("峰峰值: %1 rad").arg(mx - mn, 0, 'f', 3));
            }

            if (doReplot) plotPhase->replot(QCustomPlot::rpQueuedReplot);
        }
        else if (displayMode == DM_FREQ) {
            QCustomPlot *plotFreq = m_plotsFrequency[cardId][channel];
            if (!plotFreq || plotFreq->graphCount() == 0) return;
            if (frequency.isEmpty()) return;
            
            syncViewport(plotFreq);

            // 峰峰值统计范围 → 显示裁切：时域信号按范围裁切，
            // 频域（下方幅值谱）在 onDisplayRefresh 中使用同一范围计算
            const int pkStartInput = ui->edtPkStart->text().toInt();
            const int pkEndInput   = ui->edtPkEnd->text().toInt();
            int pkStart = 0, pkEnd = frequency.size();
            computePkRange(frequency.size(), pkStartInput, pkEndInput, &pkStart, &pkEnd);
            const int len = pkEnd - pkStart;

            double minFreq = frequency[pkStart], maxFreq = frequency[pkStart];
            for (int i = pkStart + 1; i < pkEnd; ++i) {
                if (frequency[i] < minFreq) minFreq = frequency[i];
                if (frequency[i] > maxFreq) maxFreq = frequency[i];
            }
            m_pkpkLabels[cardId][channel]->setText(
                QString("峰峰值: %1 kHz").arg(maxFreq - minFreq, 0, 'f', 2));

            plotFreq->graph(0)->setData(x.mid(pkStart, len), frequency.mid(pkStart, len));
            if (m_firstPlot[cardId][channel]) { plotFreq->rescaleAxes(); m_firstPlot[cardId][channel] = false; }
            else if (m_autoRescalePlot[cardId][channel] && (m_displayTickCount % kAutoRescaleEveryTick == 0))
                plotFreq->rescaleAxes();
            if (doReplot) plotFreq->replot(QCustomPlot::rpQueuedReplot);
        }
    } catch (...) {}
}

// =====================================================================
// updateSpectrumPlot — 瞬时频率模式下方频域幅值谱
// =====================================================================
void MainWindow::updateSpectrumPlot(int cardId, int channel,
                                    const QVector<double> &xAxis,
                                    const QVector<double> &magnitude,
                                    bool doReplot)
{
    if (cardId < 0 || cardId >= CARDS_PER_DISPLAY_GROUP || channel < 0 || channel >= 2) return;
    QCustomPlot *plot = m_plotsSpectrum[cardId][channel];
    if (!plot || plot->graphCount() == 0 || magnitude.isEmpty()) return;

    // 频域图使用软件渲染，避免与上方时域图叠加时的 OpenGL 缓冲覆盖问题（工作四）；
    // 软件渲染下视口由 resizeEvent 自动维护，无需手动同步
    if (plot->openGl()) plot->setOpenGl(false);

    const bool firstData = (plot->graph(0)->data()->size() == 0);
    plot->graph(0)->setData(xAxis, magnitude);

    // 首帧或开启自适应时自动标定坐标轴；关闭自适应后保留用户右键设置的范围
    if (firstData || m_autoRescaleSpectrum[cardId][channel]) {
        if (xAxis.size() == magnitude.size() && !xAxis.isEmpty()) {
            double xMax = xAxis.last();
            if (xMax <= 0) xMax = 1.0;
            plot->xAxis->setRange(0, xMax);
        }
        double maxMag = 0.0;
        for (double v : magnitude)
            if (v > maxMag) maxMag = v;
        plot->yAxis->setRange(0.0, maxMag > 0.0 ? maxMag * 1.05 : 1.0);
    }

    if (doReplot) plot->replot(QCustomPlot::rpQueuedReplot);
}

// =====================================================================
// 统计更新（1Hz/2Hz）
// =====================================================================
QString MainWindow::formatCardStatusText(int cardNumber,
                                         const CardStats::Snapshot& stats)
{
    return CardStatusFormatting::text(cardNumber, stats);
}

QString MainWindow::formatCardStatusTooltip(const CardStats::Snapshot& stats)
{
    return CardStatusFormatting::tooltip(stats);
}

void MainWindow::onUpdateStatistics()
{
    // 告警冷却递减（每 2s timer tick 减 1）
    if (m_saveWarnCooldown > 0) --m_saveWarnCooldown;

    if (m_imagingBypass) {
        const auto s = m_imagingBypass->snapshot();
        if (auto *recorder = DiagnosticRecorder::instance()) {
            recorder->recordEvent(QStringLiteral("imaging.bypass"),
                                  QStringLiteral("imaging_queue_snapshot"),
                                  DiagnosticRecorder::Severity::Info,
                {{QStringLiteral("session"), QString::number(s.activeSession)},
                 {QStringLiteral("enqueueAttempts"), QString::number(s.attempts)},
                 {QStringLiteral("enqueued"), QString::number(s.accepted)},
                 {QStringLiteral("dequeued"), QString::number(s.dequeued)},
                 {QStringLiteral("currentDepth"), QString::number(s.currentDepth)},
                 {QStringLiteral("peakDepth"), QString::number(s.peakDepth)},
                 {QStringLiteral("processed"), QString::number(s.processed)},
                 {QStringLiteral("processFailed"), QString::number(s.processFailed)},
                 {QStringLiteral("dropDisabled"), QString::number(s.droppedDisabled)},
                 {QStringLiteral("dropQueueFull"), QString::number(s.droppedQueueFull)},
                 {QStringLiteral("dropQueueBusy"), QString::number(s.droppedQueueBusy)},
                 {QStringLiteral("dropStopping"), QString::number(s.droppedStopping)},
                 {QStringLiteral("dropInvalidFrame"), QString::number(s.droppedInvalidFrame)},
                 {QStringLiteral("dropStaleSession"), QString::number(s.droppedStaleSession)},
                 {QStringLiteral("dropServiceNotReady"), QString::number(s.droppedServiceNotReady)},
                 {QStringLiteral("dropCallbackFailed"), QString::number(s.droppedCallbackFailed)},
                 {QStringLiteral("dropOnClear"), QString::number(s.droppedOnClear)},
                 {QStringLiteral("blocksFormed"), QString::number(s.blocksFormed)},
                 {QStringLiteral("blocksSubmitted"), QString::number(s.blocksSubmitted)},
                 {QStringLiteral("blocksSkipped"), QString::number(s.blocksSkipped)},
                 {QStringLiteral("blockExceptions"), QString::number(s.blockExceptions)},
                 {QStringLiteral("maxSubmitNs"), QString::number(s.maxSubmitNs)},
                 {QStringLiteral("maxWorkerNs"), QString::number(s.maxWorkerNs)}});
        }
    }

    // 检测保存状态意外停止（如磁盘满）并同步 UI
    if (m_netController && m_isListening) {
        bool isSavingNow = m_netController->isSaving();
        bool uiShowsSaving = (ui->btnToggleSave->text() == "停止保存");
        if (uiShowsSaving && !isSavingNow) {
            // 保存意外停止（如磁盘满），同步按钮状态
            setBtnText(ui->btnToggleSave, "开始保存");
            ui->btnToggleSave->setProperty("state", QVariant());
            ui->btnToggleSave->style()->unpolish(ui->btnToggleSave);
            ui->btnToggleSave->style()->polish(ui->btnToggleSave);
            m_reconSaveEnabled = false;
            logMessage("警告：数据保存已意外停止，请检查磁盘空间");
        }
    }

    // 计算当前组实际包含的卡数（最后一组可能不足4张）
    int firstGlobal = m_currentGroup * CARDS_PER_DISPLAY_GROUP;
    int cardsInGroup = std::min(CARDS_PER_DISPLAY_GROUP, m_nCards - firstGlobal);
    if (cardsInGroup < 0) cardsInGroup = 0;

    if (!m_netController) {
        for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i) {
            int virtualCardNum = firstGlobal + i + 1;
            if (i < cardsInGroup)
                m_lblStats[i]->setText(QString("卡%1: 等待连接...").arg(virtualCardNum));
            else
                m_lblStats[i]->setText(QString("--"));
            m_lblStats[i]->setToolTip(QString());
        }
        return;
    }

    const double HIGH_RATE_THRESHOLD = 800.0;
    bool anyExceeds = false;

    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i) {
        int globalCard = firstGlobal + i;
        int virtualCardNum = globalCard + 1;

        if (i >= cardsInGroup) {
            m_lblStats[i]->setText(QString("--"));
            m_lblStats[i]->setToolTip(QString());
            continue;
        }

        auto statsOpt = m_netController->getCardStats(globalCard);
        if (statsOpt.has_value()) {
            const auto &s = statsOpt.value();
            if (s.recvMbps > HIGH_RATE_THRESHOLD) anyExceeds = true;

            // 常驻栏只显示“丢失”；详细采集统计集中放入 tooltip，避免
            // 状态栏文本随计数增长而撑宽布局或掩盖关键信息。
            m_lblStats[i]->setText(formatCardStatusText(virtualCardNum, s));
            m_lblStats[i]->setToolTip(formatCardStatusTooltip(s));

            // 存储队列满 → 非阻塞告警（冷却期内不重复）
            uint64_t newDisc = s.saveQueueDiscards;
            if (newDisc < m_prevSaveDiscards[globalCard])
                m_prevSaveDiscards[globalCard] = newDisc;  // 控制器重建后重置
            const uint64_t delta = newDisc - m_prevSaveDiscards[globalCard];
            m_prevSaveDiscards[globalCard] = newDisc;
            if (delta > 0 && m_saveWarnCooldown == 0) {
                logMessage(QString("⚠ 卡%1: 存储队列已满（存队: %2/400），本周期丢失 %3 帧。"
                    "请将存储目录改到更快的磁盘，或减少同时存储的卡数")
                    .arg(virtualCardNum)
                    .arg(s.saveQueueDepth)
                    .arg(delta));
                m_saveWarnCooldown = 5;  // ~10秒不重复告警（2s定时器 × 5）
            }
        } else {
            m_lblStats[i]->setText(QString("卡%1: 等待连接...").arg(virtualCardNum));
            m_lblStats[i]->setToolTip(QString());
        }
    }

    if (anyExceeds && !m_highDataRateWarningShown && m_isMeasuring) {
        m_highDataRateWarningShown = true;
        QMessageBox::warning(this, "数据吞吐量警告",
            QString("当前数据吞吐量较大（>%1 Mb/s），可能导致丢包或控制失效。\n"
                    "建议降低采集时间或触发频率。").arg(HIGH_RATE_THRESHOLD));
        logMessage(QString(" 警告：检测到高数据速率（>%1 Mb/s）").arg(HIGH_RATE_THRESHOLD));
    }
}

// =====================================================================
// 日志
// =====================================================================
void MainWindow::logMessage(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    ui->txtLog->append(QString("[%1] %2").arg(timestamp).arg(message));
    QTextCursor cursor = ui->txtLog->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->txtLog->setTextCursor(cursor);
    ui->txtLog->ensureCursorVisible();

    if (auto *recorder = DiagnosticRecorder::instance()) {
        QJsonObject fields{{QStringLiteral("source"), QStringLiteral("ui")}};
        if (!m_diagnosticListenId.isEmpty())
            fields.insert(QStringLiteral("listenId"), m_diagnosticListenId);
        recorder->logText(message, DiagnosticRecorder::Severity::Info, fields);
    }
}

void MainWindow::recordDiagnosticAction(const QString &action,
                                        const QJsonObject &fields)
{
    auto *recorder = DiagnosticRecorder::instance();
    if (!recorder) return;
    QJsonObject actionFields = fields;
    actionFields.insert(QStringLiteral("source"), QStringLiteral("ui"));
    if (!m_diagnosticListenId.isEmpty())
        actionFields.insert(QStringLiteral("listenId"), m_diagnosticListenId);
    recorder->recordEvent(QStringLiteral("ui.action"), action,
                          DiagnosticRecorder::Severity::Info, actionFields);
}

QJsonObject MainWindow::diagnosticAcquisitionSnapshot(
    const QVector<QString> &targetIPs, const QString &phase) const
{
    QVector<QString> effectiveTargets = targetIPs;
    if (effectiveTargets.isEmpty() && m_netController) {
        const AcqConfig &config = m_netController->config();
        for (const std::string &ip : config.targetIPs)
            effectiveTargets.append(QString::fromStdString(ip));
    }

    QJsonArray targetArray;
    for (const QString &ip : effectiveTargets) targetArray.append(ip);
    return {
        {QStringLiteral("kind"), QStringLiteral("acquisition")},
        {QStringLiteral("phase"), phase},
          {QStringLiteral("source"), QStringLiteral("ui")},
          {QStringLiteral("listenId"), m_diagnosticListenId},
          {QStringLiteral("targetSource"), m_activeTargetSource},
          {QStringLiteral("nCards"), effectiveTargets.isEmpty() ? m_nCards : effectiveTargets.size()},
        {QStringLiteral("targetIPs"), targetArray},
        {QStringLiteral("localBindIP"), m_localBindIP},
        {QStringLiteral("scanBaseIP"), m_scanBaseIP},
        {QStringLiteral("scanIPCount"), m_scanIPCount},
        {QStringLiteral("dataTimeNs"), ui->edtDataTime->text().toInt()},
        {QStringLiteral("delayA"), ui->edtADelay->text().toInt()},
        {QStringLiteral("delayB"), ui->edtBDelay->text().toInt()},
        {QStringLiteral("displayPoints"), ui->spnDownsampleRatio->value()},
        {QStringLiteral("bitsPerChannel"), m_bitsPerChannel},
        {QStringLiteral("sampleIntervalNs"), m_sampleIntervalNs},
        {QStringLiteral("sampleRateHz"), static_cast<double>(FPGA_ADC_FREQ_HZ)},
        {QStringLiteral("sampleRateSource"), QStringLiteral("FPGA_ADC_FREQ_HZ (250MHz)")}
    };
}

void MainWindow::recordAcquisitionSnapshot(const QVector<QString> &targetIPs,
                                           const QString &phase)
{
    if (auto *recorder = DiagnosticRecorder::instance()) {
        const QJsonObject snapshot = diagnosticAcquisitionSnapshot(targetIPs, phase);
        recorder->recordSettingsSnapshot(snapshot);
        if (phase.startsWith(QStringLiteral("listen")))
            recorder->recordNetworkSnapshot(snapshot);
    }
}

void MainWindow::onDiagnosticStatusTick()
{
    auto *recorder = DiagnosticRecorder::instance();
    if (!recorder) return;

    const DiagnosticRecorder::Status status = recorder->status();
    const auto &drops = status.drops;
    const bool hasIssue = status.writeError
        || drops.queueDropped > 0
        || drops.criticalQueueDropped > 0
        || drops.noiseDropped > 0
        || drops.writeFallbackDropped > 0;
    const bool previousIssue = m_diagnosticLastWriteError
        || m_diagnosticLastQueueDropped > 0
        || m_diagnosticLastCriticalQueueDropped > 0
        || m_diagnosticLastNoiseDropped > 0
        || m_diagnosticLastFallbackDropped > 0;
    const bool changed = !m_diagnosticStatusInitialized
        || status.writeError != m_diagnosticLastWriteError
        || status.writeErrorText != m_diagnosticLastWriteErrorText
        || drops.queueDropped != m_diagnosticLastQueueDropped
        || drops.criticalQueueDropped != m_diagnosticLastCriticalQueueDropped
        || drops.noiseDropped != m_diagnosticLastNoiseDropped
        || drops.writeFallbackDropped != m_diagnosticLastFallbackDropped;

    m_diagnosticStatusInitialized = true;
    m_diagnosticLastWriteError = status.writeError;
    m_diagnosticLastWriteErrorText = status.writeErrorText;
    m_diagnosticLastQueueDropped = drops.queueDropped;
    m_diagnosticLastCriticalQueueDropped = drops.criticalQueueDropped;
    m_diagnosticLastNoiseDropped = drops.noiseDropped;
    m_diagnosticLastFallbackDropped = drops.writeFallbackDropped;

    if (!hasIssue) {
        if (previousIssue) {
            statusBar()->showMessage(QStringLiteral("诊断日志写入状态已恢复"), 5000);
            if (m_diagnosticStatusNoticeCount < 5)
                logMessage(QStringLiteral("诊断日志写入状态已恢复"));
        }
        m_diagnosticStatusNoticeCount = 0;
        return;
    }
    if (!changed || m_diagnosticStatusNoticeCount >= 5) return;

    QStringList details;
    if (status.writeError)
        details.append(QStringLiteral("写入失败：%1")
                       .arg(status.writeErrorText.isEmpty()
                                ? QStringLiteral("未知错误") : status.writeErrorText));
    if (drops.queueDropped > 0)
        details.append(QStringLiteral("队列丢弃=%1").arg(drops.queueDropped));
    if (drops.criticalQueueDropped > 0)
        details.append(QStringLiteral("高优先级队列丢弃=%1").arg(drops.criticalQueueDropped));
    if (drops.noiseDropped > 0)
        details.append(QStringLiteral("噪声限流丢弃=%1").arg(drops.noiseDropped));
    if (drops.writeFallbackDropped > 0)
        details.append(QStringLiteral("写入回退丢弃=%1").arg(drops.writeFallbackDropped));

    const QString message = QStringLiteral("诊断日志状态异常：%1").arg(details.join(QStringLiteral("；")));
    ++m_diagnosticStatusNoticeCount;
    statusBar()->showMessage(message, 8000);
    // Update the cached status before logging so this notice cannot recursively
    // retrigger itself through the recorder's own queue accounting.
    logMessage(message);
}

// =====================================================================
// 样式表加载
// =====================================================================
void MainWindow::loadStyleSheet()
{
    QFile styleFile(":/resources/styles.qss");
    if (!styleFile.exists())
        styleFile.setFileName("resources/styles.qss");
    if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
        qApp->setStyleSheet(QLatin1String(styleFile.readAll()));
        styleFile.close();
        logMessage("样式表加载成功");
    } else {
        logMessage(QString("警告：无法加载样式表 - %1").arg(styleFile.fileName()));
    }
}

// =====================================================================
// 设置保存 / 加载
// =====================================================================
void MainWindow::loadSettings()
{
    QSettings settings(paimageSettingsPath(), QSettings::IniFormat);

    // 阻塞信号，防止 setValue 触发 saveSettings 覆写未加载的参数
    ui->spnRefreshRate->blockSignals(true);
    ui->spnDownsampleRatio->blockSignals(true);
    ui->cmbDisplayType->blockSignals(true);
    ui->cmbImagingMode->blockSignals(true);

    // ── 注册表专属配置（无 UI 接口）────────────────────────────────────
    // 采集卡数量：AcquisitionParams/NCards（默认4，有效范围1~MAX_CARDS）
    // 修改方式：regedit → HKCU\Software\MC410T\MC410T_Receiver
    int savedNCards = settings.value("AcquisitionParams/NCards", 4).toInt();
    m_nCards = qBound(1, savedNCards, MAX_CARDS);
    // 控制 socket 本地绑定 IP：NetworkParams/LocalBindIP（默认空，即 INADDR_ANY）
    // 双口网卡（如 ConnectX-5 MCX512A-ACAT）只接一个口时必须填写已连接口的本地IP
    // 修改方式：reg add "HKCU\Software\MC410T\MC410T_Receiver\NetworkParams" /v LocalBindIP /t REG_SZ /d "192.168.0.100" /f
    m_localBindIP = settings.value("NetworkParams/LocalBindIP", "").toString();
    // 网段扫描范围（自动识别采集卡用）：ScanBaseIP 默认 192.168.0.2，ScanIPCount 默认 32
    m_scanBaseIP  = settings.value("NetworkParams/ScanBaseIP", QString(DEFAULT_SCAN_BASE_IP)).toString();
    m_scanIPCount = qBound(1, settings.value("NetworkParams/ScanIPCount", DEFAULT_SCAN_IP_COUNT).toInt(), MAX_CARDS);
    m_targetIPRangeText.clear();  // 尚未识别

    // ══ 数据格式参数（必须在 onDisplayTypeChanged 之前读取，否则 saveSettings 会覆写默认值）══
    // 真实采集采样率固定为 250 MHz（FPGA_ADC_FREQ_HZ，采样间隔 FPGA_ADC_INTERVAL_NS=4ns）。
    // 注册表历史值（AcquisitionParams/SampleIntervalNs）仅保留/回写，用于与线性实例
    // 统一处理；历史遗留的 5.0（200MHz 模拟）不再参与任何链路，避免环形成像
    // 参数窗口与重建核心把 200MHz 当成真实采样率下发。
    // 修改：reg add "HKCU\Software\MC410T\MC410T_Receiver\AcquisitionParams" /v BitsPerChannel /t REG_DWORD /d 32 /f
    m_bitsPerChannel = settings.value("AcquisitionParams/BitsPerChannel", 32).toInt();
    if (m_bitsPerChannel != 16 && m_bitsPerChannel != 32) m_bitsPerChannel = 32;
    m_logicalTriggersPerRound = settings.value(
        "AcquisitionParams/LogicalTriggersPerRound",
        AcqConfig::kDefaultLogicalTriggersPerRound).toInt();
    if (m_logicalTriggersPerRound <= 0)
        m_logicalTriggersPerRound = AcqConfig::kDefaultLogicalTriggersPerRound;
    const double registrySampleIntervalNs =
        settings.value("AcquisitionParams/SampleIntervalNs", FPGA_ADC_INTERVAL_NS).toDouble();
    m_sampleIntervalNs = FPGA_ADC_INTERVAL_NS;   // 全链路唯一采样率来源
    if (registrySampleIntervalNs > 0.0 &&
        std::fabs(registrySampleIntervalNs - FPGA_ADC_INTERVAL_NS) > 1e-9) {
        logMessage(QString("⚠️ 注册表历史采样间隔 %1ns 已统一为 250MHz（4.0ns/点），"
                           "历史值保留不删除，保存参数时会回写 4.0")
                       .arg(registrySampleIntervalNs, 0, 'f', 1));
    }

    ui->edtDataTime->setText(settings.value("AcquisitionParams/DataTime", "40000").toString());
    ui->edtADelay->setText(settings.value("AcquisitionParams/ADelay", "1000").toString());
    ui->edtBDelay->setText(settings.value("AcquisitionParams/BDelay", "1000").toString());
    QString defaultDir = QDir::currentPath() + "/data";
    ui->edtSaveDir->setText(settings.value("SaveParams/Directory", defaultDir).toString());
    ui->edtTriggersPerFile->setText(settings.value("SaveParams/TriggersPerFile", "1000").toString());
    ui->edtFileSuffix->setText(settings.value("SaveParams/FileSuffix", "").toString());
    ui->chkEnableDisplay->setChecked(settings.value("DisplayParams/Enabled", true).toBool());
    // ═══ 所有 DisplayParams 必须在 onDisplayTypeChanged 前读取 ═══
    ui->spnDownsampleRatio->setValue(settings.value("DisplayParams/DownsampleRatio", 1000).toInt());
    ui->spnRefreshRate->setValue(settings.value("DisplayParams/RefreshRate", 10).toInt());
    ui->edtPkStart->setText(settings.value("DisplayParams/PkStart", "0").toString());
    ui->edtPkEnd->setText(settings.value("DisplayParams/PkEnd", "10000").toString());
    m_enableDownsampling = settings.value("DisplayParams/EnableDownsampling", false).toBool();
    m_autoRescaleAxes    = settings.value("DisplayParams/AutoRescaleAxes", true).toBool();
    int displayType = settings.value("DisplayParams/Type", 1).toInt();
    if (displayType != DM_PHASE) displayType = DM_FREQ;   // 显示类型仅保留差分相位/瞬时频率
    ui->cmbDisplayType->setCurrentIndex(displayType);
    int imagingMode = settings.value("DisplayParams/ImagingMode", 0).toInt();
    if (imagingMode != 1) imagingMode = 0;                 // 0=线性扫描，1=环形扫描
    ui->cmbImagingMode->setCurrentIndex(imagingMode);
    // 解除信号阻塞
    ui->spnRefreshRate->blockSignals(false);
    ui->spnDownsampleRatio->blockSignals(false);
    ui->cmbDisplayType->blockSignals(false);
    ui->cmbImagingMode->blockSignals(false);

    // 设置加载标志，阻止 onDisplayTypeChanged 中 saveSettings 覆盖默认值
    m_loadingSettings = true;
    onRefreshRateChanged(ui->spnRefreshRate->value());
    onDisplayTypeChanged(displayType);

    rebuildDynamicUI();

    // rebuildDynamicUI 内部也会调用 onDisplayTypeChanged，此时必须仍持有标志
    m_loadingSettings = false;

    QString fmtDesc = (m_bitsPerChannel == 32)
        ? QString("250MSa/s Q16.16（满速率 %1ns/点）").arg(m_sampleIntervalNs, 0, 'f', 1)
        : QString("125MSa/s Q0.15（2抽1 %1ns/点）").arg(m_sampleIntervalNs, 0, 'f', 1);

    // ══ 测试模式开关 ════════════════════════════════════════════
    m_useTestImagingData = settings.value("TestMode/Enabled", false).toBool();

    // ══ 加载成像参数 ════════════════════════════════════════════
    // 采样率与采集侧统一固定 250 MHz：注册表历史 DaqHz 仅检测/回写，不参与链路。
    const double registryDaqHz =
        settings.value("ImagingParams/General/DaqHz", static_cast<double>(FPGA_ADC_FREQ_HZ)).toDouble();
    m_imagingGeneralParams.daq_hz = static_cast<float>(FPGA_ADC_FREQ_HZ);
    if (registryDaqHz > 0.0 && std::fabs(registryDaqHz - FPGA_ADC_FREQ_HZ) > 1.0) {
        logMessage(QString("⚠️ 注册表历史成像采样率 %1Hz 已统一为 250MHz，"
                           "历史值保留不删除，保存参数时会回写")
                       .arg(registryDaqHz, 0, 'f', 0));
    }
    m_imagingGeneralParams.depth       = settings.value("ImagingParams/General/Depth", 50000).toInt();
    m_imagingGeneralParams.isMultiFiber= settings.value("ImagingParams/General/IsMultiFiber", true).toBool();
    m_imagingGeneralParams.cardNum    = settings.value("ImagingParams/General/CardNum", 4).toInt();
    m_imagingGeneralParams.physicalChannels = settings.value("ImagingParams/General/PhysicalChannels", 8).toInt();
    m_imagingGeneralParams.channelNum = settings.value("ImagingParams/General/ChannelNum", 64).toInt();
    m_imagingScanParams.stepsize_um    = settings.value("ImagingParams/Scan/StepsizeUm", 10.0f).toFloat();
    m_imagingScanParams.move_aline     = settings.value("ImagingParams/Scan/MoveAline", 100).toInt();
    m_imagingScanParams.channel_aline  = settings.value("ImagingParams/Scan/ChannelAline", 50).toInt();
    m_imagingScanParams.nx             = settings.value("ImagingParams/Scan/Nx", 1200).toInt();
    m_imagingScanParams.ny             = settings.value("ImagingParams/Scan/Ny", 800).toInt();
    m_imagingScanParams.dx_um          = settings.value("ImagingParams/Scan/DxUm", 10.0f).toFloat();
    m_imagingScanParams.dy_um          = settings.value("ImagingParams/Scan/DyUm", 10.0f).toFloat();
    m_imagingScanParams.x0_m           = settings.value("ImagingParams/Scan/X0M", -6e-3f).toFloat();
    m_imagingScanParams.y0_m           = settings.value("ImagingParams/Scan/Y0M", 4e-3f).toFloat();
    m_imagingReconParams.delay          = settings.value("ImagingParams/Recon/Delay", 141).toInt();
    m_imagingReconParams.sos1_mps       = settings.value("ImagingParams/Recon/Sos1Mps", 1500.0f).toFloat();
    m_imagingReconParams.sos2_mps       = settings.value("ImagingParams/Recon/Sos2Mps", 1560.0f).toFloat();
    m_imagingReconParams.isDualSoS      = settings.value("ImagingParams/Recon/IsDualSoS", false).toBool();
    m_imagingReconParams.filterType     = settings.value("ImagingParams/Recon/FilterType", "Bandpass").toString();
    m_imagingReconParams.filterFreqLow_Hz  = settings.value("ImagingParams/Recon/FilterFreqLowHz", 1e6f).toFloat();
    m_imagingReconParams.filterFreqHigh_Hz = settings.value("ImagingParams/Recon/FilterFreqHighHz", 40e6f).toFloat();
    m_imagingReconParams.threshold      = settings.value("ImagingParams/Recon/Threshold", 1000.0f).toFloat();
    m_imagingReconParams.dynRange_db    = settings.value("ImagingParams/Recon/DynRangeDb", 50.0f).toFloat();
    m_imagingReconParams.outputType     = settings.value("ImagingParams/Recon/OutputType", "RF").toString();
    // ══ 新参数持久化加载 ════════════════════════════════════════════
    m_imagingReconParams.delayTimePoint = settings.value("ImagingParams/Recon/DelayTimePoint", 1601).toInt();
    m_imagingReconParams.caliCardDelay  = strToVec(settings.value("ImagingParams/Recon/CaliCardDelay",
        "0,2,2,2,4,6,6,8").toString());
    m_imagingReconParams.caliFiberDelay = strToVec(settings.value("ImagingParams/Recon/CaliFiberDelay",
        "534,484,428,372,322,264,282,202").toString());
    m_imagingReconParams.s1Period       = settings.value("ImagingParams/Recon/S1Period", 2500).toInt();
    m_imagingReconParams.isCutoffLoc    = settings.value("ImagingParams/Recon/IsCutoffLoc", false).toBool();
    m_imagingReconParams.cutoffLoc      = settings.value("ImagingParams/Recon/CutoffLoc", 2501).toInt();
    m_imagingReconParams.isCenterAlign  = settings.value("ImagingParams/Recon/IsCenterAlign", true).toBool();

    // ══ 色条范围持久化加载 ════════════════════════════════════════
    m_freqColorRange.lower = settings.value("DisplayParams/FreqColorRangeLow", 0.0).toDouble();
    m_freqColorRange.upper = settings.value("DisplayParams/FreqColorRangeHigh", 500.0).toDouble();
    m_pixelColorRange.lower = settings.value("DisplayParams/PixelColorRangeLow", 0.0).toDouble();
    m_pixelColorRange.upper = settings.value("DisplayParams/PixelColorRangeHigh", 500.0).toDouble();
    m_freqColorInited  = settings.value("DisplayParams/FreqColorInited", false).toBool();
    m_pixelColorInited = settings.value("DisplayParams/PixelColorInited", false).toBool();

    // 色条范围首次初始化：setupPlots 在 rebuildDynamicUI 中调用，色条已创建
    // 用延迟确保在 setupPlots 之后恢复范围
    QTimer::singleShot(300, this, [this]() {
        if (m_colorMap && m_colorScale) {
            m_settingColorRange = true;
            m_colorMap->setDataRange(m_freqColorRange);
            m_colorScale->axis()->setRange(m_freqColorRange);
            m_settingColorRange = false;
        }
    });

    logMessage(QString("已加载参数配置（卡数=%1，路线A WinSock，%2）").arg(m_nCards).arg(fmtDesc));
}

void MainWindow::saveSettings()
{
    QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
    settings.setValue("AcquisitionParams/NCards",   m_nCards);
    settings.setValue("NetworkParams/LocalBindIP",   m_localBindIP);
    settings.setValue("NetworkParams/ScanBaseIP",    m_scanBaseIP);
    settings.setValue("NetworkParams/ScanIPCount",   m_scanIPCount);
    settings.setValue("AcquisitionParams/DataTime", ui->edtDataTime->text());
    settings.setValue("AcquisitionParams/ADelay",   ui->edtADelay->text());
    settings.setValue("AcquisitionParams/BDelay",   ui->edtBDelay->text());
    settings.setValue("SaveParams/Directory",       ui->edtSaveDir->text());
    settings.setValue("SaveParams/TriggersPerFile", ui->edtTriggersPerFile->text());
    settings.setValue("SaveParams/FileSuffix",      ui->edtFileSuffix->text());
    settings.setValue("DisplayParams/Enabled",      ui->chkEnableDisplay->isChecked());
    settings.setValue("DisplayParams/Type",         ui->cmbDisplayType->currentIndex());
    settings.setValue("DisplayParams/ImagingMode",  ui->cmbImagingMode->currentIndex());
    settings.setValue("DisplayParams/RefreshRate",  ui->spnRefreshRate->value());
    settings.setValue("DisplayParams/PkStart",      ui->edtPkStart->text());
    settings.setValue("DisplayParams/PkEnd",        ui->edtPkEnd->text());
    settings.setValue("DisplayParams/DownsampleRatio", ui->spnDownsampleRatio->value());
    settings.setValue("DisplayParams/EnableDownsampling", m_enableDownsampling);
    settings.setValue("DisplayParams/AutoRescaleAxes",    m_autoRescaleAxes);
    // 数据格式参数
    settings.setValue("AcquisitionParams/BitsPerChannel",   m_bitsPerChannel);
    settings.setValue("AcquisitionParams/LogicalTriggersPerRound",
                      m_logicalTriggersPerRound);
    // 采样间隔固定写回 "4.0"（REG_SZ，与线性实例一致；不删除键，仅统一数值）
    settings.setValue("AcquisitionParams/SampleIntervalNs",
                      QString::number(m_sampleIntervalNs, 'f', 1));

    // ══ 保存成像参数 ════════════════════════════════════════════
    settings.setValue("ImagingParams/General/DaqHz",           m_imagingGeneralParams.daq_hz);
    settings.setValue("ImagingParams/General/Depth",           m_imagingGeneralParams.depth);
    settings.setValue("ImagingParams/General/ChannelNum",      m_imagingGeneralParams.channelNum);
    settings.setValue("ImagingParams/General/CardNum",         m_imagingGeneralParams.cardNum);
    settings.setValue("ImagingParams/General/IsFullScan",      m_imagingGeneralParams.isFullScan);
    settings.setValue("ImagingParams/General/IsFastScan",      m_imagingGeneralParams.isFastScan);
    settings.setValue("ImagingParams/General/IsMultiFiber",    m_imagingGeneralParams.isMultiFiber);
    settings.setValue("ImagingParams/General/PhysicalChannels", m_imagingGeneralParams.physicalChannels);
    settings.setValue("ImagingParams/General/ChannelNum",      m_imagingGeneralParams.channelNum);
    settings.setValue("ImagingParams/General/IsSaveReconData", m_imagingGeneralParams.isSaveReconData);
    settings.setValue("TestMode/Enabled",                      m_useTestImagingData);
    settings.setValue("ImagingParams/Scan/StepsizeUm",         m_imagingScanParams.stepsize_um);
    settings.setValue("ImagingParams/Scan/MoveAline",          m_imagingScanParams.move_aline);
    settings.setValue("ImagingParams/Scan/ChannelAline",       m_imagingScanParams.channel_aline);
    settings.setValue("ImagingParams/Scan/Nx",                 m_imagingScanParams.nx);
    settings.setValue("ImagingParams/Scan/Ny",                 m_imagingScanParams.ny);
    settings.setValue("ImagingParams/Scan/DxUm",               m_imagingScanParams.dx_um);
    settings.setValue("ImagingParams/Scan/DyUm",               m_imagingScanParams.dy_um);
    settings.setValue("ImagingParams/Scan/X0M",                m_imagingScanParams.x0_m);
    settings.setValue("ImagingParams/Scan/Y0M",                m_imagingScanParams.y0_m);
    settings.setValue("ImagingParams/Recon/Delay",             m_imagingReconParams.delay);
    settings.setValue("ImagingParams/Recon/Sos1Mps",           m_imagingReconParams.sos1_mps);
    settings.setValue("ImagingParams/Recon/Sos2Mps",           m_imagingReconParams.sos2_mps);
    settings.setValue("ImagingParams/Recon/IsDualSoS",         m_imagingReconParams.isDualSoS);
    settings.setValue("ImagingParams/Recon/FilterType",        m_imagingReconParams.filterType);
    settings.setValue("ImagingParams/Recon/FilterFreqLowHz",   m_imagingReconParams.filterFreqLow_Hz);
    settings.setValue("ImagingParams/Recon/FilterFreqHighHz",  m_imagingReconParams.filterFreqHigh_Hz);
    settings.setValue("ImagingParams/Recon/Threshold",         m_imagingReconParams.threshold);
    settings.setValue("ImagingParams/Recon/DynRangeDb",        m_imagingReconParams.dynRange_db);
    settings.setValue("ImagingParams/Recon/OutputType",        m_imagingReconParams.outputType);
    // ══ 新参数持久化保存 ════════════════════════════════════════════
    settings.setValue("ImagingParams/Recon/DelayTimePoint",    m_imagingReconParams.delayTimePoint);
    settings.setValue("ImagingParams/Recon/CaliCardDelay",     vecToStr(m_imagingReconParams.caliCardDelay));
    settings.setValue("ImagingParams/Recon/CaliFiberDelay",    vecToStr(m_imagingReconParams.caliFiberDelay));
    settings.setValue("ImagingParams/Recon/S1Period",          m_imagingReconParams.s1Period);
    settings.setValue("ImagingParams/Recon/IsCutoffLoc",       m_imagingReconParams.isCutoffLoc);
    settings.setValue("ImagingParams/Recon/CutoffLoc",         m_imagingReconParams.cutoffLoc);
    settings.setValue("ImagingParams/Recon/IsCenterAlign",     m_imagingReconParams.isCenterAlign);

    // ══ 色条范围持久化保存 ════════════════════════════════════════
    settings.setValue("DisplayParams/FreqColorRangeLow",  m_freqColorRange.lower);
    settings.setValue("DisplayParams/FreqColorRangeHigh", m_freqColorRange.upper);
    settings.setValue("DisplayParams/PixelColorRangeLow",  m_pixelColorRange.lower);
    settings.setValue("DisplayParams/PixelColorRangeHigh", m_pixelColorRange.upper);
    settings.setValue("DisplayParams/FreqColorInited",  m_freqColorInited);
    settings.setValue("DisplayParams/PixelColorInited", m_pixelColorInited);

    // 窗口几何尺寸（使用独立字段）
    {
        QRect g = geometry();
        settings.setValue("Window/X", g.x());
        settings.setValue("Window/Y", g.y());
        settings.setValue("Window/W", g.width());
        settings.setValue("Window/H", g.height());
        logMessage(QString("保存窗口几何: %1,%2 %3x%4").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()));
    }

    settings.sync();
}

// =====================================================================
// 平铺视图切换
// =====================================================================
void MainWindow::onTileViewToggled(bool checked)
{
    m_isTileView = checked;

    // 频域幅值谱仅单通道（Tab）窗口显示：任何布局变动前先同步可见性，
    // 避免平铺切换时“时域+频域双窗先显示、频域再消失”造成的闪烁与重排卡顿
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i)
        for (int j = 0; j < 2; ++j)
            if (m_plotsSpectrum[i][j])
                m_plotsSpectrum[i][j]->setVisible(!checked);

    if (checked) {
        logMessage("切换到平铺视图模式");
        // 批量布局：先禁重绘，完成全部 widget 迁移后统一刷新一次，
        // 避免逐图窗中间重排/重绘造成的“逐个出现”与未响应
        QWidget *displayArea = ui->stackedDisplayMode;
        displayArea->setUpdatesEnabled(false);
        ui->stackedDisplayMode->setCurrentIndex(1);
        // 再将 stacked widget 移入 tile 容器
        for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
            for (int ch = 0; ch < 2; ++ch) {
                if (!m_stackedWidgets[card][ch]) continue;
                QStackedWidget *stack = m_stackedWidgets[card][ch];
                QWidget *tileContainer = m_tileContainers[card][ch];
                if (!tileContainer) continue;
                if (stack->parentWidget()) {
                    QLayout *oldLayout = stack->parentWidget()->layout();
                    if (oldLayout) oldLayout->removeWidget(stack);
                }
                // 同时将 pkpk 标签从标签页迁移到平铺容器
                QLabel *pkpk = m_pkpkLabels[card][ch];
                if (pkpk && pkpk->parentWidget()) {
                    QLayout *pkLayout = pkpk->parentWidget()->layout();
                    if (pkLayout) pkLayout->removeWidget(pkpk);
                }
                QVBoxLayout *tileLayout = qobject_cast<QVBoxLayout*>(tileContainer->layout());
                if (tileLayout) {
                    while (tileLayout->count() > 1) { QLayoutItem *item = tileLayout->takeAt(1); delete item; }
                    if (pkpk) tileLayout->addWidget(pkpk);
                    tileLayout->addWidget(stack, 1);
                }
                stack->show();
            }
        }
        displayArea->setUpdatesEnabled(true);
        displayArea->update();
        syncVisiblePlotsGeometry();
    } else {
        logMessage("切换回标签页视图模式");
        QWidget *displayArea = ui->stackedDisplayMode;
        displayArea->setUpdatesEnabled(false);
        for (int card = 0; card < CARDS_PER_DISPLAY_GROUP; ++card) {
            for (int ch = 0; ch < 2; ++ch) {
                if (!m_stackedWidgets[card][ch]) continue;
                int tabIndex = card * 2 + ch;
                QStackedWidget *stack = m_stackedWidgets[card][ch];
                QWidget *tabPage = ui->tabWidget->widget(tabIndex);
                if (tabPage) {
                    QVBoxLayout *tabLayout = qobject_cast<QVBoxLayout*>(tabPage->layout());
                    // 先将 pkpk 标签从平铺容器移回，防止被平铺清理循环 delete
                    QLabel *pkpk = m_pkpkLabels[card][ch];
                    if (pkpk) {
                        QWidget *pw = pkpk->parentWidget();
                        if (pw && pw != tabPage) {
                            QLayout *pl = pw->layout();
                            if (pl) pl->removeWidget(pkpk);
                        }
                    }
                    if (tabLayout) {
                        tabLayout->addWidget(stack, 1);
                        if (pkpk) tabLayout->insertWidget(0, pkpk);
                    }
                }
                QWidget *tileContainer = m_tileContainers[card][ch];
                if (!tileContainer) continue;
                QVBoxLayout *tileLayout = qobject_cast<QVBoxLayout*>(tileContainer->layout());
                if (tileLayout) {
                    while (tileLayout->count() > 1) { QLayoutItem *item = tileLayout->takeAt(1); delete item; }
                    tileLayout->addStretch();
                }
            }
        }
        ui->stackedDisplayMode->setCurrentIndex(0);
        displayArea->setUpdatesEnabled(true);
        displayArea->update();
    }
}

// =====================================================================
// 分组导航
// =====================================================================
void MainWindow::onGroupTabChanged(int groupIndex)
{
    if (groupIndex < 0 || groupIndex >= m_numGroups) return;
    m_currentGroup = groupIndex;
    updateGroupDisplay(groupIndex);
}

void MainWindow::updateGroupDisplay(int groupIndex)
{
    if (groupIndex < 0 || groupIndex >= m_numGroups) return;
    m_currentGroup = groupIndex;

    int firstGlobal = groupIndex * CARDS_PER_DISPLAY_GROUP;
    int lastGlobal  = std::min(firstGlobal + CARDS_PER_DISPLAY_GROUP - 1, m_nCards - 1);
    ui->lblGroupInfo->setText(
        QString("第%1组 (卡%2-%3)").arg(groupIndex + 1).arg(firstGlobal + 1).arg(lastGlobal + 1));

    const bool ringMode = (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1);
    ui->stackedMainDisplay->setCurrentIndex(0);
    ui->stackedDisplayMode->setCurrentIndex(m_isTileView ? 1 : 0);
    // 环形扫描在主窗口按瞬时频率监视通道信号；线性扫描按显示类型显示时域信号
    int stackIdx = ringMode ? DM_FREQ : ui->cmbDisplayType->currentIndex();
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i)
        for (int j = 0; j < 2; ++j)
            if (m_stackedWidgets[i][j])
                m_stackedWidgets[i][j]->setCurrentIndex(stackIdx);

    // 更新 Tab 标题、平铺标题，并显示/隐藏超出实际卡数的槽位
    for (int slot = 0; slot < CARDS_PER_DISPLAY_GROUP; ++slot) {
        int globalCard = firstGlobal + slot;
        bool cardExists = (globalCard < m_nCards);

        for (int ch = 0; ch < 2; ++ch) {
            int tabIdx = slot * 2 + ch;
            QString chName = cardExists
                ? QString("卡%1-通道%2").arg(globalCard + 1).arg(ch == 0 ? 'A' : 'B')
                : QString("--");

            ui->tabWidget->setTabText(tabIdx, chName);
            ui->tabWidget->setTabVisible(tabIdx, cardExists);

            if (m_pkpkLabels[slot][ch])
                m_pkpkLabels[slot][ch]->setText(QString("峰峰值: -- kHz"));

            QWidget *tileContainer = m_tileContainers[slot][ch];
            if (tileContainer) {
                tileContainer->setVisible(cardExists);
                const auto labels = tileContainer->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
                for (QLabel *lbl : labels) {
                    if (lbl->property("tileTitle").toBool()) { lbl->setText(chName); break; }
                }
            }
        }
        m_firstPlot[slot][0] = true;
        m_firstPlot[slot][1] = true;

        // 清空图表旧数据：切换分组后，新组的图表应显示空白，
        // 而不是残留上一组（或上一次使用该槽位时）的曲线。
        // 无数据的卡在下一次 onDisplayRefresh 的 tryRead 返回 false 时会保持空白。
        for (int ch = 0; ch < 2; ++ch) {
            if (m_plotsPhase[slot][ch] && m_plotsPhase[slot][ch]->graphCount() > 0) {
                m_plotsPhase[slot][ch]->graph(0)->data()->clear();
                m_plotsPhase[slot][ch]->replot(QCustomPlot::rpQueuedReplot);
            }
            if (m_plotsFrequency[slot][ch] && m_plotsFrequency[slot][ch]->graphCount() > 0) {
                m_plotsFrequency[slot][ch]->graph(0)->data()->clear();
                m_plotsFrequency[slot][ch]->replot(QCustomPlot::rpQueuedReplot);
            }
            if (m_pkpkLabels[slot][ch])
                m_pkpkLabels[slot][ch]->setText(QString("峰峰值: -- kHz"));
        }
    }

    // 重置统计栏
    int cardsInGroup = std::min(CARDS_PER_DISPLAY_GROUP, m_nCards - firstGlobal);
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i) {
        int virtualCardNum = firstGlobal + i + 1;
        if (i < cardsInGroup)
            m_lblStats[i]->setText(QString("卡%1: 等待数据...").arg(virtualCardNum));
        else
            m_lblStats[i]->setText(QString("--"));
    }
    QTimer::singleShot(0, this, [this]() {
        syncVisiblePlotsGeometry();
    });
    logMessage(QString("切换到第%1组（卡%2~卡%3）").arg(groupIndex + 1).arg(firstGlobal + 1).arg(lastGlobal + 1));
}

// =====================================================================
// 坐标轴范围设置对话框
// =====================================================================
void MainWindow::setAxisRange(QCustomPlot *plot, bool isXAxis)
{
    if (!plot) return;
    QCPAxis *axis = isXAxis ? plot->xAxis : plot->yAxis;
    QString axisName = isXAxis ? "X轴" : "Y轴";
    double currentMin = axis->range().lower;
    double currentMax = axis->range().upper;
    QDialog dialog(this);
    dialog.setWindowTitle(QString("设置%1范围").arg(axisName));
    dialog.setMinimumWidth(300);
    QFormLayout *layout = new QFormLayout(&dialog);
    QLineEdit *editMin = new QLineEdit(QString::number(currentMin, 'g', 10));
    QLineEdit *editMax = new QLineEdit(QString::number(currentMax, 'g', 10));
    layout->addRow(QString("%1最小值:").arg(axisName), editMin);
    layout->addRow(QString("%1最大值:").arg(axisName), editMax);
    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttonBox);
    if (dialog.exec() == QDialog::Accepted) {
        bool okMin, okMax;
        double newMin = editMin->text().toDouble(&okMin);
        double newMax = editMax->text().toDouble(&okMax);
        if (okMin && okMax && newMin < newMax) {
            if (m_autoRescaleAxes) { m_autoRescaleAxes = false; saveSettings(); logMessage("已自动关闭自适应坐标轴"); }
            axis->setRange(newMin, newMax);
            plot->replot();
            logMessage(QString("%1范围已设置: [%2, %3]").arg(axisName).arg(newMin, 0, 'g', 6).arg(newMax, 0, 'g', 6));
        }
    }
}

// =====================================================================
// attachAxisEdit — 双击坐标轴刻度数字直接编辑该端范围
// 与右键“设置坐标范围”等价：编辑成功即关闭自适应并保存当前范围
// =====================================================================
void MainWindow::attachAxisEdit(QCustomPlot *plot, int card, int ch, bool isSpectrum)
{
    if (!plot) return;
    connect(plot, &QCustomPlot::mouseDoubleClick, this,
            [this, plot, card, ch, isSpectrum](QMouseEvent *e) {
        QCPAxisRect *ar = plot->axisRect();
        if (!ar) return;
        QCPAxis *axis = nullptr;
        bool isMax = false;
        const QPointF pos = e->pos();
        if (pos.x() < ar->left()) {
            // 左侧 y 轴刻度区：上半=max，下半=min（屏幕 y 向下）
            axis = plot->yAxis;
            isMax = (pos.y() < ar->center().y());
        } else if (pos.y() > ar->bottom()) {
            // 底部 x 轴刻度区：右半=max，左半=min
            axis = plot->xAxis;
            isMax = (pos.x() > ar->center().x());
        } else {
            return;
        }
        if (!axis) return;
        // 行内编辑框：位置贴着点击的刻度数字
        QLineEdit *ed = new QLineEdit(plot);
        ed->setGeometry(QRect(QPoint(e->pos().x() - 48, e->pos().y() - 12), QSize(96, 24)));
        ed->setAlignment(Qt::AlignCenter);
        ed->setText(QString::number(isMax ? axis->range().upper : axis->range().lower, 'g', 10));
        ed->show();
        ed->setFocus();
        ed->selectAll();
        QPointer<QLineEdit> edPtr(ed);
        auto applyEdit = [this, plot, axis, isMax, card, ch, isSpectrum, edPtr]() {
            if (!edPtr) return;
            bool ok = false;
            const double v = edPtr->text().toDouble(&ok);
            edPtr->deleteLater();
            if (!ok) return;
            QCPRange r = axis->range();
            if (isMax) r.upper = v; else r.lower = v;
            if (r.upper <= r.lower) return;   // 无效范围：放弃
            axis->setRange(r);
            // 与右键“设置坐标范围”语义一致：关闭自适应、记录当前范围
            bool &autoFlag = isSpectrum ? m_autoRescaleSpectrum[card][ch]
                                        : m_autoRescalePlot[card][ch];
            autoFlag = false;
            QCPRange &savedX = isSpectrum ? m_savedSpectrumXRange[card][ch]
                                          : m_savedXRange[card][ch];
            QCPRange &savedY = isSpectrum ? m_savedSpectrumYRange[card][ch]
                                          : m_savedYRange[card][ch];
            savedX = plot->xAxis->range();
            savedY = plot->yAxis->range();
            plot->replot();
        };
        connect(ed, &QLineEdit::editingFinished, this, applyEdit);
    });
}

// =====================================================================
// calculateFrequency（兼容备用）
// =====================================================================
QVector<double> MainWindow::calculateFrequency(const QVector<double> &phaseData)
{
    if (phaseData.size() < 2) return {};
    const int size = phaseData.size();
    QVector<double> frequency(size - 1);
    const double twoPi = 2.0 * M_PI;
    const double freqScale = DIFF_SAMPLE_RATE_HZ / twoPi;
    double cumulative = 0.0, prevPhaseUnwrapped = phaseData[0];
    for (int i = 1; i < size; ++i) {
        double dp = phaseData[i] - phaseData[i - 1];
        double dpMod = std::fmod(dp + M_PI, twoPi);
        if (dpMod < 0) dpMod += twoPi;
        double dpCorr = dpMod - M_PI;
        if (dpCorr == -M_PI && dp > 0) dpCorr = M_PI;
        cumulative += (dpCorr - dp);
        double curr = phaseData[i] + cumulative;
        frequency[i - 1] = (curr - prevPhaseUnwrapped) * freqScale;
        prevPhaseUnwrapped = curr;
    }
    return frequency;
}

// =====================================================================
// updateNetworkInfoLabels — 根据 m_nCards 更新网络控制面板标签
// =====================================================================
void MainWindow::updateNetworkInfoLabels()
{
    // 目标IP范围：优先使用自动识别结果；否则按扫描起始 IP + 卡数推算
    QString ipText;
    if (!m_targetIPRangeText.isEmpty()) {
        ipText = QString("目标IP: %1").arg(m_targetIPRangeText);
    } else {
        QStringList parts = m_scanBaseIP.split('.');
        int startIpLast = (parts.size() == 4) ? parts[3].toInt() : 2;
        int endIpLast   = startIpLast + m_nCards - 1;
        QString prefix  = (parts.size() == 4)
                          ? QString("%1.%2.%3").arg(parts[0]).arg(parts[1]).arg(parts[2])
                          : QString("192.168.0");
        if (m_nCards == 1)
            ipText = QString("目标IP: %1.%2").arg(prefix).arg(startIpLast);
        else
            ipText = QString("目标IP: %1.%2-%3").arg(prefix).arg(startIpLast).arg(endIpLast);
    }
    ui->lblTargetIPs->setText(ipText);

    // MAC 范围（最后一字节）
    QString macText;
    if (m_nCards == 1)
        macText = QString("MAC: 00.0A.35.01.FE.C0");
    else
        macText = QString("MAC: 00.0A.35.01.FE.C0-%1")
                  .arg(0xC0 + m_nCards - 1, 2, 16, QChar('0')).toUpper();
    ui->lblMACs->setText(macText);

    // 数据端口范围
    int portStart = BASE_PORT;
    int portEnd   = BASE_PORT + m_nCards - 1;
    QString portText;
    if (m_nCards == 1)
        portText = QString("数据端口: %1").arg(portStart);
    else
        portText = QString("数据端口: %1-%2").arg(portStart).arg(portEnd);
    ui->lblDataPorts->setText(portText);

    // tooltip 提示管理员如何修改（注册表）
    QString bindIPInfo = m_localBindIP.isEmpty()
        ? QString("未设置（INADDR_ANY）— 双口网卡请必须设置!")
        : m_localBindIP;
    ui->lblTargetIPs->setToolTip(
        QString("采集卡数量: %1\n"
                "  注册表键: HKCU\\Software\\MC410T\\MC410T_Receiver\\AcquisitionParams\\NCards\n"
                "控制包本地绑定IP: %2\n"
                "  注册表键: ...\\NetworkParams\\LocalBindIP\n"
                "  双口网卡只接一口时必须填已连接口的本地IP（如 192.168.0.100）\n"
                "  否则控制包可能从错误网口发出，采集卡收不到指令\n"
                "接收路线: WinSock")
        .arg(m_nCards)
        .arg(bindIPInfo)
        .arg("路线A WinSock")
    );
    updateNetworkInfoIndicator();
}

// =====================================================================
// updateNetworkInfoIndicator — 将只读网络信息收纳到带外圈感叹号悬停提示
// =====================================================================
void MainWindow::updateNetworkInfoIndicator()
{
    QLabel *ind = ui->lblNetInfoIndicator;
    if (!ind) return;

    QString info;
    info += ui->lblTargetIPs->text() + "\n";
    info += ui->lblMACs->text() + "\n";
    info += ui->lblControlPort->text() + "\n";
    info += ui->lblFeedbackPort->text() + "\n";
    info += ui->lblDataPorts->text();
    info += "\n──────────\n";
    const QString bindIPInfo = m_localBindIP.isEmpty()
        ? QString("未设置（INADDR_ANY）— 双口网卡请必须设置!")
        : m_localBindIP;
    info += QString("采集卡数量: %1\n").arg(m_nCards);
    info += QString("控制包本地绑定IP: %2\n").arg(bindIPInfo);
    info += "接收路线: WinSock\n";
    info += "注册表键: HKCU\\Software\\MC410T\\MC410T_Receiver";
    ind->setToolTip(info);
}

// =====================================================================
// closeEvent
// =====================================================================
void MainWindow::closeEvent(QCloseEvent *event)
{
    // 发现线程在短轮询间隔内检查取消标志并尽快退出（socket 由 RAII 关闭）
    if (m_discoveryCancel) m_discoveryCancel->store(true);
    saveSettings();

    if (!m_netController) {
        event->accept();
        return;
    }

    // 忽略本次关闭事件，等后台停止完成后再真正退出
    event->ignore();
    setEnabled(false);

    // 用 lambda 连接 stopped() 信号，确保只触发一次后就 quit
    // 使用 Qt::SingleShotConnection（Qt6）或手动 disconnect
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_netController, &NetworkController::stopped, this, [this, conn]() {
        disconnect(*conn);   // 单次触发后立即断开
        // 删除 controller（已完全停止，安全删除）
        if (m_netController) {
            m_netController->setParent(nullptr);
            delete m_netController;
            m_netController = nullptr;
        }
        QApplication::quit();
    }, Qt::QueuedConnection);

    // 超时兜底：最多 4s 后强制退出（防止某线程卡死无法正常退出）
    QTimer::singleShot(4000, this, [this]() {
        if (m_netController) {
            m_netController->setParent(nullptr);
            delete m_netController;
            m_netController = nullptr;
        }
        QApplication::quit();
    });

    m_netController->stop();
}

// =====================================================================
// 成像相关槽函数
// =====================================================================

void MainWindow::onImagingConfigClicked()
{
    // 成像方式=环形扫描时，成像参数按钮打开环形扫描参数设定窗口
    if (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 1) {
        if (!m_ringConfigDialog) {
            m_ringConfigDialog = new RingConfigDialog(m_imagingController, this);
        }
        m_ringConfigDialog->setAcquisitionParams(
            m_sampleIntervalNs, ui->edtDataTime->text().toInt());
        m_ringConfigDialog->exec();
        // Keep the production normalizer configured even while ImagingSvc is
        // stopped; configureRingAssembler() repeats this at svcReady.
        if (m_netController && m_imagingController &&
            m_imagingController->isRingMode())
            m_netController->setPhysicalRoundTimeout(
                m_imagingController->ringConfig().timeoutResetSec);
        return;
    }

    if (m_imagingConfigDialog) {
        delete m_imagingConfigDialog;
    }
    m_imagingConfigDialog = new QDialog(this);
    m_imagingConfigDialog->setWindowTitle("成像参数配置");
    m_imagingConfigDialog->setMinimumSize(500, 450);
    // 记忆上次关闭前的大小
    QSettings cfgSize(paimageSettingsPath(), QSettings::IniFormat);
    if (cfgSize.contains("ImagingConfigDialog/Size")) {
        const QSize saved = cfgSize.value("ImagingConfigDialog/Size").toSize();
        if (saved.isValid() && saved.width() >= 500 && saved.height() >= 450)
            m_imagingConfigDialog->resize(saved);
    }

    QTabWidget *tabs = new QTabWidget(m_imagingConfigDialog);
    QVBoxLayout *mainLayout = new QVBoxLayout(m_imagingConfigDialog);
    mainLayout->addWidget(tabs);

    // ---- 通用参数页 ----
    QWidget *generalPage = new QWidget();
    QFormLayout *generalLayout = new QFormLayout(generalPage);

    // 只读参数
    QLabel *lblDaqHz  = new QLabel(QString("%1 MSa/s").arg(m_imagingGeneralParams.daq_hz / 1e6f, 0, 'f', 1));
    lblDaqHz->setStyleSheet("color: #AAAAAA; background: #2A2A2A; padding: 4px 8px; border: 1px solid #3C3C3C; border-radius: 3px;");

    // 采集深度 = 采集时间(ns) / 采样间隔(ns/点)，来自左侧采集控制参数
    int acqTimeNs = ui->edtDataTime->text().toInt();
    int computedDepth = (m_sampleIntervalNs > 0)
        ? static_cast<int>(acqTimeNs / m_sampleIntervalNs)
        : 50000;
    QLabel *lblDepth = new QLabel(QString("%1 pts").arg(computedDepth));
    lblDepth->setStyleSheet("color: #AAAAAA; background: #2A2A2A; padding: 4px 8px; border: 1px solid #3C3C3C; border-radius: 3px;");

    // 可编辑参数
    QSpinBox *spnPhysicalCh = new QSpinBox();
    spnPhysicalCh->setRange(1, 64);
    spnPhysicalCh->setValue(m_imagingGeneralParams.physicalChannels);
    spnPhysicalCh->setToolTip("物理通道数（算法 card_num），4张卡×2通道=8");

    QSpinBox *spnChannelNum = new QSpinBox();
    spnChannelNum->setRange(1, 128);
    spnChannelNum->setValue(m_imagingGeneralParams.channelNum);
    spnChannelNum->setToolTip("光纤通道总数（算法 channel_num），多纤→64，单纤→8");

    // 扫描模式：全扫描 / 快速扫描（互斥单选，底层用 bool 保证互斥）
    QRadioButton *radioFull = new QRadioButton("全扫描");
    QRadioButton *radioFast = new QRadioButton("快速扫描");
    radioFull->setChecked(m_imagingGeneralParams.isFullScan);
    radioFast->setChecked(m_imagingGeneralParams.isFastScan);
    QButtonGroup *scanGroup = new QButtonGroup(generalPage);
    scanGroup->addButton(radioFull, 0);
    scanGroup->addButton(radioFast, 1);
    QObject::connect(radioFull, &QRadioButton::toggled, generalPage, [radioFast](bool checked) {
        if (checked) radioFast->setChecked(false);
    });
    QObject::connect(radioFast, &QRadioButton::toggled, generalPage, [radioFull](bool checked) {
        if (checked) radioFull->setChecked(false);
    });

    // 光纤复用模式：多光纤复用 / 单光纤直连（互斥单选）
    // 多光纤复用：4卡×8通道×8光纤/通道，depth=50000，200μs
    // 单光纤直连：4卡×8通道×1光纤/通道，depth=12500，50μs
    QRadioButton *radioMulti = new QRadioButton("多光纤复用（4卡×8通道×8光纤/通道→64纤）");
    QRadioButton *radioDirect = new QRadioButton("单光纤直连（4卡×8通道×1光纤/通道→8纤）");
    radioMulti->setChecked(m_imagingGeneralParams.isMultiFiber);
    radioDirect->setChecked(!m_imagingGeneralParams.isMultiFiber);
    QButtonGroup *fiberGroup = new QButtonGroup(generalPage);
    fiberGroup->addButton(radioMulti, 0);
    fiberGroup->addButton(radioDirect, 1);

    // 光纤模式切换时联动 channelNum（cardNum 固定为 4）
    QObject::connect(radioMulti, &QRadioButton::toggled, generalPage, [spnChannelNum, radioDirect](bool checked) {
        if (checked) {
            radioDirect->setChecked(false);
            spnChannelNum->setValue(64);
        }
    });
    QObject::connect(radioDirect, &QRadioButton::toggled, generalPage, [spnChannelNum, radioMulti](bool checked) {
        if (checked) {
            radioMulti->setChecked(false);
            spnChannelNum->setValue(8);
        } else {
            spnChannelNum->setValue(64);
        }
    });

    QCheckBox *isSaveRecon = new QCheckBox("存储重建数据");
    isSaveRecon->setChecked(m_imagingGeneralParams.isSaveReconData);

    QHBoxLayout *scanRadioLayout = new QHBoxLayout();
    scanRadioLayout->addWidget(radioFull);
    scanRadioLayout->addWidget(radioFast);
    scanRadioLayout->addStretch();

    QHBoxLayout *fiberRadioLayout = new QHBoxLayout();
    fiberRadioLayout->addWidget(radioMulti);
    fiberRadioLayout->addWidget(radioDirect);
    fiberRadioLayout->addStretch();

    // 配准文件加载（放在通用参数页，属于系统校准类参数）
    QHBoxLayout *peizhunLayout = new QHBoxLayout();
    QPushButton *btnLoadPeizhun = new QPushButton("加载配准文件...");
    btnLoadPeizhun->setProperty("isLoadPeizhun", true);
    btnLoadPeizhun->style()->unpolish(btnLoadPeizhun);
    btnLoadPeizhun->style()->polish(btnLoadPeizhun);
    QLabel *lblPeizhunStatus = new QLabel(
        m_peizhunLoaded ?
        QString("已加载: %1").arg(QFileInfo(m_peizhunFilePath).fileName()) :
        "未加载");
    lblPeizhunStatus->setObjectName("lblPeizhunStatus");
    peizhunLayout->addWidget(btnLoadPeizhun);
    peizhunLayout->addWidget(lblPeizhunStatus);
    peizhunLayout->addStretch();

    generalLayout->addRow("采样率:", lblDaqHz);
    generalLayout->addRow("采集长度:", lblDepth);
    generalLayout->addRow("物理通道数:", spnPhysicalCh);
    generalLayout->addRow("光纤通道数:", spnChannelNum);
    generalLayout->addRow("扫描模式:", scanRadioLayout);
    generalLayout->addRow("光纤复用:", fiberRadioLayout);
    generalLayout->addRow("配准校准:", peizhunLayout);
    generalLayout->addRow(isSaveRecon);
    // 通用参数页放入滚动区：窗口较小时控件不重叠，出现滚动条
    auto wrapScroll = [](QWidget *page) {
        QScrollArea *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(page);
        return sa;
    };
    tabs->addTab(wrapScroll(generalPage), "通用参数");

    // ---- 扫描参数页 ----
    QWidget *scanPage = new QWidget();
    QFormLayout *scanLayout = new QFormLayout(scanPage);

    QDoubleSpinBox *stepsize = new QDoubleSpinBox();
    stepsize->setRange(0.1, 1000); stepsize->setSuffix(" um");
    stepsize->setValue(m_imagingScanParams.stepsize_um);
    QSpinBox *moveAline = new QSpinBox();
    moveAline->setRange(1, 10000); moveAline->setValue(m_imagingScanParams.move_aline);
    QSpinBox *channelAline = new QSpinBox();
    channelAline->setRange(1, 1000); channelAline->setValue(m_imagingScanParams.channel_aline);
    QSpinBox *nx = new QSpinBox();
    nx->setRange(100, 4000); nx->setValue(m_imagingScanParams.nx);
    QSpinBox *ny = new QSpinBox();
    ny->setRange(100, 4000); ny->setValue(m_imagingScanParams.ny);
    QDoubleSpinBox *dx = new QDoubleSpinBox();
    dx->setRange(0.1, 1000); dx->setSuffix(" um");
    dx->setValue(m_imagingScanParams.dx_um);
    QDoubleSpinBox *dy = new QDoubleSpinBox();
    dy->setRange(0.1, 1000); dy->setSuffix(" um");
    dy->setValue(m_imagingScanParams.dy_um);
    QDoubleSpinBox *x0 = new QDoubleSpinBox();
    x0->setRange(-100, 100); x0->setSuffix(" mm"); x0->setDecimals(3);
    x0->setValue(m_imagingScanParams.x0_m * 1e3f);
    QDoubleSpinBox *y0 = new QDoubleSpinBox();
    y0->setRange(-100, 100); y0->setSuffix(" mm"); y0->setDecimals(3);
    y0->setValue(m_imagingScanParams.y0_m * 1e3f);

    scanLayout->addRow("电机步长:", stepsize);
    scanLayout->addRow("单帧脉冲数:", moveAline);
    scanLayout->addRow("通道覆盖步数:", channelAline);
    scanLayout->addRow("图像宽度(nx):", nx);
    scanLayout->addRow("图像高度(ny):", ny);
    scanLayout->addRow("X像素间距:", dx);
    scanLayout->addRow("Y像素间距:", dy);
    scanLayout->addRow("原点X:", x0);
    scanLayout->addRow("原点Y:", y0);
    tabs->addTab(wrapScroll(scanPage), "扫描参数");

    // ---- 重建参数页 ----
    QWidget *reconPage = new QWidget();
    QFormLayout *reconLayout = new QFormLayout(reconPage);

    QSpinBox *delay = new QSpinBox();
    delay->setRange(0, 10000); delay->setValue(m_imagingReconParams.delay);
    QDoubleSpinBox *sos1 = new QDoubleSpinBox();
    sos1->setRange(100, 10000); sos1->setSuffix(" m/s");
    sos1->setValue(m_imagingReconParams.sos1_mps);
    QDoubleSpinBox *sos2 = new QDoubleSpinBox();
    sos2->setRange(100, 10000); sos2->setSuffix(" m/s");
    sos2->setValue(m_imagingReconParams.sos2_mps);
    QCheckBox *isDualSoS = new QCheckBox("双声速模型");
    isDualSoS->setChecked(m_imagingReconParams.isDualSoS);
    QComboBox *filterType = new QComboBox();
    filterType->addItems({"Bandpass", "Highpass", "Lowpass", "None"});
    filterType->setCurrentText(m_imagingReconParams.filterType);
    QDoubleSpinBox *freqLow = new QDoubleSpinBox();
    freqLow->setRange(0, 1000); freqLow->setSuffix(" MHz");
    freqLow->setValue(m_imagingReconParams.filterFreqLow_Hz / 1e6f);
    QDoubleSpinBox *freqHigh = new QDoubleSpinBox();
    freqHigh->setRange(0, 1000); freqHigh->setSuffix(" MHz");
    freqHigh->setValue(m_imagingReconParams.filterFreqHigh_Hz / 1e6f);
    QDoubleSpinBox *threshold = new QDoubleSpinBox();
    threshold->setRange(0, 100000); threshold->setValue(m_imagingReconParams.threshold);
    QDoubleSpinBox *dynRange = new QDoubleSpinBox();
    dynRange->setRange(1, 120); dynRange->setSuffix(" dB");
    dynRange->setValue(m_imagingReconParams.dynRange_db);
    QComboBox *outputType = new QComboBox();
    outputType->addItems({"RF", "ENV", "DB"});
    outputType->setCurrentText(m_imagingReconParams.outputType);

    // ══ 新参数：时延/周期/衰减 ════════════════════════════════════
    QSpinBox *spnDelayTimePoint = new QSpinBox();
    spnDelayTimePoint->setRange(0, 50000);
    spnDelayTimePoint->setValue(m_imagingReconParams.delayTimePoint);
    spnDelayTimePoint->setToolTip("时延补偿点，单位采样点（默认 1601）");

    QSpinBox *spnS1Period = new QSpinBox();
    spnS1Period->setRange(0, 50000);
    spnS1Period->setValue(m_imagingReconParams.s1Period);
    spnS1Period->setToolTip("S1 周期，单位采样点（默认 2500）");

    QCheckBox *chkCutoff = new QCheckBox("启用末尾衰减");
    chkCutoff->setChecked(m_imagingReconParams.isCutoffLoc);
    chkCutoff->setToolTip("对末尾干扰段进行衰减处理（对应 C 接口 is_cutoff_loc）");

    QSpinBox *spnCutoffPoint = new QSpinBox();
    spnCutoffPoint->setRange(0, 50000);
    spnCutoffPoint->setValue(m_imagingReconParams.cutoffLoc);
    spnCutoffPoint->setToolTip("衰减起始采样点（默认 2501）");

    QCheckBox *chkCenterAlign = new QCheckBox("中心对齐");
    chkCenterAlign->setChecked(m_imagingReconParams.isCenterAlign);
    chkCenterAlign->setToolTip("中心对齐后边界清零");

    // ══ 采集卡/光纤标定时延（逗号分隔的整数数组）══════════════════
    QLineEdit *editCaliCardDelay = new QLineEdit(vecToStr(m_imagingReconParams.caliCardDelay));
    editCaliCardDelay->setToolTip("采集卡标定时延，逗号分隔（如 0,2,2,2,4,6,6,8）");
    QLineEdit *editCaliFiberDelay = new QLineEdit(vecToStr(m_imagingReconParams.caliFiberDelay));
    editCaliFiberDelay->setToolTip("光纤标定时延，逗号分隔（如 534,484,428,372,322,264,282,202）");

    reconLayout->addRow("自激发起始(delay):", delay);
    reconLayout->addRow("一级声速:", sos1);
    reconLayout->addRow("二级声速:", sos2);
    reconLayout->addRow(isDualSoS);
    reconLayout->addRow("滤波器类型:", filterType);
    reconLayout->addRow("低频截止:", freqLow);
    reconLayout->addRow("高频截止:", freqHigh);
    reconLayout->addRow("数据阈值:", threshold);
    reconLayout->addRow("动态范围:", dynRange);
    reconLayout->addRow("输出类型:", outputType);
    reconLayout->addRow("时延补偿点:", spnDelayTimePoint);
    reconLayout->addRow("S1 周期:", spnS1Period);
    reconLayout->addRow(chkCutoff);
    reconLayout->addRow("衰减起始采样点:", spnCutoffPoint);
    reconLayout->addRow(chkCenterAlign);
    reconLayout->addRow("采集卡标定时延:", editCaliCardDelay);
    reconLayout->addRow("光纤标定时延:", editCaliFiberDelay);
    tabs->addTab(wrapScroll(reconPage), "重建参数");

    // 按钮 —— 使用自定义布局确保顺序：设为默认 | 恢复默认 | 应用 | 取消 | 确定
    QWidget *btnWidget = new QWidget();
    QHBoxLayout *btnLayout = new QHBoxLayout(btnWidget);
    btnLayout->setContentsMargins(0, 6, 0, 0);
    btnLayout->setSpacing(8);

    QPushButton *btnSaveDefault = new QPushButton("设为默认");
    QPushButton *btnRestore  = new QPushButton("恢复默认");
    QPushButton *btnApply    = new QPushButton("应用");
    QPushButton *btnCancel   = new QPushButton("取消");
    QPushButton *btnOk       = new QPushButton("确定");

    btnRestore->setProperty("isDefaultRestore", true);
    btnRestore->style()->unpolish(btnRestore);
    btnRestore->style()->polish(btnRestore);

    btnLayout->addWidget(btnSaveDefault);
    btnLayout->addWidget(btnRestore);
    btnLayout->addStretch();
    btnLayout->addWidget(btnApply);
    btnLayout->addWidget(btnCancel);
    btnLayout->addWidget(btnOk);

    mainLayout->addWidget(btnWidget);

    // 收集参数的 lambda，供 确定/应用 复用
    auto collectParams = [this, radioFull, radioFast, radioMulti, radioDirect, isSaveRecon,
                          spnPhysicalCh, spnChannelNum,
                          stepsize, moveAline, channelAline, nx, ny, dx, dy, x0, y0,
                          delay, sos1, sos2, isDualSoS, filterType,
                          freqLow, freqHigh, threshold, dynRange, outputType,
                          spnDelayTimePoint, spnS1Period, chkCutoff, spnCutoffPoint, chkCenterAlign,
                          editCaliCardDelay, editCaliFiberDelay]()
    {
        GeneralParams general;
        general.daq_hz = m_imagingGeneralParams.daq_hz;
        int acqTimeNs = ui->edtDataTime->text().toInt();
        general.depth  = (m_sampleIntervalNs > 0)
            ? static_cast<int>(acqTimeNs / m_sampleIntervalNs)
            : 50000;
        general.isFullScan = radioFull->isChecked();
        general.isFastScan = radioFast->isChecked();
        general.isMultiFiber = radioMulti->isChecked();
        general.cardNum           = m_imagingGeneralParams.cardNum;  // 内部固定
        general.physicalChannels  = spnPhysicalCh->value();
        general.channelNum        = spnChannelNum->value();
        general.isSaveReconData = isSaveRecon->isChecked();

        ScanParams scan;
        scan.stepsize_um = static_cast<float>(stepsize->value());
        scan.move_aline = moveAline->value();
        scan.channel_aline = channelAline->value();
        scan.nx = nx->value();
        scan.ny = ny->value();
        scan.dx_um = static_cast<float>(dx->value());
        scan.dy_um = static_cast<float>(dy->value());
        scan.x0_m = static_cast<float>(x0->value() * 1e-3);
        scan.y0_m = static_cast<float>(y0->value() * 1e-3);

        ReconParams recon;
        recon.delay = delay->value();
        recon.sos1_mps = static_cast<float>(sos1->value());
        recon.sos2_mps = static_cast<float>(sos2->value());
        recon.isDualSoS = isDualSoS->isChecked();
        recon.filterType = filterType->currentText();
        recon.filterFreqLow_Hz = freqLow->value() * 1e6f;
        recon.filterFreqHigh_Hz = freqHigh->value() * 1e6f;
        recon.threshold = static_cast<float>(threshold->value());
        recon.dynRange_db = static_cast<float>(dynRange->value());
        recon.outputType = outputType->currentText();

        // ══ 新字段映射 ═══════════════════════════════════════════
        recon.delayTimePoint = spnDelayTimePoint->value();
        recon.s1Period       = spnS1Period->value();
        recon.isCutoffLoc    = chkCutoff->isChecked();
        recon.cutoffLoc      = spnCutoffPoint->value();
        recon.isCenterAlign  = chkCenterAlign->isChecked();

        // 采集卡/光纤标定时延（从逗号分隔文本读取）
        recon.caliCardDelay  = strToVec(editCaliCardDelay->text());
        recon.caliFiberDelay = strToVec(editCaliFiberDelay->text());

        // ══ 声速参数保护（C 库无条件要求 lower_sos > 0）══════════
        if (recon.sos1_mps <= 0) { recon.sos1_mps = 1500.0f; }
        if (recon.isDualSoS && recon.sos2_mps <= 0) {
            recon.sos2_mps = recon.sos1_mps;
        }

        // 配准数据（由加载的 peizhun 文件填充）
        recon.xPeizhun     = m_imagingReconParams.xPeizhun;
        recon.yPeizhun     = m_imagingReconParams.yPeizhun;

        // 存储重建数据提示目录位置
        if (general.isSaveReconData) {
            logMessage(QString("[存储] 重建数据将保存至: %1/frame_data/")
                       .arg(QCoreApplication::applicationDirPath()));
        }

        // 测试数据模式：自动覆盖参数为 F8 示例值，无需手动修改 UI
        if (m_useTestImagingData) {
            general.cardNum = kTestCardNum;           // 8
            general.depth = kTestDepth;               // 12500
            general.channelNum = 8;
            general.physicalChannels = 8;
            general.isMultiFiber = false;
            scan.move_aline = 200;
            scan.channel_aline = 175;
            scan.nx = 800;
            scan.ny = 500;
            scan.stepsize_um = 20.0f;
            scan.dx_um = 20.0f;
            scan.dy_um = 20.0f;
            scan.x0_m = -8e-3f;
            scan.y0_m = 3e-3f;
            recon.delay = 536;
            recon.sos1_mps = 1500.0f;
            recon.sos2_mps = 1500.0f;
            recon.isDualSoS = false;
            recon.filterType = "None";
            recon.threshold = 2000.0f;
            recon.dynRange_db = 50.0f;
            recon.outputType = "RF";
            logMessage(QString("[成像测试] 使用 F8 测试参数: depth=%1 card=%2 nx=%3 ny=%4 moveAline=%5")
                       .arg(general.depth).arg(general.cardNum)
                       .arg(scan.nx).arg(scan.ny).arg(scan.move_aline));
        }

        m_imagingController->configure(general, scan, recon);

        // 持久化保存
        m_imagingGeneralParams = general;
        m_imagingScanParams = scan;
        m_imagingReconParams = recon;
        saveSettings();

        logMessage("成像参数已配置");
    };

    // 按钮信号连接
    connect(btnOk, &QPushButton::clicked, this, [collectParams, this]() {
        collectParams();
        m_imagingConfigDialog->accept();
    });
    // 配准文件加载（按钮在通用参数页 -> "配准校准"行）
    QString *lastPeizhunPath = new QString;  // 堆分配，跨 lambda 持久
    connect(btnLoadPeizhun, &QPushButton::clicked, this, [this, lblPeizhunStatus, lastPeizhunPath,
                                                          editCaliCardDelay, editCaliFiberDelay]() {
        QString path = QFileDialog::getOpenFileName(m_imagingConfigDialog,
            "选择配准文件", *lastPeizhunPath, "配准文件 (*.bin);;所有文件 (*)");
        if (path.isEmpty()) return;

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            logMessage(QString("无法打开配准文件: %1").arg(path));
            return;
        }
        QDataStream ds(&f);
        ds.setByteOrder(QDataStream::LittleEndian);

        int32_t count = 0;
        ds >> count;
        if (count <= 0 || count > 1024) {
            logMessage(QString("配准文件格式错误: count=%1").arg(count));
            return;
        }

        QVector<int> xPeizhun(count), yPeizhun(count);
        for (int i = 0; i < count; ++i) ds >> xPeizhun[i];
        for (int i = 0; i < count; ++i) ds >> yPeizhun[i];

        if (ds.status() != QDataStream::Ok) {
            logMessage("配准文件读取错误");
            return;
        }

        m_imagingReconParams.xPeizhun = xPeizhun;
        m_imagingReconParams.yPeizhun = yPeizhun;
        // 注意：配准文件只包含 x/y_peizhun，不包含标定时延
        // 标定时延由 UI 中的 editCaliCardDelay/editCaliFiberDelay 提供
        m_peizhunLoaded = true;
        m_peizhunFilePath = path;
        *lastPeizhunPath = path;
        lblPeizhunStatus->setText(QString("已加载: %1").arg(QFileInfo(path).fileName()));
        logMessage(QString("配准文件已加载: %1 (%2 通道)").arg(path).arg(count));
    });

    connect(btnCancel, &QPushButton::clicked, m_imagingConfigDialog, &QDialog::reject);
    connect(btnApply, &QPushButton::clicked, this, [collectParams]() {
        collectParams();
    });

    // “设为默认”：保存当前弹窗控件值为默认参数；“恢复默认”：读取已存默认（未保存过则回退出厂值）
    auto applyDefaults = [=](const QVariantMap &d) {
        auto v = [&d](const QString &k, const QVariant &f) {
            return d.contains(k) ? d.value(k) : f;
        };
        radioFull->setChecked(v("radioFull", false).toBool());
        radioFast->setChecked(v("radioFast", true).toBool());
        radioMulti->setChecked(v("radioMulti", false).toBool());
        radioDirect->setChecked(v("radioDirect", true).toBool());
        isSaveRecon->setChecked(v("isSaveRecon", false).toBool());
        spnPhysicalCh->setValue(v("physicalCh", 8).toInt());
        spnChannelNum->setValue(v("channelNum", 64).toInt());
        stepsize->setValue(v("stepsize", 10).toDouble());
        moveAline->setValue(v("moveAline", 100).toInt());
        channelAline->setValue(v("channelAline", 50).toInt());
        nx->setValue(v("nx", 1200).toInt());
        ny->setValue(v("ny", 800).toInt());
        dx->setValue(v("dx", 10).toDouble());
        dy->setValue(v("dy", 10).toDouble());
        x0->setValue(v("x0", -6).toDouble());
        y0->setValue(v("y0", 4).toDouble());
        delay->setValue(v("delay", 141).toInt());
        sos1->setValue(v("sos1", 1500).toDouble());
        sos2->setValue(v("sos2", 1560).toDouble());
        isDualSoS->setChecked(v("isDualSoS", false).toBool());
        filterType->setCurrentText(v("filterType", "Bandpass").toString());
        freqLow->setValue(v("freqLow", 1).toDouble());
        freqHigh->setValue(v("freqHigh", 40).toDouble());
        threshold->setValue(v("threshold", 1000).toDouble());
        dynRange->setValue(v("dynRange", 50).toDouble());
        outputType->setCurrentText(v("outputType", "RF").toString());
        spnDelayTimePoint->setValue(v("delayTimePoint", 1601).toInt());
        spnS1Period->setValue(v("s1Period", 2500).toInt());
        chkCutoff->setChecked(v("cutoff", false).toBool());
        spnCutoffPoint->setValue(v("cutoffPoint", 2501).toInt());
        chkCenterAlign->setChecked(v("centerAlign", true).toBool());
        editCaliCardDelay->setText(v("caliCardDelay", "0,2,2,2,4,6,6,8").toString());
        editCaliFiberDelay->setText(v("caliFiberDelay", "534,484,428,372,322,264,282,202").toString());
    };
    auto collectDefaults = [=]() {
        QVariantMap d;
        d["radioFull"] = radioFull->isChecked();
        d["radioFast"] = radioFast->isChecked();
        d["radioMulti"] = radioMulti->isChecked();
        d["radioDirect"] = radioDirect->isChecked();
        d["isSaveRecon"] = isSaveRecon->isChecked();
        d["physicalCh"] = spnPhysicalCh->value();
        d["channelNum"] = spnChannelNum->value();
        d["stepsize"] = stepsize->value();
        d["moveAline"] = moveAline->value();
        d["channelAline"] = channelAline->value();
        d["nx"] = nx->value();
        d["ny"] = ny->value();
        d["dx"] = dx->value();
        d["dy"] = dy->value();
        d["x0"] = x0->value();
        d["y0"] = y0->value();
        d["delay"] = delay->value();
        d["sos1"] = sos1->value();
        d["sos2"] = sos2->value();
        d["isDualSoS"] = isDualSoS->isChecked();
        d["filterType"] = filterType->currentText();
        d["freqLow"] = freqLow->value();
        d["freqHigh"] = freqHigh->value();
        d["threshold"] = threshold->value();
        d["dynRange"] = dynRange->value();
        d["outputType"] = outputType->currentText();
        d["delayTimePoint"] = spnDelayTimePoint->value();
        d["s1Period"] = spnS1Period->value();
        d["cutoff"] = chkCutoff->isChecked();
        d["cutoffPoint"] = spnCutoffPoint->value();
        d["centerAlign"] = chkCenterAlign->isChecked();
        d["caliCardDelay"] = editCaliCardDelay->text();
        d["caliFiberDelay"] = editCaliFiberDelay->text();
        return d;
    };
    connect(btnSaveDefault, &QPushButton::clicked, this, [collectDefaults]() {
        QSettings s(paimageSettingsPath(), QSettings::IniFormat);
        s.setValue("ImagingConfigDialog/Defaults", QVariant(collectDefaults()));
        s.sync();
    });
    connect(btnRestore, &QPushButton::clicked, this, [=]() {
        QSettings s(paimageSettingsPath(), QSettings::IniFormat);
        applyDefaults(s.value("ImagingConfigDialog/Defaults").toMap());
    });

    // 关闭时保存窗口大小
    connect(m_imagingConfigDialog, &QDialog::finished, this, [this](int) {
        if (!m_imagingConfigDialog) return;
        QSettings s(paimageSettingsPath(), QSettings::IniFormat);
        s.setValue("ImagingConfigDialog/Size", m_imagingConfigDialog->size());
        s.sync();
    });

    if (m_imagingConfigDialog->exec() == QDialog::Accepted) {
        // 参数已通过 accepted/apply 信号收集，只需关闭
        logMessage("成像参数配置完成");
    }
}

void MainWindow::onImagingStarted()
{
    m_imagingPulseCount = 0;
    m_roundUi.reset();   // 新成像会话：每轮帧/块计数与快照准入空间清零
    // 保存当前频率颜色图色条范围（切到像素图前）并持久化
    if (m_colorMap) {
        m_freqColorRange = m_colorMap->dataRange();
        saveSettings();
    }
    m_lblImagingStatus->setText("正在启动…");
    m_lblImagingStatus->setStyleSheet(
        "color: #FFAA00; background: transparent; padding: 2px 8px;"
        "font-size: 12px;");
    logMessage("成像重建已启动");
}

void MainWindow::onImagingStopped()
{
    // 保存当前像素图色条范围（切回频率颜色图前）并持久化
    if (m_colorMap) {
        m_pixelColorRange = m_colorMap->dataRange();
        saveSettings();
    }

    m_lblImagingStatus->setText("成像已停止");
    m_lblImagingStatus->setStyleSheet(
        "color: #888888; background: transparent; padding: 2px 8px;"
        "font-size: 12px;");
    logMessage("成像重建已停止");

    // 恢复各通道瞬时频率热图（横轴=采样点，纵轴=通道编号）
    if (m_colorMap) {
        int numChannels = m_nCards * 2;
        int numSamples  = 1024;
        m_colorMap->data()->setSize(numSamples, numChannels);
        m_colorMap->data()->setRange(QCPRange(0, numSamples-1), QCPRange(0, numChannels-1));
        m_colorMapPlot->xAxis->setLabel("采样点");
        m_colorMapPlot->yAxis->setLabel("通道编号");
        m_colorMapPlot->xAxis->setRange(0, numSamples-1);
        m_colorMapPlot->yAxis->setRange(-0.5, numChannels-0.5);
        QVector<double> yTicks;
        QVector<QString> yLabels;
        for (int i = 0; i < numChannels; ++i) {
            yTicks << i;
            yLabels << QString("卡%1-%2").arg(i/2+1).arg(i%2==0?'A':'B');
        }
        QSharedPointer<QCPAxisTickerText> textTicker(new QCPAxisTickerText);
        textTicker->addTicks(yTicks, yLabels);
        m_colorMapPlot->yAxis->setTicker(textTicker);
        m_colorScale->setLabel("频率 (kHz)");
        m_colorScale->axis()->setLabelColor(QColor(180, 180, 180));
        m_colorMapPlot->replot(QCustomPlot::rpQueuedReplot);
    }

    // 恢复保存的频率颜色图色条范围
    if (m_colorMap) {
        m_settingColorRange = true;
        m_colorMap->setDataRange(m_freqColorRange);
        m_colorScale->axis()->setRange(m_freqColorRange);
        m_colorMapPlot->replot(QCustomPlot::rpQueuedReplot);
        m_settingColorRange = false;
    }

    // ══ 确保底部信息栏始终可见（线性扫描模式下 statsLayout 可能被挤压）══
    for (int i = 0; i < CARDS_PER_DISPLAY_GROUP; ++i) {
        if (m_lblStats[i]) {
            m_lblStats[i]->setVisible(true);
            m_lblStats[i]->setMinimumHeight(20);
        }
    }
    if (ui->lblGroupInfo) {
        ui->lblGroupInfo->setVisible(true);
        ui->lblGroupInfo->setMinimumHeight(20);
    }
}

void MainWindow::onImagingImageReady(const QImage &image, int seq)
{
    if (image.isNull()) return;
    m_roundUi.onFrame();   // 线性模式输出帧沿用同一 per-round 计数
    saveReconImage(image, "linear");

    // 从 ImagingController 获取原始 float 帧数据
    QVector<float> frameData = m_imagingController->getLatestFrameData();
    int w = m_imagingController->frameWidth();
    int h = m_imagingController->frameHeight();

    if (frameData.isEmpty() || w <= 0 || h <= 0) return;

    // 线性扫描：重建图像显示在独立弹窗（首次重建帧到达时创建并弹出）
    if (m_cmbImagingMode && m_cmbImagingMode->currentIndex() == 0) {
        if (!m_imagingDisplayWindow) {
            m_imagingDisplayWindow = new ImagingDisplayWindow();
        }
        if (!m_imagingDisplayWindow->isVisible()) {
            m_imagingDisplayWindow->show();
            m_imagingDisplayWindow->raise();
            m_imagingDisplayWindow->activateWindow();
        }
        m_imagingDisplayWindow->showImage(frameData, w, h, seq);
    }

    // ══ 帧管线耗时统计 ════════════════════════════════════════
    static uint64_t lastFrameUs = 0;
    uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (lastFrameUs != 0) {
        uint64_t totalUs = nowUs - lastFrameUs;
        // 每 30 帧输出一次平均耗时，避免刷屏
        static int frameCount = 0;
        static uint64_t accumUs = 0;
        ++frameCount;
        accumUs += totalUs;
        if (frameCount >= 30) {
            uint64_t avgUs = accumUs / frameCount;
            logMessage(QString("[成像帧耗时] %1 帧平均 %2 ms（约 %3 FPS）")
                       .arg(frameCount)
                       .arg(avgUs / 1000.0, 0, 'f', 1)
                       .arg(1000000.0 / avgUs, 0, 'f', 1));
            frameCount = 0;
            accumUs = 0;
        }
    }
    lastFrameUs = nowUs;
}

void MainWindow::onImagingError(const QString &error)
{
    m_imagingServiceReady.store(false, std::memory_order_release);
    if (m_imagingBypass) m_imagingBypass->setServiceReady(false);
    logMessage(QString("成像错误: %1").arg(error));
    m_imagingTimer->stop();
    m_imagingEnabled = false;
    setImagingParamControlsEnabled(true);   // 启动失败等场景不会经过 svcStopped，直接恢复控件
    if (m_chkRealtimeImaging) {
        m_chkRealtimeImaging->setEnabled(true);
        QSignalBlocker blocker(m_chkRealtimeImaging);
        m_chkRealtimeImaging->setChecked(false);
    }
    if (m_lblImagingStatus) {
        m_lblImagingStatus->setText("成像错误");
        m_lblImagingStatus->setStyleSheet(
            "color: #FF4444; background: transparent; padding: 2px 8px;"
            "font-size: 12px;");
    }
}

// =====================================================================
// saveReconImage — 实时重建图像 PNG 保存（随数据保存开关联动）
// 文件名：数据保存框文件后缀 + 时间戳（+ 波长/模式标签）
// =====================================================================
void MainWindow::saveReconImage(const QImage &image, const QString &tag)
{
    if (!m_reconSaveEnabled || image.isNull() || m_reconSaveDir.isEmpty()) return;

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    const QString stem = m_reconSaveSuffix.trimmed();
    const QString name = stem.isEmpty()
        ? QString("%1_%2.png").arg(ts).arg(tag)
        : QString("%1_%2_%3.png").arg(stem).arg(ts).arg(tag);
    const QString path = QDir(m_reconSaveDir).filePath(name);
    if (!image.save(path, "PNG")) {
        logMessage(QString("[图像保存] PNG 写入失败: %1").arg(path));
    }
}

// =====================================================================
// 加载测试数据 bin 文件
// =====================================================================
bool MainWindow::loadTestImagingData()
{
    QString exeDir = QCoreApplication::applicationDirPath();
    QString dataPath = exeDir + "/all_frames_f8_cpp_float_v2.bin";

    if (!readBinaryFloats(dataPath.toUtf8().constData(), m_testImagingData)) {
        logMessage(QString("[成像测试] 无法打开数据文件: %1").arg(dataPath));
        return false;
    }
    m_testImagingPulseIndex = 0;

    // 测试数据使用固定已知参数，不依赖 UI 持久化值
    int onePulse  = kTestCardNum * kTestDepth; // 8 * 12500 = 100000
    int totalPulses = static_cast<int>(m_testImagingData.size()) / onePulse;

    if (totalPulses == 0) {
        logMessage(QString("[成像测试] 数据文件过小: %1 floats, 每脉冲需 %2")
                   .arg(m_testImagingData.size()).arg(onePulse));
        return false;
    }

    logMessage(QString("[成像测试] 加载 %1: %2 floats, %3 脉冲")
               .arg(dataPath)
               .arg(m_testImagingData.size())
               .arg(totalPulses));
    return true;
}

// =====================================================================
// 成像脉冲馈送（独立 5ms 定时器，独立于 33ms 显示刷新）
// 每 tick 馈送一个脉冲给 ImagingSvc
// kUseTestImagingData=true 时从 bin 文件读取，否则从 DisplayBuffer 实时读取
// =====================================================================
void MainWindow::feedImagingPulse()
{
    if (!m_imagingController || !m_imagingController->isRunning())
        return;
    if (m_imagingController->isRingMode())
        return;   // 环形模式由独立工作线程馈送，主线程定时器不参与
    if (!m_imagingEnabled) return;

    // ══ 馈送阶段计时 ════════════════════════════════════════════
    uint64_t tFeedStart = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (m_useTestImagingData) {
        // ── 测试数据模式：从预加载的 bin 文件中逐脉冲馈送 ──
        if (m_testImagingData.empty() && !loadTestImagingData())
            return;

        // 使用测试数据固定深度，不依赖 UI 持久化值
        int depth     = kTestDepth;     // 12500
        int onePulse  = kTestCardNum * depth; // 8 * 12500 = 100000

        // 检查是否所有脉冲馈送完毕，循环从头开始
        if (static_cast<size_t>(m_testImagingPulseIndex * onePulse) >= m_testImagingData.size()) {
            m_testImagingPulseIndex = 0;
            m_imagingPulseCount = 0;
            logMessage("[成像测试] 数据循环，重新开始馈送");
        }

        const float *pulseBase = m_testImagingData.data() +
                                 m_testImagingPulseIndex * onePulse;
        static uint16_t testSeq = 0;
        ++testSeq;

        // 映射：bin卡 [0,1]→our卡0, [2,3]→卡1, [4,5]→卡2, [6,7]→卡3
        for (int card = 0; card < 4; ++card) {
            int testA = card * 2;
            int testB = card * 2 + 1;
            if (testB >= kTestCardNum) break;

            const float *srcA = pulseBase + testA * depth;
            const float *srcB = pulseBase + testB * depth;

            QVector<float> freqA(depth);
            QVector<float> freqB(depth);
            memcpy(freqA.data(), srcA, depth * sizeof(float));
            memcpy(freqB.data(), srcB, depth * sizeof(float));

            m_imagingController->feedPulseData(card, testSeq, freqA, freqB);
        }

        m_imagingController->flushPulseData();

        // ══ 馈送阶段计时 ════════════════════════════════════
        uint64_t tFeedEnd = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        static uint64_t accumFeed = 0;
        static int feedCount = 0;
        accumFeed += (tFeedEnd - tFeedStart);
        if (++feedCount >= 200) {
            logMessage(QString("[成像馈送耗时] %1 次平均 %2 us/脉冲")
                       .arg(feedCount).arg(accumFeed / feedCount));
            feedCount = 0; accumFeed = 0;
        }

        m_imagingPulseCount++;
        m_testImagingPulseIndex++;

        // 存储重建数据：按帧保存（每 move_aline 个脉冲合并为一个文件）
        if (m_imagingGeneralParams.isSaveReconData) {
            static QByteArray frameBuf;
            static int savedPulseCount = 0;
            static int frameFileIndex = 0;
            if (savedPulseCount == 0) {
                frameBuf.clear();
                frameBuf.reserve(onePulse * m_imagingScanParams.move_aline * 4);
            }
            frameBuf.append(reinterpret_cast<const char*>(pulseBase),
                            onePulse * sizeof(float));
            savedPulseCount++;
            if (savedPulseCount >= m_imagingScanParams.move_aline) {
                QString frameDir = QApplication::applicationDirPath() + "/frame_data";
                QDir().mkpath(frameDir);
                QString fname = QString("%1/frame_%2.bin")
                    .arg(frameDir).arg(++frameFileIndex, 3, 10, QChar('0'));
                QFile pf(fname);
                if (pf.open(QIODevice::WriteOnly)) {
                    pf.write(frameBuf);
                    pf.close();
                }
                savedPulseCount = 0;
                frameBuf.clear();
            }
        }

        int moveAline = m_imagingScanParams.move_aline;
        int progress = m_imagingPulseCount % moveAline;
        if (progress == 0) progress = moveAline;
        m_lblImagingStatus->setText(
            QString("测试脉冲 %1/%2 · 输出 %3 帧")
            .arg(progress).arg(moveAline).arg(m_roundUi.frameCount()));
        return;
    }

    // ── 实时数据模式：从 DisplayBuffer 读取采集卡数据 ──
    if (!m_netController) return;

    static int64_t feedCounter = 0;
    feedCounter++;

    bool hasNewData = false;
    for (int globalCard = 0; globalCard < m_nCards; ++globalCard) {
        DisplayBuffer *db = m_netController->displayBuffer(globalCard);
        if (!db) continue;
        DisplayBuffer::FullResSnapshot snap;
        if (db->peekLatestFull(snap) && snap.valid) {
            // 跳过触发序号未变的数据（停止测量后 DisplayBuffer 保留旧快照）
            if (snap.triggerSeq == m_lastFeedSeq[globalCard]) continue;
            m_lastFeedSeq[globalCard] = snap.triggerSeq;
            hasNewData = true;
            size_t szA = snap.freqA.size();
            size_t szB = snap.freqB.size();
            QVector<float> freqA(static_cast<int>(szA), 0.0f);
            QVector<float> freqB(static_cast<int>(szB), 0.0f);
            if (szA > 0) memcpy(freqA.data(), snap.freqA.data(), szA * sizeof(float));
            if (szB > 0) memcpy(freqB.data(), snap.freqB.data(), szB * sizeof(float));
            m_imagingController->feedPulseData(globalCard, snap.triggerSeq, freqA, freqB);
            if (feedCounter % 600 == 0) {
                logMessage(QString("[成像馈送] 卡%1 seq=%2 freqA=%3 freqB=%4")
                           .arg(globalCard).arg(snap.triggerSeq)
                           .arg(freqA.size()).arg(freqB.size()));
            }
        }
    }

    // 无新数据时跳过（避免停止测量后反复馈送旧数据）
    if (!hasNewData) return;

    // 所有卡馈送完成后提交脉冲
    m_imagingController->flushPulseData();

    // ══ 馈送阶段计时（实时模式）════════════════════════════
    uint64_t tFeedEnd = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    static uint64_t accumFeed = 0;
    static int feedCount = 0;
    accumFeed += (tFeedEnd - tFeedStart);
    if (++feedCount >= 200) {
        logMessage(QString("[成像馈送耗时] %1 次平均 %2 us/脉冲")
                   .arg(feedCount).arg(accumFeed / feedCount));
        feedCount = 0; accumFeed = 0;
    }

    m_imagingPulseCount++;

    int moveAline = m_imagingScanParams.move_aline;
    int progress  = m_imagingPulseCount % moveAline;
    if (progress == 0) progress = moveAline;
    m_lblImagingStatus->setText(QString("采集脉冲 %1/%2 · 输出 %3 帧")
                                .arg(progress).arg(moveAline)
                                .arg(m_roundUi.frameCount()));
}

// =====================================================================
// 环形模式实时馈送（独立工作线程，高速率化）
// DataProcessor 每完成一个触发即入队（ringFeedSink），本工作线程按序送入
// RingBlockAssembler；组满 perChannelBlock 后回调提交环形块。
// 与 UI 主线程完全解耦，避免 latest-only 显示缓冲在高触发率下丢触发。
// =====================================================================
void MainWindow::startRingFeedWorker()
{
    if (!m_imagingBypass) return;
    const auto beginNs = paimage::SocketReceiver::now();
    m_imagingBypass->clear(ImagingSubmitResult::StaleSession);
    m_imagingBypass->setEnabled(true);
    m_imagingBypass->setServiceReady(m_imagingServiceReady.load(std::memory_order_acquire));
    const auto endNs = paimage::SocketReceiver::now();
    recordDiagnosticAction(QStringLiteral("imaging_queue_initialized"),
        {{"startMonotonicNs", QString::number(beginNs)},
         {"endMonotonicNs", QString::number(endNs)},
         {"durationNs", QString::number(endNs - beginNs)},
         {"clearReason", QStringLiteral("stale_session")}});
}

void MainWindow::stopRingFeedWorker()
{
    if (!m_imagingBypass) return;
    const auto beginNs = paimage::SocketReceiver::now();
    m_imagingBypass->setEnabled(false);
    m_imagingBypass->clear(ImagingSubmitResult::Disabled);
    const auto endNs = paimage::SocketReceiver::now();
    recordDiagnosticAction(QStringLiteral("imaging_queue_cleared"),
        {{"startMonotonicNs", QString::number(beginNs)},
         {"endMonotonicNs", QString::number(endNs)},
         {"durationNs", QString::number(endNs - beginNs)},
         {"clearReason", QStringLiteral("disabled")}});
}

ImagingSubmitResult MainWindow::ringFeedSink(const TriggerGroupConstPtr& frame)
{
    return m_imagingBypass ? m_imagingBypass->tryPush(frame)
                           : ImagingSubmitResult::Disabled;
}

void MainWindow::configureRingAssembler()
{
    if (!m_imagingController || !m_ringAssembler) return;
    const auto configureBeginNs = paimage::SocketReceiver::now();
    const RingReconCudaConfig &cfg = m_imagingController->ringConfig();
    const bool roundConfigValid =
        cfg.enabledChannelCount > 0 && cfg.alinesPerChannelPerBlock > 0 &&
        cfg.sampDepth > 0 && cfg.alinesPerChannelPerFrame > 0 &&
        cfg.alinesPerFrame > 0 && cfg.alinesPerFrame % cfg.enabledChannelCount == 0 &&
        cfg.alinesPerFrame / cfg.enabledChannelCount == 2 * cfg.alinesPerChannelPerFrame &&
        cfg.alinesPerFrame % (cfg.enabledChannelCount * cfg.alinesPerChannelPerBlock) == 0;
    if (!roundConfigValid) {
        recordDiagnosticAction(QStringLiteral("imaging_assembler_invalid_config"),
            {{"enabledChannelCount", cfg.enabledChannelCount},
             {"alinesPerFrame", cfg.alinesPerFrame},
             {"alinesPerChannelPerFrame", cfg.alinesPerChannelPerFrame},
             {"alinesPerChannelPerBlock", cfg.alinesPerChannelPerBlock},
             {"sampDepth", cfg.sampDepth},
             {"logicalRoundConfigSource", QStringLiteral(
                 "RingReconCudaConfig.alinesPerFrame/enabledChannelCount")}});
        logMessage(QStringLiteral("[环形采集] 轮次配置不一致，拒绝初始化组包器"));
        return;
    }
    const int logicalTriggersPerRound = cfg.alinesPerFrame / cfg.enabledChannelCount;
    if (m_netController) {
        m_netController->setLogicalTriggersPerRound(
            static_cast<std::uint64_t>(logicalTriggersPerRound));
        m_netController->setPhysicalRoundTimeout(cfg.timeoutResetSec);
    }

    int enabled[8];
    for (int i = 0; i < 8; ++i) enabled[i] = cfg.enabledChannels[i] ? 1 : 0;
    if (m_imagingBypass) {
        std::array<bool, 8> channelMask{};
        for (int i = 0; i < 8; ++i) channelMask[i] = enabled[i] != 0;
        m_imagingBypass->setEnabledChannels(channelMask);
    }
    const double sectorWidth = 360.0 / cfg.enabledChannelCount;
    const double step = sectorWidth / cfg.alinesPerChannelPerFrame;

    { std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
      m_ringBoundaryPending = false;
      m_ringBoundaryCards = 0;
      m_ringBoundaryApplied = false;
      m_pendingCountPresentation.clear();
      m_lastRingTimeoutBoundarySession = 0;
      m_lastRingTimeoutBoundaryGeneration = 0;
      m_ringAssembler->configure(enabled, cfg.alinesPerChannelPerBlock,
                                 cfg.sampDepth, cfg.sectorStartDeg, sectorWidth,
                                 step, cfg.alinesPerChannelPerFrame,
                                 cfg.triggerWlOdd, cfg.timeoutResetSec); }
    m_ringAssemblerConfigured = true;
    m_roundUi.reset();   // 新组包会话：每轮计数与快照准入空间清零
    m_ringTimeoutSaveDone = false;   // 新会话：允许超时到点保存
    m_ringSnapshotAdmissionBlocked.store(false, std::memory_order_release);
    m_ringResetCommandSent.store(false, std::memory_order_release);
    const auto configureEndNs = paimage::SocketReceiver::now();
    recordDiagnosticAction(QStringLiteral("imaging_assembler_initialized"),
        {{"startMonotonicNs", QString::number(configureBeginNs)},
         {"endMonotonicNs", QString::number(configureEndNs)},
         {"durationNs", QString::number(configureEndNs - configureBeginNs)},
         {"enabledChannels", cfg.enabledChannelCount},
         {"alinesPerChannelPerBlock", cfg.alinesPerChannelPerBlock},
         {"configuredLogicalTriggersPerRound", logicalTriggersPerRound},
         {"logicalRoundConfigSource", QStringLiteral(
             "RingReconCudaConfig.alinesPerFrame/enabledChannelCount")}});
    logMessage(QString("[环形采集] 组包器已配置: 通道=%1 每块=%2 step=%3°")
               .arg(cfg.enabledChannelCount)
               .arg(cfg.alinesPerChannelPerBlock)
               .arg(step, 0, 'f', 4));
}

// =====================================================================
// updateRingImagingStatus — 环形模式“成像中”状态反馈
// x = 当前块已收到的触发脉冲数，y = 每通道每块 Aline 数（组包器块进度）；
// 首个触发脉冲到达即由等待状态切换为计数显示；帧计数沿用 m_roundUi。
// 仅主线程调用（工作线程通过 QMetaObject::invokeMethod 切回）。
// =====================================================================
void MainWindow::updateRingImagingStatus()
{
    if (!m_lblImagingStatus || !m_imagingEnabled) return;
    if (!m_imagingController || !m_imagingController->isRingMode() || !m_ringAssembler) return;

    const QString amberStyle =
        "color: #FFAA00; background: transparent; padding: 2px 8px;"
        "font-size: 12px;";
    std::pair<int, int> prog;
    { std::lock_guard<std::mutex> lock(m_ringAssemblerMutex);
      prog = m_ringAssembler->blockProgress(); }
    const int pulses = prog.first;
    const int perBlock = prog.second;
    if (perBlock <= 0) {
        m_lblImagingStatus->setText("成像中，等待重建数据…");
        m_lblImagingStatus->setStyleSheet(amberStyle);
        return;
    }

    const paimage::RingRoundUiState::Snapshot roundUi = m_roundUi.snapshot();
    const auto blocksDone = roundUi.blockCount;
    if (pulses <= 0 && blocksDone <= 0) {
        m_lblImagingStatus->setText("成像中，等待重建数据…");
        m_lblImagingStatus->setStyleSheet(amberStyle);
        return;
    }

    // 块刚完成、下一块首个脉冲未到：短暂显示 y/y，避免闪回“等待数据”
    const int displayPulses = (pulses > 0) ? pulses : perBlock;
    m_lblImagingStatus->setText(
        QString("采集脉冲 %1/%2 · 输出 %3 帧")
        .arg(displayPulses).arg(perBlock)
        .arg(roundUi.frameCount));
    m_lblImagingStatus->setStyleSheet(amberStyle);
}
