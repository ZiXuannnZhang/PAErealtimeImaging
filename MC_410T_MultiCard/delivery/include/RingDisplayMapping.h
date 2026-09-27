#pragma once

// 环形显示层几何纯函数（显示层专用，不参与成像/重建/保存链路）。
//
// 坐标口径（TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md 事实 1/§2.2）：
//   重建网格像素中心为端点式 linspace：x_mm(col) = −fov/2 + col·spacing，
//   spacing = fov/(nx−1)（真实像素间距，≠ gridSize；gridSize 仅决定 nx=ceil(fov/gridSize)）。
//   屏幕行 iy 自顶向下对应重建 y 由小到大（行 0 = y=−fov/2 在屏幕顶，即屏幕上=−y 的
//   既定翻转显示）；屏幕口径 y 轴标签按显示值 v = ny−1−row 取 y_mm = +fov/2 − row·spacing
//   （等价于 v·spacing − fov/2，上=+）。
//
// 单一几何来源：本头文件全部换算只依赖 spacingMm 与 nx（fov ≡ spacing·(nx−1) 由
// spacing 定义反推）。毫米标签与掩膜 LUT 共用同一 spacing，保证同一像素的标签值
// 与掩膜判定自洽（验收 4）。
//
// 掩膜判定：像素中心到环心 (0,0) 的距离 < R_mask ⇒ 该像素显示为 0（渲染期置零，
// 不改任何缓存/保存数据）。实现为按行的列区间 LUT（圆的弦截口）。

#include <cmath>
#include <vector>

namespace ringdisplay {

// 一行内被掩的列区间 [c0, c1]（含端点）；c0 > c1 表示该行无掩膜像素。
struct RowSpan {
    int c0 = 1;
    int c1 = 0;
    bool empty() const { return c0 > c1; }
    bool covers(int col) const { return col >= c0 && col <= c1; }
};

// 真实像素间距（毫米）= fov/(nx−1)；nx<=1 时无定义，返回 0（调用方回退像素刻度）。
inline double pixelSpacingMm(int nx, double fovMm)
{
    return (nx > 1 && fovMm > 0.0) ? fovMm / static_cast<double>(nx - 1) : 0.0;
}

// 列 → x 毫米（屏幕左=−fov/2，右=+fov/2）。等价于 −fov/2 + col·spacing。
inline double colToXmm(int col, int nx, double spacingMm)
{
    return (static_cast<double>(col) - static_cast<double>(nx - 1) / 2.0) * spacingMm;
}

// 重建口径行 → y 毫米：行 0（屏幕顶）= −fov/2。掩膜几何用本口径（与 |y| 无关，同式）。
inline double rowToYmmRecon(int row, int nx, double spacingMm)
{
    return (static_cast<double>(row) - static_cast<double>(nx - 1) / 2.0) * spacingMm;
}

// 屏幕口径显示值 v（0 在屏幕底、ny−1 在顶）→ y 毫米（上=+）：y_mm = v·spacing − fov/2。
inline double displayValueToYmm(int displayValue, int nx, double spacingMm)
{
    return (static_cast<double>(displayValue) - static_cast<double>(nx - 1) / 2.0) * spacingMm;
}

// x 毫米 → 最近像素列（四舍五入，钳位 [0, nx−1]）。
inline int xmmToCol(double xmm, int nx, double spacingMm)
{
    if (spacingMm <= 0.0 || nx <= 0) return 0;
    const double col = xmm / spacingMm + static_cast<double>(nx - 1) / 2.0;
    const int c = static_cast<int>(std::lround(col));
    return c < 0 ? 0 : (c > nx - 1 ? nx - 1 : c);
}

// 屏幕口径 y 毫米 → 显示值（0=屏幕底，ny−1=屏幕顶；上=+ 口径）。
inline int ymmToDisplayValue(double ymm, int nx, double spacingMm)
{
    if (spacingMm <= 0.0 || nx <= 0) return 0;
    const double v = ymm / spacingMm + static_cast<double>(nx - 1) / 2.0;
    const int value = static_cast<int>(std::lround(v));
    return value < 0 ? 0 : (value > nx - 1 ? nx - 1 : value);
}

// ── 分数（亚像素）变体：双击刻度编辑的端点值为未取整的数据坐标，换算保持
// 分数精度（内部视图状态仍是像素空间）；与上方整数版同式，仅不做取整/钳位。
inline double colToXmmF(double col, int nx, double spacingMm)
{
    return (col - static_cast<double>(nx - 1) / 2.0) * spacingMm;
}

inline double displayValueToYmmF(double displayValue, int nx, double spacingMm)
{
    return (displayValue - static_cast<double>(nx - 1) / 2.0) * spacingMm;
}

inline double xmmToColF(double xmm, int nx, double spacingMm)
{
    return xmm / spacingMm + static_cast<double>(nx - 1) / 2.0;
}

inline double ymmToDisplayValueF(double ymm, int nx, double spacingMm)
{
    return ymm / spacingMm + static_cast<double>(nx - 1) / 2.0;
}

// 单像素掩膜判定（与 buildMaskRows 同式）：像素中心距环心 < radiusMm ⇒ 掩。
inline bool isPixelMasked(int col, int row, int nx, double spacingMm, double radiusMm)
{
    if (spacingMm <= 0.0 || radiusMm <= 0.0 || nx <= 0) return false;
    if (col < 0 || col >= nx || row < 0 || row >= nx) return false;
    const double h = static_cast<double>(nx - 1) / 2.0;
    const double dx = static_cast<double>(col) - h;
    const double dy = static_cast<double>(row) - h;
    const double rc = radiusMm / spacingMm;
    return dx * dx + dy * dy < rc * rc;
}

// 掩膜行区间 LUT（spec §1.4 maskRows(nx, spacingMm, radiusMm)）：
// 返回 size=nx 的每行被掩列区间（圆的弦截口；行中心 |y|≥R 的行整行不掩）。
// 边界列用单像素判定式直接校正，保证 LUT 与 isPixelMasked 逐像素一致。
// R<=0 / spacing<=0 → 全不掩；R 覆盖整行时该行区间为 [0, nx−1]。
inline std::vector<RowSpan> buildMaskRows(int nx, double spacingMm, double radiusMm)
{
    std::vector<RowSpan> rows;
    if (nx <= 0) return rows;
    rows.assign(static_cast<size_t>(nx), RowSpan{});
    if (spacingMm <= 0.0 || radiusMm <= 0.0) return rows;

    const double h = static_cast<double>(nx - 1) / 2.0;
    const double rc = radiusMm / spacingMm;
    const double rc2 = rc * rc;
    for (int row = 0; row < nx; ++row) {
        const double dy = static_cast<double>(row) - h;
        const double q = rc2 - dy * dy;
        if (q <= 0.0)
            continue;   // 行中心 |y| ≥ R：整行不掩（保持 RowSpan{} 空区间）
        const double halfW = std::sqrt(q);
        auto masked = [h, dy, rc2](int col) {
            const double dx = static_cast<double>(col) - h;
            return dx * dx + dy * dy < rc2;
        };
        // 解析估计 + 直接谓词校正（消除 sqrt/ceil 的浮点边界误差）
        int c0 = static_cast<int>(std::floor(h - halfW)) + 1;
        if (c0 < 0) c0 = 0;
        while (c0 > 0 && masked(c0 - 1)) --c0;
        while (c0 < nx && !masked(c0)) ++c0;
        int c1 = static_cast<int>(std::ceil(h + halfW)) - 1;
        if (c1 > nx - 1) c1 = nx - 1;
        while (c1 + 1 < nx && masked(c1 + 1)) ++c1;
        while (c1 >= 0 && !masked(c1)) --c1;
        if (c0 <= c1)
            rows[static_cast<size_t>(row)] = RowSpan{c0, c1};
    }
    return rows;
}

}  // namespace ringdisplay
