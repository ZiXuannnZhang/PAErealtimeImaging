#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QVector>
#include <QSettings>
#include <QTabBar>
#include <QDialog>
#include <QJsonObject>
#include <QImage>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QShowEvent>
#include <QResizeEvent>
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <memory>
#include "qcustomplot.h"
#include "DataTypes.h"
#include "CardStatusFormatting.h"
#include "AcqConfig.h"
#include "Constants.h"
#include "ImagingParams.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Ui {
class MainWindow;
}

class NetworkController;
class ImagingController;
class RingConfigDialog;
class ImagingDisplayWindow;
class RingBlockAssembler;
class ImagingBypass;
enum class ImagingSubmitResult : std::uint8_t;

// 命令行开关：--target-ips <ip,ip,...> 指定固定目标采集卡 IP 列表
// （真机固定 IP 与同机 UDP 联调均可用；省略时自动执行网段扫描）
extern QStringList g_targetIPs;

// 显示类型下拉索引（顺序与 cmbDisplayType 完全一致）
enum DisplayModeIndex {
    DM_PHASE = 0,   // 差分相位
    DM_FREQ  = 1,   // 瞬时频率
    DM_LINEAR = 2,  // 线性扫描
    DM_RING  = 3    // 环形扫描
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 逐卡状态栏的稳定文本/tooltip 格式，UI 与无窗口单元测试共用。
    static QString formatCardStatusText(int cardNumber,
                                        const CardStats::Snapshot& stats);
    static QString formatCardStatusTooltip(const CardStats::Snapshot& stats);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onStartListenClicked();
    void onStartMeasureClicked();
    void onConfigParamsClicked(bool fromStartMeasure = false);
    void onExportDiagnosticClicked();
    void onPrepareSystemCaptureClicked();
    void onSelectDirClicked();
    void onToggleSaveClicked();
    void onAutoSaveToggled(bool checked);   // 自动保存勾选切换（环形：触发开始/圈末或超时重置停止）
    QString advanceAutoSession();           // 方案2：分配下一会话目录（编号+1）并注册会话代，返回新目录
    void initAutoSaveSavers();              // 方案B：在当前控制器上（重）建自动保存（监听重启后恢复用）
    void onEnableDisplayToggled(bool checked);
    void onRealtimeImagingToggled(bool checked);
    void onDisplayTypeChanged(int index);
    void onImagingModeChanged(int index);
    void onRefreshRateChanged(int value);
    void onTileViewToggled(bool checked);
    void onUpdateStatistics();
    void onDisplayRefresh();          // 30fps pull 模式
    void onGroupTabChanged(int groupIndex);
    void onImagingConfigClicked();
    void onImagingStarted();
    void onImagingStopped();
    void onImagingImageReady(const QImage &image, int seq);
    void onImagingError(const QString &error);
    void setImagingParamControlsEnabled(bool enable); // 实时成像期间锁定成像参数/采集控制，svcStopped 后恢复
    void onDiagnosticStatusTick();
    void onSystemCaptureStatusTick();

private:
    void setupUI();
    void rebuildDynamicUI();          // 根据当前 m_nCards 重建 Tab/图表/统计栏
    void setupPlots();
    void recreatePlots();  // 重新创建所有图表（用于修复OpenGL缓冲区问题）
    void createConnections();
    void updatePlot(int cardId, int channel,
                    const QVector<double> &data,
                    const QVector<double> &frequency = QVector<double>(),
                    const QVector<double> &xAxis = QVector<double>(),
                    bool doReplot = true);
    void updateSpectrumPlot(int cardId, int channel,
                            const QVector<double> &xAxis,
                            const QVector<double> &magnitude,
                            bool doReplot = true);
    void logMessage(const QString &message);
    void recordDiagnosticAction(const QString &action,
                                const QJsonObject &fields = QJsonObject());
    void recordAcquisitionSnapshot(const QVector<QString> &targetIPs,
                                   const QString &phase);
    QJsonObject diagnosticAcquisitionSnapshot(const QVector<QString> &targetIPs,
                                              const QString &phase) const;
    void loadSettings();
    void saveSettings();
    void loadStyleSheet();
    void updateGroupDisplay(int groupIndex);
    void syncVisiblePlotsGeometry();
    void setAxisRange(QCustomPlot *plot, bool isXAxis);
    // 坐标轴数字直接编辑（双击轴刻度数字 → 行内编辑 → 等同右键“设置坐标范围”）。
    // isSpectrum：频域幅值谱图使用独立的自适应/保存范围标志组。
    void attachAxisEdit(QCustomPlot *plot, int card, int ch, bool isSpectrum);
    QVector<double> calculateFrequency(const QVector<double> &phaseData);
    void updateNetworkInfoLabels(); // 根据 m_nCards 刷新网络控制面板标签
    void updateNetworkInfoIndicator(); // 把只读网络信息汇总到感叹号悬停提示
    void saveReconImage(const QImage &image, const QString &tag); // 实时重建图像 PNG 保存
    void updateRingImagingStatus(); // 环形模式成像运行状态反馈（当前块脉冲数，主线程调用）
    // targetSourceKind: "explicit_target_ips" 或 "config_ack_discovery"
    void startListeningWithIPs(const QVector<QString>& onlineIPs,
                               const QString& targetSourceKind); // 发现/显式目标完成后：创建 controller 并启动监听（主线程）
    void updateSystemCaptureStatus(const QString &status,
                                   const QJsonObject &fields = QJsonObject());
    bool invalidateSystemCaptureRequest();

    // UI 对象
    Ui::MainWindow *ui;

    // 注册表持久化的配置（无 UI 接口，通过注册表直接修改）
    int     m_nCards;       // 采集卡数量，注册表键 AcquisitionParams/NCards（默认4；自动识别后更新）
    QString m_localBindIP;  // 控制 socket 本地绑定IP，注册表键 NetworkParams/LocalBindIP
                            // 双口网卡只接一个口时必填（如 "192.168.0.100"），空=INADDR_ANY
    QString m_scanBaseIP;   // 网段扫描起始 IP，注册表键 NetworkParams/ScanBaseIP（默认 192.168.0.2）
    int     m_scanIPCount;  // 网段扫描 IP 数量，注册表键 NetworkParams/ScanIPCount（默认 32，上限 MAX_CARDS）
    QString m_targetIPRangeText; // 自动识别后目标 IP 范围显示文本（如 "192.168.0.2 ~ 192.168.0.5"）

    // 每组显示的卡数（固定4张，但最后一组可能不足4张）
    static constexpr int CARDS_PER_DISPLAY_GROUP = CARDS_PER_BOX; // 4

    // 动态创建的状态栏标签（数量 = 每组卡数，固定4个槽位）
    QLabel *m_lblStats[CARDS_PER_BOX];

    // 分组导航（分组数随 nCards 动态变化）
    QTabBar   *m_groupTabBar;
    QWidget   *m_placeholderPage;
    int        m_currentGroup;
    int        m_numGroups;  // ceil(m_nCards / CARDS_PER_DISPLAY_GROUP)

    // 动态 Tab + 图表（按最大卡数分配，运行时只用前 m_nCards 个）
    QStackedWidget *m_stackedWidgets[MAX_CARDS][2];
    QCustomPlot    *m_plotsPhase    [MAX_CARDS][2];
    QCustomPlot    *m_plotsFrequency[MAX_CARDS][2];
    QCustomPlot    *m_plotsSpectrum [MAX_CARDS][2];
    QLabel         *m_pkpkLabels    [MAX_CARDS][2];
    QWidget        *m_tileContainers[MAX_CARDS][2];

    // 颜色图
    QCustomPlot    *m_colorMapPlot;
    QCPColorMap    *m_colorMap;
    QCPColorScale  *m_colorScale;

    bool     m_isTileView;

    // pk-pk 历史（备用）
    std::deque<double> m_pkpkHistory[MAX_CARDS][2];

    // 核心模块
    NetworkController *m_netController;

    // 定时器
    QTimer *m_statsTimer;
    QTimer *m_diagnosticStatusTimer;
    QTimer *m_systemCaptureStatusTimer;
    QTimer *m_displayTimer;   // 30fps pull 定时器
    QTimer *m_ringTimeoutTimer = nullptr;   // 物理空闲超时到点保存 PNG
    std::atomic<bool> m_ringTimeoutSaveDone{false};  // shared timeout boundary/UI timer de-dup
    std::atomic<bool> m_ringTimeoutAutoSessionDone{false};

    // 状态标志
    bool m_isListening;
    bool m_isMeasuring;
    bool m_highDataRateWarningShown;
    bool m_pendingAutoSave;  // 重新监听后需要自动恢复保存（停止监听前处于保存状态）
    bool m_scanning = false; // 网段扫描进行中（防止重复触发）

    // CONFIG-ACK 动态发现会话：结果回主线程后按 discoveryId 接纳，取消标志供后台线程退出。
    QString m_activeDiscoveryId;
    std::shared_ptr<std::atomic<bool>> m_discoveryCancel;
    QString m_activeTargetSource;   // 最近一次监听启动的目标来源（诊断快照用）

    // 诊断上下文：每次开始监听生成新的会话标识，随 UI 日志/网络快照传递。
    QString m_diagnosticListenId;
    bool m_diagnosticStatusInitialized = false;
    bool m_diagnosticLastWriteError = false;
    QString m_diagnosticLastWriteErrorText;
    quint64 m_diagnosticLastQueueDropped = 0;
    quint64 m_diagnosticLastCriticalQueueDropped = 0;
    quint64 m_diagnosticLastNoiseDropped = 0;
    quint64 m_diagnosticLastFallbackDropped = 0;
    int m_diagnosticStatusNoticeCount = 0;
    QLabel *m_systemCaptureStatusLabel = nullptr;
    QString m_systemCaptureRequestPath;
    QString m_systemCaptureOutputDirectory;
    QString m_systemCaptureTrialId;
    QString m_systemCaptureSessionToken;
    QString m_systemCaptureLastState;

    // 存储队列溢出告警跟踪
    uint64_t m_prevSaveDiscards[MAX_CARDS];  // 上次 onUpdateStatistics 时各卡的 saveQueueDiscards 值
    int      m_saveWarnCooldown;             // 剩余静默计时（每个 statsTimer tick 减1，=0时可再告警）

    // 显示控制
    bool m_displayEnabled;
    bool m_enableDownsampling;
    bool m_autoRescaleAxes;
    bool m_autoRescalePlot[CARDS_PER_DISPLAY_GROUP][2]; // 逐图自适应坐标轴标志（默认true）
    bool m_autoRescaleSpectrum[CARDS_PER_DISPLAY_GROUP][2]; // 频域图逐图自适应坐标轴标志（默认true）
    QCPRange m_savedSpectrumXRange[CARDS_PER_DISPLAY_GROUP][2];
    QCPRange m_savedSpectrumYRange[CARDS_PER_DISPLAY_GROUP][2];
    QCPRange m_savedXRange[CARDS_PER_DISPLAY_GROUP][2]; // 取消自适应时恢复的X范围
    QCPRange m_savedYRange[CARDS_PER_DISPLAY_GROUP][2]; // 取消自适应时恢复的Y范围
    QGroupBox *m_grpImaging;        // 成像控制分组框
    int  m_displayCounter[MAX_CARDS];
    bool m_firstPlot[MAX_CARDS][2];
    int  m_displayInterval;
    bool m_openglInitialized;  // OpenGL是否已初始化

    // 数据格式参数（注册表配置，无 UI 接口；真实采集固定 250MHz=FPGA_ADC_FREQ_HZ）
    int    m_bitsPerChannel    = 32;    // 每通道位宽（16 或 32）
    double m_sampleIntervalNs  = FPGA_ADC_INTERVAL_NS;   // 采样间隔 ns（4.0=250MHz 满速率）
    int    m_logicalTriggersPerRound = AcqConfig::kDefaultLogicalTriggersPerRound;

    // pull 模式计数器（配合 spnRefreshRate 跳帧）
    int m_refreshCounter;
    // 实际显示刷新计数（每次 onDisplayRefresh 通过跳帧门槛后自增，供 rescaleAxes 节流）
    int m_displayTickCount;
    // 单通道切 tab 后强制立即刷新一次新前台通道（不等下一个刷新周期）
    bool m_forceRefreshOnce = false;

    // 加载标志：loadSettings 中禁止 saveSettings 递归覆盖默认值
    bool m_loadingSettings;

    // ══ 成像扩展 ═══════════════════════════════════════════════════
    ImagingController *m_imagingController;

    // 成像参数持久化
    GeneralParams m_imagingGeneralParams;
    ScanParams    m_imagingScanParams;
    ReconParams   m_imagingReconParams;

    // 成像参数配置对话框
    QDialog *m_imagingConfigDialog;

    // 环形扫描控制台（M3）
    RingConfigDialog *m_ringConfigDialog = nullptr;   // 环形扫描参数设定窗口
    ImagingDisplayWindow *m_imagingDisplayWindow = nullptr;   // 线性扫描实时成像独立弹窗
    RingBlockAssembler *m_ringAssembler = nullptr;   // 阶段B：真实采集组包器
    mutable std::mutex m_ringAssemblerMutex;          // 仅保护成像组包器，不与采集/保存共享
    std::atomic<bool> m_ringAssemblerConfigured{false};
    // Count-boundary events are emitted when the first card observes the
    // final logical identity. The Ring consumer applies the boundary only
    // after every enabled card for that identity has been consumed.
    bool m_ringBoundaryPending = false;
    uint64_t m_ringBoundarySession = 0;
    uint64_t m_ringBoundaryGeneration = 0;
    uint16_t m_ringBoundaryTrigger = 0;
    uint32_t m_ringBoundaryCards = 0;
    bool m_ringBoundaryApplied = false;
    uint64_t m_lastRingBoundarySession = 0;
    uint64_t m_lastRingBoundaryGeneration = 0;
    uint16_t m_lastRingBoundaryTrigger = 0;
    uint64_t m_lastRingTimeoutBoundarySession = 0;
    uint64_t m_lastRingTimeoutBoundaryGeneration = 0;
    bool m_restartRingOnSvcStop = false;   // 运行中修改环形参数后，待停止完成时自动重启

    // 独立有界成像旁路。队列持有共享只读触发帧，不复制整卡 A/B 数据。
    std::unique_ptr<ImagingBypass> m_imagingBypass;
    std::atomic<bool> m_imagingServiceReady{false};

    // 成像状态
    bool m_imagingEnabled;
    QPushButton *m_btnImagingConfig;
    QComboBox *m_cmbImagingMode;
    QCheckBox *m_chkRealtimeImaging;

    bool               m_useTestImagingData;     // 测试数据模式开关（注册表持久化）
    std::vector<float> m_testImagingData;        // 测试数据全部脉冲
    int                m_testImagingPulseIndex;  // 当前脉冲序号（测试模式）
    bool               m_peizhunLoaded;          // 配准文件是否已加载
    QString            m_peizhunFilePath;        // 配准文件路径（持久化）
    QLabel      *m_lblImagingStatus;   // 成像进度/状态指示
    QTimer      *m_imagingTimer;       // 独立成像馈送定时器（5ms）
    int          m_imagingPulseCount;  // 当前帧已采集脉冲数
    std::atomic<int> m_ringBlockCounter{0};  // 环形重建已提交组包数（工作线程写/UI读）
    int          m_imagingFrameCount;  // 已输出帧数
    QCPRange     m_freqColorRange;     // 频率颜色图保存的色条范围
    QCPRange     m_pixelColorRange;    // 像素图保存的色条范围
    bool         m_settingColorRange = false; // 程序设置色条范围时的互斥标记
    bool         m_freqColorInited;    // 频率颜色图是否已执行首帧自适应
    bool         m_pixelColorInited;   // 像素图是否已执行首帧自适应
    uint16_t     m_lastFeedSeq[MAX_CARDS]; // 各卡上次馈送的触发序号，用于去重
    void feedImagingPulse();           // 单次成像馈送
    void startRingFeedWorker();        // 环形模式：独立工作线程消费触发队列→组包器
    void stopRingFeedWorker();
    ImagingSubmitResult ringFeedSink(const TriggerGroupConstPtr& frame);
    void configureRingAssembler();     // 按控制器环形配置初始化组包器
    bool loadTestImagingData();        // 加载测试数据bin文件

    // 实时重建图像保存（随数据保存开关联动）
    bool    m_reconSaveEnabled = false;
    QString m_reconSaveDir;
    QString m_reconSaveSuffix;

    // 自动保存（环形）：勾选后随采集触发开始、随圈末/超时重置停止；
    // 会话数据存于 输入路径上一级 下的三位数编号文件夹（001、002…）
    bool m_autoSaveEnabled = false;
    int  m_autoSaveNext = 0;   // 下一编号（勾选时扫描基线目录最大三位数编号，会话开始前自增）
};

#endif // MAINWINDOW_H
