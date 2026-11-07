#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <mutex>
#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

// ============================================================
// MotorScanLinkage — LabVIEW 上位机电机参数联动（只读消费端）
//
// 默认读取 <exe目录>/motor_scan_params.json。仅 direction 参与联动，
// seq/writtenAt/source 用于诊断；其他字段保留在文件中，程序不消费。
// 文件格式（formatVersion 1）：
// {
//   "formatVersion": 1,
//   "seq": 123,                      // 写入方单调递增（可选）
//   "writtenAt": "2026-10-14T15:00:00.000",
//   "source": "LabVIEW:pact-PC-RFSG_V8b_20260618.vi",
//   "direction": "CCW",              // "CCW"|"CW"（也接受 "逆"|"顺"）
//   "xStepsNum": 2000,
//   "xStepsPerPeriod": 2
// }
//
// 触发闩锁（TriggerLatch，默认开）：
//   本轮重建方向必须在首个触发时锁定。监视器带一个应用闸门：
//     - 未收到本轮触发（源空闲 / 轮间隔）→ 闸门开：读到变化即应用；
//     - 收到本轮首个触发后 → 闸门关：继续读文件、继续刷新状态行，但不再应用，
//       直到源空闲超时（TimeoutBoundary）或计数重置（CountBoundary）再解冻。
//   闸门只控制“何时把参数交给下游”，不改变超时/计数重置等任何既有轮次语义。
// ============================================================

namespace motor_scan_linkage {

struct Params {
    bool hasDirection = false;
    bool directionCcw = true;   // true = 逆时针（逆），false = 顺时针（顺）
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

QString describeDirection(bool ccw);   // "逆时针(逆)" / "顺时针(顺)"

} // namespace motor_scan_linkage

// 定时轮询监视器（仅主线程使用）：
//   - 周期读参数文件（默认 500ms，照 System Capture 状态文件口径）；
//   - 解析成功且扫描方向与上次不同时
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
    QString paramsFile() const { return m_settings.paramsFile; }
    void setParamsFile(const QString &path); // 主线程切换文件，不改变物理轮闩锁
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
    // 可由采集线程调用；与主线程同步参数消费互斥，首触发返回前完成关闸。
    void setApplyGate(bool open);
    void onPhysicalRoundEvent(const paimage::PhysicalRoundEvent &event);
    // 同步消费槽拒绝时保留待应用值，下一次空闲轮询重试，不丢失参数。
    void retryPendingApply(const QString &reason);
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
    std::recursive_mutex m_applyMutex;
    QString m_applyError;
    bool m_pendingApply = false;     // 冻结期间累积了未应用的参数变化
};
