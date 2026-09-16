#include "RingImageWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMenu>
#include <QAction>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QPointer>
#include <cmath>

namespace {
constexpr int kMarginLeft = 46;    // y 轴刻度数字
constexpr int kMarginBottom = 28;  // x 轴刻度数字
constexpr int kMarginTop = 6;
constexpr int kMarginRight = 6;
constexpr double kZoomStep = 1.25;
constexpr double kZoomMin = 1.0;
constexpr double kZoomMax = 64.0;
}

RingImageWidget::RingImageWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(0, 0);
    setMouseTracking(false);
    setContextMenuPolicy(Qt::DefaultContextMenu);
}

void RingImageWidget::setImage(const QImage &img)
{
    m_image = img;
    if (!m_initialized) {
        // 首帧：1:1 + 数据居中
        m_zoom = 1.0;
        m_center = QPointF(img.width() / 2.0, img.height() / 2.0);
        m_initialized = true;
    }
    clampCenter();
    update();
}

void RingImageWidget::resetView()
{
    m_zoom = 1.0;
    m_center = QPointF(m_image.width() / 2.0, m_image.height() / 2.0);
    update();
}

int RingImageWidget::squareSide() const
{
    return m_squareSide;
}

int RingImageWidget::squareTop() const
{
    return m_squareTop;
}

QRectF RingImageWidget::squareViewport() const
{
    const int availW = qMax(1, width() - kMarginLeft - kMarginRight);
    const int availH = qMax(1, height() - kMarginTop - kMarginBottom);
    const int side = qMin(availW, availH);
    const int x = kMarginLeft + (availW - side) / 2;
    const int y = kMarginTop + (availH - side) / 2;
    return QRectF(x, y, side, side);
}

QRectF RingImageWidget::visibleDataRect() const
{
    if (m_image.isNull() || m_zoom <= 0.0)
        return QRectF();
    const double w = m_image.width() / m_zoom;
    const double h = m_image.height() / m_zoom;
    QRectF r(m_center.x() - w / 2.0, m_center.y() - h / 2.0, w, h);
    // 钳制到图像范围
    if (r.left() < 0) r.moveLeft(0);
    if (r.top() < 0) r.moveTop(0);
    if (r.right() > m_image.width()) r.moveRight(m_image.width());
    if (r.bottom() > m_image.height()) r.moveBottom(m_image.height());
    return r;
}

QPointF RingImageWidget::dataToWidget(const QPointF &p) const
{
    const QRectF vp = squareViewport();
    const QRectF vis = visibleDataRect();
    if (vis.width() <= 0 || vis.height() <= 0)
        return vp.center();
    return QPointF(vp.left() + (p.x() - vis.left()) / vis.width() * vp.width(),
                   vp.top() + (p.y() - vis.top()) / vis.height() * vp.height());
}

QPointF RingImageWidget::widgetToData(const QPointF &p) const
{
    const QRectF vp = squareViewport();
    const QRectF vis = visibleDataRect();
    if (vp.width() <= 0 || vp.height() <= 0)
        return m_center;
    return QPointF(vis.left() + (p.x() - vp.left()) / vp.width() * vis.width(),
                   vis.top() + (p.y() - vp.top()) / vp.height() * vis.height());
}

void RingImageWidget::clampCenter()
{
    if (m_image.isNull()) return;
    const double w = m_image.width() / m_zoom;
    const double h = m_image.height() / m_zoom;
    m_center.setX(qBound(w / 2.0, m_center.x(), m_image.width() - w / 2.0));
    m_center.setY(qBound(h / 2.0, m_center.y(), m_image.height() - h / 2.0));
}

void RingImageWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(38, 38, 38));   // 图框底色，与旧版一致

    if (m_image.isNull()) return;

    const QRectF vp = squareViewport();
    const QRectF vis = visibleDataRect();

    // 图像：源=可见数据区，目标=正方形视口（默认 1:1 时像素一一对应）
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom > 1.0);
    p.drawImage(QRectF(vp.left(), vp.top(), vp.width(), vp.height()),
                m_image, vis);

    // 坐标轴：x 从左到右，y 反向（0 在上，与旧版一致）
    p.setPen(QPen(QColor(160, 160, 160)));
    p.drawLine(QPointF(vp.left(), vp.bottom()), QPointF(vp.right(), vp.bottom()));
    p.drawLine(QPointF(vp.left(), vp.top()), QPointF(vp.left(), vp.bottom()));

    const int nx = m_image.width();
    const int ny = m_image.height();
    const double x0 = vis.left();
    const double x1 = vis.right();
    const double y0 = vis.top();        // 数据行 0（图像顶部）
    const double y1 = vis.bottom();

    QFont f = p.font();
    f.setPointSizeF(8.0);
    p.setFont(f);

    // x 刻度：数据坐标 0..nx-1
    for (int k = 0; k < 5; ++k) {
        const double t = k / 4.0;
        const double xd = x0 + t * (x1 - x0);
        const int xi = qBound(0, static_cast<int>(std::lround(xd)), nx - 1);
        const double wx = vp.left() + t * vp.width();
        p.drawLine(QPointF(wx, vp.bottom()), QPointF(wx, vp.bottom() + 4));
        p.drawText(QRectF(wx - 30, vp.bottom() + 5, 60, 16),
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(xi));
    }
    // y 刻度：反向（0 在上），显示 0..ny-1
    for (int k = 0; k < 5; ++k) {
        const double t = k / 4.0;
        const double yd = y0 + t * (y1 - y0);
        const int yi = qBound(0, static_cast<int>(std::lround(yd)), ny - 1);
        const double display = ny - 1 - yi;   // 反向显示
        const double wy = vp.top() + t * vp.height();
        p.drawLine(QPointF(vp.left() - 4, wy), QPointF(vp.left(), wy));
        p.drawText(QRectF(0, wy - 8, kMarginLeft - 8, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(static_cast<int>(display)));
    }
}

void RingImageWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const QRectF vp = squareViewport();
    m_squareSide = static_cast<int>(vp.width());
    m_squareTop = static_cast<int>(vp.top());
    clampCenter();
    update();
}

void RingImageWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        // 必须接受按下事件，Qt 才会为本控件合成并派发双击事件
        // （坐标轴数字行内编辑依赖 mouseDoubleClickEvent）
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RingImageWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && !m_image.isNull()) {
        const QPointF d = widgetToData(QPointF(event->pos())) -
                          widgetToData(QPointF(m_lastPos));
        m_center -= d;
        clampCenter();
        m_lastPos = event->pos();
        update();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void RingImageWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        unsetCursor();
    }
    QWidget::mouseReleaseEvent(event);
}

void RingImageWidget::wheelEvent(QWheelEvent *event)
{
    if (m_image.isNull()) return;
    const QPointF before = widgetToData(QPointF(event->position()));
    const double factor = std::pow(kZoomStep, event->angleDelta().y() / 120.0);
    m_zoom = qBound(kZoomMin, m_zoom * factor, kZoomMax);
    clampCenter();
    const QPointF after = widgetToData(QPointF(event->position()));
    m_center += before - after;   // 保持光标处数据点不动
    clampCenter();
    update();
    event->accept();
}

void RingImageWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *actSave = menu.addAction("保存图片...");
    QAction *actCopy = menu.addAction("复制到剪贴板");
    menu.addSeparator();
    QAction *actRange = menu.addAction("设置色条范围...");
    QAction *actAuto = menu.addAction("自适应数据范围");
    QAction *actReset = menu.addAction("还原视图");
    QAction *sel = menu.exec(event->globalPos());
    if (sel == actSave)       emit saveRequested();
    else if (sel == actCopy)  emit copyRequested();
    else if (sel == actRange) emit setRangeRequested();
    else if (sel == actAuto)  emit autoRangeRequested();
    else if (sel == actReset) emit resetViewRequested();
}

// 双击坐标轴刻度数字 → 行内编辑该端数据范围（等同设置坐标范围）：
// 编辑一端保持对端不动，按统一缩放重新计算视图（方形视口，两轴同步缩放）。
void RingImageWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_image.isNull()) { QWidget::mouseDoubleClickEvent(event); return; }

    const QRectF vp = squareViewport();
    const QRectF vis = visibleDataRect();
    if (vis.width() <= 0 || vis.height() <= 0) return;

    // 命中测试：与 paintEvent 中刻度标签矩形一致
    int hitAxis = -1;      // 0=x 轴，1=y 轴
    double t = 0.0;
    QRectF labelRect;
    for (int k = 0; k < 5; ++k) {
        const double tk = k / 4.0;
        const double wx = vp.left() + tk * vp.width();
        const QRectF xr(wx - 30, vp.bottom() + 5, 60, 16);
        if (xr.contains(event->position())) { hitAxis = 0; t = tk; labelRect = xr; break; }
        const double wy = vp.top() + tk * vp.height();
        const QRectF yr(0, wy - 8, kMarginLeft - 8, 16);
        if (yr.contains(event->position())) { hitAxis = 1; t = tk; labelRect = yr; break; }
    }
    if (hitAxis < 0) { QWidget::mouseDoubleClickEvent(event); return; }

    const double x0 = vis.left(), x1 = vis.right();
    const double y0 = vis.top(),  y1 = vis.bottom();
    // 编辑初始值 = 该端当前显示的数字（y 轴显示数为反向：ny-1-行号）
    QString initText;
    if (hitAxis == 0) {
        initText = QString::number((t < 0.5) ? x0 : x1, 'g', 10);
    } else {
        const double row = (t < 0.5) ? y0 : y1;
        initText = QString::number((m_image.height() - 1) - row, 'g', 10);
    }

    QLineEdit *ed = new QLineEdit(this);
    QRect er = labelRect.toRect();
    er.setWidth(qMax(er.width(), 60));
    ed->setGeometry(er);
    ed->setAlignment(Qt::AlignCenter);
    ed->setText(initText);
    ed->show();
    ed->setFocus();
    ed->selectAll();
    QPointer<QLineEdit> edPtr(ed);
    connect(ed, &QLineEdit::editingFinished, this,
            [this, edPtr, hitAxis, t, x0, x1, y0, y1]() {
        if (!edPtr || m_image.isNull()) return;
        bool ok = false;
        const double v = edPtr->text().toDouble(&ok);
        edPtr->deleteLater();
        if (!ok) return;
        const double nxImg = static_cast<double>(m_image.width());
        const double nyImg = static_cast<double>(m_image.height());
        if (hitAxis == 0) {
            double nx0 = x0, nx1 = x1;
            if (t < 0.5) nx0 = qBound(0.0, v, nx1 - 1.0);
            else         nx1 = qBound(nx0 + 1.0, v, nxImg);
            m_zoom = qBound(kZoomMin, nxImg / (nx1 - nx0), kZoomMax);
            m_center.setX((nx0 + nx1) / 2.0);
        } else {
            const double newRow = (nyImg - 1) - v;   // 显示数反向映射回数据行
            double ny0 = y0, ny1 = y1;
            if (t < 0.5) ny0 = qBound(0.0, newRow, ny1 - 1.0);
            else         ny1 = qBound(ny0 + 1.0, newRow, nyImg - 1);
            m_zoom = qBound(kZoomMin, nyImg / (ny1 - ny0), kZoomMax);
            m_center.setY((ny0 + ny1) / 2.0);
        }
        clampCenter();
        update();
    });
}
