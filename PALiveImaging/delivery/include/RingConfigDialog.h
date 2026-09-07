#pragma once

#include <QDialog>
#include "ring_recon_cuda.h"

class ImagingController;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// 环形扫描参数设定窗口（工作三：替代原环形扫描控制台）
// “扫描参数”选项卡：采集与数据格式（采样率/采样深度只读，来源与线性一致）
//                   + 通道勾选与扇区（单圈总A线数 → 每通道每波长换算）
// “重建参数”选项卡：成像网格与DAS + 声速模型 + 预处理 + 显示
// 窗口尺寸与底部按钮栏（恢复默认/应用/取消/确定）与线性参数设定窗口保持一致。
class RingConfigDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RingConfigDialog(ImagingController *controller, QWidget *parent = nullptr);

    // 同步线性/采集侧参数：采样率与采样深度只读显示（来源与线性扫描一致）
    void setAcquisitionParams(double sampleIntervalNs, int acqTimeNs);

    RingReconCudaConfig config() const;   // 当前控件值（含换算后的每通道每波长每圈A线数）
    bool applyConfig();                   // 校验并下发配置（应用/确定共用）

protected:
    void showEvent(QShowEvent *event) override;   // 每次显示时套用记忆的大小
    void hideEvent(QHideEvent *event) override;   // 记忆上次关闭前的大小

private:
    void buildUi();
    void restoreDefaults();               // 恢复默认参数（底部“恢复默认”按钮）

    ImagingController *m_controller;

    int    m_sampDepth = 4000;   // 采样深度只读值（点/A-line，与线性采集一致）
    double m_daqHz = 250e6;      // 采样率只读值（Hz，与线性采集一致）

    // 扫描参数
    QSpinBox        *m_spnBlock;
    QLabel          *m_lblSampDepth;      // 只读（来源：采集时间/采样间隔，与线性一致）
    QCheckBox       *m_chkShiftWL2;
    QLabel          *m_lblDaqHz;          // 只读（来源：采样间隔，与线性一致）
    QCheckBox       *m_chkCh[8];
    QSpinBox        *m_spnTotalAlines;    // 单圈总A线数（含双波长）
    QDoubleSpinBox  *m_spnSectorStart;
    // 重建参数
    QDoubleSpinBox  *m_spnFovMm;
    QDoubleSpinBox  *m_spnGridMm;
    QDoubleSpinBox  *m_spnRadiusMm;
    QDoubleSpinBox  *m_spnFovDeg;
    QDoubleSpinBox  *m_spnFovTheta0;
    QComboBox       *m_cmbApod;
    QDoubleSpinBox  *m_spnDistWeight;
    QDoubleSpinBox  *m_spnMinDistMm;
    QCheckBox       *m_chkMaskOob;
    QDoubleSpinBox  *m_spnSos1;
    QDoubleSpinBox  *m_spnSos2;
    QSpinBox        *m_spnLayerCount;
    QDoubleSpinBox  *m_spnLayerRadiusMm;
    QSpinBox        *m_spnSysDelay1;
    QSpinBox        *m_spnSysDelay2;
    QSpinBox        *m_spnMaskLen;
    QCheckBox       *m_chkDbr;
    QCheckBox       *m_chkDelayCut;
    QCheckBox       *m_chkImpair;
    QDoubleSpinBox  *m_spnImValue1;
    QDoubleSpinBox  *m_spnImValue2;
    QDoubleSpinBox  *m_spnCRecon;

    QPushButton     *m_btnRestore;
    QPushButton     *m_btnApply;
    QPushButton     *m_btnOk;
    QPushButton     *m_btnCancel;
};