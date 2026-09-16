#pragma once

#include <QWidget>
#include <QVector>
#include <QEvent>
#include <vector>
#include <memory>
#include "RingImageWidget.h"
#include "RingColorBarWidget.h"

class QLabel;
class QVBoxLayout;

// 实时成像独立窗口（方向1重构版：环形专用）。
// 532nm/1064nm 双波长并排显示，底层渲染由 RingImageWidget（自绘 QImage）完成，
// 不再经过 QCustomPlot 的 colorize/replot 深重绘路径。
// 保留：灰色一体背景、双图+双色标、1:1 像素、拖拽/滚轮缩放、色标对齐、
// 尺寸记忆、右键菜单（保存/复制/色条范围/自适应/还原视图）。
// 快照转换已解耦：float→QImage 灰度转换在后台线程（renderFrame），UI 线程
// 仅 applyRingFrame 贴图，避免成像刷新阻塞时域频域信号显示。
class ImagingDisplayWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ImagingDisplayWindow(QWidget *parent = nullptr);
    ~ImagingDisplayWindow() override;

    // 纯函数：把单波长 float 缓冲（dn×dn，x-major）按 [lo,hi] 范围转为灰度 QImage。
    // 可在任意线程调用（不涉及 GUI 对象），供后台转换与右键重绘共用。
    static QImage renderFrame(const float *buf, int dn, double lo, double hi);

    // 后台转换完成后的回投入口（仅 UI 线程调用）：保存数据缓存并贴图。
    // frameIdx 与主窗口“输出 x 帧”一致（圈末/超时重置后重新计数）
    void applyRingFrame(std::shared_ptr<std::vector<float>> frame, int nx, int frameIdx,
                        const QImage &img1, const QImage &img2);
    int lastSeq() const { return m_lastSeq; }
    // 按当前色标范围（m_range1/2）将窗口两幅图像渲染为 1600×1600 ARGB32 并保存 PNG
    bool saveWindowPngs(const QString &dir, const QString &suffix, int seq) const;

    // 当前色标范围（供后台转换捕获）
    RingImageWidget::Range range1() const { return m_range1; }
    RingImageWidget::Range range2() const { return m_range2; }

    // 线性模式（方向1后不再支持，保留空实现避免调用方改动）
    void showImage(const QVector<float> &frameData, int nx, int ny, int seq);

private:
    void buildUi();
    void applyRangeToAll(int index, const RingImageWidget::Range &r);
    void syncBarHeights();       // 尺寸/状态变化后，让色标高度跟随图像正方形

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    RingImageWidget    *m_img1 = nullptr;
    RingImageWidget    *m_img2 = nullptr;
    RingColorBarWidget *m_bar1 = nullptr;
    RingColorBarWidget *m_bar2 = nullptr;
    QLabel             *m_lblStatus = nullptr;
    QLabel             *m_lblTitle1 = nullptr;
    QLabel             *m_lblTitle2 = nullptr;

    RingImageWidget::Range m_range1{0.0, 500.0};
    RingImageWidget::Range m_range2{0.0, 500.0};
    // 最近一帧全分辨率缓存（wl1 + wl2 连续存放，各 nx*nx float；
    // 后台线程转换后回投共享所有权，右键改范围即时重绘用）
    std::shared_ptr<std::vector<float>> m_ringFrame;
    int                 m_lastDn = 0;
    int                 m_lastSeq = 0;
};
