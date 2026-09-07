#include "RingColorBarWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QLineEdit>
#include <QPointer>

namespace {
constexpr int kBarWidth = 14;      // 渐变条宽度
constexpr int kLabelWidth = 60;    // 刻度数字宽度
constexpr int kTitleGap = 6;
}

RingColorBarWidget::RingColorBarWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setMinimumSize(0, 0);
    setFixedWidth(kBarWidth + kLabelWidth + kTitleGap + 22);
}

void RingColorBarWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(38, 38, 38));   // 与图像框背景统一

    const int h = qMax(1, m_barHeight);
    const int top = qBound(0, m_barTop, qMax(0, height() - h));
    const QRectF bar(kBarWidth / 4.0, top, kBarWidth, h);

    QLinearGradient grad(bar.topLeft(), bar.bottomLeft());
    // 视觉方向：上端=上限=白，下端=下限=黑（映射关系不变：白=上限、黑=下限）
    grad.setColorAt(0.0, QColor(255, 255, 255));   // 上=上限=白
    grad.setColorAt(1.0, QColor(0, 0, 0));         // 下=下限=黑
    p.fillRect(bar, grad);
    p.setPen(QPen(QColor(80, 80, 80)));
    p.drawRect(bar);

    // 刻度：5 个，随范围
    QFont f = p.font();
    f.setPointSizeF(8.0);
    p.setFont(f);
    p.setPen(QColor(160, 160, 160));
    for (int k = 0; k < 5; ++k) {
        const double t = k / 4.0;
        const double wy = bar.top() + t * bar.height();
        // 上端显示上限、下端显示下限（与渐变白上黑下一致）
        const double val = m_range.upper - t * (m_range.upper - m_range.lower);
        p.drawLine(QPointF(bar.right() + 1, wy), QPointF(bar.right() + 6, wy));
        p.drawText(QRectF(bar.right() + 8, wy - 8, kLabelWidth, 16),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::number(val, 'g', 4));
    }

    // 标签：垂直文字
    p.save();
    p.translate(width() - 4, height() / 2.0);
    p.rotate(-90);
    p.setPen(QColor(180, 180, 180));
    p.drawText(QRect(-height() / 2.0, 0, height(), 16),
               Qt::AlignCenter, "Amplitude (a.u.)");
    p.restore();
}

// 双击刻度数字 → 行内编辑该端范围（上端=白=upper，下端=黑=lower），
// 发出 rangeEdited 由窗口联动灰度映射重绘与色标范围记忆
void RingColorBarWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int h = qMax(1, m_barHeight);
    const int top = qBound(0, m_barTop, qMax(0, height() - h));
    const QRectF bar(kBarWidth / 4.0, top, kBarWidth, h);

    double t = -1.0;
    QRectF labelRect;
    for (int k = 0; k < 5; ++k) {
        const double tk = k / 4.0;
        const double wy = bar.top() + tk * bar.height();
        const QRectF lr(bar.right() + 8, wy - 8, kLabelWidth, 16);
        if (lr.contains(event->position())) { t = tk; labelRect = lr; break; }
    }
    if (t < 0.0) { QWidget::mouseDoubleClickEvent(event); return; }

    const bool isUpper = (t < 0.5);   // 上端=上限，下端=下限
    QLineEdit *ed = new QLineEdit(this);
    ed->setGeometry(labelRect.toRect());
    ed->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ed->setText(QString::number(isUpper ? m_range.upper : m_range.lower, 'g', 10));
    ed->show();
    ed->setFocus();
    ed->selectAll();
    QPointer<QLineEdit> edPtr(ed);
    connect(ed, &QLineEdit::editingFinished, this, [this, edPtr, isUpper]() {
        if (!edPtr) return;
        bool ok = false;
        const double v = edPtr->text().toDouble(&ok);
        edPtr->deleteLater();
        if (!ok) return;
        double lo = m_range.lower, hi = m_range.upper;
        if (isUpper) hi = v; else lo = v;
        if (hi <= lo) return;   // 无效范围：放弃
        m_range = {lo, hi};
        update();
        emit rangeEdited(lo, hi);
    });
}
