#include "ring_recon.h"
#include "ring_recon_cuda.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
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

static void testDbrBounds() {
    constexpr int depth = 5;
    const std::vector<double> raw{1,2,3,4,5, 11,12,13,14,15};
    ringrecon::PreprocessParams p;
    p.delayCut = false;
    p.maskLength = 3;
    for (int rows : {-1, 0, 2, depth, depth + 1}) {
        p.dbrmaskExtra = rows - p.maskLength;
        const int expected = std::clamp(rows, 0, depth);
        need(ringrecon::dbrZeroRows(depth, p.maskLength, p.dbrmaskExtra) == expected, "DBR boundary");
        const auto out = ringrecon::preprocessBlock(raw, depth, 2, p);
        need(out.size() == raw.size(), "DBR batch shape");
        for (size_t k = 0; k < raw.size(); ++k)
            need(out[k] == (int(k % depth) < expected ? 0.f : float(raw[k])), "DBR touched the wrong sample");
    }
    need(ringrecon::dbrZeroRows(depth, std::numeric_limits<int>::max(), std::numeric_limits<std::int64_t>::max()) == depth,
         "DBR positive wide limit");
    need(ringrecon::dbrZeroRows(depth, std::numeric_limits<int>::min(), std::numeric_limits<std::int64_t>::min()) == 0,
         "DBR negative wide limit");
    need(ringrecon::preprocessBlock(raw, 0, 2, p).empty(), "accepted zero DBR depth");
    need(ringrecon::preprocessBlock(raw, depth, -1, p).empty(), "accepted negative DBR count");
    need(ringrecon::preprocessBlock(raw, depth, 3, p).empty(), "accepted undersized DBR input");
    p.delayCut = true;
    p.systemDelay = 0;
    need(ringrecon::preprocessBlock(raw, depth, 2, p).empty(), "accepted invalid 1-based cut");
    p.systemDelay = depth + 1;
    need(ringrecon::preprocessBlock(raw, depth, 2, p).empty(), "accepted cut beyond depth");
}

static void testVariableDepth(bool splice, bool linear, bool uniform, bool hostOnly) {
    constexpr double pi = 3.14159265358979323846;
    constexpr int depth = 64, nd = 4;
    const float angles[nd]{20, 65, 205, 285};
    const float radii[nd]{.003f, .003f, .004f, .004f};
    const float sectors[nd]{0, 0, 180, 180};
    const int delays[nd]{1, 1, uniform ? 1 : 38, uniform ? 1 : 38};
    std::vector<std::vector<float>> lines;
    for (int j = 0; j < nd; ++j) {
        std::vector<double> raw(depth);
        for (int i = 0; i < depth; ++i) raw[i] = 100 * (j + 1) + i;
        ringrecon::PreprocessParams p;
        p.dbrRemove = false;
        p.systemDelay = delays[j];
        lines.push_back(ringrecon::preprocessBlock(raw, depth, 1, p));
    }
    ringrecon::DasSampleBatch b;
    need(!ringrecon::packDasSamples({{1,2},{3}}, b), "accepted DAS line shorter than two samples");
    need(ringrecon::packDasSamples(lines, b), "failed to pack DAS lines");
    need(b.stride == depth && b.samples.size() == depth * nd, "incorrect packed stride");
    need(uniform ? b.validNt.empty() : b.validNt.size() == nd, "wrong uniform/variable path");
    int validNt[nd];
    for (int j = 0; j < nd; ++j) {
        const int length = depth - delays[j] + 1;
        validNt[j] = uniform ? b.stride : b.validNt[j];
        need(validNt[j] == length, "incorrect retained line length");
        for (int i = 0; i < length; ++i)
            need(b.samples[j * b.stride + i] == 100 * (j + 1) + delays[j] - 1 + i, "sample belongs to wrong A-line");
        // Nonzero padding must not enter either interpolation mode.
        std::fill(b.samples.begin() + j * b.stride + length, b.samples.begin() + (j + 1) * b.stride, 99999.f);
    }
    if (hostOnly) return;

    RingReconCudaConfig cfg;
    ring_recon_cuda_set_defaults(&cfg);
    cfg.sampDepth = depth; cfg.daqHz = 1e7; cfg.fov = .004; cfg.gridSize = .0005;
    cfg.soundSpeedRadiiCount = 0; cfg.soundSpeedsCount = 1; cfg.soundSpeeds[0] = 1490;
    cfg.interpolation = linear ? 0 : 1;
    cfg.spliceMode = splice;
    cfg.spliceBlendDeg = 0;
    cfg.sectorStartDeg = 0;
    cfg.enabledChannelCount = 2;
    std::vector<float> x, y;
    ringrecon::makeGrid(cfg.fov, cfg.gridSize, x, y);
    void* rawHandle = nullptr;
    need(!ring_recon_cuda_create(&cfg, &rawHandle), ring_recon_cuda_last_error());
    auto handle = std::unique_ptr<void, decltype(&ring_recon_cuda_destroy)>(rawHandle, ring_recon_cuda_destroy);
    const float* mask = splice ? sectors : nullptr;
    const auto append = [&](const int* lengths) {
        return ring_recon_cuda_append_variable_depth(handle.get(), b.samples.data(), b.stride, nd,
                                                      lengths, angles, radii, mask, 180);
    };
    const int badNt[nd]{depth, depth, depth + 1, depth};
    need(append(badNt) != 0, "accepted invalid per-line length");
    need(!append(validNt), ring_recon_cuda_last_error());
    std::vector<float> acc(x.size() * y.size()), weight(acc.size());
    int blocks = 0;
    need(!ring_recon_cuda_get_state(handle.get(), acc.data(), weight.data(), &blocks) && blocks == 1,
         "incorrect append count after rejecting invalid batch");
    bool hitLast = false, missedShort = false;
    const double arc = (angles[nd-1] - angles[0]) / double(nd-1) * pi / 180;
    for (size_t ix = 0; ix < x.size(); ++ix) for (size_t iy = 0; iy < y.size(); ++iy) {
        double sum = 0, sumW = 0;
        for (int j = 0; j < nd; ++j) {
            if (splice && ((y[iy] < 0) != (sectors[j] == 180))) continue;
            const double a = angles[j] * pi / 180, dx = x[ix] - radii[j] * cos(a), dy = y[iy] - radii[j] * sin(a);
            const double d = hypot(dx, dy), tf = d * cfg.daqHz / cfg.soundSpeeds[0];
            const int i = int(floor(tf + (linear ? 0 : .5)));
            const double w = -arc * (dx * cos(a) + dy * sin(a)) / d;
            if (i + int(linear) < validNt[j]) {
                sum += w * (100 * (j + 1) + delays[j] - 1 + (linear ? tf : i));
                hitLast |= i + int(linear) == validNt[j] - 1;
            } else if (validNt[j] < depth) missedShort = true;
            sumW += fabs(w);
        }
        const size_t k = ix * y.size() + iy;
        need(fabs(acc[k] - sum) < .003 && fabs(weight[k] - sumW) < 2e-5, "variable-depth DAS disagrees with per-line oracle");
    }
    if (!uniform) need(hitLast && missedShort, "fixture missed the short-line interpolation boundary");
    if (uniform) {
        need(!ring_recon_cuda_reset(handle.get()), ring_recon_cuda_last_error());
        const int rc = mask ? ring_recon_cuda_append_angles_radii_sector(handle.get(), b.samples.data(), depth, nd, angles, radii, mask, 180)
                            : ring_recon_cuda_append_angles_radii(handle.get(), b.samples.data(), depth, nd, angles, radii);
        need(!rc, ring_recon_cuda_last_error());
        auto legacyAcc = acc, legacyWeight = weight;
        need(!ring_recon_cuda_get_state(handle.get(), legacyAcc.data(), legacyWeight.data(), nullptr), ring_recon_cuda_last_error());
        need(legacyAcc == acc && legacyWeight == weight, "uniform-depth DAS changed numerically");
    }
}

int main(int argc, char** argv) {
    try {
        const bool hostOnly = argc == 2 && std::string(argv[1]) == "--host-only";
        testDbrBounds();
        for (bool splice : {false, true}) for (bool linear : {false, true})
            for (bool uniform : {false, true}) testVariableDepth(splice, linear, uniform, hostOnly);
        std::cout << "DBR masking / DAS line layout PASS\n";
        if (hostOnly) return 0;
        run(.00390625); // exactly coincident detector/pixel
        run(.0038);     // distance below one grid step, without MinDistance clamping
        std::cout << "DAS geometric weights / zero distance / reserved ABI PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
