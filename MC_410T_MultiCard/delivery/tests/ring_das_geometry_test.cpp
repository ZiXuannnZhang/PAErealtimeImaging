#include "ring_recon.h"
#include "ring_recon_cuda.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

static void need(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Independent double-precision geometric oracle, with a linear time signal:
// interpolation is exact, and the sensor amplitude distinguishes distance/window weights.
static void run(double radius) {
    constexpr int nt = 256, nd = 6;
    constexpr double pi = 3.14159265358979323846;
    ringrecon::ReconParams p;
    p.fs = 1e7; p.R = radius; p.fov = .015625; p.gridSize = .0018;
    std::vector<float> x, y, signal(nt * nd);
    ringrecon::makeGrid(p.fov, p.gridSize, x, y);
    for (int j = 0; j < nd; ++j)
        for (int t = 0; t < nt; ++t) signal[t + j * nt] = float(.01 * t + .3 * j + 1);
    std::vector<float> expected(x.size() * y.size());
    double oldDifference = 0;
    for (size_t ix = 0; ix < x.size(); ++ix) for (size_t iy = 0; iy < y.size(); ++iy) {
        double sum = 0, weight = 0, oldSum = 0, oldWeight = 0;
        for (int j = 0; j < nd; ++j) {
            double a = j * pi / 3, sx = radius * cos(a), sy = radius * sin(a);
            double dx = x[ix] - sx, dy = y[iy] - sy, distance = hypot(dx, dy);
            if (distance <= 1e-12) continue;
            double w = -(dx * cos(a) + dy * sin(a)) / distance;
            double value = .01 * distance * p.fs / p.c + .3 * j + 1;
            sum += w * value; weight += fabs(w);
            oldSum += w * value / distance; oldWeight += fabs(w) / distance;
        }
        expected[ix * y.size() + iy] = float(sum / weight);
        oldDifference = std::max(oldDifference, fabs(sum / weight - oldSum / oldWeight));
    }
    need(oldDifference > .05, "oracle does not distinguish retired distance weighting");
    ringrecon::IncrementalState state;
    ringrecon::dasReconAppend(signal, nt, nd, p, 0, 300, x, y, state);
    auto cpu = ringrecon::normalizedImage(state);
    RingReconCudaConfig cfg;
    ring_recon_cuda_set_defaults(&cfg);
    cfg.fov = p.fov; cfg.gridSize = p.gridSize; cfg.radius = radius; cfg.daqHz = p.fs;
    cfg.soundSpeedRadiiCount = 0; cfg.soundSpeedsCount = 1; cfg.soundSpeeds[0] = p.c;
    cfg.sampDepth = nt;
    std::vector<float> baseline;
    for (bool obsoleteValues : {false, true}) {
        cfg.reservedApod = obsoleteValues ? 2 : 0;
        cfg.reservedDistanceWeight = obsoleteValues ? 7 : 0;
        cfg.reservedMinDistance = obsoleteValues ? .1 : 0;
        void* handle = nullptr;
        need(!ring_recon_cuda_create(&cfg, &handle), ring_recon_cuda_last_error());
        need(!ring_recon_cuda_append(handle, signal.data(), nt, nd, 0, 300), ring_recon_cuda_last_error());
        std::vector<float> gpu(expected.size());
        need(!ring_recon_cuda_snapshot(handle, int(x.size()), 1, gpu.data()), ring_recon_cuda_last_error());
        ring_recon_cuda_destroy(handle);
        double error = 0;
        for (size_t i = 0; i < expected.size(); ++i) {
            need(std::isfinite(cpu[i]) && std::isfinite(gpu[i]), "nonfinite at detector");
            error = std::max({error, fabs(double(cpu[i]) - expected[i]), fabs(double(gpu[i]) - expected[i])});
        }
        std::cout << "radius=" << radius << " obsolete=" << obsoleteValues << " max error=" << error << '\n';
        need(error < 5e-4, "CPU/CUDA geometry disagrees with independent oracle");
        if (obsoleteValues) need(gpu == baseline, "reserved ABI values changed DAS");
        else baseline = gpu;
    }
}

int main() {
    try {
        run(.00390625); // exactly coincident detector/pixel
        run(.0038);     // distance below one grid step, without MinDistance clamping
        std::cout << "DAS geometric weights / zero distance / reserved ABI PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
