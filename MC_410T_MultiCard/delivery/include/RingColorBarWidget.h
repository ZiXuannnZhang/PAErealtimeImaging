#pragma once

#include <QWidget>

// 环扫实时成像色标条：垂直黑→白渐变 + 5 个刻度 + 标签“Amplitude (a.u.)”。
// 渐变条高度由窗口按图像正方形边长设置，上下沿与图像对齐。
// 双击刻度数字可行内编辑该端范围（等同“设置色条范围”，灰度映射随新范围重绘）。
class RingColorBarWidget : public QWidget
{
    Q_OBJECT
public:
    struct Range {
        double lower = 0.0;
        double upper = 500.0;
    };

    explicit RingColorBarWidget(QWidget *parent = nullptr);

    void setRange(double lower, double upper) { m_range = {lower, upper}; update(); }
    // top/height 为渐变条相对本控件的几何（由窗口按图像正方形视口传入）
    void setBarGeometry(int top, int height) { m_barTop = top; m_barHeight = height; update(); }
    Range range() const { return m_range; }

signals:
    // 用户通过双击刻度数字行内编辑修改了范围（lower/upper 之一）
    void rangeEdited(double lower, double upper);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    Range m_range{0.0, 500.0};
    int   m_barTop = 0;
    int   m_barHeight = 200;
};
