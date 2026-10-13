// 显示层几何纯函数测试（TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md
// 「验收 1」）：映射往返/边界 + 掩膜行区间 LUT + LUT↔单像素判定逐像素一致性。
// 纯 std 实现（RingDisplayMapping.h 无 Qt 依赖），无 GUI。
#include "../include/RingDisplayMapping.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using ringdisplay::RowSpan;
using ringdisplay::buildMaskRows;
using ringdisplay::colToXmm;
using ringdisplay::displayValueToYmm;
using ringdisplay::isPixelMasked;
using ringdisplay::pixelSpacingMm;
using ringdisplay::rowToYmmRecon;
using ringdisplay::ymmToDisplayValue;
using ringdisplay::xmmToCol;

namespace {
int g_failures = 0;

void check(bool cond, const std::string &msg)
{
    if (!cond) {
        std::printf("FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

void checkNear(double a, double b, double tol, const std::string &msg)
{
    if (!(std::fabs(a - b) <= tol)) {
        std::printf("FAIL: %s (a=%.17g b=%.17g diff=%.3g)\n",
                    msg.c_str(), a, b, std::fabs(a - b));
        ++g_failures;
    }
}

// ── 映射往返/边界 ────────────────────────────────────────────────────
void testMappingRoundTrip()
{
    // nx 奇/偶两口径（偶数 nx 无中心像素）：端点式 linspace，spacing=fov/(nx−1)
    const struct { int nx; double fovMm; } cases[] = {
        {3600, 36.0},   // 默认配置（fov=36mm, gridSize=0.01 → nx=ceil(3600)=3600）
        {3601, 36.0},   // 奇数 nx（ceil 进位，存在精确中心像素）
        {5,    4.0},
        {4,    4.0},
        {2,    1.0},    // 最小合法网格
        {1024, 25.6},
    };
    for (const auto &c : cases) {
        const double spacing = pixelSpacingMm(c.nx, c.fovMm);
        checkNear(spacing, c.fovMm / (c.nx - 1), 0.0,
                  "spacing=fov/(nx−1) 精确相等 nx=" + std::to_string(c.nx));
        // 端点边界：col 0/nx−1 = ±fov/2
        checkNear(colToXmm(0, c.nx, spacing), -c.fovMm / 2.0, 1e-9,
                  "col 0 = −fov/2 nx=" + std::to_string(c.nx));
        checkNear(colToXmm(c.nx - 1, c.nx, spacing), c.fovMm / 2.0, 1e-9,
                  "col nx−1 = +fov/2 nx=" + std::to_string(c.nx));
        // 重建口径行 0 = −fov/2（屏幕顶）；屏幕口径显示值 ny−1 = +fov/2（上=+）
        checkNear(rowToYmmRecon(0, c.nx, spacing), -c.fovMm / 2.0, 1e-9,
                  "row 0(重建口径) = −fov/2 nx=" + std::to_string(c.nx));
        checkNear(displayValueToYmm(c.nx - 1, c.nx, spacing), c.fovMm / 2.0, 1e-9,
                  "显示值 ny−1(屏幕顶) = +fov/2 nx=" + std::to_string(c.nx));
        checkNear(displayValueToYmm(0, c.nx, spacing), -c.fovMm / 2.0, 1e-9,
                  "显示值 0(屏幕底) = −fov/2 nx=" + std::to_string(c.nx));
        // 任务文档 §2.2 规格公式等价性：colToXmm ≡ −fov/2 + col·spacing（容差
        // 1e-9 mm = 单一 spacing 来源推导 fov 的浮点级差异上界）
        for (int col = 0; col < c.nx; ++col) {
            const double specForm = -c.fovMm / 2.0 + col * spacing;
            checkNear(colToXmm(col, c.nx, spacing), specForm, 1e-9,
                      "colToXmm ≡ 规格公式 nx=" + std::to_string(c.nx));
        }
        // 往返互逆（全列）
        for (int col = 0; col < c.nx; ++col) {
            if (xmmToCol(colToXmm(col, c.nx, spacing), c.nx, spacing) != col) {
                check(false, "xmm 往返互逆 nx=" + std::to_string(c.nx)
                      + " col=" + std::to_string(col));
                break;
            }
        }
        // 往返互逆（显示值）
        for (int v = 0; v < c.nx; ++v) {
            if (ymmToDisplayValue(displayValueToYmm(v, c.nx, spacing), c.nx, spacing) != v) {
                check(false, "y 显示值往返互逆 nx=" + std::to_string(c.nx)
                      + " v=" + std::to_string(v));
                break;
            }
        }
    }
    // 越界钳位
    check(xmmToCol(-1e6, 5, 1.0) == 0, "xmmToCol 负向钳位 0");
    check(xmmToCol(1e6, 5, 1.0) == 4, "xmmToCol 正向钳位 nx−1");
    check(ymmToDisplayValue(-1e6, 5, 1.0) == 0, "ymmToDisplayValue 负向钳位 0");
    check(ymmToDisplayValue(1e6, 5, 1.0) == 4, "ymmToDisplayValue 正向钳位 ny−1");
    // 未注入/退化：spacing<=0 → 0（调用方回退像素刻度）
    check(pixelSpacingMm(5, 0.0) == 0.0, "fov=0 → spacing=0");
    check(pixelSpacingMm(1, 36.0) == 0.0, "nx=1 → spacing=0");
}

void testFovGridSizeCombos()
{
    // fov/gridSize 组合边界：nx=ceil(fov/gridSize)，spacing=fov/(nx−1)≠gridSize
    const struct { double fov; double grid; } combos[] = {
        {36.0, 0.01},    // 默认：nx=3600，spacing≈10.0028µm
        {10.0, 0.3},     // 整除有余：nx=ceil(33.33)=34
        {7.2,  0.02},    // nx=360
        {5.0,  5.0},     // nx=1（退化：spacing 无定义→0）
        {0.5,  0.2},     // nx=ceil(2.5)=3
    };
    for (const auto &c : combos) {
        const int nx = static_cast<int>(std::ceil(c.fov / c.grid));
        if (nx <= 1) {
            check(pixelSpacingMm(nx, c.fov) == 0.0, "退化网格 spacing=0");
            continue;
        }
        const double spacing = pixelSpacingMm(nx, c.fov);
        checkNear(spacing, c.fov / (nx - 1), 0.0,
                  "spacing 公式 fov=" + std::to_string(c.fov));
        // spacing ≠ gridSize（事实 1；默认配置差 ~0.028%）
        check(std::fabs(spacing - c.grid) > 0.0,
              "spacing≠gridSize fov=" + std::to_string(c.fov));
        // 全列往返
        bool ok = true;
        for (int col = 0; col < nx; ++col)
            if (xmmToCol(colToXmm(col, nx, spacing), nx, spacing) != col) { ok = false; break; }
        check(ok, "组合往返 fov=" + std::to_string(c.fov));
    }
    // 默认配置量化检查：spacing = 36/3599 ≈ 10.0028µm（事实 1），
    // 与 gridSize=0.01mm 相对差 ~0.028%（spacing≠gridSize）
    const double spacing = pixelSpacingMm(3600, 36.0);
    checkNear(spacing, 36.0 / 3599.0, 0.0, "默认 spacing=36/3599");
    check(std::fabs(spacing - 0.01) / 0.01 > 2.0e-4, "默认 spacing≠gridSize（~0.028%）");
}

// ── 掩膜行区间 LUT ──────────────────────────────────────────────────
bool spansEqual(const RowSpan &a, int c0, int c1)
{
    if (c0 > c1) return a.empty();
    return a.c0 == c0 && a.c1 == c1;
}

void testMaskLutChords()
{
    // 手算弦截口：nx=5、spacing=1（fov=4mm）、R=1.5mm → rc=1.5（像素单位）
    // 行 dy=0：|dx|<1.5 → [1,3]；行 dy=±1：dx²+1<2.25 → |dx|<1.118 → [1,3]；
    // 行 dy=±2：q≤0 → 整行不掩。
    {
        const auto rows = buildMaskRows(5, 1.0, 1.5);
        check(rows.size() == 5, "LUT size=nx");
        check(spansEqual(rows[0], 1, 0), "row0 空区间");
        check(spansEqual(rows[1], 1, 3), "row1 弦 [1,3]");
        check(spansEqual(rows[2], 1, 3), "row2 弦 [1,3]");
        check(spansEqual(rows[3], 1, 3), "row3 弦 [1,3]");
        check(spansEqual(rows[4], 1, 0), "row4 空区间");
    }
    // 偶数 nx：无中心像素（事实 1），R 极小 → 全不掩
    {
        const auto rows = buildMaskRows(8, 1.0, 0.1);
        bool allEmpty = true;
        for (const auto &r : rows) if (!r.empty()) allEmpty = false;
        check(allEmpty, "偶数 nx + R 极小 → 全不掩");
    }
    // 奇数 nx + R 极小（< spacing/2）：仅精确中心像素被掩
    {
        const auto rows = buildMaskRows(5, 1.0, 0.4);
        check(spansEqual(rows[0], 1, 0), "奇数小 R row0 空");
        check(spansEqual(rows[1], 1, 0), "奇数小 R row1 空");
        check(spansEqual(rows[2], 2, 2), "奇数小 R 仅中心像素 [2,2]");
        check(spansEqual(rows[3], 1, 0), "奇数小 R row3 空");
        check(spansEqual(rows[4], 1, 0), "奇数小 R row4 空");
    }
    // R > fov/2：每行均有非空截口（任务文档「R>fov/2 全掩」按像素中心几何
    // 精确化为“全行掩膜”；整幅逐位全掩需 R ≥ fov/√2，见下一用例）
    {
        const int nx = 8;
        const double fov = 7.0;   // spacing=1
        const auto rows = buildMaskRows(nx, 1.0, 3.6);   // fov/2=3.5 < 3.6 < 4.95
        bool everyRowMasked = true;
        for (const auto &r : rows) if (r.empty()) everyRowMasked = false;
        check(everyRowMasked, "R>fov/2 每行均有非空截口");
        // 中心行全掩、四角未被掩
        check(rows[nx / 2 - 1].c0 == 0 && rows[nx / 2 - 1].c1 == nx - 1,
              "R>fov/2 中心行近全掩");
        check(isPixelMasked(0, 0, nx, 1.0, 3.6) == false, "R<fov/√2 角像素不掩");
    }
    // R ≥ fov/√2（最大像素中心距 = 端点像素对角距离 fov/√2）→ 整幅全掩
    {
        const int nx = 8;
        const double cornerDist = 3.5 * std::sqrt(2.0);   // fov=7, 端点角像素
        const auto rows = buildMaskRows(nx, 1.0, cornerDist * (1.0 + 1e-12));
        bool allFull = true;
        for (const auto &r : rows)
            if (r.empty() || r.c0 != 0 || r.c1 != nx - 1) allFull = false;
        check(allFull, "R≥fov/√2 整幅全掩（每行 [0,nx−1]）");
    }
    // 退化输入：R<=0 / spacing<=0 → 全不掩
    {
        const auto rows0 = buildMaskRows(5, 1.0, 0.0);
        const auto rowsNeg = buildMaskRows(5, 1.0, -1.0);
        const auto rowsNoSp = buildMaskRows(5, 0.0, 1.5);
        bool ok = true;
        for (int i = 0; i < 5; ++i)
            if (!rows0[i].empty() || !rowsNeg[i].empty() || !rowsNoSp[i].empty()) ok = false;
        check(ok, "R<=0 / spacing<=0 全不掩");
        check(buildMaskRows(0, 1.0, 1.0).empty(), "nx=0 → 空 LUT");
    }
    // 大 R（≫fov）：整幅全掩且无越界
    {
        const auto rows = buildMaskRows(6, 1.0, 100.0);
        bool ok = rows.size() == 6;
        for (const auto &r : rows)
            if (r.empty() || r.c0 != 0 || r.c1 != 5) ok = false;
        check(ok, "R≫fov 整幅全掩 [0,nx−1]");
    }
}

void testLutPredicateAgreement()
{
    // LUT 与单像素判定逐像素一致（多个口径，含默认 3600 配置）
    const struct { int nx; double spacing; double r; } cases[] = {
        {5, 1.0, 1.5},
        {4, 1.0, 1.2},
        {6, 0.7, 2.1},
        {8, 0.5, 1.7},
        {3600, 36.0 / 3599.0, 6.57},   // 默认配置（spacing≈10.0028µm）
        {3601, 0.01, 6.57},            // 奇数 nx 默认间距
    };
    for (const auto &c : cases) {
        const auto rows = buildMaskRows(c.nx, c.spacing, c.r);
        if (static_cast<int>(rows.size()) != c.nx) {
            check(false, "LUT size nx=" + std::to_string(c.nx));
            continue;
        }
        for (int row = 0; row < c.nx; ++row) {
            for (int col = 0; col < c.nx; ++col) {
                const bool byLut = rows[static_cast<size_t>(row)].covers(col);
                const bool byPx = isPixelMasked(col, row, c.nx, c.spacing, c.r);
                if (byLut != byPx) {
                    check(false, "LUT↔判定一致 nx=" + std::to_string(c.nx)
                          + " (" + std::to_string(col) + "," + std::to_string(row) + ")");
                    return;
                }
            }
        }
    }
    // 默认配置自检：环心被掩、环外角点不掩、半径边界两侧像素判定正确
    const int nx = 3600;
    const double spacing = pixelSpacingMm(nx, 36.0);
    check(isPixelMasked(1800, 1800, nx, spacing, 6.57), "默认配置 环心被掩");
    check(!isPixelMasked(0, 0, nx, spacing, 6.57), "默认配置 角点不掩");
    // 沿 +x 找半径边界像素：边界两侧判定相反且与 mm 距离一致
    const double h = (nx - 1) / 2.0;
    const int colAtR = static_cast<int>(h + 6.57 / spacing);
    const double dIn = std::fabs(colToXmm(colAtR, nx, spacing));
    const double dOut = std::fabs(colToXmm(colAtR + 1, nx, spacing));
    check(dIn < 6.57 && dOut > 6.57, "边界像素距离跨越 R");
    check(isPixelMasked(colAtR, 1800, nx, spacing, 6.57), "R 内侧被掩");
    check(!isPixelMasked(colAtR + 1, 1800, nx, spacing, 6.57), "R 外侧不掩");
    // 标签↔掩膜自洽（验收 4）：用毫米标签值反推判定与掩膜判定一致
    for (int k = 0; k < 200; ++k) {
        const int col = 1500 + k * 3;
        const int row = 1801;
        const double xmm = colToXmm(col, nx, spacing);
        const double ymm = rowToYmmRecon(row, nx, spacing);
        const bool byLabel = std::sqrt(xmm * xmm + ymm * ymm) < 6.57;
        if (byLabel != isPixelMasked(col, row, nx, spacing, 6.57)) {
            check(false, "标签↔掩膜自洽 col=" + std::to_string(col));
            return;
        }
    }
}

}  // namespace

int main()
{
    testMappingRoundTrip();
    testFovGridSizeCombos();
    testMaskLutChords();
    testLutPredicateAgreement();
    if (g_failures == 0) {
        std::printf("ring_display_mapping_test: PASS\n");
        return 0;
    }
    std::printf("ring_display_mapping_test: %d FAILURES\n", g_failures);
    return 1;
}
