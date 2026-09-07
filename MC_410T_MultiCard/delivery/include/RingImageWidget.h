#pragma once

#include <QWidget>
#include <QImage>
#include <QPointF>

class QAction;

// 环扫实时成像图像控件：
// 显示已按显示范围映射好的灰度 QImage，默认 1:1 像素（每个数据像素=1 屏幕像素），
// 支持鼠标拖拽平移、滚轮缩放；自带坐标刻度（5 个刻度，y 轴反向）与右键菜单。
class RingImageWidget : public QWidget
{
    Q_OBJECT
public:
    struct Range {
        double lower = 0.0;
        double upper = 500.0;
    };

    explicit RingImageWidget(QWidget *parent = nullptr);

    // 设置显示图像（灰度，已按 Range 映射）；保留尺寸变化时重新居中
    void setImage(const QImage &img);
    const QImage &image() const { return m_image; }

    // 当前数据范围提示（右键“自适应数据范围”用）
    void setDataRangeHint(const Range &r) { m_dataRange = r; }
    Range dataRangeHint() const { return m_dataRange; }

    void resetView();          // 还原视图：1:1 + 居中
    int squareSide() const;    // 当前正方形视口边长（像素），供色标对齐
    int squareTop() const;     // 当前正方形视口顶部 Y（像素），供色标对齐

signals:
    void saveRequested();
    void copyRequested();
    void setRangeRequested();
    void autoRangeRequested();
    void resetViewRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    // 双击坐标轴刻度数字：行内编辑该端数据范围（等同设置坐标范围）
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QRectF squareViewport() const;      // 最大内接正方形（预留轴刻度边距）
    QRectF visibleDataRect() const;     // 当前可见的数据像素矩形
    QPointF dataToWidget(const QPointF &p) const;
    QPointF widgetToData(const QPointF &p) const;
    void clampCenter();

    QImage  m_image;
    Range   m_dataRange;
    double  m_zoom = 1.0;               // 屏幕像素 / 数据像素
    QPointF m_center;                   // 视口中心对应的数据坐标
    bool    m_dragging = false;
    QPoint  m_lastPos;
    int     m_squareSide = 0;
    int     m_squareTop = 0;
    bool    m_initialized = false;
};
