// frame_diff — 逐帧 float32 比较（A3 口径）：逐位一致 / ±1ULP / 双精度残差 / 超界样本
// 用法: frame_diff <fileA> <fileB>   （A=参考，B=实际）
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

static int ulpDist(float a, float b) {
    int ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if (ia & 0x80000000) ia = 0x80000000 - ia;
    if (ib & 0x80000000) ib = 0x80000000 - ib;
    return ia > ib ? ia - ib : ib - ia;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cout << "usage: frame_diff <A> <B>" << std::endl;
        return 2;
    }
    std::ifstream fa(argv[1], std::ios::binary);
    std::ifstream fb(argv[2], std::ios::binary);
    if (!fa || !fb) {
        std::cout << "OPEN-FAIL" << std::endl;
        return 2;
    }
    fa.seekg(0, std::ios::end);
    fb.seekg(0, std::ios::end);
    const std::streamoff la = fa.tellg(), lb = fb.tellg();
    if (la != lb) {
        std::cout << "RESULT " << argv[1] << " DIFF-SIZE " << la << " vs " << lb << std::endl;
        return 2;
    }
    fa.seekg(0, std::ios::beg);
    fb.seekg(0, std::ios::beg);
    const size_t n = static_cast<size_t>(la) / 4;
    std::vector<float> a(n), b(n);
    fa.read(reinterpret_cast<char *>(a.data()), static_cast<std::streamsize>(la));
    fb.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(lb));
    double l1 = 0.0;
    for (size_t i = 0; i < n; ++i) l1 += std::fabs(static_cast<double>(b[i]));
    const double bound = 1e-12 * (l1 + 1.0);
    size_t bitwise = 0, ulp1 = 0, residue = 0, beyond = 0;
    double maxAbs = 0.0;
    std::cout << std::setprecision(10);
    for (size_t i = 0; i < n; ++i) {
        const float x = a[i], y = b[i];
        if (x == y) { ++bitwise; continue; }
        const int u = ulpDist(x, y);
        const double ad = std::fabs(static_cast<double>(x) - static_cast<double>(y));
        if (ad > maxAbs) maxAbs = ad;
        if (u <= 1) ++ulp1;
        else if (ad <= bound) ++residue;
        else {
            ++beyond;
            if (beyond <= 10)
                std::cout << "  BEYOND idx=" << i << " ref=" << x << " act=" << y
                          << " ulp=" << u << " abs=" << ad << std::endl;
        }
    }
    std::cout << "RESULT " << argv[1] << " n=" << n << " bitwise=" << bitwise
              << " ulp1=" << ulp1 << " residue=" << residue << " beyond=" << beyond
              << " max_abs=" << maxAbs << " bound=" << bound
              << (beyond == 0 ? " OK" : " VIOLATION") << std::endl;
    return beyond == 0 ? 0 : 1;
}
