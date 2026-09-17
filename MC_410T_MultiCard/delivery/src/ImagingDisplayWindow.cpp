#include "PaimageAcquisition/SettingsPath.h"
#include "ImagingDisplayWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QClipboard>
#include <QFileDialog>
#include <QDialog>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QSettings>
#include <QImage>
#include <QDateTime>
#include <QDir>

ImagingDisplayWindow::ImagingDisplayWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("环形扫描实时成像");
    // 窗口大小记忆：恢复上次关闭前的几何（位置+尺寸）
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    const bool restored = restoreGeometry(s.value("ImagingWindow/Geometry").toByteArray());
    if (restored) {
        const QRect avail = screen() ? screen()->availableGeometry()
                                     : QGuiApplication::primaryScreen()->availableGeometry();
        if (avail.isValid() && (width() > avail.width() || height() > avail.height())) {
            resize(qMin(width(), avail.width()), qMin(height(), avail.height()));
        }
    } else {
        resize(1382, 800);
    }

    // 色标范围记忆：恢复上次关闭前的范围（默认 0~500）
    const double r1lo = s.value("ImagingWindow/Range1Low", 0.0).toDouble();
    const double r1hi = s.value("ImagingWindow/Range1High", 500.0).toDouble();
    const double r2lo = s.value("ImagingWindow/Range2Low", 0.0).toDouble();
    const double r2hi = s.value("ImagingWindow/Range2High", 500.0).toDouble();
    if (r1hi > r1lo) m_range1 = {r1lo, r1hi};
    if (r2hi > r2lo) m_range2 = {r2lo, r2hi};

    // 统一深灰背景（30,30,30）：图像、色标、留白处于同一灰色面板
    setStyleSheet("ImagingDisplayWindow { background-color: #262626; }");
    buildUi();
}

ImagingDisplayWindow::~ImagingDisplayWindow()
{
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    s.setValue("ImagingWindow/Geometry", saveGeometry());
    s.setValue("ImagingWindow/Range1Low",  m_range1.lower);
    s.setValue("ImagingWindow/Range1High", m_range1.upper);
    s.setValue("ImagingWindow/Range2Low",  m_range2.lower);
    s.setValue("ImagingWindow/Range2High", m_range2.upper);
}

void ImagingDisplayWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *imgRow = new QHBoxLayout;
    auto *col1 = new QVBoxLayout;
    m_lblTitle1 = new QLabel("532nm", this);
    m_lblTitle1->setAlignment(Qt::AlignCenter);
    m_lblTitle1->setStyleSheet("color: #CCCCCC; background: transparent;");
    m_lblTitle1->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextEditable);
    auto *row1 = new QHBoxLayout;
    row1->setSpacing(0);

    m_img1 = new RingImageWidget(this);
    row1->addWidget(m_img1, 1);

    auto *barWrap1 = new QWidget(this);
    barWrap1->setStyleSheet("QWidget { background-color: #262626; }");
    barWrap1->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    barWrap1->setMinimumSize(0, 0);
    auto *barLayout1 = new QVBoxLayout(barWrap1);
    barLayout1->setContentsMargins(0, 0, 0, 0);
    barLayout1->setSpacing(0);
    m_bar1 = new RingColorBarWidget(barWrap1);
    barLayout1->addWidget(m_bar1, 1);
    row1->addWidget(barWrap1);

    col1->addWidget(m_lblTitle1);
    col1->addLayout(row1, 1);
    imgRow->addLayout(col1, 1);

    auto *col2 = new QVBoxLayout;
    m_lblTitle2 = new QLabel("1064nm", this);
    m_lblTitle2->setAlignment(Qt::AlignCenter);
    m_lblTitle2->setStyleSheet("color: #CCCCCC; background: transparent;");
    m_lblTitle2->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextEditable);
    auto *row2 = new QHBoxLayout;
    row2->setSpacing(0);

    m_img2 = new RingImageWidget(this);
    row2->addWidget(m_img2, 1);

    auto *barWrap2 = new QWidget(this);
    barWrap2->setStyleSheet("QWidget { background-color: #262626; }");
    barWrap2->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    barWrap2->setMinimumSize(0, 0);
    auto *barLayout2 = new QVBoxLayout(barWrap2);
    barLayout2->setContentsMargins(0, 0, 0, 0);
    barLayout2->setSpacing(0);
    m_bar2 = new RingColorBarWidget(barWrap2);
    barLayout2->addWidget(m_bar2, 1);
    row2->addWidget(barWrap2);

    col2->addWidget(m_lblTitle2);
    col2->addLayout(row2, 1);
    imgRow->addLayout(col2, 1);
    root->addLayout(imgRow, 1);

    m_lblStatus = new QLabel("就绪", this);
    m_lblStatus->setStyleSheet("color: #888888; background: transparent; padding: 2px 8px;");
    root->addWidget(m_lblStatus);

    // 右键菜单动作（每个图像区独立）
    auto connectMenu = [this](RingImageWidget *w, int index) {
        connect(w, &RingImageWidget::saveRequested, this, [this, index]() {
            RingImageWidget *w = index == 0 ? m_img1 : m_img2;
            QString fn = QFileDialog::getSaveFileName(
                this, "保存图片", QString("环形成像_%1.png").arg(index + 1),
                "PNG (*.png);;BMP (*.bmp);;JPG (*.jpg)");
            if (!fn.isEmpty())
                w->grab().save(fn);
        });
        connect(w, &RingImageWidget::copyRequested, this, [this, index]() {
            RingImageWidget *w = index == 0 ? m_img1 : m_img2;
            QApplication::clipboard()->setPixmap(w->grab());
        });
        connect(w, &RingImageWidget::setRangeRequested, this, [this, index]() {
            RingImageWidget::Range r = index == 0 ? m_range1 : m_range2;
            QDialog dlg(this);
            dlg.setWindowTitle("设置色条范围");
            QFormLayout *fl = new QFormLayout(&dlg);
            auto *minSp = new QDoubleSpinBox(&dlg);
            minSp->setRange(-1e9, 1e9);
            minSp->setValue(r.lower);
            auto *maxSp = new QDoubleSpinBox(&dlg);
            maxSp->setRange(-1e9, 1e9);
            maxSp->setValue(r.upper);
            fl->addRow("下限", minSp);
            fl->addRow("上限", maxSp);
            auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            fl->addRow(bb);
            if (dlg.exec() != QDialog::Accepted)
                return;
            double lo = minSp->value();
            double hi = maxSp->value();
            if (hi <= lo) { double t = lo; lo = hi; hi = t; }
            if (index == 0) {
                m_range1 = {lo, hi};
                m_bar1->setRange(lo, hi);
            } else {
                m_range2 = {lo, hi};
                m_bar2->setRange(lo, hi);
            }
            // 立即用新范围重绘当前帧（图像与色标同步变化）
            RingImageWidget *w = index == 0 ? m_img1 : m_img2;
            if (m_lastDn > 0 && m_ringFrame &&
                m_ringFrame->size() >= static_cast<size_t>(m_lastDn) * m_lastDn * 2) {
                const float *src = m_ringFrame->data()
                    + (index == 1 ? static_cast<size_t>(m_lastDn) * m_lastDn : 0);
                w->setImage(renderFrame(src, m_lastDn, lo, hi));
            emit presentationChanged();
            }
        });
        connect(w, &RingImageWidget::autoRangeRequested, this, [this, index]() {
            if (m_lastDn <= 0 || !m_ringFrame ||
                m_ringFrame->size() < static_cast<size_t>(m_lastDn) * m_lastDn * 2)
                return;
            const float *src = m_ringFrame->data()
                + (index == 1 ? static_cast<size_t>(m_lastDn) * m_lastDn : 0);
            const size_t n = static_cast<size_t>(m_lastDn) * m_lastDn;
            double mn = std::numeric_limits<double>::infinity();
            double mx = -mn;
            for (size_t i = 0; i < n; ++i) {
                const double v = static_cast<double>(src[i]);
                if (!std::isfinite(v)) continue;
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            if (!std::isfinite(mn)) { mn = 0.0; mx = 1.0; }
            if (mx <= mn) { mx = mn + 1.0; }
            RingImageWidget::Range r{mn, mx};
            if (index == 0) {
                m_range1 = r;
                m_bar1->setRange(r.lower, r.upper);
                m_img1->setImage(renderFrame(src, m_lastDn, r.lower, r.upper));
            emit presentationChanged();
            } else {
                m_range2 = r;
                m_bar2->setRange(r.lower, r.upper);
                m_img2->setImage(renderFrame(src, m_lastDn, r.lower, r.upper));
            emit presentationChanged();
            }
        });
        connect(w, &RingImageWidget::resetViewRequested, w, &RingImageWidget::resetView);
    };
    connectMenu(m_img1, 0);
    connectMenu(m_img2, 1);

    // 色标轴数字双击编辑：等同“设置色条范围”——更新范围并按新范围重绘
    // 当前帧（灰度映射与更改的数字对应），范围同时记忆
    auto applyBarRange = [this](int index, double lo, double hi) {
        if (hi <= lo) return;
        if (index == 0) {
            m_range1 = {lo, hi};
            m_bar1->setRange(lo, hi);
        } else {
            m_range2 = {lo, hi};
            m_bar2->setRange(lo, hi);
        }
        if (m_lastDn > 0 && m_ringFrame &&
            m_ringFrame->size() >= static_cast<size_t>(m_lastDn) * m_lastDn * 2) {
            const float *src = m_ringFrame->data()
                + (index == 1 ? static_cast<size_t>(m_lastDn) * m_lastDn : 0);
            RingImageWidget *w = index == 0 ? m_img1 : m_img2;
            w->setImage(renderFrame(src, m_lastDn, lo, hi));
            emit presentationChanged();
        }
    };
    connect(m_bar1, &RingColorBarWidget::rangeEdited, this,
            [applyBarRange](double lo, double hi) { applyBarRange(0, lo, hi); });
    connect(m_bar2, &RingColorBarWidget::rangeEdited, this,
            [applyBarRange](double lo, double hi) { applyBarRange(1, lo, hi); });
}

QImage ImagingDisplayWindow::renderFrame(const float *buf, int dn, double lo, double hi)
{
    if (!buf || dn <= 0) return QImage();
    const double span = hi - lo;
    // 缓冲区布局：x-major（索引 = xIdx*dn + yIdx，与 MATLAB column-major 参考一致）。
    // 屏幕映射：列 = x 轴（右），行自顶向下对应 y 由小到大。首通道起点
    // （9 点钟方向 = 数学角 180°，x 最小、y 居中）落在图像左侧中心，
    // 且重建随角度沿顺时针方向展开。
    QImage img(dn, dn, QImage::Format_Grayscale8);
    // 分块转置读取：源缓冲按列存放（xIdx 固定、yIdx 连续）。逐块读取可完全
    // 利用缓存行，避免逐像素跨 3600×4 字节步长导致的缓存行失效（旧实现每
    // 像素一次缓存未命中，两幅 3600² 图转换耗时秒级，阻塞信号显示刷新）。
    constexpr int kBlock = 32;
    for (int x0 = 0; x0 < dn; x0 += kBlock) {
        const int xEnd = qMin(x0 + kBlock, dn);
        for (int r0 = 0; r0 < dn; r0 += kBlock) {
            const int rEnd = qMin(r0 + kBlock, dn);
            uchar *lines[kBlock];
            for (int r = r0; r < rEnd; ++r)
                lines[r - r0] = img.scanLine(r);
            for (int x = x0; x < xEnd; ++x) {
                const float *col = buf + static_cast<size_t>(x) * dn;
                for (int r = r0; r < rEnd; ++r) {
                    const double v = col[r];
                    double t = 0.0;
                    if (std::isfinite(v) && span > 0.0)
                        t = (v - lo) / span;
                    t = qBound(0.0, t, 1.0);
                    lines[r - r0][x] = static_cast<uchar>(t * 255.0);
                }
            }
        }
    }
    return img;
}

void ImagingDisplayWindow::applyRingFrame(std::shared_ptr<std::vector<float>> frame,
                                          int nx, int frameIdx,
                                          const QImage &img1, const QImage &img2)
{
    if (!frame || nx <= 0) return;

    // 标题可写，但每次帧更新后恢复默认值
    if (m_lblTitle1) m_lblTitle1->setText("532nm");
    if (m_lblTitle2) m_lblTitle2->setText("1064nm");

    // 方案A：全分辨率缓存（后台线程已完成 float→QImage 转换；此处仅共享数据
    // 所有权与贴图，右键改范围/自适应按缓存即时重绘）
    m_ringFrame = std::move(frame);

    const bool sizeChanged = (nx != m_lastDn);
    m_lastDn = nx;
    m_lastSeq = frameIdx;   // 与主窗口相同的“圈末/超时重置后重新计数”序号
    m_img1->setImage(img1);
    m_img2->setImage(img2);
    if (sizeChanged) {
        m_img1->resetView();
        m_img2->resetView();
    }
    m_bar1->setRange(m_range1.lower, m_range1.upper);
    m_bar2->setRange(m_range2.lower, m_range2.upper);
    if (m_lblStatus) m_lblStatus->setText(QString("第 %1 帧").arg(frameIdx));
    syncBarHeights();
}

bool ImagingDisplayWindow::saveWindowPngs(const QString &dir, const QString &suffix,
                                          int seq) const
{
    const auto writer = capturePngWriter(seq);
    return writer && writer(dir, suffix);
}

ImagingDisplayWindow::PngWriter ImagingDisplayWindow::capturePngWriter(int seq) const
{
    const auto frame = m_ringFrame;
    const auto range1 = m_range1, range2 = m_range2;
    const int srcN = m_lastDn;
    if (srcN <= 0 || !frame || frame->size() < static_cast<size_t>(srcN) * srcN * 2)
        return {};
    return [frame, range1, range2, srcN, seq](const QString& dir, const QString& suffix) {
    constexpr int kOut = 1600;

    // 与窗口渲染一致的映射：当前手动/自适应色标范围（m_range1/2），块平均降采样到 1600²；
    // 方向与 renderFrame 相同（列=x、行自顶向下 y 由小到大，顺时针展开）
    auto render = [srcN](const float *buf, const RingImageWidget::Range &r) {
        QImage img(kOut, kOut, QImage::Format_ARGB32);
        const double lo = r.lower;
        const double span = r.upper - r.lower;
        for (int y = 0; y < kOut; ++y) {
            const int y0 = y * srcN / kOut;
            const int y1 = std::max(y0 + 1, (y + 1) * srcN / kOut);
            const int sy0 = y0;   // 顺时针展开：行自顶向下 = 源 y 索引由小到大
            const int sy1 = y1;
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < kOut; ++x) {
                const int x0 = x * srcN / kOut;
                const int x1 = std::max(x0 + 1, (x + 1) * srcN / kOut);
                double s = 0.0;
                int cnt = 0;
                for (int sx = x0; sx < x1; ++sx) {
                    for (int sy = sy0; sy < sy1; ++sy) {
                        const double v = buf[static_cast<size_t>(sx) * srcN + sy];
                        if (std::isfinite(v)) { s += v; ++cnt; }
                    }
                }
                double t = 0.0;
                if (cnt > 0 && span > 0.0)
                    t = (s / cnt - lo) / span;
                t = qBound(0.0, t, 1.0);
                const int g = static_cast<int>(t * 255.0 + 0.5);
                line[x] = qRgba(g, g, g, 255);
            }
        }
        return img;
    };

    const QImage img1 = render(frame->data(), range1);
    const QImage img2 = render(frame->data() + static_cast<size_t>(srcN) * srcN, range2);
    if (img1.isNull() || img2.isNull()) return false;

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    const QString stem = suffix.trimmed();
    const QString base = stem.isEmpty() ? ts : stem + "_" + ts;
    const QString tag = QString("_frame%1").arg(seq);
    bool ok = true;
    ok = img1.save(QDir(dir).filePath(base + tag + "_532nm.png"), "PNG") && ok;
    ok = img2.save(QDir(dir).filePath(base + tag + "_1064nm.png"), "PNG") && ok;
    return ok;
    };
}

void ImagingDisplayWindow::showImage(const QVector<float> &, int, int, int)
{
    // 方向1后环扫窗口不再支持线性模式（按需求不做兼容）
}

void ImagingDisplayWindow::syncBarHeights()
{
    if (!m_img1 || !m_bar1 || !m_img2 || !m_bar2) return;
    m_bar1->setBarGeometry(m_img1->squareTop(), m_img1->squareSide());
    m_bar2->setBarGeometry(m_img2->squareTop(), m_img2->squareSide());
}

void ImagingDisplayWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this]() { syncBarHeights(); });
}

void ImagingDisplayWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        QTimer::singleShot(0, this, [this]() { syncBarHeights(); });
}

void ImagingDisplayWindow::closeEvent(QCloseEvent *event)
{
    QSettings s(paimageSettingsPath(), QSettings::IniFormat);
    s.setValue("ImagingWindow/Geometry", saveGeometry());
    s.setValue("ImagingWindow/Range1Low",  m_range1.lower);
    s.setValue("ImagingWindow/Range1High", m_range1.upper);
    s.setValue("ImagingWindow/Range2Low",  m_range2.lower);
    s.setValue("ImagingWindow/Range2High", m_range2.upper);
    QWidget::closeEvent(event);
}
