#include "RingConfigDialog.h"
#include "ImagingController.h"

#include <QComboBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QSettings>
#include <QHideEvent>
#include <QShowEvent>
#include <QScrollArea>
#include <QFrame>
#include <QMessageBox>
#include <algorithm>

RingConfigDialog::RingConfigDialog(ImagingController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setWindowTitle("环形扫描参数设定");
    // 与线性参数设定窗口保持一致的窗口尺寸
    setMinimumSize(500, 450);
    buildUi();

    // 记忆上次关闭前的大小
    QSettings s("MC410T", "MC410T_Receiver");
    const QSize saved = s.value("RingConfigDialog/Size").toSize();
    if (saved.isValid() && saved.width() >= 500 && saved.height() >= 450)
        resize(saved);
}

void RingConfigDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs);

    // ══ 扫描参数：采集与数据格式 + 通道勾选与扇区 ══
    auto *scanPage = new QWidget;
    auto *scanLayout = new QVBoxLayout(scanPage);

    auto *grpData = new QGroupBox("采集与数据格式");
    auto *fData = new QFormLayout(grpData);
    m_spnBlock = new QSpinBox; m_spnBlock->setRange(2, 10000); m_spnBlock->setSingleStep(2); m_spnBlock->setValue(200);
    m_chkShiftWL2 = new QCheckBox("1064 跨块对齐（首块不偏移）"); m_chkShiftWL2->setChecked(true);
    // 采样率/采样深度只读：数据来源与线性扫描一致（采集时间/采样间隔），由 MainWindow 下发
    const QString roStyle = "color:#AAAAAA; background:#2A2A2A; padding:4px 8px; border:1px solid #3C3C3C; border-radius:3px;";
    m_lblSampDepth = new QLabel(QString::number(m_sampDepth));
    m_lblSampDepth->setStyleSheet(roStyle);
    m_lblSampDepth->setAlignment(Qt::AlignCenter);
    m_lblDaqHz = new QLabel(QString::number(m_daqHz, 'f', 0));
    m_lblDaqHz->setStyleSheet(roStyle);
    m_lblDaqHz->setAlignment(Qt::AlignCenter);
    fData->addRow("每通道每块A-line数", m_spnBlock);
    fData->addRow("采样深度(点/A-line)", m_lblSampDepth);
    fData->addRow("", m_chkShiftWL2);
    fData->addRow("采样率(Hz)", m_lblDaqHz);
    scanLayout->addWidget(grpData);

    auto *grpCh = new QGroupBox("通道勾选与扇区分配（逆时针，从 9 点钟方向起均分 360°）");
    auto *fCh = new QFormLayout(grpCh);
    auto *chRow = new QHBoxLayout;
    for (int c = 0; c < 8; ++c) {
        m_chkCh[c] = new QCheckBox(QString("通道%1").arg(c + 1));
        m_chkCh[c]->setChecked(true);
        chRow->addWidget(m_chkCh[c]);
    }
    fCh->addRow("启用通道", chRow);
    m_spnTotalAlines = new QSpinBox; m_spnTotalAlines->setRange(2, 1000000);
    m_spnTotalAlines->setSingleStep(100); m_spnTotalAlines->setValue(8000);
    m_spnTotalAlines->setToolTip("单圈总A-line数（含双波长）。\n"
        "每通道每波长每圈A-line数 = 单圈总A-line数 / (启用通道数 × 2)");
    m_spnSectorStart = new QDoubleSpinBox; m_spnSectorStart->setRange(-360, 360);
    m_spnSectorStart->setDecimals(1); m_spnSectorStart->setValue(0.0);
    m_spnSectorStart->setToolTip("0° = 9 点钟方向，正值逆时针偏转。");
    fCh->addRow("单圈总A-line数(含双波长)", m_spnTotalAlines);
    fCh->addRow("首通道扇区起点(°,0=9点钟)", m_spnSectorStart);
    scanLayout->addWidget(grpCh);
    scanLayout->addStretch();
    auto *scanScroll = new QScrollArea;
    scanScroll->setWidgetResizable(true);
    scanScroll->setFrameShape(QFrame::NoFrame);
    scanScroll->setWidget(scanPage);
    tabs->addTab(scanScroll, "扫描参数");

    // ══ 重建参数：成像网格与DAS + 声速模型 + 预处理 + 显示 ══
    auto *reconPage = new QWidget;
    auto *reconLayout = new QVBoxLayout(reconPage);

    auto *grpGrid = new QGroupBox("成像网格与 DAS");
    auto *fGrid = new QFormLayout(grpGrid);
    m_spnFovMm = new QDoubleSpinBox; m_spnFovMm->setRange(1, 500); m_spnFovMm->setDecimals(3); m_spnFovMm->setValue(36.0);
    m_spnGridMm = new QDoubleSpinBox; m_spnGridMm->setRange(0.005, 5); m_spnGridMm->setDecimals(3); m_spnGridMm->setValue(0.1);
    m_spnRadiusMm = new QDoubleSpinBox; m_spnRadiusMm->setRange(0.1, 50); m_spnRadiusMm->setDecimals(4); m_spnRadiusMm->setValue(6.57);
    m_spnFovDeg = new QDoubleSpinBox; m_spnFovDeg->setRange(1, 360); m_spnFovDeg->setValue(360);
    m_spnFovTheta0 = new QDoubleSpinBox; m_spnFovTheta0->setRange(-360, 360); m_spnFovTheta0->setDecimals(1); m_spnFovTheta0->setValue(0.0);
    m_spnFovTheta0->setToolTip("成像扇区起始角，0° = 9 点钟方向，正值逆时针偏转（与首通道扇区起点一致）。");
    m_cmbApod = new QComboBox; m_cmbApod->addItems({"none", "hann", "hamming"});
    m_spnDistWeight = new QDoubleSpinBox; m_spnDistWeight->setRange(0, 10); m_spnDistWeight->setValue(1);
    m_spnMinDistMm = new QDoubleSpinBox; m_spnMinDistMm->setRange(0, 10); m_spnMinDistMm->setDecimals(4); m_spnMinDistMm->setValue(0);
    m_chkMaskOob = new QCheckBox("越界置零"); m_chkMaskOob->setChecked(true);
    // 注：CoverageDeg/Theta0Deg 仅用于离线整帧文件重建脚本，实时链路逐 A-line 角度由扇区模型给出，故不在界面提供；
    // 插值固定为 linear（DAS 反投影需要分数采样点插值，nearest 仅调试用）。
    fGrid->addRow("FOV(mm)", m_spnFovMm);
    fGrid->addRow("GridSize(mm)", m_spnGridMm);
    fGrid->addRow("Radius(mm)", m_spnRadiusMm);
    fGrid->addRow("成像扇区角宽(°)", m_spnFovDeg);
    fGrid->addRow("成像扇区起始角(°,0=9点钟)", m_spnFovTheta0);
    fGrid->addRow("Apodization", m_cmbApod);
    fGrid->addRow("距离权重指数", m_spnDistWeight);
    fGrid->addRow("MinDistance(mm,0=自动)", m_spnMinDistMm);
    fGrid->addRow("", m_chkMaskOob);
    reconLayout->addWidget(grpGrid);

    auto *grpSos = new QGroupBox("声速模型");
    auto *fSos = new QFormLayout(grpSos);
    m_spnSos1 = new QDoubleSpinBox; m_spnSos1->setRange(100, 10000); m_spnSos1->setValue(1490);
    m_spnSos2 = new QDoubleSpinBox; m_spnSos2->setRange(100, 10000); m_spnSos2->setValue(1540);
    m_spnLayerCount = new QSpinBox; m_spnLayerCount->setRange(0, 1); m_spnLayerCount->setValue(0);
    m_spnLayerRadiusMm = new QDoubleSpinBox; m_spnLayerRadiusMm->setRange(0.1, 50);
    m_spnLayerRadiusMm->setDecimals(4); m_spnLayerRadiusMm->setValue(3.0);
    m_spnLayerRadiusMm->setEnabled(false);
    m_spnLayerRadiusMm->setToolTip("双声速分层边界半径（环心到边界），0=单声速。");
    fSos->addRow("声速1(m/s)", m_spnSos1);
    fSos->addRow("声速2(m/s)", m_spnSos2);
    fSos->addRow("双声速边界数(0=单声速)", m_spnLayerCount);
    fSos->addRow("边界半径(mm)", m_spnLayerRadiusMm);
    connect(m_spnLayerCount, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int v) { m_spnLayerRadiusMm->setEnabled(v > 0); });
    reconLayout->addWidget(grpSos);

    auto *grpPre = new QGroupBox("预处理");
    auto *fPre = new QFormLayout(grpPre);
    m_spnSysDelay1 = new QSpinBox; m_spnSysDelay1->setRange(1, 100000); m_spnSysDelay1->setValue(358);
    m_spnSysDelay2 = new QSpinBox; m_spnSysDelay2->setRange(1, 100000); m_spnSysDelay2->setValue(371);
    m_spnMaskLen = new QSpinBox; m_spnMaskLen->setRange(0, 100000); m_spnMaskLen->setValue(300);
    m_chkDbr = new QCheckBox("扣除DBR强信号"); m_chkDbr->setChecked(true);
    m_chkDelayCut = new QCheckBox("延时截断"); m_chkDelayCut->setChecked(true);
    m_chkImpair = new QCheckBox("阈值削顶"); m_chkImpair->setChecked(false);
    m_spnImValue1 = new QDoubleSpinBox; m_spnImValue1->setRange(0, 1e7); m_spnImValue1->setValue(2000);
    m_spnImValue2 = new QDoubleSpinBox; m_spnImValue2->setRange(0, 1e7); m_spnImValue2->setValue(400);
    fPre->addRow("sysDelay1(532nm)", m_spnSysDelay1);
    fPre->addRow("sysDelay2(1064nm)", m_spnSysDelay2);
    fPre->addRow("DBR mask长度", m_spnMaskLen);
    fPre->addRow("", m_chkDbr);
    fPre->addRow("", m_chkDelayCut);
    fPre->addRow("", m_chkImpair);
    fPre->addRow("imValue1(532nm)", m_spnImValue1);
    fPre->addRow("imValue2(1064nm)", m_spnImValue2);
    reconLayout->addWidget(grpPre);

    auto *grpDisp = new QGroupBox("显示");
    auto *fDisp = new QFormLayout(grpDisp);
    m_spnCRecon = new QDoubleSpinBox; m_spnCRecon->setRange(1, 1e6); m_spnCRecon->setValue(100);
    fDisp->addRow("colorbar范围(±)", m_spnCRecon);
    reconLayout->addWidget(grpDisp);
    reconLayout->addStretch();
    auto *reconScroll = new QScrollArea;
    reconScroll->setWidgetResizable(true);
    reconScroll->setFrameShape(QFrame::NoFrame);
    reconScroll->setWidget(reconPage);
    tabs->addTab(reconScroll, "重建参数");

    // 底部按钮栏：与线性参数设定窗口一致（恢复默认 | 应用 | 取消 | 确定）
    auto *btnRow = new QHBoxLayout;
    m_btnRestore = new QPushButton("恢复默认");
    m_btnApply = new QPushButton("应用");
    m_btnCancel = new QPushButton("取消");
    m_btnOk = new QPushButton("确定");
    btnRow->addWidget(m_btnRestore);
    btnRow->addStretch();
    btnRow->addWidget(m_btnApply);
    btnRow->addWidget(m_btnCancel);
    btnRow->addWidget(m_btnOk);
    root->addLayout(btnRow);

    connect(m_btnRestore, &QPushButton::clicked, this, &RingConfigDialog::restoreDefaults);
    connect(m_btnApply, &QPushButton::clicked, this, [this]() { applyConfig(); });
    connect(m_btnOk, &QPushButton::clicked, this, [this]() {
        if (applyConfig()) accept();
    });
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
}

// 采样率/采样深度只读：每次打开/启动成像前由 MainWindow 用当前线性采集参数刷新
void RingConfigDialog::setAcquisitionParams(double sampleIntervalNs, int acqTimeNs)
{
    if (sampleIntervalNs <= 0.0) return;
    m_daqHz = 1e9 / sampleIntervalNs;
    m_lblDaqHz->setText(QString::number(m_daqHz, 'f', 0));
    if (acqTimeNs > 0) {
        m_sampDepth = std::max(1, static_cast<int>(acqTimeNs / sampleIntervalNs));
        m_lblSampDepth->setText(QString::number(m_sampDepth));
    }
}

void RingConfigDialog::restoreDefaults()
{
    // 采样率/采样深度不参与恢复默认：数据来源固定为线性采集参数
    m_spnBlock->setValue(200);
    m_chkShiftWL2->setChecked(true);
    for (int c = 0; c < 8; ++c) m_chkCh[c]->setChecked(true);
    m_spnTotalAlines->setValue(8000);
    m_spnSectorStart->setValue(0.0);
    m_spnFovMm->setValue(36.0);
    m_spnGridMm->setValue(0.1);
    m_spnRadiusMm->setValue(6.57);
    m_spnFovDeg->setValue(360);
    m_spnFovTheta0->setValue(0.0);
    m_cmbApod->setCurrentIndex(0);
    m_spnDistWeight->setValue(1);
    m_spnMinDistMm->setValue(0);
    m_chkMaskOob->setChecked(true);
    m_spnSos1->setValue(1490);
    m_spnSos2->setValue(1540);
    m_spnLayerCount->setValue(0);
    m_spnLayerRadiusMm->setValue(3.0);
    m_spnLayerRadiusMm->setEnabled(false);
    m_spnSysDelay1->setValue(358);
    m_spnSysDelay2->setValue(371);
    m_spnMaskLen->setValue(300);
    m_chkDbr->setChecked(true);
    m_chkDelayCut->setChecked(true);
    m_chkImpair->setChecked(false);
    m_spnImValue1->setValue(2000);
    m_spnImValue2->setValue(400);
    m_spnCRecon->setValue(100);
}

RingReconCudaConfig RingConfigDialog::config() const
{
    RingReconCudaConfig cfg{};
    int cnt = 0;
    for (int c = 0; c < 8; ++c) {
        cfg.enabledChannels[c] = m_chkCh[c]->isChecked() ? 1 : 0;
        if (cfg.enabledChannels[c]) ++cnt;
    }
    cfg.enabledChannelCount = cnt;
    cfg.alinesPerChannelPerBlock = m_spnBlock->value();
    // 单圈总A-line数(含双波长) → 每通道每波长每圈A-line数
    cfg.alinesPerChannelPerFrame = (cnt > 0) ? m_spnTotalAlines->value() / (cnt * 2) : 0;
    cfg.alinesPerFrame = m_spnTotalAlines->value();
    // 扇区起点 UI 语义：0 = 9 点钟方向（标准角度 180°），正值逆时针偏转
    cfg.sectorStartDeg = 180.0 + m_spnSectorStart->value();
    cfg.sectorCcw = 1;
    cfg.triggerWlOdd = 1;

    cfg.sampDepth = m_sampDepth;
    cfg.reconDepth = cfg.sampDepth;
    cfg.alinesPerBlock = m_spnBlock->value();
    cfg.rawColsPerBlock = m_spnBlock->value();
    cfg.shiftWL2 = m_chkShiftWL2->isChecked() ? 1 : 0;
    cfg.daqHz = m_daqHz;
    cfg.radius = m_spnRadiusMm->value() * 1e-3;
    cfg.soundSpeedsCount = 2;
    cfg.soundSpeeds[0] = m_spnSos1->value();
    cfg.soundSpeeds[1] = m_spnSos2->value();
    cfg.soundSpeedRadiiCount = m_spnLayerCount->value();
    if (cfg.soundSpeedRadiiCount > 0)
        cfg.soundSpeedRadii[0] = m_spnLayerRadiusMm->value() * 1e-3;
    cfg.fov = m_spnFovMm->value() * 1e-3;
    cfg.gridSize = m_spnGridMm->value() * 1e-3;
    // CoverageDeg/Theta0Deg 为离线文件重建参数，实时链路固定 360/0
    cfg.coverageDeg = 360.0;
    cfg.theta0Deg = 0.0;
    cfg.fovDeg = m_spnFovDeg->value();
    // 成像扇区起始角 UI 语义：0 = 9 点钟方向（标准角度 180°），正值逆时针
    cfg.fovTheta0Deg = 180.0 + m_spnFovTheta0->value();
    cfg.apodType = m_cmbApod->currentIndex();
    cfg.distanceWeightExponent = m_spnDistWeight->value();
    cfg.interpolation = 0;   // 固定 linear（DAS 分数采样点插值）
    cfg.minDistance = m_spnMinDistMm->value() * 1e-3;
    cfg.maskOutOfRange = m_chkMaskOob->isChecked() ? 1 : 0;
    cfg.dbrSigRemove = m_chkDbr->isChecked() ? 1 : 0;
    cfg.maskLength = m_spnMaskLen->value();
    cfg.delayCut = m_chkDelayCut->isChecked() ? 1 : 0;
    cfg.singalImpair = m_chkImpair->isChecked() ? 1 : 0;
    cfg.imValue[0] = m_spnImValue1->value();
    cfg.imValue[1] = m_spnImValue2->value();
    cfg.sysDelay[0] = m_spnSysDelay1->value();
    cfg.sysDelay[1] = m_spnSysDelay2->value();
    cfg.reconMode = 3;
    cfg.readMode = 0;
    return cfg;
}

bool RingConfigDialog::applyConfig()
{
    if (!m_controller) return false;

    int cnt = 0;
    for (int c = 0; c < 8; ++c)
        if (m_chkCh[c]->isChecked()) ++cnt;
    if (cnt == 0) {
        QMessageBox::warning(this, "参数错误", "请至少勾选一个通道。");
        return false;
    }

    const int total = m_spnTotalAlines->value();
    const int divisor = cnt * 2;    // 启用通道数 × 激发光波长数(2)
    if (total % divisor != 0) {
        QMessageBox::warning(this, "参数错误",
            QString("单圈总A-line数 %1 无法按 %2 个通道 × 2 个波长整除，请调整单圈总A-line数。")
                .arg(total).arg(cnt));
        return false;
    }
    const int k = total / divisor;
    if (k < 2) {
        QMessageBox::warning(this, "参数错误",
            QString("换算后每通道每波长每圈A-line数 K=%1 过小（至少 2）。").arg(k));
        return false;
    }
    const int blockWl = m_spnBlock->value() / 2;   // 每块单波长A-line数
    if (k % blockWl != 0) {
        QMessageBox::warning(this, "参数错误",
            QString("换算后每通道每波长每圈A-line数 K=%1 需能被每块单波长A-line数 %2 整除，"
                    "请调整单圈总A-line数或每通道每块A-line数。").arg(k).arg(blockWl));
        return false;
    }

    m_controller->configureRing(config());
    return true;
}

void RingConfigDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    QSettings s("MC410T", "MC410T_Receiver");
    const QSize saved = s.value("RingConfigDialog/Size").toSize();
    if (saved.isValid() && saved.width() >= 500 && saved.height() >= 450)
        resize(saved);
}

void RingConfigDialog::hideEvent(QHideEvent *event)
{
    QSettings s("MC410T", "MC410T_Receiver");
    s.setValue("RingConfigDialog/Size", size());
    s.sync();
    QDialog::hideEvent(event);
}