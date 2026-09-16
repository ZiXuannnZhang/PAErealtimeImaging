#pragma once

#include <QDialog>
#include "Constants.h"
#include "ring_recon_cuda.h"

class ImagingController;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
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
    void sysDelayPerChannel(int out[8][2]) const;   // 每通道双波长延时截断（[通道][波长]）
    bool applyConfig();                   // 校验并下发配置（应用/确定共用）

protected:
    void showEvent(QShowEvent *event) override;   // 每次显示时套用记忆的大小
    void hideEvent(QHideEvent *event) override;   // 记忆上次关闭前的大小

private:
    void buildUi();
    void restoreDefaults();               // 恢复默认参数（底部“恢复默认”按钮）
    void saveDefaults();                  // 将当前参数保存为默认（底部“设为默认”按钮）

    ImagingController *m_controller;

    int    m_sampDepth = 4000;   // 采样深度只读值（点/A-line，与线性采集一致）
    double m_daqHz = FPGA_ADC_FREQ_HZ;   // 采样率只读值（Hz，真实采集固定 250 MHz）

    // 扫描参数
    QSpinBox        *m_spnBlock;
    QLabel          *m_lblSampDepth;      // 只读（来源：采集时间/采样间隔，与线性一致）
    QCheckBox       *m_chkShiftWL2;
    QLabel          *m_lblDaqHz;          // 只读（来源：采样间隔，与线性一致）
    QCheckBox       *m_chkCh[8];
    QCheckBox       *m_chkRegister;      // 配准模式：多扫描半径重建（勾选=各通道各自半径）
    QCheckBox       *m_chkSplice;        // 拼接模式：各通道只反投影到各自扇区（仅配准模式可用）
    QDoubleSpinBox  *m_spnSpliceBlend;   // 拼接羽化宽度（°，0=硬切；仅拼接模式可用）
    QDoubleSpinBox  *m_spnRadiusCh[8];   // 每通道重建半径(mm)，勾选通道生效可写
    QSpinBox        *m_spnSysDelayCh[8][2]; // 每通道双波长延时截断：[ch][0]=wl1(532nm)，[ch][1]=wl2(1064nm)
    QSpinBox        *m_spnTotalAlines;    // 单圈总A线数（含双波长）
    QDoubleSpinBox  *m_spnSectorStart;
    QDoubleSpinBox  *m_spnTimeoutReset;   // 超时重置（秒，0=关闭）
    // 重建参数
    QDoubleSpinBox  *m_spnFovMm;
    QDoubleSpinBox  *m_spnGridMm;
    QDoubleSpinBox  *m_spnFovDeg;
    QDoubleSpinBox  *m_spnFovTheta0;
    QComboBox       *m_cmbApod;
    QDoubleSpinBox  *m_spnDistWeight;
    QDoubleSpinBox  *m_spnMinDistMm;
    QCheckBox       *m_chkMaskOob;
    QLineEdit       *m_edtSosRadii;    // 分层声速边界 [mm]（空/0=单一声速，多个用逗号分隔）
    QLineEdit       *m_edtSosSpeeds;   // 各层声速 [m/s]（元素数=边界数+1，逗号分隔）
    QSpinBox        *m_spnMaskLen;
    QCheckBox       *m_chkDbr;
    QCheckBox       *m_chkDelayCut;
    QCheckBox       *m_chkImpair;
    QDoubleSpinBox  *m_spnImValue1;
    QDoubleSpinBox  *m_spnImValue2;

    QPushButton     *m_btnSaveDefault;
    QPushButton     *m_btnRestore;
    QPushButton     *m_btnApply;
    QPushButton     *m_btnOk;
    QPushButton     *m_btnCancel;
};
