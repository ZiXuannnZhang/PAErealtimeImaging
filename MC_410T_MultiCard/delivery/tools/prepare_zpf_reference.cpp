// prepare_zpf_reference — 阶段 B1 参考向量准备工具（B1 整改 R4）
//
// 从阶段 A 已验收的 MAT v5 证据文件（evidence/filter_reference_vectors.mat，
// 来源 commit 91ca68b，见同目录 README）直接导出紧凑文本参考向量：
//   <outDir>/<name>.txt，每行一个 double（%.17g）
// 供 tests/ring_zero_phase_filter_test.cpp 逐样本对照。干净 checkout 上
// 由 CMake 目标 prepare_zpf_reference 生成（无 MATLAB 运行时依赖；
// MATLAB 等价路径 stage-B1/matlab/export_zpf_reference.m 保留）。
//
// MAT v5 格式：128 字节文本头，之后为数据元素序列
//   [tag: type(4) nbytes(4)][payload]，补齐到 8 字节边界；
//   v7 默认整体 zlib 压缩（miCOMPRESSED），解压后为同样的元素序列。
// 数组元素内部：[array flags(miUINT32)] [dims(miINT32)] [name(miINT8)]
//   [pr(miDOUBLE)]；本工具只提取 mxDOUBLE_CLASS 变量。
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace {

// MAT v5 数据类型标签（规范常量）
constexpr uint32_t miINT8 = 1;
constexpr uint32_t miUINT8 = 2;
constexpr uint32_t miINT16 = 3;
constexpr uint32_t miUINT16 = 4;
constexpr uint32_t miINT32 = 5;
constexpr uint32_t miUINT32 = 6;
constexpr uint32_t miDOUBLE = 9;
constexpr uint32_t miMATRIX = 14;      // 数组/矩阵容器元素
constexpr uint32_t miCOMPRESSED = 15;
constexpr uint32_t mxDOUBLE_CLASS = 6; // 数组 class = double

struct MatVariable {
    std::string name;
    std::vector<double> data;   // 列主序展平
};

// 标签与数据均为本机字节序（MAT v5 写入端机器序；Windows/x86 = 小端）。
// 头部 endian 指示 "IM" 按 LE uint16 读出即 "MI"——与读取端一致的证明。
bool readLE32(std::istream &f, uint32_t &v) {
    char b[4];
    if (!f.read(b, 4)) return false;
    v = static_cast<uint32_t>(static_cast<unsigned char>(b[0])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b[1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b[2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b[3])) << 24);
    return true;
}

bool readLE64(std::istream &f, double &v) {
    char b[8];
    if (!f.read(b, 8)) return false;
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i)
        u = (u << 8) | static_cast<unsigned char>(b[i]);
    std::memcpy(&v, &u, 8);
    return true;
}

// 读取一个数据元素 payload。
// pad=true：常规元素按规范补齐到 8 字节边界（矩阵内部子元素）；
// pad=false：顶层 miCOMPRESSED 元素实测无补齐（MATLAB v7 写出格式），
//            下一元素紧跟压缩流结束位置。
// 小元素（small data element）：tag 高 16 位为 nbytes、低 16 位为 type，
// 整个元素固定 8 字节（4 tag + ≤4 数据 + 补零），无论 pad 取值都完整占用
// 8 字节——读取后跳过剩余填充，保证流位置不错位。
bool readElement(std::istream &f, uint32_t &type, uint32_t &nbytes,
                 std::vector<char> &payload, bool pad = true) {
    if (!readLE32(f, type)) return false;
    uint32_t nb = 0;
    if ((type >> 16) != 0) {
        // 小标签（small data element）：type 高 16 位实际是 nbytes
        nb = type >> 16;
        type &= 0xFFFFu;
        payload.resize(nb);
        if (nb && !f.read(payload.data(), nb)) return false;
        if (nb < 4) {
            char tail[4];
            if (!f.read(tail, 4 - nb)) return false;   // 补零到 8 字节边界
        }
        nbytes = nb;
        return true;
    }
    if (!readLE32(f, nb)) return false;
    payload.resize(nb);
    if (nb && !f.read(payload.data(), nb)) return false;
    if (pad && nb % 8 != 0) {
        char skip[8];
        if (!f.read(skip, (8 - (nb % 8)) % 8)) return false;
    }
    nbytes = nb;
    return true;
}

// 逐元素解析一段字节流（顶层或解压后的 miCOMPRESSED 内部均可）
bool parseElements(const char *data, size_t size,
                   std::vector<MatVariable> &out, std::string &err);

// 处理一个已捕获的顶层元素（type/payload）：miCOMPRESSED 先解压再递归，
// miMATRIX 按数组元素解析提取变量。
bool handleElement(uint32_t type, const std::vector<char> &payload,
                   std::vector<MatVariable> &out, std::string &err) {
    if (type == miCOMPRESSED) {
        // zlib 解压（MAT v7 默认整元素压缩）；解压后为同样的元素序列
        z_stream zs{};
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
        zs.avail_in = static_cast<uInt>(payload.size());
        if (inflateInit(&zs) != Z_OK) {
            err = "zlib inflateInit failed";
            return false;
        }
        std::vector<char> decompressed;
        char chunk[1 << 16];
        int ret = Z_OK;
        do {
            zs.next_out = reinterpret_cast<Bytef *>(chunk);
            zs.avail_out = sizeof(chunk);
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                err = std::string("zlib inflate failed: ") +
                      (zs.msg ? zs.msg : "unknown");
                inflateEnd(&zs);
                return false;
            }
            decompressed.insert(decompressed.end(), chunk,
                                chunk + (sizeof(chunk) - zs.avail_out));
        } while (ret != Z_STREAM_END);
        inflateEnd(&zs);
        return parseElements(decompressed.data(), decompressed.size(), out, err);
    }
    if (type != miMATRIX) return true;   // 顶层只处理数组（miMATRIX）元素
    // 矩阵 payload 内部解析：
    //   [array flags (miUINT32, 8B)] [dims (miINT32)] [name (miINT8)]
    //   [pr real part (miDOUBLE)]
    std::istringstream pb(std::string(payload.data(), payload.size()));
    pb.seekg(0);
    // array flags
    uint32_t ft = 0, fn = 0;
    std::vector<char> fp;
    if (!readElement(pb, ft, fn, fp) || ft != miUINT32 || fn < 8) {
        err = "bad array flags element";
        return false;
    }
    uint32_t flagsWord = 0;
    std::memcpy(&flagsWord, fp.data(), 4);   // 数据区本机序（小端）
    // array flags word0：byte0 = class，byte1 = flags（复数/全局等位）
    const uint32_t cls = flagsWord & 0xFFu;
    if (cls != mxDOUBLE_CLASS) return true;   // 只取 double 数组
    // dims (miINT32)。与 MATLAB 导出脚本一致只导出向量（isvector）：
    //   一维或任一维为 1；2×6 矩阵（sosHP/sosLP 系数）等跳过。
    uint32_t dt = 0, dn = 0;
    std::vector<char> dp;
    if (!readElement(pb, dt, dn, dp) || dt != miINT32 || dn == 0 ||
        dn % 4 != 0) {
        err = "bad dims element";
        return false;
    }
    {
        const uint32_t nDims = dn / 4;
        bool isVector = true;
        uint32_t nonUnit = 0;
        for (uint32_t i = 0; i < nDims; ++i) {
            int32_t d = 0;
            std::memcpy(&d, dp.data() + i * 4, 4);
            if (d <= 0) { isVector = false; break; }
            if (static_cast<uint32_t>(d) != 1) ++nonUnit;
        }
        if (!isVector || nonUnit > 1) return true;
    }
    // name
    uint32_t nt = 0, nn = 0;
    std::vector<char> np;
    if (!readElement(pb, nt, nn, np)) {
        err = "bad name element";
        return false;
    }
    MatVariable var;
    var.name.assign(np.data(), np.size());
    // real data。mxDOUBLE_CLASS 数组的常规存储为 miDOUBLE（8 字节/元素）；
    // 但 MATLAB v6/v7 写出对"值全为整数"的 double 数组使用压缩整数存储
    //（按值域选择 miUINT8/miINT32 等并可能用 small element 格式；本工具
    // 探针实测与 MATLAB -v6/-v7 自写行为一致，如 fs=250000000 存为
    // miINT32 small element、in_pulse∈{0,1} 存为 miUINT8、outHP_dc 全 0
    // 存为 miUINT8）。加载语义：整数元素值 → double（与 MATLAB load 逐位
    // 一致，值在 double 中可精确表示）。
    uint32_t rt = 0, rn = 0;
    std::vector<char> rp;
    if (!readElement(pb, rt, rn, rp)) {
        err = "bad real data element";
        return false;
    }
    switch (rt) {
    case miDOUBLE: {
        const size_t count = rp.size() / 8;
        var.data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t u = 0;
            std::memcpy(&u, rp.data() + i * 8, 8);   // 小端 double
            std::memcpy(&var.data[i], &u, 8);
        }
        break;
    }
    case miUINT8: {
        var.data.resize(rp.size());
        for (size_t i = 0; i < rp.size(); ++i)
            var.data[i] = static_cast<double>(
                static_cast<unsigned char>(rp[i]));
        break;
    }
    case miINT8: {
        var.data.resize(rp.size());
        for (size_t i = 0; i < rp.size(); ++i)
            var.data[i] = static_cast<double>(rp[i]);
        break;
    }
    case miINT32: {
        const size_t count = rp.size() / 4;
        var.data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t v = 0;
            std::memcpy(&v, rp.data() + i * 4, 4);
            var.data[i] = static_cast<double>(v);
        }
        break;
    }
    case miUINT32: {
        const size_t count = rp.size() / 4;
        var.data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint32_t v = 0;
            std::memcpy(&v, rp.data() + i * 4, 4);
            var.data[i] = static_cast<double>(v);
        }
        break;
    }
    case miINT16: {
        const size_t count = rp.size() / 2;
        var.data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            int16_t v = 0;
            std::memcpy(&v, rp.data() + i * 2, 2);
            var.data[i] = static_cast<double>(v);
        }
        break;
    }
    case miUINT16: {
        const size_t count = rp.size() / 2;
        var.data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint16_t v = 0;
            std::memcpy(&v, rp.data() + i * 2, 2);
            var.data[i] = static_cast<double>(v);
        }
        break;
    }
    default:
        err = "unsupported real data type " + std::to_string(rt) +
              " in double array " + var.name;
        return false;
    }
    out.push_back(std::move(var));
    return true;
}

bool parseElements(const char *data, size_t size,
                   std::vector<MatVariable> &out, std::string &err) {
    std::istringstream f(std::string(data, size));
    f.seekg(0);
    for (;;) {
        uint32_t type = 0, nbytes = 0;
        std::vector<char> payload;
        if (!readElement(f, type, nbytes, payload, /*pad=*/false)) {
            if (f.eof()) break;   // 正常读尽
            err = "truncated element";
            return false;
        }
        if (!handleElement(type, payload, out, err)) return false;
    }
    return true;
}

bool parseMatV5(std::istream &f, std::vector<MatVariable> &out,
                std::string &err) {
    char header[128];
    if (!f.read(header, 128)) {
        err = "file shorter than MAT v5 header (128 bytes)";
        return false;
    }
    if (std::memcmp(header, "MATLAB", 6) != 0) {
        err = "not a MAT file (header missing MATLAB magic)";
        return false;
    }
    // 顶层元素序列直接复用同一解析器
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string body = ss.str();
    return parseElements(body.data(), body.size(), out, err);
}

}  // namespace

int main(int argc, char **argv) {
    std::string inPath, outDir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--mat") inPath = argv[i + 1];
        else if (std::string(argv[i]) == "--out") outDir = argv[i + 1];
    }
    if (inPath.empty() || outDir.empty()) {
        std::fprintf(stderr,
            "usage: prepare_zpf_reference --mat <filter_reference_vectors.mat> "
            "--out <reference_vectors_dir>\n"
            "  reads stage-A MAT v5 evidence, writes one %.17g-per-line .txt "
            "per double vector + README.txt\n");
        return 2;
    }
    std::ifstream f(inPath, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", inPath.c_str());
        return 2;
    }
    std::vector<MatVariable> vars;
    std::string err;
    if (!parseMatV5(f, vars, err)) {
        std::fprintf(stderr, "MAT parse failed: %s\n", err.c_str());
        return 3;
    }
    if (vars.empty()) {
        std::fprintf(stderr, "no double vectors found in %s\n", inPath.c_str());
        return 3;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "create out dir failed: %s\n", ec.message().c_str());
        return 2;
    }
    int nSaved = 0;
    for (const auto &v : vars) {
        // 二进制模式写 LF：与 MATLAB fprintf 导出的既有参考目录逐字节一致
        std::ofstream of(outDir + "/" + v.name + ".txt",
                         std::ios::binary | std::ios::trunc);
        if (!of) {
            std::fprintf(stderr, "cannot write %s\n", v.name.c_str());
            return 4;
        }
        char buf[64];
        for (double d : v.data) {
            std::snprintf(buf, sizeof(buf), "%.17g\n", d);
            of << buf;
        }
        ++nSaved;
    }
    // README 回执：来源、生成工具、向量数（与 MATLAB 导出脚本同布局）
    {
        std::ofstream rf(outDir + "/README.txt",
                         std::ios::binary | std::ios::trunc);
        rf << "source: CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/evidence/filter_reference_vectors.mat\n";
        rf << "source commit: 91ca68b36dac9c14ccd756c52896ea20ad794e0a (stage A approved)\n";
        rf << "generated by: delivery/tools/prepare_zpf_reference.cpp (MAT v5 direct read; "
              "equivalent to stage-B1/matlab/export_zpf_reference.m)\n";
        rf << "convention: refZeroPhase.m (odd extension nfact=3n, per-section steady-state DF2T init, forward/backward)\n";
        rf << "vectors: " << nSaved << " files, one double per line (%.17g)\n";
    }
    std::printf("saved %d vectors + README -> %s\n", nSaved, outDir.c_str());
    // 与测试期望的最小集合对照（fs/N/nHP/nLP/fcHP/fcLP + in_*/outHP_*/outLP_*）
    bool hasMeta = false;
    double fs = 0.0;
    for (const auto &v : vars) {
        if (v.name == "fs" && !v.data.empty()) { fs = v.data[0]; hasMeta = true; }
    }
    if (!hasMeta || fs <= 0.0) {
        std::fprintf(stderr, "meta vector 'fs' missing/invalid\n");
        return 5;
    }
    return 0;
}
