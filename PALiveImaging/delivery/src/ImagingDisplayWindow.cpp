#include "ImagingDisplayWindow.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QDialog>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <limits>

ImagingDisplayWindow::ImagingDisplayWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("实时成像");
    resize(960, 640);

    auto *root = new QVBoxLayout(this);

    auto *imgRow = new QHBoxLayout;
    auto *col1 = new QVBoxLayout;
    m_lblTitle1 = new QLabel("532nm", this);
    m_lblTitle1->setAlignment(Qt::AlignCenter);
    m_plot = new QCustomPlot(this);
    m_plot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_plot->setMinimumSize(0, 0);
    col1->addWidget(m_lblTitle1);
    col1->addWidget(m_plot, 1);
    imgRow->addLayout(col1, 1);

    auto *col2 = new QVBoxLayout;
    m_lblTitle2 = new QLabel("1064nm", this);
    m_lblTitle2->setAlignment(Qt::AlignCenter);
    m_plot2 = new QCustomPlot(this);
    m_plot2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_plot2->setMinimumSize(0, 0);
    col2->addWidget(m_lblTitle2);
    col2->addWidget(m_plot2, 1);
    imgRow->addLayout(col2, 1);
    root->addLayout(imgRow, 1);

    m_lblStatus = new QLabel("就绪", this);
    m_lblStatus->setStyleSheet("color: #888888; background: transparent; padding: 2px 8px;");
    root->addWidget(m_lblStatus);

    setupPlot(m_plot, m_colorMap, m_colorScale, m_pixelColorRange, "线性扫描");
    setupPlot(m_plot2, m_colorMap2, m_colorScale2, m_pixelColorRange2, "环形扫描 1064nm");

    m_plot2->setVisible(false);
    m_lblTitle2->setVisible(false);
}

void ImagingDisplayWindow::setupPlot(QCustomPlot *plot, QCPColorMap *&colorMap,
                                     QCPColorScale *&colorScale, QCPRange &range,
                                     const QString &title)
{
    plot->setOpenGl(false);
    plot->setNotAntialiasedElements(QCP::aeAll);
    plot->setBackground(QBrush(QColor(30, 30, 30)));
    plot->axisRect()->setBackground(QBrush(QColor(38, 38, 38)));

    colorMap = new QCPColorMap(plot->xAxis, plot->yAxis);
    colorMap->data()->setSize(800, 500);
    colorMap->data()->setRange(QCPRange(0, 799), QCPRange(0, 499));
    colorMap->setInterpolate(true);
    colorMap->setTightBoundary(false);
    {
        QCPColorGradient grayGrad;
        grayGrad.setColorStopAt(0.0, QColor(0, 0, 0));
        grayGrad.setColorStopAt(1.0, QColor(255, 255, 255));
        colorMap->setGradient(grayGrad);
    }
    colorMap->setDataRange(range);

    colorScale = new QCPColorScale(plot);
    plot->plotLayout()->addElement(0, 1, colorScale);
    colorScale->setType(QCPAxis::atRight);
    colorScale->setLabel("重建幅值 (dB)");
    colorScale->axis()->setLabelColor(QColor(180, 180, 180));
    colorScale->axis()->setTickLabelColor(QColor(160, 160, 160));
    colorScale->axis()->setBasePen(QPen(QColor(80, 80, 80)));
    colorMap->setColorScale(colorScale);

    plot->xAxis->setLabel("像素 X");
    plot->yAxis->setLabel("像素 Y");
    plot->xAxis->setLabelColor(QColor(180, 180, 180));
    plot->yAxis->setLabelColor(QColor(180, 180, 180));
    plot->xAxis->setTickLabelColor(QColor(160, 160, 160));
    plot->yAxis->setTickLabelColor(QColor(160, 160, 160));
    plot->xAxis->setBasePen(QPen(QColor(80, 80, 80)));
    plot->yAxis->setBasePen(QPen(QColor(80, 80, 80)));
    plot->xAxis->setRange(0, 799);
    plot->yAxis->setRange(0, 499);
    plot->yAxis->setRangeReversed(true);
    plot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
    plot->yAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // 右键菜单：与主窗口颜色图一致
    plot->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(plot, &QWidget::customContextMenuRequested, this, [this, plot, colorMap, colorScale, &range, title](const QPoint &pos) {
        QMenu menu(plot);
        QAction *actSave = menu.addAction("保存图片...");
        menu.addSeparator();
        QAction *actCopyClip = menu.addAction("复制到剪贴板");
        menu.addSeparator();
        QAction *actSetColorRange = menu.addAction("设置色条范围...");
        QAction *actAutoRange = menu.addAction("自适应数据范围");
        QAction *actResetView = menu.addAction("还原视图");
        QAction *selected = menu.exec(plot->mapToGlobal(pos));
        if (selected == actSave) {
            QString fn = QFileDialog::getSaveFileName(plot, "保存图片",
                title + ".png", "PNG (*.png);;BMP (*.bmp);;JPG (*.jpg)");
            if (!fn.isEmpty()) {
                if (fn.endsWith(".png", Qt::CaseInsensitive)) plot->savePng(fn);
                else if (fn.endsWith(".bmp", Qt::CaseInsensitive)) plot->saveBmp(fn);
                else if (fn.endsWith(".jpg", Qt::CaseInsensitive)) plot->saveJpg(fn);
            }
        } else if (selected == actCopyClip) {
            QApplication::clipboard()->setPixmap(plot->toPixmap());
        } else if (selected == actSetColorRange) {
            QDialog dlg(plot);
            dlg.setWindowTitle("设置色条范围");
            QFormLayout *fl = new QFormLayout(&dlg);
            QDoubleSpinBox *minSp = new QDoubleSpinBox(); minSp->setRange(-1e9, 1e9);
            minSp->setValue(colorMap->dataRange().lower);
            QDoubleSpinBox *maxSp = new QDoubleSpinBox(); maxSp->setRange(-1e9, 1e9);
            maxSp->setValue(colorMap->dataRange().upper);
            fl->addRow("最小值:", minSp);
            fl->addRow("最大值:", maxSp);
            QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            fl->addRow(bb);
            if (dlg.exec() == QDialog::Accepted) {
                QCPRange manualRng(minSp->value(), maxSp->value());
                colorMap->setDataRange(manualRng);
                colorScale->axis()->setRange(manualRng);
                range = manualRng;
                plot->replot(QCustomPlot::rpQueuedReplot);
            }
        } else if (selected == actAutoRange) {
            double minVal = std::numeric_limits<double>::max();
            double maxVal = std::numeric_limits<double>::lowest();
            int ksz = colorMap->data()->keySize();
            int vsz = colorMap->data()->valueSize();
            bool hasFinite = false;
            for (int ky = 0; ky < ksz; ++ky)
                for (int vz = 0; vz < vsz; ++vz) {
                    double v = colorMap->data()->cell(ky, vz);
                    if (std::isfinite(v)) {
                        if (v < minVal) minVal = v;
                        if (v > maxVal) maxVal = v;
                        hasFinite = true;
                    }
                }
            if (hasFinite && maxVal > minVal) {
                QCPRange autoRng(minVal, maxVal);
                colorMap->setDataRange(autoRng);
                colorScale->axis()->setRange(autoRng);
                range = autoRng;
                plot->replot(QCustomPlot::rpQueuedReplot);
            }
        } else if (selected == actResetView) {
            int w = colorMap->data()->keySize();
            int h = colorMap->data()->valueSize();
            plot->xAxis->setRange(0, w - 1);
            plot->yAxis->setRange(0, h - 1);
            plot->replot(QCustomPlot::rpQueuedReplot);
        }
    });

    plot->replot();
}

void ImagingDisplayWindow::showImage(const QVector<float> &frameData, int nx, int ny, int seq)
{
    if (frameData.size() < nx * ny || nx <= 0 || ny <= 0) return;

    m_ringMode = false;
    m_plot2->setVisible(false);
    m_lblTitle2->setVisible(false);
    setWindowTitle("线性扫描实时成像");

    if (m_firstFrame) {
        m_colorMap->data()->setSize(nx, ny);
        m_colorMap->data()->setRange(QCPRange(0, nx - 1), QCPRange(0, ny - 1));
        m_plot->xAxis->setRange(0, nx - 1);
        m_plot->yAxis->setRange(0, ny - 1);
        m_plot->yAxis->setRangeReversed(true);
        m_plot->xAxis->ticker()->setTickCount(5);
        m_plot->yAxis->ticker()->setTickCount(5);
        m_plot->axisRect()->setupFullAxesBox();
        m_colorMap->setDataRange(m_pixelColorRange);
        m_colorScale->axis()->setRange(m_pixelColorRange);
        m_firstFrame = false;
    }

    for (int col = 0; col < nx; ++col)
        for (int row = 0; row < ny; ++row)
            m_colorMap->data()->setCell(col, row, static_cast<double>(frameData[col * ny + row]));

    m_plot->replot(QCustomPlot::rpImmediateRefresh);
    if (m_lblStatus) m_lblStatus->setText(QString("第 %1 帧").arg(seq));
}

void ImagingDisplayWindow::showRingImage(const QVector<float> &wl1, const QVector<float> &wl2,
                                         int nx, int ny, int seq)
{
    if (wl1.size() < nx * ny || wl2.size() < nx * ny || nx <= 0 || ny <= 0) return;

    m_ringMode = true;
    m_plot2->setVisible(true);
    m_lblTitle2->setVisible(true);
    setWindowTitle("环形扫描实时成像（532nm / 1064nm）");

    if (m_firstFrame) {
        for (QCPColorMap *cm : {m_colorMap, m_colorMap2}) {
            cm->data()->setSize(nx, ny);
            cm->data()->setRange(QCPRange(0, nx - 1), QCPRange(0, ny - 1));
        }
        for (QCustomPlot *plot : {m_plot, m_plot2}) {
            plot->xAxis->setRange(0, nx - 1);
            plot->yAxis->setRange(0, ny - 1);
            plot->yAxis->setRangeReversed(true);
            plot->xAxis->ticker()->setTickCount(5);
            plot->yAxis->ticker()->setTickCount(5);
            plot->axisRect()->setupFullAxesBox();
        }
        m_colorMap->setDataRange(m_pixelColorRange);
        m_colorScale->axis()->setRange(m_pixelColorRange);
        m_colorMap2->setDataRange(m_pixelColorRange2);
        m_colorScale2->axis()->setRange(m_pixelColorRange2);
        m_firstFrame = false;
    }

    for (int col = 0; col < nx; ++col) {
        for (int row = 0; row < ny; ++row) {
            m_colorMap->data()->setCell(col, row, static_cast<double>(wl1[col * ny + row]));
            m_colorMap2->data()->setCell(col, row, static_cast<double>(wl2[col * ny + row]));
        }
    }

    m_plot->replot(QCustomPlot::rpImmediateRefresh);
    m_plot2->replot(QCustomPlot::rpImmediateRefresh);
    if (m_lblStatus) m_lblStatus->setText(QString("第 %1 帧").arg(seq));
}