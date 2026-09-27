// ring_enhancer_bench — 环形重建增强算子微基准（预算测试 Layer 1，任务文档
// TASKS/环形增强实时预算测试_20260927-141217.md §4；只测不改生产代码）。
//
// 矩阵：开关 off/bipolar/freq/both × 长度 3830/12330 × Hmax 2/20（fc=10, order=2）。
// 数据源：优先实录 .dat 列（readColumn 列式读取，8 字节 double，sampDepth=4000，
// wlOffset=301 即 14.dat 口径；3830 = 4000−171+1，对应 DelayCut(sysDelay=171) 后
// 保留长度）；长度超过实录列深（12330 > 4000）时用合成数据（高斯导数 N 形脉冲 +
// 带限随机相位噪声 ≤60 MHz，RMS 与实录列对齐），来源在输出行标注。
// 计时：steady_clock；每组预热 ≥20 次、正式 ≥5 轮，输出各轮均值的均值/中位/极差。
// 内置语义断言沿 ring_signal_enhancer_test：off 恒等（memcmp）、常数线不变、
// p=t 斜率抵消、常数线 DC 增益 ×Hmax、H(f) 单调与 H(0)=Hmax。
//
// 用法：ring_enhancer_bench [--data <14.dat>] [--rounds 5] [--warmup 20]
//                           [--lines 64] [--fs-hz 250e6]
#include "RingSignalEnhancer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kSampDepth = 4000;
constexpr int kWlOffset = 301;     // 14.dat 口径（id=14）
constexpr int kLenDefault = 3830;  // 4000 - 171 + 1（DelayCut 后保留长度）
constexpr int kLenLarge = 12330;
constexpr double kPi = 3.14159265358979323846;

std::string argStr(const std::vector<std::string> &args, const std::string &key,
                   const std::string &def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return args[i + 1];
    return def;
}
int argInt(const std::vector<std::string> &args, const std::string &key, int def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stoi(args[i + 1]);
    return def;
}
double argDouble(const std::vector<std::string> &args, const std::string &key,
                 double def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key) return std::stod(args[i + 1]);
    return def;
}

int failures = 0;
void require(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL  " << message << "\n";
    }
}

// ring_svc_selftest.cpp 的 readColumn 列式读取（8 字节 double/样本）。
bool readColumn(const std::string &path, int sampDepth, long long col0,
                std::vector<double> &col) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(col0 * static_cast<long long>(sampDepth) * 8);
    col.resize(static_cast<size_t>(sampDepth));
    f.read(reinterpret_cast<char *>(col.data()),
           static_cast<std::streamsize>(sampDepth) * 8);
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth) * 8;
}

// 高斯导数 N 形脉冲（正负双瓣）+ 随机相位带限噪声（0.5–60 MHz，低频加权）。
std::vector<float> syntheticLine(int length, double fs, double rmsTarget,
                                 unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    std::vector<float> line(static_cast<size_t>(length), 0.0f);
    const double sigma = 0.2e-6;                 // 0.2 µs 包络
    const double t0 = 0.25 * length / fs;
    for (int i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / fs;
        const double u = (t - t0) / sigma;
        line[static_cast<size_t>(i)] =
            static_cast<float>(-u * std::exp(-0.5 * u * u));
    }

    const int kTones = 64;
    std::vector<double> phase(static_cast<size_t>(kTones));
    std::vector<double> freq(static_cast<size_t>(kTones));
    std::vector<double> amp(static_cast<size_t>(kTones));
    for (int k = 0; k < kTones; ++k) {
        freq[static_cast<size_t>(k)] =
            0.5e6 + (60.0e6 - 0.5e6) * static_cast<double>(k) / (kTones - 1);
        phase[static_cast<size_t>(k)] = 2.0 * kPi * uniform(rng);
        amp[static_cast<size_t>(k)] =
            1.0 / (1.0 + freq[static_cast<size_t>(k)] / 10.0e6);  // 覆盖 H(f) 提升区
    }
    std::vector<double> noise(static_cast<size_t>(length), 0.0);
    for (int i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / fs;
        double v = 0.0;
        for (int k = 0; k < kTones; ++k)
            v += amp[static_cast<size_t>(k)] *
                 std::sin(2.0 * kPi * freq[static_cast<size_t>(k)] * t +
                          phase[static_cast<size_t>(k)]);
        noise[static_cast<size_t>(i)] = v;
    }

    double pulseSq = 0.0, noiseSq = 0.0;
    for (float v : line) pulseSq += static_cast<double>(v) * v;
    for (double v : noise) noiseSq += v * v;
    const double pulseRms = std::sqrt(pulseSq / std::max(1, length));
    const double noiseRms = std::sqrt(noiseSq / std::max(1, length));
    const double kPulseFrac = 0.8, kNoiseFrac = 0.2;   // 脉冲:噪声 ≈ 4:1 (RMS)
    const double pulseScale =
        pulseRms > 0.0 ? rmsTarget * kPulseFrac / pulseRms : 0.0;
    const double noiseScale =
        noiseRms > 0.0 ? rmsTarget * kNoiseFrac / noiseRms : 0.0;
    for (int i = 0; i < length; ++i) {
        line[static_cast<size_t>(i)] = static_cast<float>(
            line[static_cast<size_t>(i)] * pulseScale +
            noise[static_cast<size_t>(i)] * noiseScale);
    }
    return line;
}

struct CellResult {
    double medianUs = 0.0;
    double meanUs = 0.0;
    double rangeUs = 0.0;
};

CellResult benchConfig(RingSignalEnhancer &enhancer,
                       const std::vector<std::vector<float>> &lines,
                       int rounds, int warmup) {
    const size_t n = lines.size();
    std::vector<float> work(lines.front().size());
    std::vector<double> lineUs(lines.front().size());
    for (int w = 0; w < warmup; ++w) {
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(work.data(), lines[i].data(), work.size() * sizeof(float));
            enhancer.apply(work.data(), static_cast<int>(work.size()));
        }
    }
    std::vector<double> roundMeans;
    for (int r = 0; r < rounds; ++r) {
        double total = 0.0;
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(work.data(), lines[i].data(), work.size() * sizeof(float));
            const auto t0 = std::chrono::steady_clock::now();
            enhancer.apply(work.data(), static_cast<int>(work.size()));
            const auto t1 = std::chrono::steady_clock::now();
            lineUs[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
            total += lineUs[i];
        }
        roundMeans.push_back(total / static_cast<double>(n));
    }
    std::vector<double> sorted = roundMeans;
    std::sort(sorted.begin(), sorted.end());
    CellResult result;
    result.medianUs = sorted[sorted.size() / 2];
    double sum = 0.0;
    double lo = roundMeans.front(), hi = roundMeans.front();
    for (double v : roundMeans) {
        sum += v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    result.meanUs = sum / static_cast<double>(roundMeans.size());
    result.rangeUs = hi - lo;
    return result;
}

void emitCsv(const std::string &src, const std::string &sw, int length, double hmax,
             const CellResult &r, int rounds, int warmup) {
    std::cout << "CSV  " << src << ',' << sw << ',' << length << ',' << hmax
              << ',' << r.medianUs << ',' << r.meanUs << ',' << r.rangeUs
              << ',' << (r.medianUs * 1600.0 / 1000.0)    // 每块 ms（8 通道 × 200 根）
              << ',' << (r.medianUs * 8000.0 / 1000.0)    // 每圈外推 ms（alinesPerFrame=8000）
              << ',' << rounds << ',' << warmup << "\n";
}

void runAssertions(double fs) {
    // off：逐位恒等
    {
        RingSignalEnhancer enhancer;
        RingEnhanceConfig cfg;
        enhancer.setConfig(cfg, fs);
        std::vector<float> line{1.5f, -2.25f, 3.75f, 0.125f, 9.0f, -7.5f};
        const auto expected = line;
        require(enhancer.apply(line.data(), static_cast<int>(line.size())),
                "off apply succeeds");
        require(std::memcmp(line.data(), expected.data(),
                            line.size() * sizeof(float)) == 0,
                "off is bitwise identity (memcmp)");
    }
    // bipolar：常数线不变；p=t 斜率抵消（内部点 |g|<1e-7）
    {
        RingSignalEnhancer enhancer;
        RingEnhanceConfig cfg;
        cfg.enableBipolarCompensation = true;
        enhancer.setConfig(cfg, fs);
        std::vector<float> constant(256, 2.0f);
        require(enhancer.apply(constant.data(), static_cast<int>(constant.size())),
                "bipolar constant apply succeeds");
        for (float v : constant)
            require(std::abs(v - 2.0f) < 1e-6f,
                    "bipolar keeps constant A-line unchanged");
        std::vector<float> ramp(256);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = static_cast<float>(static_cast<double>(i) / fs);
        require(enhancer.apply(ramp.data(), static_cast<int>(ramp.size())),
                "bipolar ramp apply succeeds");
        for (size_t i = 1; i + 1 < ramp.size(); ++i)
            require(std::abs(ramp[i]) < 1e-7f, "bipolar cancels p=t slope");
    }
    // freq：常数线 DC 增益 ×Hmax（默认与最坏 Hmax 两档）
    for (double hmax : {2.0, 20.0}) {
        RingSignalEnhancer enhancer;
        RingEnhanceConfig cfg;
        cfg.enableFreqCompensation = true;
        cfg.freqCompFcMhz = 10.0;
        cfg.freqCompHmax = hmax;
        cfg.freqCompOrder = 2.0;
        enhancer.setConfig(cfg, fs);
        std::vector<float> constant(512, 3.25f);
        require(enhancer.apply(constant.data(), static_cast<int>(constant.size())),
                "freq constant apply succeeds");
        for (float v : constant)
            require(std::abs(v - static_cast<float>(3.25 * hmax)) < 1e-4f,
                    "freq gives constant A-line the DC gain xHmax");
    }
    // H(f) 形状：H(0)=Hmax、单调不增、趋于 1
    {
        RingEnhanceConfig cfg;
        cfg.freqCompFcMhz = 10.0;
        cfg.freqCompHmax = 2.0;
        cfg.freqCompOrder = 2.0;
        require(std::abs(ring_enhance::frequencyGain(0.0, cfg) - 2.0) < 1e-12,
                "H(0) equals Hmax");
        double previous = ring_enhance::frequencyGain(0.0, cfg);
        for (double f = 1e6; f <= 100e6; f += 1e6) {
            const double current = ring_enhance::frequencyGain(f, cfg);
            require(current <= previous + 1e-12, "H(f) monotonically decreasing");
            require(current >= 1.0, "H(f) stays >= 1");
            previous = current;
        }
        require(std::abs(ring_enhance::frequencyGain(100e6, cfg) - 1.0) < 1e-12,
                "H(f) tends to 1");
    }
}

struct Dataset {
    std::string src;
    int length;
    std::vector<std::vector<float>> lines;
};

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string dataPath = argStr(args, "--data");
    const int rounds = std::max(5, argInt(args, "--rounds", 5));
    const int warmup = std::max(20, argInt(args, "--warmup", 20));
    const int nLines = std::max(8, argInt(args, "--lines", 64));
    const double fs = argDouble(args, "--fs-hz", 250e6);
    std::cout << std::fixed << std::setprecision(4);

    runAssertions(fs);
    std::cout << (failures == 0 ? "ASSERT  all semantic assertions PASS"
                                : "ASSERT  assertions FAILED") << "\n";

    std::vector<Dataset> datasets;
    bool realOk = false;
    if (!dataPath.empty()) {
        // 实录列：wl1/wl2 各半，跨全帧均匀取样；RMS 估计用于合成对齐
        std::vector<std::vector<float>> realLines;
        double sq = 0.0;
        bool allOk = true;
        for (int i = 0; i < nLines; ++i) {
            const int wlIdx = (i / 2) * (kSampDepth / (nLines / 2));
            const int wl = (i % 2 == 0) ? 0 : 1;
            const long long col0 = kWlOffset - 1 + 2LL * wlIdx + wl;
            std::vector<double> col;
            if (!readColumn(dataPath, kSampDepth, col0, col)) { allOk = false; break; }
            std::vector<float> line(static_cast<size_t>(kSampDepth));
            for (int r = 0; r < kSampDepth; ++r)
                sq += col[static_cast<size_t>(r)] * col[static_cast<size_t>(r)];
            for (int r = 0; r < kSampDepth; ++r)
                line[static_cast<size_t>(r)] = static_cast<float>(col[static_cast<size_t>(r)]);
            realLines.push_back(std::move(line));
        }
        if (allOk && !realLines.empty()) {
            const double rms = std::sqrt(
                sq / static_cast<double>(realLines.size() * kSampDepth));
            // 3830 = 4000−171+1：丢弃前 170 样本（DelayCut sysDelay=171 的保留段）
            for (auto &l : realLines)
                l.erase(l.begin(), l.begin() + (kSampDepth - kLenDefault));
            datasets.push_back({"real-14dat-cols", kLenDefault, std::move(realLines)});
            // 长度 > 实录列深：只能合成（证据中注明）
            std::vector<std::vector<float>> synLarge;
            for (int i = 0; i < nLines; ++i)
                synLarge.push_back(syntheticLine(kLenLarge, fs, rms,
                                                 1000003u + static_cast<unsigned>(i)));
            datasets.push_back({"synthetic-Nwave-noise", kLenLarge, std::move(synLarge)});
            std::cout << "DATA  real=" << dataPath << " lines=" << nLines
                      << " rms=" << rms << "\n";
            realOk = true;
        } else {
            std::cout << "DATA  real=" << dataPath << " status=unreadable\n";
        }
    }
    if (!realOk) {
        // 无实录数据：全合成矩阵（证据中注明来源与理由）
        std::vector<std::vector<float>> synDefault, synLarge;
        for (int i = 0; i < nLines; ++i) {
            synDefault.push_back(syntheticLine(kLenDefault, fs, 100.0,
                                               1000003u + static_cast<unsigned>(i)));
            synLarge.push_back(syntheticLine(kLenLarge, fs, 100.0,
                                             2000003u + static_cast<unsigned>(i)));
        }
        datasets.push_back({"synthetic-Nwave-noise", kLenDefault, std::move(synDefault)});
        datasets.push_back({"synthetic-Nwave-noise", kLenLarge, std::move(synLarge)});
        std::cout << "DATA  source=synthetic-only (no real .dat provided/readable)\n";
    }

    struct Switch {
        const char *name;
        bool bipolar;
        bool freq;
    };
    const Switch switches[] = {
        {"off", false, false}, {"bipolar", true, false},
        {"freq", false, true}, {"both", true, true}};
    const double hmaxValues[] = {2.0, 20.0};
    std::cout << "CSV  src,switch,length,hmax,median_us,mean_us,range_us,"
                 "per_block_ms_1600al,per_round_ms_8000al,rounds,warmup\n";
    for (const auto &ds : datasets) {
        for (const auto &sw : switches) {
            for (double hmax : hmaxValues) {
                RingEnhanceConfig cfg;
                cfg.enableBipolarCompensation = sw.bipolar;
                cfg.enableFreqCompensation = sw.freq;
                cfg.freqCompFcMhz = 10.0;
                cfg.freqCompHmax = hmax;
                cfg.freqCompOrder = 2.0;
                RingSignalEnhancer enhancer;
                enhancer.setConfig(cfg, fs);
                const CellResult r = benchConfig(enhancer, ds.lines, rounds, warmup);
                emitCsv(ds.src, sw.name, ds.length, hmax, r, rounds, warmup);
            }
        }
    }

    std::cout << (failures == 0 ? "PASS  ring_enhancer_bench"
                                : "FAIL  ring_enhancer_bench") << "\n";
    return failures == 0 ? 0 : 1;
}
