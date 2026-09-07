// ring_recon_view — 双波长逐块实时重建可视化验证查看器
// 引擎可选：cpu（M1 RingRecon）或 cuda（M2 ring_recon_cuda.dll，需 RING_RECON_VIEW_CUDA）。
#include "ring_recon.h"
#ifdef RING_RECON_VIEW_CUDA
#include "ring_recon_cuda.h"
#endif

#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using ringrecon::IncrementalState;
using ringrecon::PreprocessParams;
using ringrecon::ReconParams;
using ringrecon::StreamConfig;

namespace {

struct DataSpec {
    int    sampDepth;
    int    wlOffset;
    int    alinesPerFrame;
    int    sysDelay1;
    int    sysDelay2;
    double radius;
    double coverageDeg;
    double fovDeg;
};

DataSpec specFor(int id) {
    if (id == 11) return {4000, 151, 4000, 171, 184, 6.57e-3, 180.0, 180.0};
    return {4000, 301, 8000, 358, 371, 6.57e-3, 360.0, 360.0};
}

std::string argStr(const std::vector<std::string>& args, const std::string& key,
                   const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}
int argInt(const std::vector<std::string>& args, const std::string& key, int def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stoi(args[i + 1]);
    return def;
}
double argDouble(const std::vector<std::string>& args, const std::string& key, double def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stod(args[i + 1]);
    return def;
}

// MATLAB imagesc + colormap gray + clim([-100,100]) 的等价显示
QImage toGrayImage(const std::vector<float>& img, int nx, int ny) {
    QImage q(nx, ny, QImage::Format_Grayscale8);
    const float lo = -100.0f, hi = 100.0f;
    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            const float v = img[static_cast<size_t>(ix) * ny + iy];
            const int g = qBound(0, static_cast<int>((v - lo) * 255.0f / (hi - lo)), 255);
            q.setPixel(ix, ny - 1 - iy, qRgb(g, g, g));
        }
    }
    return q;
}

class ViewWindow : public QMainWindow {
public:
    ViewWindow(StreamConfig cfg, std::string dataPath, std::string engine,
               int sleepMs, bool autoQuit, QWidget* parent = nullptr)
        : QMainWindow(parent), cfg_(std::move(cfg)), dataPath_(std::move(dataPath)),
          engine_(std::move(engine)), sleepMs_(sleepMs), autoQuit_(autoQuit) {
        setWindowTitle(QString("RingRecon %1 双波长实时重建验证 — %2")
                           .arg(engine_ == "cuda" ? "M2 CUDA" : "M1 CPU")
                           .arg(QString::fromStdString(dataPath_)));

        auto* central = new QWidget(this);
        auto* layout = new QHBoxLayout(central);
        for (int w = 0; w < 2; ++w) {
            labels_[w] = new QLabel(central);
            labels_[w]->setFixedSize(420, 420);
            labels_[w]->setScaledContents(true);
            labels_[w]->setAlignment(Qt::AlignCenter);
            labels_[w]->setText(QString("波长 %1 等待数据…").arg(w + 1));
            layout->addWidget(labels_[w]);
        }
        setCentralWidget(central);
        statusBar()->showMessage("初始化…");
        resize(900, 520);

        ringrecon::makeGrid(cfg_.recon.fov, cfg_.recon.gridSize, xv_, yv_);
        nx_ = static_cast<int>(xv_.size());
        ny_ = static_cast<int>(yv_.size());
        nBlocks_ = cfg_.alinesPerFrame / cfg_.alinesPerBlock;
        nWlPerBlock_ = cfg_.alinesPerBlock / 2;
        nWlPerFrame_ = cfg_.alinesPerFrame / 2;
        dtheta_ = cfg_.recon.coverageDeg / nWlPerFrame_;
        spanDeg_ = (nWlPerBlock_ - 1) * dtheta_;

        if (engine_ == "cuda") {
#ifdef RING_RECON_VIEW_CUDA
            RingReconCudaConfig cc;
            ring_recon_cuda_set_defaults(&cc);
            cc.dataNum = 0;  // 实际采集时由前端指定数据源
            cc.sampDepth = cfg_.sampDepth;
            cc.alinesPerFrame = cfg_.alinesPerFrame;
            cc.alinesPerBlock = cfg_.alinesPerBlock;
            cc.rawColsPerBlock = cfg_.alinesPerBlock;
            cc.wlOffset = cfg_.wlOffset;
            cc.shiftWL2 = cfg_.shiftWL2 ? 1 : 0;
            cc.radius = cfg_.recon.R;
            cc.coverageDeg = cfg_.recon.coverageDeg;
            cc.fovDeg = cfg_.recon.fovDeg;
            cc.fovTheta0Deg = cfg_.recon.fovTheta0Deg;
            cc.gridSize = cfg_.recon.gridSize;
            cc.sysDelay[0] = cfg_.pre[0].systemDelay;
            cc.sysDelay[1] = cfg_.pre[1].systemDelay;
            cc.maskLength = cfg_.pre[0].maskLength;
            cc.dbrSigRemove = cfg_.pre[0].dbrRemove ? 1 : 0;
            cc.delayCut = cfg_.pre[0].delayCut ? 1 : 0;
            cc.singalImpair = cfg_.pre[0].signalImpair ? 1 : 0;
            cc.imValue[0] = cfg_.pre[0].imValue;
            cc.imValue[1] = cfg_.pre[1].imValue;
            cc.interpolation = (cfg_.recon.interpolation == "linear") ? 0 : 1;
            cc.distanceWeightExponent = cfg_.recon.distanceWeightExponent;
            cc.maskOutOfRange = cfg_.recon.maskOutOfRange ? 1 : 0;
            if (ring_recon_cuda_create(&cc, &cudaH_[0]) != 0 ||
                ring_recon_cuda_create(&cc, &cudaH_[1]) != 0) {
                QMessageBox::critical(this, "CUDA 错误",
                                      QString::fromUtf8(ring_recon_cuda_last_error()));
                QTimer::singleShot(0, qApp, &QCoreApplication::quit);
                return;
            }
#else
            QMessageBox::critical(this, "CUDA 错误",
                                  "当前查看器未启用 CUDA 引擎，请使用 build_ring_recon_view.cmd 重新构建。");
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return;
#endif
        }

        QTimer::singleShot(0, this, [this] { processNextBlock(); });
    }

    ~ViewWindow() override {
#ifdef RING_RECON_VIEW_CUDA
        if (cudaH_[0]) ring_recon_cuda_destroy(cudaH_[0]);
        if (cudaH_[1]) ring_recon_cuda_destroy(cudaH_[1]);
#endif
    }

private:
    void processNextBlock() {
        if (k_ >= nBlocks_) {
            statusBar()->showMessage(
                QString("重建完成：%1 块，累计 %2 s").arg(nBlocks_).arg(totalMs_ / 1000.0, 0, 'f', 1));
            if (autoQuit_)
                QTimer::singleShot(100, qApp, &QCoreApplication::quit);
            return;
        }

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<double> raw, wl1, wl2, curWL2Last;
        if (!ringrecon::readRawBlock(dataPath_, cfg_.sampDepth, cfg_.wlOffset,
                                     cfg_.alinesPerBlock, k_, raw)) {
            statusBar()->showMessage("读取块失败：" + QString::number(k_ + 1));
            return;
        }
        ringrecon::splitBlock(raw, cfg_.sampDepth, nWlPerBlock_, cfg_.shiftWL2,
                              prevWL2Last_, std::vector<double>(), wl1, wl2, curWL2Last);
        prevWL2Last_ = curWL2Last;

        const double startDeg = cfg_.recon.theta0Deg + k_ * nWlPerBlock_ * dtheta_;
        for (int w = 0; w < 2; ++w) {
            const std::vector<double>& src = (w == 0 ? wl1 : wl2);
            std::vector<float> bscan = ringrecon::preprocessBlock(
                src, cfg_.sampDepth, nWlPerBlock_, cfg_.pre[w]);
            const int nt = static_cast<int>(bscan.size() / nWlPerBlock_);

            std::vector<float> img;
            if (engine_ == "cuda") {
#ifdef RING_RECON_VIEW_CUDA
                if (ring_recon_cuda_append(cudaH_[w], bscan.data(), nt, nWlPerBlock_,
                                           startDeg, spanDeg_) != 0) {
                    statusBar()->showMessage(QString("CUDA 错误：%1")
                                                 .arg(QString::fromUtf8(ring_recon_cuda_last_error())));
                    return;
                }
                std::vector<float> acc(static_cast<size_t>(nx_) * ny_);
                std::vector<float> accW(static_cast<size_t>(nx_) * ny_);
                ring_recon_cuda_get_state(cudaH_[w], acc.data(), accW.data(), nullptr);
                img.resize(acc.size());
                for (size_t i = 0; i < img.size(); ++i)
                    img[i] = acc[i] / std::max(accW[i], 1e-12f);
#endif
            } else {
                ringrecon::dasReconAppend(bscan, nt, nWlPerBlock_, cfg_.recon,
                                          startDeg, spanDeg_, xv_, yv_, st_[w]);
                img = ringrecon::normalizedImage(st_[w]);
            }

            labels_[w]->setPixmap(QPixmap::fromImage(toGrayImage(img, nx_, ny_)));
            labels_[w]->setToolTip(QString("波长 %1 第 %2 块").arg(w + 1).arg(k_ + 1));
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs_ += ms;
        ++k_;
        statusBar()->showMessage(QString("块 %1/%2 · 本块 %3 ms · 累计 %4 s")
                                     .arg(k_).arg(nBlocks_).arg(ms, 0, 'f', 1)
                                     .arg(totalMs_ / 1000.0, 0, 'f', 1));
        QTimer::singleShot(sleepMs_, this, [this] { processNextBlock(); });
    }

private:
    StreamConfig cfg_;
    std::string dataPath_;
    std::string engine_;
    int sleepMs_ = 50;
    bool autoQuit_ = false;

    QLabel* labels_[2] = {nullptr, nullptr};
    std::vector<float> xv_, yv_;
    int nx_ = 0, ny_ = 0, nBlocks_ = 0, nWlPerBlock_ = 0, nWlPerFrame_ = 0;
    double dtheta_ = 0.0, spanDeg_ = 0.0;
    int k_ = 0;
    double totalMs_ = 0.0;

    IncrementalState st_[2];
    void* cudaH_[2] = {nullptr, nullptr};
    std::vector<double> frameLastWL2_, prevWL2Last_;
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string dataPath = argStr(args, "--data",
        "D:\\zzx\\data\\20260716\\11.dat");
    const std::string engine = argStr(args, "--engine", "cpu");
    const int id = argInt(args, "--id", 11);
    const double gridMm = argDouble(args, "--grid-mm", 0.1);
    const int block = argInt(args, "--block", 200);
    const int sleepMs = argInt(args, "--sleep-ms", 50);
    const bool autoQuit = argInt(args, "--auto-quit", 0) != 0;

    if (engine != "cpu" && engine != "cuda") {
        QMessageBox::critical(nullptr, "参数错误", "--engine 只能是 cpu 或 cuda");
        return 2;
    }

    const DataSpec spec = specFor(id);
    StreamConfig cfg;
    cfg.sampDepth = spec.sampDepth;
    cfg.alinesPerFrame = spec.alinesPerFrame;
    cfg.alinesPerBlock = block;
    cfg.wlOffset = spec.wlOffset;
    cfg.shiftWL2 = true;
    cfg.recon.fs = 200e6;
    cfg.recon.c = 1490.0;
    cfg.recon.R = spec.radius;
    cfg.recon.fov = 36e-3;
    cfg.recon.gridSize = gridMm * 1e-3;
    cfg.recon.coverageDeg = spec.coverageDeg;
    cfg.recon.theta0Deg = 0.0;
    cfg.recon.fovDeg = spec.fovDeg;
    cfg.recon.fovTheta0Deg = 0.0;
    cfg.recon.distanceWeightExponent = 1.0;
    cfg.recon.maskOutOfRange = true;
    cfg.recon.interpolation = "linear";
    for (int w = 0; w < 2; ++w) {
        cfg.pre[w].systemDelay = (w == 0 ? spec.sysDelay1 : spec.sysDelay2);
        cfg.pre[w].dbrmaskExtra = (w == 1 ? spec.sysDelay2 - spec.sysDelay1 : 0);
        cfg.pre[w].maskLength = 300;
        cfg.pre[w].dbrRemove = true;
        cfg.pre[w].delayCut = true;
        cfg.pre[w].signalImpair = false;
        cfg.pre[w].imValue = (w == 0 ? 2000.0 : 400.0);
    }

    ViewWindow win(cfg, dataPath, engine, sleepMs, autoQuit);
    win.show();
    return app.exec();
}