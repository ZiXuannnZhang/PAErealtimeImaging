// 电机参数联动（LabVIEW → 本程序）单元测试：
//   1) motor_scan_linkage::parseParamsJson  —— 参数文件 JSON 解析/取值约束；
//   2) deriveTotalAlines / totalAlinesConsistent —— 单圈总A-line数换算规则；
//   3) MotorScanLinkageMonitor —— 轮询解析/变化检测/状态语义；
//   4) MotorScanLinkageMonitor 触发闩锁 —— 闸门冻结/补发/开关语义；
//   5) RingBlockAssembler sectorCcw —— 顺时针扫描的角度镜像与扇区回绕。
// 内轮语义断言（无外部测试框架），与 ring_block_assembler_test 同口径。
#include "MotorScanLinkage.h"
#include "RingBlockAssembler.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Runner {
    bool ok = true;
    std::string test;

    void check(bool condition, const std::string &message)
    {
        if (!condition) {
            ok = false;
            std::cerr << "FAIL " << test << ": " << message << '\n';
        }
    }
};

using motor_scan_linkage::Params;
using motor_scan_linkage::ParseStatus;

void testParseParamsJson(Runner &r)
{
    r.test = "parse_params_json";
    {
        Params p;
        QString err;
        const QByteArray json =
            "{\"formatVersion\":1,\"seq\":3,\"direction\":\"CCW\","
            "\"xStepsNum\":2000,\"xStepsPerPeriod\":2,"
            "\"writtenAt\":\"2026-10-14T15:00:00.000\","
            "\"source\":\"LabVIEW:pact-PC-RFSG_V8b_20260618.vi\"}";
        const ParseStatus st = motor_scan_linkage::parseParamsJson(json, &p, &err);
        r.check(st == ParseStatus::Ok, "valid json should parse");
        r.check(p.hasDirection && p.directionCcw, "CCW -> directionCcw=true");
        r.check(p.hasXStepsNum && p.xStepsNum == 2000, "xStepsNum=2000");
        r.check(p.xStepsPerPeriod == 2, "xStepsPerPeriod=2");
        r.check(p.seq == 3, "seq=3");
    }
    {
        // 中文方向与 CW/顺
        Params p;
        const ParseStatus st = motor_scan_linkage::parseParamsJson(
            "{\"formatVersion\":1,\"direction\":\"顺\",\"xStepsNum\":100}",
            &p, nullptr);
        r.check(st == ParseStatus::Ok, "chinese direction should parse");
        r.check(p.hasDirection && !p.directionCcw, "顺 -> directionCcw=false");
        r.check(p.hasXStepsNum && p.xStepsNum == 100, "xStepsNum=100");
    }
    {
        Params p;
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"formatVersion\":1,\"direction\":\"逆\"}", &p, nullptr)
                    == ParseStatus::Ok,
                "direction-only file is valid");
        r.check(p.hasDirection && p.directionCcw && !p.hasXStepsNum,
                "direction-only flags");
    }
    {
        // formatVersion 缺失/不支持
        Params p;
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"direction\":\"CCW\",\"xStepsNum\":1}", &p, nullptr)
                    == ParseStatus::BadFormat,
                "missing formatVersion rejected");
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"formatVersion\":2,\"direction\":\"CCW\"}", &p, nullptr)
                    == ParseStatus::BadFormat,
                "formatVersion 2 rejected");
        // 非法 direction
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"formatVersion\":1,\"direction\":\"left\"}", &p, nullptr)
                    == ParseStatus::BadFormat,
                "invalid direction rejected");
        // 非法 xStepsNum
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"formatVersion\":1,\"xStepsNum\":0}", &p, nullptr)
                    == ParseStatus::BadFormat,
                "xStepsNum=0 rejected");
        // 两者都缺
        r.check(motor_scan_linkage::parseParamsJson(
                    "{\"formatVersion\":1,\"seq\":1}", &p, nullptr)
                    == ParseStatus::BadFormat,
                "empty linkage rejected");
        // JSON 语法错误
        r.check(motor_scan_linkage::parseParamsJson("{oops", &p, nullptr)
                    == ParseStatus::ParseFailed,
                "syntax error rejected");
    }
}

void testDeriveTotalAlines(Runner &r)
{
    r.test = "derive_total_alines";
    using motor_scan_linkage::TotalAlinesRule;
    // 现场口径：XStepsNum=2000、启用通道=2 → 4000（与既有实机配置一致）
    r.check(motor_scan_linkage::deriveTotalAlines(
                TotalAlinesRule::PerEnabledChannel, 2000, 2, 2) == 4000,
            "perEnabledChannel: 2000*2=4000");
    r.check(motor_scan_linkage::deriveTotalAlines(
                TotalAlinesRule::PerPeriod, 2000, 2, 2) == 4000,
            "perPeriod: 2000*2=4000");
    r.check(motor_scan_linkage::deriveTotalAlines(
                TotalAlinesRule::DirectionOnly, 2000, 2, 2) == 0,
            "directionOnly: no derivation");
    r.check(motor_scan_linkage::deriveTotalAlines(
                TotalAlinesRule::PerEnabledChannel, 2000, 2, 0) == 0,
            "no channels -> no derivation");
    r.check(motor_scan_linkage::deriveTotalAlines(
                TotalAlinesRule::PerPeriod, 2000, 0, 2) == 0,
            "perPeriod without period -> no derivation");

    // totalAlinesConsistent：与 RingConfigDialog::applyConfig 换算约束同口径
    r.check(motor_scan_linkage::totalAlinesConsistent(4000, 2),
            "4000 with 2 channels consistent");
    r.check(!motor_scan_linkage::totalAlinesConsistent(4001, 2),
            "4001 not divisible by 4");
    r.check(!motor_scan_linkage::totalAlinesConsistent(2, 2),
            "K too small (2/(2*2)=0)");
    r.check(!motor_scan_linkage::totalAlinesConsistent(4, 2),
            "K=1 too small");

    // 规则键回退：未知键回退 perEnabledChannel
    using motor_scan_linkage::totalAlinesRuleFromKey;
    r.check(totalAlinesRuleFromKey(QStringLiteral("perPeriod"))
                == TotalAlinesRule::PerPeriod, "perPeriod key");
    r.check(totalAlinesRuleFromKey(QStringLiteral("directionOnly"))
                == TotalAlinesRule::DirectionOnly, "directionOnly key");
    r.check(totalAlinesRuleFromKey(QStringLiteral("bogus"))
                == TotalAlinesRule::PerEnabledChannel, "unknown key falls back");
}

// 轮询监视器：手写参数文件 → tick → 捕获信号（不进事件循环，直接驱动）。
void testMonitor(Runner &r)
{
    r.test = "monitor";
    QTemporaryDir dir;
    r.check(dir.isValid(), "temp dir");
    const QString path = dir.filePath(QStringLiteral("motor_scan_params.json"));

    MotorScanLinkageMonitor::Settings st;
    st.paramsFile = path;
    st.pollMs = 500;
    MotorScanLinkageMonitor monitor(st);

    int changedCount = 0;
    Params lastCaptured;
    QString lastStatus;
    QObject::connect(&monitor, &MotorScanLinkageMonitor::linkageParamsChanged,
                     [&](const Params &p) { ++changedCount; lastCaptured = p; });
    QObject::connect(&monitor, &MotorScanLinkageMonitor::linkageStatusChanged,
                     [&](const QString &s) { lastStatus = s; });

    // 文件缺失：状态提示，无参数信号
    monitor.tick();
    r.check(changedCount == 0, "missing file: no params signal");
    r.check(lastStatus.contains(QStringLiteral("未联动")),
            "missing file status mentions 未联动");

    // 首次出现：CCW + 2000
    {
        QFile f(path);
        r.check(f.open(QIODevice::WriteOnly), "write params file");
        f.write("{\"formatVersion\":1,\"seq\":1,\"direction\":\"CCW\",\"xStepsNum\":2000}");
        f.close();
    }
    monitor.tick();
    r.check(changedCount == 1, "first appearance emits once");
    r.check(lastCaptured.hasDirection && lastCaptured.directionCcw, "CCW captured");
    r.check(lastCaptured.hasXStepsNum && lastCaptured.xStepsNum == 2000, "2000 captured");
    r.check(lastStatus.contains(QStringLiteral("已联动")), "linked status");

    // 相同内容重复 tick：不再发信号（内容一致）
    monitor.tick();
    r.check(changedCount == 1, "unchanged params: no extra signal");

    // 方向翻转：顺
    {
        QFile f(path);
        r.check(f.open(QIODevice::WriteOnly), "rewrite params file");
        f.write("{\"formatVersion\":1,\"seq\":2,\"direction\":\"CW\",\"xStepsNum\":2000}");
        f.close();
    }
    monitor.tick();
    r.check(changedCount == 2, "direction flip emits");
    r.check(lastCaptured.hasDirection && !lastCaptured.directionCcw, "CW captured");

    // directionInverted：写入方布尔语义相反
    MotorScanLinkageMonitor::Settings st2;
    st2.paramsFile = path;
    st2.directionInverted = true;
    MotorScanLinkageMonitor monitor2(st2);
    Params captured2;
    QObject::connect(&monitor2, &MotorScanLinkageMonitor::linkageParamsChanged,
                     [&](const Params &p) { captured2 = p; });
    monitor2.tick();
    r.check(captured2.hasDirection && captured2.directionCcw,
            "inverted: CW file read as CCW");
}

// 触发闩锁：闸门关闭时冻结应用，开闸后补发一次（且只补发一次）。
void testMonitorTriggerLatch(Runner &r)
{
    r.test = "monitor_trigger_latch";

    auto writeParams = [](const QString &path, const char *json) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(json);
        f.close();
        return true;
    };

    QTemporaryDir dir;
    r.check(dir.isValid(), "temp dir");
    const QString path = dir.filePath(QStringLiteral("motor_scan_params.json"));

    MotorScanLinkageMonitor::Settings st;
    st.paramsFile = path;
    st.triggerLatch = true;
    MotorScanLinkageMonitor monitor(st);

    int changedCount = 0;
    Params lastCaptured;
    QObject::connect(&monitor, &MotorScanLinkageMonitor::linkageParamsChanged,
                     [&](const Params &p) { ++changedCount; lastCaptured = p; });

    // 1) 闸门默认开：首次读到即应用（与既有行为一致）
    r.check(writeParams(path,
        "{\"formatVersion\":1,\"seq\":1,\"direction\":\"CCW\",\"xStepsNum\":2000}"),
        "write initial params");
    monitor.tick();
    r.check(changedCount == 1, "gate open: first read applies");
    r.check(lastCaptured.hasXStepsNum && lastCaptured.xStepsNum == 2000,
            "gate open: 2000 applied");

    // 2) 冻结（模拟收到本轮首个触发）：文件改动不再应用，但记为待应用
    monitor.setApplyGate(false);
    r.check(!monitor.applyGateOpen(), "gate reports frozen");
    r.check(writeParams(path,
        "{\"formatVersion\":1,\"seq\":2,\"direction\":\"CW\",\"xStepsNum\":3000}"),
        "write frozen params");
    monitor.tick();
    r.check(changedCount == 1, "frozen: no apply");
    r.check(monitor.applyPending(), "frozen: change recorded as pending");
    monitor.tick();
    r.check(changedCount == 1, "frozen: still no apply on repeat tick");
    // 冻结期间再改一次：只保留最终值，不叠加多次发出
    r.check(writeParams(path,
        "{\"formatVersion\":1,\"seq\":3,\"direction\":\"CCW\",\"xStepsNum\":4000}"),
        "write frozen params again");
    monitor.tick();
    r.check(changedCount == 1, "frozen: second change still not applied");

    // 3) 解冻（模拟源空闲超时）：下一次 tick 补发一次，且只发一次
    monitor.setApplyGate(true);
    monitor.tick();
    r.check(changedCount == 2, "unfreeze: deferred change applied exactly once");
    r.check(lastCaptured.hasXStepsNum && lastCaptured.xStepsNum == 4000,
            "unfreeze: latest value applied");
    r.check(!monitor.applyPending(), "pending cleared after apply");
    monitor.tick();
    r.check(changedCount == 2, "no repeat emit after catch-up");

    // 4) 自始至终未应用过 + 冻结：开闸后才首次应用
    {
        QTemporaryDir dir2;
        const QString path2 = dir2.filePath(QStringLiteral("p.json"));
        MotorScanLinkageMonitor::Settings st2;
        st2.paramsFile = path2;
        MotorScanLinkageMonitor m2(st2);
        int n2 = 0;
        Params c2;
        QObject::connect(&m2, &MotorScanLinkageMonitor::linkageParamsChanged,
                         [&](const Params &p) { ++n2; c2 = p; });
        m2.setApplyGate(false);
        r.check(writeParams(path2,
            "{\"formatVersion\":1,\"direction\":\"CCW\",\"xStepsNum\":1000}"),
            "write p2");
        m2.tick();
        r.check(n2 == 0, "never-applied + frozen: no emit");
        m2.setApplyGate(true);
        m2.tick();
        r.check(n2 == 1 && c2.hasXStepsNum && c2.xStepsNum == 1000,
                "first apply deferred until unfreeze");
    }

    // 5) TriggerLatch=false：闸门入口空操作，保持旧行为
    {
        QTemporaryDir dir3;
        const QString path3 = dir3.filePath(QStringLiteral("p.json"));
        MotorScanLinkageMonitor::Settings st3;
        st3.paramsFile = path3;
        st3.triggerLatch = false;
        MotorScanLinkageMonitor m3(st3);
        int n3 = 0;
        QObject::connect(&m3, &MotorScanLinkageMonitor::linkageParamsChanged,
                         [&](const Params &) { ++n3; });
        r.check(writeParams(path3,
            "{\"formatVersion\":1,\"direction\":\"CCW\",\"xStepsNum\":2000}"),
            "write p3");
        m3.tick();
        r.check(n3 == 1, "latch disabled: first read applies");
        m3.setApplyGate(false);   // 空操作
        r.check(m3.applyGateOpen(), "latch disabled: gate stays open");
        r.check(writeParams(path3,
            "{\"formatVersion\":1,\"direction\":\"CW\",\"xStepsNum\":5000}"),
            "rewrite p3");
        m3.tick();
        r.check(n3 == 2, "latch disabled: change still applies immediately");
    }
}

// RingBlockAssembler 顺时针角度：行 k 的角度 = 扇区起点 − k*step（镜像）。
void testAssemblerCcwCw(Runner &r)
{
    r.test = "assembler_ccw_cw";
    // 1 个通道（物理 0），perChannelBlock=4，perChannelFrame=2，
    // sectorStart=180、sectorWidth=360、step=90（简化几何）
    int enabled[8] = {1, 0, 0, 0, 0, 0, 0, 0};

    auto anglesOf = [&enabled](int sectorCcw) {
        RingBlockAssembler assembler;
        assembler.configure(enabled, 4, 8, 180.0, 360.0, 90.0, 2, 1, 0.0, sectorCcw);
        std::vector<float> angles;
        assembler.setBlockCallback(
            [&](std::vector<float> &&, std::vector<float> &&a,
                std::vector<uint8_t> &&, int, const paimage::RoundIdentity &, bool) {
                angles = a;
            });
        const paimage::RoundIdentity round{1, 0};
        const std::vector<float> line(8, 1.0f);
        for (int t = 0; t < 4; ++t)
            assembler.pushChannelLine(0, static_cast<uint16_t>(t), round,
                                       line.data(), 8, t == 3);
        return angles;
    };

    const std::vector<float> ccw = anglesOf(1);
    const std::vector<float> cw = anglesOf(0);
    // triggerWlOdd=1：g=0(wl1,k=0) g=1(wl2,kNext=1) g=2(wl1,k=1) g=3(wl2,kNext=2)
    // 既有组包器语义：首圈末行 wl2 的 kNext 不回绕（kRaw < perChannelFrame 时
    // kNext = kRaw+1），角度越过一圈起点（360）；后续圈才经 % 回绕。
    // CCW 期望角：0→180, 1→270, 2→270, 3→360
    const std::vector<float> expectCcw = {180.0f, 270.0f, 270.0f, 360.0f};
    // CW 期望角：k 项取负（扇区 s 项不变）：0→180, 1→90, 2→90, 3→0
    const std::vector<float> expectCw = {180.0f, 90.0f, 90.0f, 0.0f};
    auto nearEqual = [](const std::vector<float> &a, const std::vector<float> &b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])) > 1e-4)
                return false;
        return true;
    };
    r.check(nearEqual(ccw, expectCcw), "CCW angles match legacy formula");
    r.check(nearEqual(cw, expectCw), "CW angles are mirrored (sign flip on k)");
    r.check(!nearEqual(cw, ccw), "CW differs from CCW");
    // 镜像不变量（本测例 sectorStart=180）：CCW 与 CW 逐元素关于起点轴对称，
    // 即 ccw[i] + cw[i] ≡ 2*sectorStart ≡ 0 (mod 360)，与边界回绕策略无关。
    bool mirrorOk = ccw.size() == cw.size();
    for (size_t i = 0; mirrorOk && i < ccw.size(); ++i) {
        const double sum = std::fmod(
            static_cast<double>(ccw[i]) + static_cast<double>(cw[i]), 360.0);
        if (std::fabs(sum) > 1e-3 && std::fabs(sum - 360.0) > 1e-3)
            mirrorOk = false;
    }
    r.check(mirrorOk, "CW is the exact mirror of CCW (sum == 0 mod 360)");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);
    Runner runner;
    runner.ok = true;

    testParseParamsJson(runner);
    testDeriveTotalAlines(runner);
    testMonitor(runner);
    testMonitorTriggerLatch(runner);
    testAssemblerCcwCw(runner);

    if (!runner.ok) {
        std::cerr << "motor_scan_linkage_test FAILED\n";
        return 1;
    }
    std::cout << "motor_scan_linkage_test PASS\n";
    return 0;
}
