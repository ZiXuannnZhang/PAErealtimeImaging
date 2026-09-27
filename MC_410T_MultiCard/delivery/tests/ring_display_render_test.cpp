// 显示渲染掩膜测试（TASKS/环形显示层优化_显示掩膜与毫米坐标_20260928-003123.md
// 「验收 1」）：renderFrame 掩膜断言 + 默认恒等 + PNG 写入器掩膜行为。
//
// 基线参照：baselineRenderFrame / baselineRenderPng 为 fb6370b（本分支基线）中
// renderFrame / capturePngWriter render lambda 的逐字转录（沿
// ring_enhancer_parity_test 的内嵌参考实现先例），用于证明：
//   1) 掩膜关闭（默认参数）时与新实现输出逐位一致；
//   2) 掩膜启用后掩膜内=0、掩膜外与基线逐位一致；
//   3) PNG 写入器掩膜关闭/开启输出与预期逐位一致（环内块全掩→0、环外不变）。
// 运行需 offscreen 平台（同 diagnostic_dialog_test 先例：QT_QPA_PLATFORM=offscreen）。
#include "ImagingDisplayWindow.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char *msg)
{
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++g_failures;
    }
}

// ── 基线逐字转录（fb6370b：src/ImagingDisplayWindow.cpp renderFrame）──────
QImage baselineRenderFrame(const float *buf, int dn, double lo, double hi)
{
    if (!buf || dn <= 0) return QImage();
    const double span = hi - lo;
    QImage img(dn, dn, QImage::Format_Grayscale8);
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

// ── 基线逐字转录（fb6370b：capturePngWriter render lambda）───────────────
QImage baselineRenderPng(const float *buf, int srcN, const RingImageWidget::Range &r)
{
    constexpr int kOut = 1600;
    QImage img(kOut, kOut, QImage::Format_ARGB32);
    const double lo = r.lower;
    const double span = r.upper - r.lower;
    for (int y = 0; y < kOut; ++y) {
        const int y0 = y * srcN / kOut;
        const int y1 = std::max(y0 + 1, (y + 1) * srcN / kOut);
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < kOut; ++x) {
            const int x0 = x * srcN / kOut;
            const int x1 = std::max(x0 + 1, (x + 1) * srcN / kOut);
            double s = 0.0;
            int cnt = 0;
            for (int sx = x0; sx < x1; ++sx) {
                for (int sy = y0; sy < y1; ++sy) {
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
}

// 掩膜启用时的 PNG 预期：掩膜内源像素按 0 参与块平均（与新实现同式）
QImage expectedMaskedRenderPng(const float *buf, int srcN,
                               const RingImageWidget::Range &r,
                               const std::vector<ringdisplay::RowSpan> &rows)
{
    constexpr int kOut = 1600;
    QImage img(kOut, kOut, QImage::Format_ARGB32);
    const double lo = r.lower;
    const double span = r.upper - r.lower;
    for (int y = 0; y < kOut; ++y) {
        const int y0 = y * srcN / kOut;
        const int y1 = std::max(y0 + 1, (y + 1) * srcN / kOut);
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < kOut; ++x) {
            const int x0 = x * srcN / kOut;
            const int x1 = std::max(x0 + 1, (x + 1) * srcN / kOut);
            double s = 0.0;
            int cnt = 0;
            for (int sx = x0; sx < x1; ++sx) {
                for (int sy = y0; sy < y1; ++sy) {
                    double v = buf[static_cast<size_t>(sx) * srcN + sy];
                    if (rows[static_cast<size_t>(sy)].covers(sx))
                        v = 0.0;
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
}

bool imagesBitwiseEqual(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()) return false;
    if (a.bytesPerLine() != b.bytesPerLine()) return false;
    const int lines = a.height();
    for (int y = 0; y < lines; ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(a.bytesPerLine())) != 0)
            return false;
    }
    return true;
}

QImage loadPng(const QString &path)
{
    QImage img(path);
    return img.convertToFormat(QImage::Format_ARGB32);
}

// capturePngWriter 落盘名含毫秒时间戳（<suffix>_<yyyyMMdd_HHmmss_zzz>_frame<seq>_
// <wl>nm.png，fb6370b 既有行为），用目录枚举按前缀定位本次产物。
QImage loadWriterPng(const QDir &d, const QString &prefix, int seq, const char *wl)
{
    const QString filter = QStringLiteral("%1_*_frame%2_%3nm.png")
                               .arg(prefix).arg(seq).arg(wl);
    const QStringList hits = d.entryList(QStringList{filter}, QDir::Files);
    if (hits.isEmpty()) return QImage();
    return loadPng(d.filePath(hits.first()));
}

}  // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const int n = 32;                       // 小网格（快），掩膜几何非平凡
    const double spacingMm = 1.0;           // fov = 31mm
    const double fovMm = spacingMm * (n - 1);
    const double radiusMm = 8.0;

    // 合成数据：确定伪随机 + 一个 NaN（isfinite 路径两侧一致）
    auto frame = std::make_shared<std::vector<float>>(static_cast<size_t>(n) * n * 2);
    for (int x = 0; x < n; ++x)
        for (int r = 0; r < n; ++r) {
            const size_t i = static_cast<size_t>(x) * n + r;
            (*frame)[i] = 60.0f + 40.0f * static_cast<float>(
                std::fmod(0.37 * x + 0.53 * r, 1.0));
            (*frame)[static_cast<size_t>(n) * n + i] = 120.0f - 30.0f * static_cast<float>(
                std::fmod(0.61 * x + 0.29 * r, 1.0));
        }
    (*frame)[static_cast<size_t>(n / 2) * n + n / 2] = std::nanf("");

    const RingImageWidget::Range range{0.0, 500.0};
    const auto maskRows = ringdisplay::buildMaskRows(n, spacingMm, radiusMm);

    // ── 1) renderFrame 默认恒等：无掩膜参数 / 掩膜关闭 / LUT 尺寸失配 → 与基线逐位一致
    {
        const QImage got1 = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                              range.lower, range.upper);
        const QImage base = baselineRenderFrame(frame->data(), n,
                                                range.lower, range.upper);
        check(imagesBitwiseEqual(got1, base), "renderFrame 无掩膜参数与基线逐位一致");

        auto offMask = std::make_shared<ImagingDisplayWindow::DisplayMask>();
        offMask->nx = n;
        offMask->spacingMm = spacingMm;
        offMask->enabled = false;
        offMask->radiusMm = radiusMm;
        offMask->rows = maskRows;
        const QImage got2 = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                              range.lower, range.upper, offMask);
        check(imagesBitwiseEqual(got2, base), "renderFrame 掩膜关闭与基线逐位一致");

        auto staleMask = std::make_shared<ImagingDisplayWindow::DisplayMask>(*offMask);
        staleMask->enabled = true;
        staleMask->nx = n + 1;   // LUT 与该帧尺寸失配 → 回退不掩
        const QImage got3 = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                              range.lower, range.upper, staleMask);
        check(imagesBitwiseEqual(got3, base), "LUT 尺寸失配回退与基线逐位一致");

        auto emptySpacing = std::make_shared<ImagingDisplayWindow::DisplayMask>(*offMask);
        emptySpacing->enabled = true;
        emptySpacing->spacingMm = 0.0;   // spacing 未注入 → 回退不掩
        emptySpacing->rows.clear();
        const QImage got4 = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                              range.lower, range.upper, emptySpacing);
        check(imagesBitwiseEqual(got4, base), "spacing 未注入回退与基线逐位一致");
    }

    // ── 2) renderFrame 掩膜断言：掩膜内=0、掩膜外与基线逐位一致
    {
        auto onMask = std::make_shared<ImagingDisplayWindow::DisplayMask>();
        onMask->nx = n;
        onMask->spacingMm = spacingMm;
        onMask->enabled = true;
        onMask->radiusMm = radiusMm;
        onMask->rows = maskRows;
        const QImage got = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                             range.lower, range.upper, onMask);
        const QImage base = baselineRenderFrame(frame->data(), n,
                                                range.lower, range.upper);
        check(got.size() == base.size(), "掩膜渲染尺寸一致");
        int maskedCount = 0;
        bool insideZero = true, outsideEqual = true;
        for (int x = 0; x < n; ++x) {
            for (int r = 0; r < n; ++r) {
                const uchar g = got.scanLine(r)[x];
                const uchar gb = base.scanLine(r)[x];
                if (ringdisplay::isPixelMasked(x, r, n, spacingMm, radiusMm)) {
                    ++maskedCount;
                    // v=0 经 [0,500] 色标 → t=0 → 字节 0
                    if (g != 0) insideZero = false;
                } else if (g != gb) {
                    outsideEqual = false;
                }
            }
        }
        check(maskedCount > 0, "存在被掩像素");
        check(insideZero, "掩膜内字节=0（环内置零）");
        check(outsideEqual, "掩膜外与基线逐位一致（环外不变）");
        check(maskedCount < n * n, "掩膜未覆盖全图（环外存在）");
    }

    // ── 3) PNG 写入器（capturePngWriter）：关闭=基线逐位一致；开启=环内块 0/环外不变
    {
        QTemporaryDir dir;
        check(dir.isValid(), "临时目录有效");
        ImagingDisplayWindow win;
        const QImage img1 = ImagingDisplayWindow::renderFrame(frame->data(), n,
                                                              range.lower, range.upper);
        const QImage img2 = ImagingDisplayWindow::renderFrame(
            frame->data() + static_cast<size_t>(n) * n, n, range.lower, range.upper);
        win.applyRingFrame(frame, n, 1, img1, img2);
        win.setGridGeometry(spacingMm, fovMm);
        win.setDisplayMask(false, radiusMm);

        const auto writerOff = win.capturePngWriter(1);
        check(static_cast<bool>(writerOff), "掩膜关闭 PNG writer 有效");
        check(writerOff(dir.path(), QStringLiteral("off")), "掩膜关闭 PNG 保存成功");
        const QDir outDir(dir.path());
        const QImage pngOff1 = loadWriterPng(outDir, QStringLiteral("off"), 1, "532");
        const QImage pngOff2 = loadWriterPng(outDir, QStringLiteral("off"), 1, "1064");
        const QImage expOff1 = baselineRenderPng(frame->data(), n, range);
        const QImage expOff2 = baselineRenderPng(
            frame->data() + static_cast<size_t>(n) * n, n, range);
        check(imagesBitwiseEqual(pngOff1, expOff1), "PNG(关) wl1 与基线逐位一致");
        check(imagesBitwiseEqual(pngOff2, expOff2), "PNG(关) wl2 与基线逐位一致");

        // 掩膜开启：环内块全掩 → 输出 0；环外块与关闭掩膜输出逐位一致；
        // 整图与 expectedMaskedRenderPng 逐位一致
        win.setDisplayMask(true, radiusMm);
        const auto writerOn = win.capturePngWriter(2);
        check(static_cast<bool>(writerOn), "掩膜开启 PNG writer 有效");
        check(writerOn(dir.path(), QStringLiteral("on")), "掩膜开启 PNG 保存成功");
        const QImage pngOn1 = loadWriterPng(outDir, QStringLiteral("on"), 2, "532");
        const QImage expOn1 = expectedMaskedRenderPng(frame->data(), n, range, maskRows);
        check(imagesBitwiseEqual(pngOn1, expOn1), "PNG(开) wl1 与预期逐位一致");

        // 区域断言：全掩块黑、全非掩块与 off 相同（降采样块级）
        const int kOut = 1600;
        int fullMaskedBlocks = 0, fullOutsideBlocks = 0;
        bool insideBlack = true, outsideSame = true;
        for (int y = 0; y < kOut; ++y) {
            const int sy0 = y * n / kOut;
            const int sy1 = std::max(sy0 + 1, (y + 1) * n / kOut);
            for (int x = 0; x < kOut; ++x) {
                const int sx0 = x * n / kOut;
                const int sx1 = std::max(sx0 + 1, (x + 1) * n / kOut);
                bool allMasked = true, noneMasked = true;
                for (int sx = sx0; sx < sx1; ++sx)
                    for (int sy = sy0; sy < sy1; ++sy) {
                        if (ringdisplay::isPixelMasked(sx, sy, n, spacingMm, radiusMm))
                            noneMasked = false;
                        else
                            allMasked = false;
                    }
                const QRgb on = pngOn1.pixel(x, y);
                const QRgb off = pngOff1.pixel(x, y);
                if (allMasked) {
                    ++fullMaskedBlocks;
                    if (qGray(on) != 0) insideBlack = false;
                } else if (noneMasked) {
                    ++fullOutsideBlocks;
                    if (on != off) outsideSame = false;
                }
            }
        }
        check(fullMaskedBlocks > 0, "存在全掩块");
        check(fullOutsideBlocks > 0, "存在全非掩块");
        check(insideBlack, "环内块全掩 → 输出 0");
        check(outsideSame, "环外块与掩膜关闭输出逐位一致");
    }

    if (g_failures == 0) {
        std::printf("ring_display_render_test: PASS\n");
        return 0;
    }
    std::printf("ring_display_render_test: %d FAILURES\n", g_failures);
    return 1;
}
