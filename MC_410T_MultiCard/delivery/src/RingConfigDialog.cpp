#include "PaimageAcquisition/SettingsPath.h"
#include "RingConfigDialog.h"
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
        m_chkCh[c] = new QCheckBox(QString("通道%1").arg(c + 1));
        m_chkCh[c]->setChecked(true);
        chRow->addWidget(m_chkCh[c]);
    }
    fCh->addRow("启用通道", chRow);
    // 每通道重建参数：重建半径 + 波长1/波长2 延时截断（同一行排列）。
    // 半径随配准模式/通道勾选联动；双波长延时始终可编辑（与通道是否勾选无关）。
    for (int c = 0; c < 8; ++c) {
        m_spnRadiusCh[c] = new QDoubleSpinBox;
        m_spnRadiusCh[c]->setRange(0.1, 50);
        m_spnRadiusCh[c]->setDecimals(4);
        m_spnRadiusCh[c]->setValue(6.57);
        m_spnRadiusCh[c]->setSuffix(" mm");
        m_spnRadiusCh[c]->setToolTip(
            QString("通道%1 重建半径（该通道传感器的实际扫描旋转半径）").arg(c + 1));

        for (int w = 0; w < 2; ++w) {
            m_spnSysDelayCh[c][w] = new QSpinBox;
            m_spnSysDelayCh[c][w]->setRange(1, 100000);
            m_spnSysDelayCh[c][w]->setValue(w == 0 ? 358 : 371);
            m_spnSysDelayCh[c][w]->setToolTip(
                QString("通道%1 波长%2 延时截断起点（1-based，采样点）。\n"
                        "用于消除全系统链路延迟导致的时域未对齐。")
                    .arg(c + 1).arg(w == 0 ? 1 : 2));
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
        fCh->addRow(QString("通道%1重建半径").arg(c + 1), row);
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

    // ══ 零相位滤波（阶段 B1）：高通先于低通；两开关独立 ══
    auto *grpZp = new QGroupBox("零相位滤波（A-line 预处理，高通先于低通）");
    auto *fZp = new QFormLayout(grpZp);
    m_chkZpHp = new QCheckBox("高通零相位滤波"); m_chkZpHp->setChecked(false);
    m_chkZpHp->setToolTip("启用后在延时截断之后对每根完整 A-line 做零相位高通滤波。\n"
                          "截止频率为单程 −3dB 点；前后向各滤一次，该频率处约 −6dB。");
    m_spnZpHpMhz = new QDoubleSpinBox;
    m_spnZpHpMhz->setRange(0.0001, 500); m_spnZpHpMhz->setDecimals(4);
    m_spnZpHpMhz->setValue(0.4); m_spnZpHpMhz->setSuffix(" MHz");
    m_spnZpHpOrder = new QSpinBox;
    m_spnZpHpOrder->setRange(1, 8); m_spnZpHpOrder->setValue(4);
    auto *hpRow = new QWidget;
    auto *hpLay = new QHBoxLayout(hpRow);
    hpLay->setContentsMargins(0, 0, 0, 0);
    hpLay->addWidget(m_chkZpHp);
    hpLay->addWidget(new QLabel("截止"));
    hpLay->addWidget(m_spnZpHpMhz);
    hpLay->addWidget(new QLabel("阶数"));
    hpLay->addWidget(m_spnZpHpOrder);
    hpLay->addStretch();
    fZp->addRow("", hpRow);

    m_chkZpLp = new QCheckBox("低通零相位滤波"); m_chkZpLp->setChecked(false);
    m_chkZpLp->setToolTip("启用后在延时截断之后对每根完整 A-line 做零相位低通滤波（在高通之后）。\n"
                          "截止频率为单程 −3dB 点；前后向各滤一次，该频率处约 −6dB。");
    m_spnZpLpMhz = new QDoubleSpinBox;
    m_spnZpLpMhz->setRange(0.0001, 500); m_spnZpLpMhz->setDecimals(4);
    m_spnZpLpMhz->setValue(40); m_spnZpLpMhz->setSuffix(" MHz");
    m_spnZpLpOrder = new QSpinBox;
    m_spnZpLpOrder->setRange(1, 8); m_spnZpLpOrder->setValue(4);
    auto *lpRow = new QWidget;
    auto *lpLay = new QHBoxLayout(lpRow);
    lpLay->setContentsMargins(0, 0, 0, 0);
    lpLay->addWidget(m_chkZpLp);
    lpLay->addWidget(new QLabel("截止"));
    lpLay->addWidget(m_spnZpLpMhz);
    lpLay->addWidget(new QLabel("阶数"));
    lpLay->addWidget(m_spnZpLpOrder);
    lpLay->addStretch();
    fZp->addRow("", lpRow);

    m_lblZpRange = new QLabel;
    fZp->addRow("有效范围", m_lblZpRange);
    reconLayout->addWidget(grpZp);

    // 开关联动：关闭时禁用该行数值输入并保留值（再次启用可继续编辑）
    auto syncZpEnabled = [this]() {
        m_spnZpHpMhz->setEnabled(m_chkZpHp->isChecked());
        m_spnZpHpOrder->setEnabled(m_chkZpHp->isChecked());
        m_spnZpLpMhz->setEnabled(m_chkZpLp->isChecked());
        m_spnZpLpOrder->setEnabled(m_chkZpLp->isChecked());
    };
    connect(m_chkZpHp, &QCheckBox::toggled, this, syncZpEnabled);
    connect(m_chkZpLp, &QCheckBox::toggled, this, syncZpEnabled);
    syncZpEnabled();

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
    // 零相位滤波（B1 整改 R2）：动态刷新合法频段提示，但不改 spin 上限。
    // 旧实现 setMaximum(fs/2) 会在采样率降低时静默压缩当前值（如 40MHz
    // 在 fs=50MHz 时被 QDoubleSpinBox 改成 25MHz），恢复采样率后原值丢失，
    // 与"不静默改保存值"矛盾。控件保持稳定可编辑范围（0.0001–500 MHz），
    // 实际合法性（0 < fc < fs/2）由启用/应用时的校验提示（applyConfig +
    // 服务端），越界值原样保留。
    const double halfMhz = m_daqHz * 0.5 / 1e6;
    m_lblZpRange->setText(QString("当前采样率 %1 Hz；启用滤波的截止必须 ∈ (0, %2 MHz)，"
                                  "双开时高通 < 低通；越界值将在应用时被拒绝")
                              .arg(m_daqHz, 0, 'f', 0).arg(halfMhz, 0, 'f', 1));
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
    // 零相位滤波：旧配置缺键默认全关（出厂 0.4MHz/40MHz、单程 4 阶）
    m_chkZpHp->setChecked(val("zpHp", false).toBool());
    m_spnZpHpMhz->setValue(val("zpHpMhz", 0.4).toDouble());
    m_spnZpHpOrder->setValue(val("zpHpOrder", 4).toInt());
    m_chkZpLp->setChecked(val("zpLp", false).toBool());
    m_spnZpLpMhz->setValue(val("zpLpMhz", 40).toDouble());
    m_spnZpLpOrder->setValue(val("zpLpOrder", 4).toInt());
    s.endGroup();
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
    // 零相位滤波：关闭时也保存当前值（保留输入值，再次启用无需重填）
    s.setValue("zpHp", m_chkZpHp->isChecked());
    s.setValue("zpHpMhz", m_spnZpHpMhz->value());
    s.setValue("zpHpOrder", m_spnZpHpOrder->value());
    s.setValue("zpLp", m_chkZpLp->isChecked());
    s.setValue("zpLpMhz", m_spnZpLpMhz->value());
    s.setValue("zpLpOrder", m_spnZpLpOrder->value());
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
    // 零相位滤波：UI MHz → Hz 只在此处转换一次（服务端全按 Hz 校验/设计）。
    // 字段沿用基线 reserved 命名：filterLow/wLow/n1 = 高通，filterHigh/wHigh/n2 = 低通。
    cfg.filterLow = m_chkZpHp->isChecked() ? 1 : 0;
    cfg.wLow = m_spnZpHpMhz->value() * 1e6;
    cfg.n1 = m_spnZpHpOrder->value();
    cfg.filterHigh = m_chkZpLp->isChecked() ? 1 : 0;
    cfg.wHigh = m_spnZpLpMhz->value() * 1e6;
    cfg.n2 = m_spnZpLpOrder->value();
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

    // ══ 零相位滤波前置校验（阶段 B1；服务端会重复校验，此处尽早给出 UI 提示）══
    // 只校验"启用"的滤波器；全关时不因保留的无效截止值阻塞旧路径。
    // 使用当前真实采样率 m_daqHz（setAcquisitionParams 刷新），非固定 250MHz。
    if (m_chkZpHp->isChecked() || m_chkZpLp->isChecked()) {
        const double halfFs = m_daqHz * 0.5;
        const double hpHz = m_spnZpHpMhz->value() * 1e6;
        const double lpHz = m_spnZpLpMhz->value() * 1e6;
        if (m_chkZpHp->isChecked() && !(hpHz > 0.0 && hpHz < halfFs)) {
            QMessageBox::warning(this, "参数错误",
                QString("高通截止 %1 MHz 超出有效范围 (0, %2 MHz)。\n"
                        "截止为单程 −3dB 点；当前采样率 %3 Hz。")
                    .arg(m_spnZpHpMhz->value(), 0, 'f', 4)
                    .arg(halfFs / 1e6, 0, 'f', 1)
                    .arg(m_daqHz, 0, 'f', 0));
            return false;
        }
        if (m_chkZpLp->isChecked() && !(lpHz > 0.0 && lpHz < halfFs)) {
            QMessageBox::warning(this, "参数错误",
                QString("低通截止 %1 MHz 超出有效范围 (0, %2 MHz)。\n"
                        "截止为单程 −3dB 点；当前采样率 %3 Hz。")
                    .arg(m_spnZpLpMhz->value(), 0, 'f', 4)
                    .arg(halfFs / 1e6, 0, 'f', 1)
                    .arg(m_daqHz, 0, 'f', 0));
            return false;
        }
        if (m_chkZpHp->isChecked() && m_chkZpLp->isChecked() && !(hpHz < lpHz)) {
            QMessageBox::warning(this, "参数错误",
                QString("高通截止 %1 MHz 必须低于低通截止 %2 MHz。")
                    .arg(m_spnZpHpMhz->value(), 0, 'f', 4)
                    .arg(m_spnZpLpMhz->value(), 0, 'f', 4));
            return false;
        }
        // C2 规则（与阶段 A filterDbrConfigCheck 一致）：逐启用通道/波长
        // 校验实际置零长度 E 与 D、延时裁剪组合；一条不满足即拒绝本组配置。
        // E = min(maskLength + 该波长 extra, sampDepth)，DBR 关闭为 0。
        for (int c = 0; c < 8; ++c) {
            if (!m_chkCh[c]->isChecked()) continue;
            for (int w = 0; w < 2; ++w) {
                const int D = m_spnSysDelayCh[c][w]->value();
                const int extra = (w == 1)
                    ? m_spnSysDelayCh[c][1]->value() - m_spnSysDelayCh[c][0]->value()
                    : 0;
                const int E = m_chkDbr->isChecked()
                    ? std::min(std::max(0, m_spnMaskLen->value() + extra), m_sampDepth)
                    : 0;
                const bool delayCut = m_chkDelayCut->isChecked();
                if (E <= 0) continue;   // E=0 允许（含 DBR 关）
                if (!delayCut) {
                    QMessageBox::warning(this, "参数错误",
                        QString("零相位滤波与未裁剪的 DBR 置零前缀不兼容"
                                "（通道%1 波长%2：实际置零 %3 样本、延时截断未开启）。\n"
                                "本版不支持该组合（不是数学禁忌）；"
                                "请启用延时截断或调小 DBR mask长度。")
                            .arg(c + 1).arg(w + 1).arg(E));
                    return false;
                }
                if (E >= D) {
                    QMessageBox::warning(this, "参数错误",
                        QString("DBR 置零末端必须早于延时裁剪起点"
                                "（通道%1 波长%2：实际置零 %3 ≥ 延时截断起点 %4）。\n"
                                "请调小 DBR mask长度或调整该通道延时截断。")
                            .arg(c + 1).arg(w + 1).arg(E).arg(D));
                    return false;
                }
            }
        }
        // 短线校验（B1 整改 R1 UI 侧）：与服务端同一规则按启用通道×波长
        // 尽早提示。有效线长 = delayCut 开 ? sampDepth−D+1 : sampDepth，
        // 必须严格大于 3×最大启用阶数。
        {
            const int need = 3 * std::max(m_chkZpHp->isChecked() ? m_spnZpHpOrder->value() : 0,
                                          m_chkZpLp->isChecked() ? m_spnZpLpOrder->value() : 0);
            for (int c = 0; c < 8; ++c) {
                if (!m_chkCh[c]->isChecked()) continue;
                for (int w = 0; w < 2; ++w) {
                    const int D = m_spnSysDelayCh[c][w]->value();
                    const int outRows = m_chkDelayCut->isChecked()
                        ? (m_sampDepth - D + 1) : m_sampDepth;
                    if (outRows <= need) {
                        QMessageBox::warning(this, "参数错误",
                            QString("通道%1/波长%2：有效线长 %3 必须大于延拓长度 %4"
                                    "（3×最大阶数；延时截断起点 D=%5，采样深度 %6）。\n"
                                    "请调小阶数或该通道延时截断起点。")
                                .arg(c + 1).arg(w + 1).arg(outRows).arg(need)
                                .arg(D).arg(m_sampDepth));
                        return false;
                    }
                }
            }
        }
    }

    int sysDelayCh[8][2] = {{0}};
    sysDelayPerChannel(sysDelayCh);
    // B1 整改 R3：configureRing 在服务忙（运行/启停过渡）时拒绝应用并
    // 返回 false——UI 不当作成功，不发出轮次策略变更，不半生效。
    if (!m_controller->configureRing(config(), sysDelayCh)) {
        QMessageBox::warning(this, "无法应用",
            "成像服务正在运行或启动/停止中，环形参数未应用。\n"
            "请先停止实时成像，再修改并应用参数。");
        return false;
    }
    // 应用成功后将物理轮次启动策略同步给当前 NetworkController（MainWindow 转发）
    emit roundPolicyChanged(startupFilterTriggerCount(), disableCountBoundary());
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
