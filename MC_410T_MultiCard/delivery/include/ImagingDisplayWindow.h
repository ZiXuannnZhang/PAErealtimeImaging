#pragma once

#include <QWidget>
#include <QVector>
#include <QEvent>
#include <vector>
#include <memory>
#include <functional>
#include "RingDisplayMapping.h"
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
    // 显示掩膜状态（渲染期置零用）：行区间 LUT 按 (nx, spacingMm, radiusMm) 构建。
    // 仅显示层状态：不进 RingReconCudaConfig / ring JSON / ImagingSvc，不写
    // m_ringFrame 缓存（掩膜关闭即完整恢复）。
    struct DisplayMask {
        int nx = 0;                       // LUT 对应的帧尺寸（不匹配时视为未启用）
        double spacingMm = 0.0;           // 构建 LUT 用的像素间距（未注入=0 → 回退不掩）
        bool enabled = false;
        double radiusMm = 0.0;
        std::vector<ringdisplay::RowSpan> rows;   // size=nx；空=不掩
    };

    explicit ImagingDisplayWindow(QWidget *parent = nullptr);
    ~ImagingDisplayWindow() override;

    // 纯函数：把单波长 float 缓冲（dn×dn，x-major）按 [lo,hi] 范围转为灰度 QImage。
    // 可在任意线程调用（不涉及 GUI 对象），供后台转换与右键重绘共用。
    // mask 非空且启用、LUT 尺寸与 dn 匹配时，掩膜内像素渲染期置零；mask 为空
    // （默认）时与既有基线输出逐位一致。
    static QImage renderFrame(const float *buf, int dn, double lo, double hi,
                              const std::shared_ptr<const DisplayMask> &mask = {});

    // 显示掩膜（工作一）：环内像素显示置零。变更即按当前缓存重建 LUT、重绘
    // 当前帧并 emit presentationChanged（沿既有 range 变更刷新捕获路径）。
    void setDisplayMask(bool enabled, double radiusMm);
    // 毫米网格几何（工作二）：spacing=fov/(nx−1)（真实像素间距）。未注入
    // （spacing<=0）前刻度回退现行像素刻度、掩膜保持不生效。
    void setGridGeometry(double spacingMm, double fovMm);
    // 当前掩膜状态（共享只读快照；MainWindow 后台渲染线程按值捕获）
    std::shared_ptr<const DisplayMask> displayMask() const { return m_mask; }
    // UI 线程：按帧尺寸确保 LUT 匹配（不匹配则按当前 spacing/半径重建并缓存），
    // 返回与该帧 nx 匹配的掩膜快照。快照渲染路径每次派发前调用。
    std::shared_ptr<const DisplayMask> displayMaskForFrame(int nx);

    // 后台转换完成后的回投入口（仅 UI 线程调用）：保存数据缓存并贴图。
    // frameIdx 与主窗口“输出 x 帧”一致（圈末/超时重置后重新计数）
    void applyRingFrame(std::shared_ptr<std::vector<float>> frame, int nx, int frameIdx,
                        const QImage &img1, const QImage &img2);
    int lastSeq() const { return m_lastSeq; }
    // 按当前色标范围（m_range1/2）将窗口两幅图像渲染为 1600×1600 ARGB32 并保存 PNG
    bool saveWindowPngs(const QString &dir, const QString &suffix, int seq) const;
    // UI-thread capture only; the returned writer owns immutable pixels and
    // ranges and can render/save on a worker after Ring/CUDA reset.
    using PngWriter = std::function<bool(const QString&, const QString&)>;
    PngWriter capturePngWriter(int seq) const;
signals:
    void presentationChanged();
public:

    // 当前色标范围（供后台转换捕获）
    RingImageWidget::Range range1() const { return m_range1; }
    RingImageWidget::Range range2() const { return m_range2; }

    // 线性模式（方向1后不再支持，保留空实现避免调用方改动）
    void showImage(const QVector<float> &frameData, int nx, int ny, int seq);

private:
    void buildUi();
    void applyRangeToAll(int index, const RingImageWidget::Range &r);
    void syncBarHeights();       // 尺寸/状态变化后，让色标高度跟随图像正方形
    // 按 (nx, m_spacingMm, 当前 enabled/radius) 重建掩膜 LUT 并替换 m_mask。
    // spacing 未注入或 nx 无效时构建“enabled 但空 LUT”的快照（渲染回退不掩）。
    void rebuildMaskLut(int nx);
    // 用当前掩膜状态重绘缓存帧（m_ringFrame + m_range1/2）；无缓存帧时仅返回。
    void rerenderCurrentFrame();

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

    // 显示掩膜快照（shared_ptr 原子替换；后台线程按值捕获只读副本）
    std::shared_ptr<const DisplayMask> m_mask;
    // 毫米网格几何（setGridGeometry 注入；spacing<=0 = 未注入 → 像素刻度回退）
    double m_spacingMm = 0.0;
    double m_fovMm = 0.0;
};
