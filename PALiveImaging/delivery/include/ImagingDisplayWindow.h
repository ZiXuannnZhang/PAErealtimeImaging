#pragma once

#include <QWidget>
#include <QVector>
#include "qcustomplot.h"

class QLabel;

// 实时成像独立窗口（线性/环形共用）
// 线性扫描：单幅重建图像；环形扫描：532nm/1064nm 双波长并排显示。
// 各图继承主窗口颜色图能力与右键菜单（保存/复制/色条范围/自适应/还原视图）。
class ImagingDisplayWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ImagingDisplayWindow(QWidget *parent = nullptr);

    // 线性扫描：显示一帧重建数据（列主序，nx*ny）
    void showImage(const QVector<float> &frameData, int nx, int ny, int seq);

    // 环形扫描：显示双波长重建数据（各 nx*ny，列主序）
    void showRingImage(const QVector<float> &wl1, const QVector<float> &wl2,
                       int nx, int ny, int seq);

private:
    void setupPlot(QCustomPlot *plot, QCPColorMap *&colorMap, QCPColorScale *&colorScale,
                   QCPRange &range, const QString &title);

    QCustomPlot    *m_plot = nullptr;
    QCustomPlot    *m_plot2 = nullptr;
    QCPColorMap    *m_colorMap = nullptr;
    QCPColorMap    *m_colorMap2 = nullptr;
    QCPColorScale  *m_colorScale = nullptr;
    QCPColorScale  *m_colorScale2 = nullptr;
    QLabel         *m_lblStatus = nullptr;
    QLabel         *m_lblTitle1 = nullptr;
    QLabel         *m_lblTitle2 = nullptr;
    QCPRange        m_pixelColorRange{0, 500};
    QCPRange        m_pixelColorRange2{0, 500};
    bool            m_firstFrame = true;
    bool            m_ringMode = false;
};