#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>

// ============================================================
// MotorScanLinkage — LabVIEW 上位机电机参数联动（只读消费端）
//
// 联动接口：LabVIEW 侧桥接（ActiveX 轮询主 VI 前面板控件）把
//   扫描方向（顺/逆 → CW/CCW）
//   电机步数 X Steps Num（可选附 X steps per Period）
// 写入一个 JSON 参数文件（默认 <exe目录>/motor_scan_params.json）；
// 本模块只负责读取、解析、推导与变化检测，不改 LabVIEW 侧任何内容。
//
// 消费路径（MainWindow 接线）：
//   方向  → RingConfigDialog::setMotorScanDirection →
//           RingReconCudaConfig.sectorCcw（休眠字段，此前恒为 1）→
//           RingBlockAssembler 角度符号 + ImagingSvc 拼接扇区锚点；
//   步数  → deriveTotalAlines() 按规则换算单圈总A-line数 →
//           RingConfigDialog 单圈总A-line数 → applyConfig 既有下发路径
//           （含每圈设计触发数 ringLogicalTriggersPerRound、运行中自动重启）。
//
// 文件格式（formatVersion 1，字段均可选但 direction/xStepsNum 至少其一）：
// {
//   "formatVersion": 1,
//   "seq": 123,                      // 写入方单调递增（可选）
//   "writtenAt": "2026-10-14T15:00:00.000",
//   "source": "LabVIEW:pact-PC-RFSG_V8b_20260618.vi",
//   "direction": "CCW",              // "CCW"|"CW"（也接受 "逆"|"顺"）
//   "xStepsNum": 2000,
//   "xStepsPerPeriod": 2             // 可选，仅 PerPeriod 规则使用
// }
//
// 触发闩锁（TriggerLatch，默认开）：
//   单圈总A-line数 / 每圈设计触发数由 xStepsNum 决定，所以“本轮用哪套参数”必须在
//   本轮第一根 A-line 之前定死。监视器因此带一个应用闸门：
//     - 未收到本轮触发（源空闲 / 轮间隔）→ 闸门开：读到变化即应用；
//     - 收到本轮首个触发后 → 闸门关：继续读文件、继续刷新状态行，但不再应用，
//       直到源空闲超时（TimeoutBoundary）或计数重置（CountBoundary）再解冻。
//   闸门只控制“何时把参数交给下游”，不改变超时/计数重置等任何既有轮次语义。
// ============================================================

namespace motor_scan_linkage {

struct Params {
    bool hasDirection = false;
    bool directionCcw = true;   // true = 逆时针（逆），false = 顺时针（顺）
    bool hasXStepsNum = false;
    int xStepsNum = 0;
    int xStepsPerPeriod = 0;
    quint64 seq = 0;
    QString writtenAt;
    QString source;
};

enum class ParseStatus {
    Ok,
    FileMissing,
    ReadFailed,
    ParseFailed,   // JSON 语法错误
    BadFormat     // 结构/取值不符合 formatVersion 1 约定
};

// 解析入口（单测直接喂 JSON 文本）；文件版本只做读文件 + 转调。
// error 仅在非 Ok 时填充（无文件时为空）。
ParseStatus parseParamsJson(const QByteArray &json, Params *out, QString *error);
ParseStatus parseParamsFile(const QString &path, Params *out, QString *error);

// 单圈总A-line数换算规则（INI 键 MotorScanLinkage/TotalAlinesRule）：
//   perEnabledChannel（默认）: totalAlines = XStepsNum × 启用通道数。
//       物理依据：电机每步 1 次激光触发，每圈触发数 = XStepsNum；
//       每次触发给每个启用通道 1 根 A-line（RingBlockAssembler 几何约定）。
//       现场口径验证：XStepsNum=2000、启用通道=2 → 4000，
//       与既有实机配置 单圈总A-line数=4000 一致。
//   perPeriod: totalAlines = XStepsNum × XStepsPerPeriod（备选 FPGA 语义）。
//   directionOnly: 只联动方向，不改单圈总A-line数。
enum class TotalAlinesRule { PerEnabledChannel, PerPeriod, DirectionOnly };

QString totalAlinesRuleKey(TotalAlinesRule rule);
TotalAlinesRule totalAlinesRuleFromKey(const QString &key);   // 未知键回退默认

// 返回 0 = 该规则下不推导（DirectionOnly、缺输入或参数非法）。
int deriveTotalAlines(TotalAlinesRule rule, int xStepsNum,
                      int xStepsPerPeriod, int enabledChannelCount);

// 与 RingConfigDialog::applyConfig 的换算约束同口径的预校验：
// total % (cnt×2)==0 且 total/(cnt×2) ≥ 2（整除 + K 下限）。
// 每块整除约束（K % (block/2)==0）依赖对话框当前块值，由对话框侧校验。
bool totalAlinesConsistent(int totalAlines, int enabledChannelCount);

QString describeDirection(bool ccw);   // "逆时针(逆)" / "顺时针(顺)"

} // namespace motor_scan_linkage

// 定时轮询监视器（仅主线程使用）：
//   - 周期读参数文件（默认 500ms，照 System Capture 状态文件口径）；
//   - 解析成功且 (direction, xStepsNum, xStepsPerPeriod) 与上次不同时
//     发 linkageParamsChanged；重新出现（例如文件曾被删除）也会再次发出；
//   - 应用闸门（触发闩锁）关闭时不发信号，只把变化记为待应用，开闸后补发一次；
//   - 状态文本变化时发 linkageStatusChanged（日志/对话框联动状态行共用）。
class MotorScanLinkageMonitor : public QObject
{
    Q_OBJECT

public:
    struct Settings {
        bool enabled = true;
        QString paramsFile;          // 必填（MainWindow 按默认路径填充）
        bool directionInverted = false;   // true = 写入方布尔语义与 逆/顺 相反
        int pollMs = 500;
        // 触发闩锁（默认开）。false = 关闭闸门机制，退化为旧行为（读到变化即应用）。
        bool triggerLatch = true;
    };

    explicit MotorScanLinkageMonitor(Settings settings, QObject *parent = nullptr);

    void start();
    // 测试缝：直接驱动一次轮询（生产路径经 QTimer；单测不进事件循环）
    void tick();
    bool fileSeen() const { return m_fileSeen; }
    motor_scan_linkage::Params lastParams() const { return m_lastParams; }
    QString lastStatus() const { return m_lastStatus; }

    // 应用闸门（触发闩锁）：
    //   open = true  → 允许把当前文件参数作为本轮生效参数发出（未成像/轮间隔的常态）；
    //   open = false → 冻结：tick() 仍读文件、仍刷新状态行，但不发 linkageParamsChanged；
    //                  冻结期间累积的变化在开闸后的下一次 tick 补发一次。
    // Settings::triggerLatch=false 时本入口为空操作（保持旧行为）。
    // 允许从采集侧回调线程调用（内部为原子标志）。
    void setApplyGate(bool open);
    bool applyGateOpen() const { return m_applyGateOpen.load(std::memory_order_relaxed); }
    bool applyPending() const { return m_pendingApply; }

signals:
    void linkageParamsChanged(const motor_scan_linkage::Params &params);
    void linkageStatusChanged(const QString &status);

private:
    void setStatus(const QString &status);

    Settings m_settings;
    QTimer m_timer;
    bool m_fileSeen = false;          // 本次运行期间至少成功读到过一次
    bool m_haveParams = false;       // 是否已观测到过有效参数（用于首帧/重现变化检测）
    motor_scan_linkage::Params m_lastParams;   // 最近一次观测到的参数（非“已应用”）
    QString m_lastStatus;
    std::atomic<bool> m_applyGateOpen{true};   // 触发闩锁：默认开
    bool m_pendingApply = false;     // 冻结期间累积了未应用的参数变化
};
