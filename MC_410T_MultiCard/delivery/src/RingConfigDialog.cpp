#include "PaimageAcquisition/SettingsPath.h"
#include "RingConfigDialog.h"
#include "AcqConfig.h"
#include "ChannelNaming.h"
#include "ImagingController.h"
#include "RoundPolicySettings.h"

#include <QComboBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
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

namespace {

// 双声速模型参数解析（与 Handoff/Ringscan_DAS_loop_realtime_dual.m 定义一致）：
//   分层声速边界 [mm]：空/0=单一声速，可多个（上限 8），逗号/空格分隔；
//   各层声速 [m/s]：元素数 = 边界数 + 1（无边界时允许 1 个及以上，取首个为单声速）。
bool parseSoundSpeedModel(const QString &radiiText, const QString &speedsText,
                          double radiiM[8], int *radiiCount,
                          double speedsMps[8], int *speedsCount,
                          QString *err)
{
    *radiiCount = 0;
    *speedsCount = 0;

    const QStringList radiiParts = radiiText.split(
        QRegularExpression("[,\\s，；;]+"), Qt::SkipEmptyParts);
    for (const QString &s : radiiParts) {
        bool ok = false;
        const double v = s.toDouble(&ok);
        if (!ok) {
            if (err) *err = QString("分层声速边界包含非数字: %1").arg(s);
            return false;
        }
        if (v > 0.0) {
            if (*radiiCount >= 8) {
                if (err) *err = "分层声速边界最多支持 8 个";
                return false;
            }
            radiiM[(*radiiCount)++] = v * 1e-3;
        }
    }
    for (int i = 1; i < *radiiCount; ++i) {
        if (radiiM[i] <= radiiM[i - 1]) {
            if (err) *err = "分层声速边界必须严格递增";
            return false;
        }
    }

    const QStringList speedsParts = speedsText.split(
        QRegularExpression("[,\\s，；;]+"), Qt::SkipEmptyParts);
    if (speedsParts.isEmpty()) {
        if (err) *err = "各层声速不能为空";
        return false;
    }
    for (const QString &s : speedsParts) {
        bool ok = false;
        const double v = s.toDouble(&ok);
        if (!ok) {
            if (err) *err = QString("各层声速包含非数字: %1").arg(s);
            return false;
        }
        if (v <= 0.0) {
            if (err) *err = "各层声速必须为正数";
            return false;
        }
        if (*speedsCount >= 8) {
            if (err) *err = "各层声速最多支持 8 个";
            return false;
        }
        speedsMps[(*speedsCount)++] = v;
    }
    if (*radiiCount > 0 && *speedsCount != *radiiCount + 1) {
        if (err) *err = QString("各层声速元素数应为边界数+1：当前边界 %1 个，声速 %2 个")
                            .arg(*radiiCount).arg(*speedsCount);
        return false;
    }
    return true;
}

}  // namespace

// 前端滤波持久化键：与 RingConfigDialog/Defaults group 内既有键同名风格（camelCase），
// 与既有键走同一套读写路径（paimageSettingsPath()+IniFormat 的同一 group）。
// 三处共用同一组字面量，避免 restoreDefaults / saveDefaults / 启动读取失步。
namespace {
const QString kKeyFeHpEnable   = QStringLiteral("feHpEnable");
const QString kKeyFeHpCutoff   = QStringLiteral("feHpCutoffMhz");
const QString kKeyFeHpOrder    = QStringLiteral("feHpOrder");
const QString kKeyFeLpEnable   = QStringLiteral("feLpEnable");
const QString kKeyFeLpCutoff   = QStringLiteral("feLpCutoffMhz");
const QString kKeyFeLpOrder    = QStringLiteral("feLpOrder");
}  // namespace

RingConfigDialog::RingConfigDialog(ImagingController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setWindowTitle("环形扫描参数设定");
    // 与线性参数设定窗口保持一致的窗口尺寸
    setMinimumSize(500, 450);
    buildUi();

    // “设为默认”保存的默认值在构造时即加载：下次启动程序即为上次设为默认的值；
    // 未保存过时回退到出厂硬编码默认值
    restoreDefaults();

    // 记忆上次关闭前的大小
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
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
    // 配准模式：勾选=各通道使用各自的重建半径（多扫描半径重建）
    // 拼接模式：仅配准模式勾选时可用；勾选=各通道只反投影到各自扇区
    m_chkRegister = new QCheckBox("配准模式（多扫描半径重建）");
    m_chkRegister->setToolTip("勾选：各通道按各自的重建半径重建；\n"
                              "不勾选：全部通道统一使用通道1下方输入的重建半径。");
    m_chkSplice = new QCheckBox("拼接模式");
    m_chkSplice->setToolTip("仅勾选配准模式时可用；勾选后各通道只反投影到各自扇区，\n"
                            "避免其他扇区通道在本扇区画出弧线伪影。");
    auto *regRow = new QWidget;
    auto *regLay = new QHBoxLayout(regRow);
    regLay->setContentsMargins(0, 0, 0, 0);
    regLay->addWidget(m_chkRegister);
    regLay->addWidget(m_chkSplice);
    regLay->addStretch();
    fCh->addRow("", regRow);
    m_spnSpliceBlend = new QDoubleSpinBox;
    m_spnSpliceBlend->setRange(0.0, 5.0);
    m_spnSpliceBlend->setSingleStep(0.1);
    m_spnSpliceBlend->setDecimals(1);
    m_spnSpliceBlend->setValue(1.5);
    m_spnSpliceBlend->setSuffix(" °");
    m_spnSpliceBlend->setToolTip("拼接模式边界过渡：0=硬切（v1）；\n"
                                 ">0 时相邻扇区边界按余弦羽化平滑过渡。");
    fCh->addRow("拼接羽化宽度", m_spnSpliceBlend);
    auto *chRow = new QHBoxLayout;
    for (int c = 0; c < 8; ++c) {
        // 命名与主程序时域/频域信号选项卡逐位对应（前端「卡1-通道A」⇒ 这里「1-A」）。
        // 换算只在 ChannelNaming 一处，避免两套命名各自演化后对不上现场通道。
        // 持久化键仍是 ch%1，与显示文本无关，既有配置无需迁移。
        m_chkCh[c] = new QCheckBox(ChannelNaming::channelShortName(c));
        m_chkCh[c]->setChecked(true);
        chRow->addWidget(m_chkCh[c]);
    }
    fCh->addRow("启用通道", chRow);
    // 每通道重建参数：重建半径 + 波长1/波长2 延时截断（同一行排列）。
    // 半径随配准模式/通道勾选联动；双波长延时始终可编辑（与通道是否勾选无关）。
    for (int c = 0; c < 8; ++c) {
        const QString chName = ChannelNaming::channelShortName(c);
        m_spnRadiusCh[c] = new QDoubleSpinBox;
        m_spnRadiusCh[c]->setRange(0.1, 50);
        m_spnRadiusCh[c]->setDecimals(4);
        m_spnRadiusCh[c]->setValue(6.57);
        m_spnRadiusCh[c]->setSuffix(" mm");
        m_spnRadiusCh[c]->setToolTip(
            QString("%1 重建半径（该通道传感器的实际扫描旋转半径）").arg(chName));

        for (int w = 0; w < 2; ++w) {
            m_spnSysDelayCh[c][w] = new QSpinBox;
            m_spnSysDelayCh[c][w]->setRange(1, 100000);
            m_spnSysDelayCh[c][w]->setValue(w == 0 ? 358 : 371);
            m_spnSysDelayCh[c][w]->setToolTip(
                QString("%1 波长%2 延时截断起点（1-based，采样点）。\n"
                        "用于消除全系统链路延迟导致的时域未对齐。")
                    .arg(chName).arg(w == 0 ? 1 : 2));
        }

        auto *row = new QWidget;
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->addWidget(m_spnRadiusCh[c]);
        rowLay->addWidget(new QLabel(QStringLiteral("波长1延时")));
        rowLay->addWidget(m_spnSysDelayCh[c][0]);
        rowLay->addWidget(new QLabel(QStringLiteral("波长2延时")));
        rowLay->addWidget(m_spnSysDelayCh[c][1]);
        rowLay->addStretch();
        fCh->addRow(QString("%1 重建半径").arg(chName), row);
    }
    // 使能联动：配准模式勾选时按通道勾选决定；未勾选时仅通道1可写（统一半径）
    auto syncRadiusEditable = [this]() {
        const bool reg = m_chkRegister->isChecked();
        for (int c = 0; c < 8; ++c) {
            const bool enabled = reg ? m_chkCh[c]->isChecked() : (c == 0);
            m_spnRadiusCh[c]->setEnabled(enabled);
        }
    };
    // 拼接模式仅在配准模式下可选；取消配准模式时同时取消拼接模式。
    // 羽化宽度仅在拼接模式下可编辑，否则置 0（0=硬切，服务端按半扇区宽 clamp）。
    auto syncSpliceEnabled = [this]() {
        const bool reg = m_chkRegister->isChecked();
        const bool splice = reg && m_chkSplice->isChecked();
        m_chkSplice->setEnabled(reg);
        if (!reg) {
            m_chkSplice->setChecked(false);
            m_spnSpliceBlend->setValue(0.0);
        }
        m_spnSpliceBlend->setEnabled(splice);
    };
    for (int c = 0; c < 8; ++c)
        connect(m_chkCh[c], &QCheckBox::toggled, this, syncRadiusEditable);
    connect(m_chkRegister, &QCheckBox::toggled, this, syncRadiusEditable);
    connect(m_chkRegister, &QCheckBox::toggled, this, syncSpliceEnabled);
    connect(m_chkSplice, &QCheckBox::toggled, this, syncSpliceEnabled);
    syncRadiusEditable();
    syncSpliceEnabled();
    m_spnTotalAlines = new QSpinBox; m_spnTotalAlines->setRange(2, 1000000);
    m_spnTotalAlines->setSingleStep(100); m_spnTotalAlines->setValue(8000);
    m_spnTotalAlines->setToolTip("单圈总A-line数（含双波长）。\n"
        "每通道每波长每圈A-line数 = 单圈总A-line数 / (启用通道数 × 2)");
    m_spnSectorStart = new QDoubleSpinBox; m_spnSectorStart->setRange(-360, 360);
    m_spnSectorStart->setDecimals(1); m_spnSectorStart->setValue(0.0);
    m_spnSectorStart->setToolTip("0° = 9 点钟方向，正值逆时针偏转。");
    fCh->addRow("单圈总A-line数(含双波长)", m_spnTotalAlines);
    fCh->addRow("首通道扇区起点(°,0=9点钟)", m_spnSectorStart);
    m_spnTimeoutReset = new QDoubleSpinBox; m_spnTimeoutReset->setRange(0, 3600);
    m_spnTimeoutReset->setDecimals(1); m_spnTimeoutReset->setValue(0.0);
    m_spnTimeoutReset->setSuffix(" s");
    m_spnTimeoutReset->setToolTip("0=关闭。真实采集中未采满整圈即停机时，超过该秒数无新触发"
                                  "后，下一触发判定为新一圈并重置旧图像（建议大于单圈内最大触发间隔）。");
    fCh->addRow("超时重置(0=关闭)", m_spnTimeoutReset);
    scanLayout->addWidget(grpCh);

    // ══ 物理轮次启动策略（Session B）══
    // 两个参数完全独立；持久化接入现有 RingConfigDialog/Defaults “设为默认/恢复默认”链，
    // Apply/OK 成功后经 roundPolicyChanged 转发给 NetworkController。
    auto *grpRound = new QGroupBox("物理轮次启动策略");
    auto *fRound = new QFormLayout(grpRound);
    m_spnStartupFilterTriggers = new QSpinBox;
    m_spnStartupFilterTriggers->setRange(0, 1000000);
    m_spnStartupFilterTriggers->setValue(1);
    m_spnStartupFilterTriggers->setToolTip(
        "每个物理轮次开头过滤的 distinct physical trigger 数（软件侧过滤）。\n"
        "单位是 distinct physical trigger，不是 packet/卡数：\n"
        "多卡看到同一 trigger 只消耗一个过滤名额。\n"
        "0 = 不过滤启动 trigger；1 = 兼容行为（过滤首枚）；N = 过滤前 N 枚。");
    fRound->addRow("启动过滤触发数", m_spnStartupFilterTriggers);
    m_chkDisableCountBoundary = new QCheckBox("禁用计数重置");
    m_chkDisableCountBoundary->setChecked(false);
    m_chkDisableCountBoundary->setToolTip(
        "未勾选：达到配置的逻辑触发数仍产生现有计数边界（CountBoundary）。\n"
        "勾选：达到配置逻辑触发数不产生固定计数边界，物理轮次继续，直到超时重置。\n"
        "超时仍是轮次边界；与“启动过滤触发数”完全独立。");
    fRound->addRow("", m_chkDisableCountBoundary);
    scanLayout->addWidget(grpRound);
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
    m_spnGridMm = new QDoubleSpinBox; m_spnGridMm->setRange(0.005, 5); m_spnGridMm->setDecimals(3); m_spnGridMm->setValue(0.01);
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
    fGrid->addRow("成像扇区角宽(°)", m_spnFovDeg);
    fGrid->addRow("成像扇区起始角(°,0=9点钟)", m_spnFovTheta0);
    fGrid->addRow("Apodization", m_cmbApod);
    fGrid->addRow("距离权重指数", m_spnDistWeight);
    fGrid->addRow("MinDistance(mm,0=自动)", m_spnMinDistMm);
    fGrid->addRow("", m_chkMaskOob);
    reconLayout->addWidget(grpGrid);

    auto *grpSos = new QGroupBox("声速模型（双声速，与 Ringscan_DAS_loop_realtime_dual 一致）");
    auto *fSos = new QFormLayout(grpSos);
    m_edtSosRadii = new QLineEdit("0");
    m_edtSosRadii->setPlaceholderText("例如 3；空或 0=单一声速，多个边界用逗号分隔");
    m_edtSosSpeeds = new QLineEdit("1490,1540");
    m_edtSosSpeeds->setPlaceholderText("例如 1490,1540；元素数=边界数+1");
    fSos->addRow("分层声速边界(mm)", m_edtSosRadii);
    fSos->addRow("各层声速(m/s)", m_edtSosSpeeds);
    reconLayout->addWidget(grpSos);

    // ══ 前端滤波（逐 A-line 零相位）══
    // 只作用于两条前端显示路径（时域/频域信号显示、实时成像）消费的 frontend
    // clone；保存与发布路径的数据不参与滤波。与「预处理」（重建侧）完全独立：
    // 本组参数走 MainWindow -> NetworkController 下发到 Frontend Preprocessing
    // 配置，不写入 RingReconCudaConfig / ImagingSvc / ring_recon。
    auto *grpFeFilter = new QGroupBox("前端滤波（逐 A-line 零相位）");
    auto *fFeFilter = new QFormLayout(grpFeFilter);
    // QDoubleSpinBox 只能表达闭区间；用 3 位小数下 (0, 125) 的可实现开区间
    // [0.001, 124.999] 表达「(0, 125) MHz，开区间」。frontend_filter::validate
    // 仍是权威校验（必须 > 0 且 < 125），任何绕过控件的下发都会被拒绝。
    auto makeCutoff = []() {
        auto *spn = new QDoubleSpinBox;
        spn->setRange(0.001, 124.999);
        spn->setDecimals(3);
        spn->setSuffix(" MHz");
        return spn;
    };
    auto makeOrder = []() {
        auto *spn = new QSpinBox;
        spn->setRange(frontend_filter::kMinOrder, frontend_filter::kMaxOrder);
        return spn;
    };
    m_chkFeHpEnable = new QCheckBox("启用高通");
    m_chkFeHpEnable->setChecked(true);
    m_spnFeHpCutoffMhz = makeCutoff();
    m_spnFeHpCutoffMhz->setValue(0.4);
    m_spnFeHpOrder = makeOrder();
    m_spnFeHpOrder->setValue(2);
    m_chkFeLpEnable = new QCheckBox("启用低通");
    m_chkFeLpEnable->setChecked(true);
    m_spnFeLpCutoffMhz = makeCutoff();
    m_spnFeLpCutoffMhz->setValue(60.0);
    m_spnFeLpOrder = makeOrder();
    m_spnFeLpOrder->setValue(2);
    m_spnFeHpCutoffMhz->setToolTip("高通截止频率（开区间 (0, 125) MHz）。");
    m_spnFeHpOrder->setToolTip("高通 Butterworth 阶数（1 ~ 8）。");
    m_spnFeLpCutoffMhz->setToolTip("低通截止频率（开区间 (0, 125) MHz）。");
    m_spnFeLpOrder->setToolTip("低通 Butterworth 阶数（1 ~ 8）。");
    fFeFilter->addRow("", m_chkFeHpEnable);
    fFeFilter->addRow("高通截止(MHz)", m_spnFeHpCutoffMhz);
    fFeFilter->addRow("高通阶数", m_spnFeHpOrder);
    fFeFilter->addRow("", m_chkFeLpEnable);
    fFeFilter->addRow("低通截止(MHz)", m_spnFeLpCutoffMhz);
    fFeFilter->addRow("低通阶数", m_spnFeLpOrder);
    // 开关未勾选时该路的截止与阶数控件置为 disabled，数值保留不清空。
    connect(m_chkFeHpEnable, &QCheckBox::toggled, this,
            [this](bool) { syncFrontendFilterControls(); });
    connect(m_chkFeLpEnable, &QCheckBox::toggled, this,
            [this](bool) { syncFrontendFilterControls(); });
    syncFrontendFilterControls();
    reconLayout->addWidget(grpFeFilter);

    auto *grpPre = new QGroupBox("预处理");
    auto *fPre = new QFormLayout(grpPre);
    m_spnMaskLen = new QSpinBox; m_spnMaskLen->setRange(0, 100000); m_spnMaskLen->setValue(300);
    m_chkDbr = new QCheckBox("扣除DBR强信号"); m_chkDbr->setChecked(true);
    m_chkDelayCut = new QCheckBox("延时截断"); m_chkDelayCut->setChecked(true);
    m_chkImpair = new QCheckBox("阈值削顶"); m_chkImpair->setChecked(false);
    m_spnImValue1 = new QDoubleSpinBox; m_spnImValue1->setRange(0, 1e7); m_spnImValue1->setValue(2000);
    m_spnImValue2 = new QDoubleSpinBox; m_spnImValue2->setRange(0, 1e7); m_spnImValue2->setValue(400);
    fPre->addRow("DBR mask长度", m_spnMaskLen);
    fPre->addRow("", m_chkDbr);
    fPre->addRow("", m_chkDelayCut);
    fPre->addRow("", m_chkImpair);
    fPre->addRow("imValue1(532nm)", m_spnImValue1);
    fPre->addRow("imValue2(1064nm)", m_spnImValue2);
    reconLayout->addWidget(grpPre);

    reconLayout->addStretch();
    auto *reconScroll = new QScrollArea;
    reconScroll->setWidgetResizable(true);
    reconScroll->setFrameShape(QFrame::NoFrame);
    reconScroll->setWidget(reconPage);
    tabs->addTab(reconScroll, "重建参数");

    // 底部按钮栏：与线性参数设定窗口一致（设为默认 | 恢复默认 | 应用 | 取消 | 确定）
    auto *btnRow = new QHBoxLayout;
    m_btnSaveDefault = new QPushButton("设为默认");
    m_btnRestore = new QPushButton("恢复默认");
    m_btnApply = new QPushButton("应用");
    m_btnCancel = new QPushButton("取消");
    m_btnOk = new QPushButton("确定");
    btnRow->addWidget(m_btnSaveDefault);
    btnRow->addWidget(m_btnRestore);
    btnRow->addStretch();
    btnRow->addWidget(m_btnApply);
    btnRow->addWidget(m_btnCancel);
    btnRow->addWidget(m_btnOk);
    root->addLayout(btnRow);

    connect(m_btnSaveDefault, &QPushButton::clicked, this, &RingConfigDialog::saveDefaults);
    connect(m_btnRestore, &QPushButton::clicked, this, &RingConfigDialog::restoreDefaults);
    connect(m_btnApply, &QPushButton::clicked, this, [this]() { applyConfig(); });
    connect(m_btnOk, &QPushButton::clicked, this, [this]() {
        if (applyConfig()) accept();
    });
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
}

// 采样率/采样深度只读：每次打开/启动成像前由 MainWindow 用当前线性采集参数刷新。
// 全链路真实采集采样率固定 250 MHz（MainWindow 已把注册表历史值统一为 4.0ns）。
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
    // 优先使用“设为默认”保存的参数；未保存过则回退到出厂硬编码默认值
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    // 物理轮次启动策略：与下方其他参数同一 Defaults group，经共享 helper 读写
    // （无历史 key 时回退出厂 1 / false）
    const RoundPolicySettings::Policy roundPolicy = RoundPolicySettings::load(s);
    m_spnStartupFilterTriggers->setValue(
        static_cast<int>(std::min<quint64>(roundPolicy.startupFilterTriggerCount,
                                           m_spnStartupFilterTriggers->maximum())));
    m_chkDisableCountBoundary->setChecked(roundPolicy.disableCountBoundary);
    s.beginGroup("RingConfigDialog/Defaults");
    auto val = [&s](const QString &k, const QVariant &dflt) {
        return s.contains(k) ? s.value(k) : dflt;
    };
    m_spnBlock->setValue(val("block", 200).toInt());
    m_chkShiftWL2->setChecked(val("shiftWL2", true).toBool());
    for (int c = 0; c < 8; ++c)
        m_chkCh[c]->setChecked(val(QString("ch%1").arg(c), true).toBool());
    // 配准模式与每通道重建半径（旧键 radiusMm 一次性迁移为通道1默认值）
    const double defaultRadiusMm = val("radiusMm", 6.57).toDouble();
    const bool regDefault = val("multiRadius", false).toBool();
    m_chkRegister->setChecked(regDefault);
    m_chkSplice->setChecked(regDefault && val("spliceMode", false).toBool());
    m_spnSpliceBlend->setValue(val("spliceBlendDeg", 1.5).toDouble());
    for (int c = 0; c < 8; ++c)
        m_spnRadiusCh[c]->setValue(val(QString("radiusCh%1").arg(c), defaultRadiusMm).toDouble());
    m_spnTotalAlines->setValue(val("totalAlines", 8000).toInt());
    m_spnSectorStart->setValue(val("sectorStart", 0.0).toDouble());
    m_spnTimeoutReset->setValue(val("timeoutResetSec", 0.0).toDouble());
    m_spnFovMm->setValue(val("fovMm", 36.0).toDouble());
    m_spnGridMm->setValue(val("gridMm", 0.01).toDouble());
    m_spnFovDeg->setValue(val("fovDeg", 360.0).toDouble());
    m_spnFovTheta0->setValue(val("fovTheta0", 0.0).toDouble());
    m_cmbApod->setCurrentIndex(val("apod", 0).toInt());
    m_spnDistWeight->setValue(val("distWeight", 1).toInt());
    m_spnMinDistMm->setValue(val("minDistMm", 0).toDouble());
    m_chkMaskOob->setChecked(val("maskOob", true).toBool());
    m_edtSosRadii->setText(val("sosRadii", "0").toString());
    m_edtSosSpeeds->setText(val("sosSpeeds", "1490,1540").toString());
    // 双波长延时截断已改为每通道独立：旧全局键 sysDelay1/sysDelay2 作为各通道迁移回退值
    const int legacyDelay1 = val("sysDelay1", 358).toInt();
    const int legacyDelay2 = val("sysDelay2", 371).toInt();
    for (int c = 0; c < 8; ++c) {
        m_spnSysDelayCh[c][0]->setValue(val(QString("sysDelayCh%1Wl1").arg(c), legacyDelay1).toInt());
        m_spnSysDelayCh[c][1]->setValue(val(QString("sysDelayCh%1Wl2").arg(c), legacyDelay2).toInt());
    }
    m_spnMaskLen->setValue(val("maskLen", 300).toInt());
    m_chkDbr->setChecked(val("dbr", true).toBool());
    m_chkDelayCut->setChecked(val("delayCut", true).toBool());
    m_chkImpair->setChecked(val("impair", false).toBool());
    m_spnImValue1->setValue(val("imValue1", 2000).toDouble());
    m_spnImValue2->setValue(val("imValue2", 400).toDouble());
    // 前端滤波：与上面的既有键同一 Defaults group、同一读写路径（同一 val()
    // 取值）；无历史键时回退出厂默认 true/0.4/2 + true/60.0/2。停用路的数值
    // 一并保留，不清空。
    m_chkFeHpEnable->setChecked(val(kKeyFeHpEnable, true).toBool());
    m_spnFeHpCutoffMhz->setValue(val(kKeyFeHpCutoff, 0.4).toDouble());
    m_spnFeHpOrder->setValue(val(kKeyFeHpOrder, 2).toInt());
    m_chkFeLpEnable->setChecked(val(kKeyFeLpEnable, true).toBool());
    m_spnFeLpCutoffMhz->setValue(val(kKeyFeLpCutoff, 60.0).toDouble());
    m_spnFeLpOrder->setValue(val(kKeyFeLpOrder, 2).toInt());
    s.endGroup();
    // 开关与截止/阶数控件的 disabled 联动随取值一并恢复
    syncFrontendFilterControls();
}

void RingConfigDialog::saveDefaults()
{
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    s.beginGroup("RingConfigDialog/Defaults");
    s.setValue("block", m_spnBlock->value());
    s.setValue("shiftWL2", m_chkShiftWL2->isChecked());
    for (int c = 0; c < 8; ++c)
        s.setValue(QString("ch%1").arg(c), m_chkCh[c]->isChecked());
    s.setValue("totalAlines", m_spnTotalAlines->value());
    s.setValue("sectorStart", m_spnSectorStart->value());
    s.setValue("timeoutResetSec", m_spnTimeoutReset->value());
    s.setValue("fovMm", m_spnFovMm->value());
    s.setValue("gridMm", m_spnGridMm->value());
    s.setValue("multiRadius", m_chkRegister->isChecked());
    s.setValue("spliceMode", m_chkSplice->isChecked());
    s.setValue("spliceBlendDeg", m_spnSpliceBlend->value());
    for (int c = 0; c < 8; ++c)
        s.setValue(QString("radiusCh%1").arg(c), m_spnRadiusCh[c]->value());
    s.setValue("fovDeg", m_spnFovDeg->value());
    s.setValue("fovTheta0", m_spnFovTheta0->value());
    s.setValue("apod", m_cmbApod->currentIndex());
    s.setValue("distWeight", m_spnDistWeight->value());
    s.setValue("minDistMm", m_spnMinDistMm->value());
    s.setValue("maskOob", m_chkMaskOob->isChecked());
    s.setValue("sosRadii", m_edtSosRadii->text());
    s.setValue("sosSpeeds", m_edtSosSpeeds->text());
    for (int c = 0; c < 8; ++c) {
        s.setValue(QString("sysDelayCh%1Wl1").arg(c), m_spnSysDelayCh[c][0]->value());
        s.setValue(QString("sysDelayCh%1Wl2").arg(c), m_spnSysDelayCh[c][1]->value());
    }
    s.setValue("maskLen", m_spnMaskLen->value());
    s.setValue("dbr", m_chkDbr->isChecked());
    s.setValue("delayCut", m_chkDelayCut->isChecked());
    s.setValue("impair", m_chkImpair->isChecked());
    s.setValue("imValue1", m_spnImValue1->value());
    s.setValue("imValue2", m_spnImValue2->value());
    // 前端滤波 6 个键与其他默认参数同一次“设为默认”落盘（同一 Defaults group）
    const frontend_filter::Config feFilter = frontendFilterConfig();
    s.setValue(kKeyFeHpEnable, feFilter.hpEnable);
    s.setValue(kKeyFeHpCutoff, feFilter.hpCutoffMhz);
    s.setValue(kKeyFeHpOrder, feFilter.hpOrder);
    s.setValue(kKeyFeLpEnable, feFilter.lpEnable);
    s.setValue(kKeyFeLpCutoff, feFilter.lpCutoffMhz);
    s.setValue(kKeyFeLpOrder, feFilter.lpOrder);
    s.endGroup();
    // 物理轮次启动策略与其他默认参数同一次“设为默认”落盘（同一 Defaults group，
    // 独立 key；helper 内部自行 sync）
    RoundPolicySettings::save(s, RoundPolicySettings::Policy{
        static_cast<quint64>(m_spnStartupFilterTriggers->value()),
        m_chkDisableCountBoundary->isChecked()});
    s.sync();
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
    cfg.timeoutResetSec = m_spnTimeoutReset->value();

    cfg.sampDepth = m_sampDepth;
    cfg.reconDepth = cfg.sampDepth;
    cfg.alinesPerBlock = m_spnBlock->value();
    cfg.shiftWL2 = m_chkShiftWL2->isChecked() ? 1 : 0;
    cfg.daqHz = m_daqHz;
    // 多扫描半径配准：勾选配准模式时各通道使用各自半径；否则统一使用通道1半径
    cfg.multiRadius = m_chkRegister->isChecked() ? 1 : 0;
    cfg.spliceMode = (cfg.multiRadius && m_chkSplice->isChecked()) ? 1 : 0;
    cfg.spliceBlendDeg = cfg.spliceMode ? m_spnSpliceBlend->value() : 0.0;
    for (int c = 0; c < 8; ++c)
        cfg.radiusPerChannel[c] = (cfg.multiRadius ? m_spnRadiusCh[c]->value()
                                                   : m_spnRadiusCh[0]->value()) * 1e-3;
    cfg.radius = cfg.radiusPerChannel[0];   // 兼容字段：默认半径 = 通道1半径

    // 双声速模型参数（与 Handoff/Ringscan_DAS_loop_realtime_dual.m 定义一致）
    QString ignoredErr;
    if (!parseSoundSpeedModel(m_edtSosRadii->text(), m_edtSosSpeeds->text(),
                              cfg.soundSpeedRadii, &cfg.soundSpeedRadiiCount,
                              cfg.soundSpeeds, &cfg.soundSpeedsCount,
                              &ignoredErr)) {
        cfg.soundSpeedRadiiCount = -1;   // applyConfig 会拦截并给出具体错误
    }

    cfg.fov = m_spnFovMm->value() * 1e-3;
    cfg.gridSize = m_spnGridMm->value() * 1e-3;
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
    // C 结构体中的 sysDelay[2] 保留为通道1值（兼容旧协议/离线工具）；
    // 实时链路的每通道双波长延时由 sysDelayPerChannel() 随配置一并下发。
    cfg.sysDelay[0] = m_spnSysDelayCh[0][0]->value();
    cfg.sysDelay[1] = m_spnSysDelayCh[0][1]->value();
    return cfg;
}

void RingConfigDialog::sysDelayPerChannel(int out[8][2]) const
{
    for (int c = 0; c < 8; ++c)
        for (int w = 0; w < 2; ++w)
            out[c][w] = m_spnSysDelayCh[c][w]->value();
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

    // 双声速模型参数校验（格式/递增/元素数关系）
    double radiiM[8] = {0};
    int radiiCount = 0;
    double speedsMps[8] = {0};
    int speedsCount = 0;
    QString sosErr;
    if (!parseSoundSpeedModel(m_edtSosRadii->text(), m_edtSosSpeeds->text(),
                              radiiM, &radiiCount, speedsMps, &speedsCount, &sosErr)) {
        QMessageBox::warning(this, "参数错误", sosErr);
        return false;
    }

    // 前端滤波参数校验（任务 3.4）。任一不满足则拒绝本次下发：配置保持原值不变，
    // 本次整体不下发（含重建配置），并在界面提示原因。
    {
        const frontend_filter::Config feFilter = frontendFilterConfig();
        const frontend_filter::Validation feResult = frontend_filter::validate(feFilter);
        if (feResult != frontend_filter::Validation::Ok) {
            QMessageBox::warning(this, "参数错误",
                                 QString::fromUtf8(
                                     frontend_filter::validationMessage(feResult)));
            return false;
        }
    }

    int sysDelayCh[8][2] = {{0}};
    sysDelayPerChannel(sysDelayCh);
    const RingReconCudaConfig applied = config();
    m_controller->configureRing(applied, sysDelayCh);
    // 应用成功后将物理轮次启动策略同步给当前 NetworkController（MainWindow 转发）
    emit roundPolicyChanged(startupFilterTriggerCount(), disableCountBoundary());
    // 前端滤波参数经独立接线下发到 Frontend Preprocessing 配置（MainWindow 转发），
    // 与重建配置的下发相互独立。
    emit frontendFilterChanged(frontendFilterConfig());
    // 每圈设计触发数 = 单圈总A-line数 / 启用通道数（前端刷新闸门阈值的唯一来源）。
    // 改参数即下发，不等实时成像开启、不等 svcReady。
    const int logicalTriggers =
        ringLogicalTriggersPerRound(applied.alinesPerFrame, applied.enabledChannelCount);
    if (logicalTriggers > 0)
        emit ringRoundTriggersChanged(static_cast<quint64>(logicalTriggers));
    return true;
}

quint64 RingConfigDialog::startupFilterTriggerCount() const
{
    return static_cast<quint64>(m_spnStartupFilterTriggers->value());
}

bool RingConfigDialog::disableCountBoundary() const
{
    return m_chkDisableCountBoundary->isChecked();
}

frontend_filter::Config RingConfigDialog::frontendFilterConfig() const
{
    frontend_filter::Config cfg;
    cfg.hpEnable    = m_chkFeHpEnable->isChecked();
    cfg.hpCutoffMhz = m_spnFeHpCutoffMhz->value();
    cfg.hpOrder     = m_spnFeHpOrder->value();
    cfg.lpEnable    = m_chkFeLpEnable->isChecked();
    cfg.lpCutoffMhz = m_spnFeLpCutoffMhz->value();
    cfg.lpOrder     = m_spnFeLpOrder->value();
    return cfg;
}

void RingConfigDialog::syncFrontendFilterControls()
{
    const bool hp = m_chkFeHpEnable->isChecked();
    m_spnFeHpCutoffMhz->setEnabled(hp);
    m_spnFeHpOrder->setEnabled(hp);
    const bool lp = m_chkFeLpEnable->isChecked();
    m_spnFeLpCutoffMhz->setEnabled(lp);
    m_spnFeLpOrder->setEnabled(lp);
}

frontend_filter::Config RingConfigDialog::loadFrontendFilterDefaults()
{
    frontend_filter::Config cfg;   // 出厂默认：true/0.4/2 + true/60.0/2
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("RingConfigDialog/Defaults"));
    auto val = [&s](const QString &k, const QVariant &dflt) {
        return s.contains(k) ? s.value(k) : dflt;
    };
    cfg.hpEnable    = val(kKeyFeHpEnable, true).toBool();
    cfg.hpCutoffMhz = val(kKeyFeHpCutoff, 0.4).toDouble();
    cfg.hpOrder     = val(kKeyFeHpOrder, 2).toInt();
    cfg.lpEnable    = val(kKeyFeLpEnable, true).toBool();
    cfg.lpCutoffMhz = val(kKeyFeLpCutoff, 60.0).toDouble();
    cfg.lpOrder     = val(kKeyFeLpOrder, 2).toInt();
    s.endGroup();
    return cfg;
}

void RingConfigDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    const QSize saved = s.value("RingConfigDialog/Size").toSize();
    if (saved.isValid() && saved.width() >= 500 && saved.height() >= 450)
        resize(saved);
}

void RingConfigDialog::hideEvent(QHideEvent *event)
{
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    s.setValue("RingConfigDialog/Size", size());
    s.sync();
    QDialog::hideEvent(event);
}
