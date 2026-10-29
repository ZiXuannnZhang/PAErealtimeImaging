#include "MotorScanLinkage.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>
#include <limits>

namespace motor_scan_linkage {
namespace {

// 方向字符串约定：写入方默认用 CCW/CW；同时接受中文 逆/顺，
// 避免手写文件或不同桥接版本互相不兼容。
bool parseDirection(const QString &text, bool *ccw)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("ccw") || t == QStringLiteral("逆")
        || t == QStringLiteral("counterclockwise"))
        { *ccw = true; return true; }
    if (t == QStringLiteral("cw") || t == QStringLiteral("顺")
        || t == QStringLiteral("clockwise"))
        { *ccw = false; return true; }
    return false;
}

} // namespace

ParseStatus parseParamsJson(const QByteArray &json, Params *out, QString *error)
{
    if (out) *out = Params{};
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = parseError.errorString();
        return ParseStatus::ParseFailed;
    }
    const QJsonObject obj = doc.object();

    const int formatVersion = obj.value(QStringLiteral("formatVersion")).toInt(-1);
    if (formatVersion != 1) {
        if (error) *error = QStringLiteral("formatVersion %1 不支持（需要 1）").arg(formatVersion);
        return ParseStatus::BadFormat;
    }

    Params params;
    const QJsonValue dir = obj.value(QStringLiteral("direction"));
    if (!dir.isUndefined()) {
        if (!dir.isString() || !parseDirection(dir.toString(), &params.directionCcw)) {
            if (error) *error = QStringLiteral("direction 取值无效（CCW/CW/逆/顺）");
            return ParseStatus::BadFormat;
        }
        params.hasDirection = true;
    }

    const QJsonValue steps = obj.value(QStringLiteral("xStepsNum"));
    if (!steps.isUndefined()) {
        const double v = steps.toDouble(-1.0);
        if (v < 1.0 || v > 100000000.0 || v != static_cast<double>(static_cast<qlonglong>(v))) {
            if (error) *error = QStringLiteral("xStepsNum 取值无效（需为正整数）");
            return ParseStatus::BadFormat;
        }
        params.xStepsNum = static_cast<int>(v);
        params.hasXStepsNum = true;
    }

    const QJsonValue perPeriod = obj.value(QStringLiteral("xStepsPerPeriod"));
    if (!perPeriod.isUndefined()) {
        const double v = perPeriod.toDouble(-1.0);
        if (v < 1.0 || v > 1000000.0 || v != static_cast<double>(static_cast<qlonglong>(v))) {
            if (error) *error = QStringLiteral("xStepsPerPeriod 取值无效（需为正整数）");
            return ParseStatus::BadFormat;
        }
        params.xStepsPerPeriod = static_cast<int>(v);
    }

    if (!params.hasDirection && !params.hasXStepsNum) {
        if (error) *error = QStringLiteral("direction 与 xStepsNum 至少需要其一");
        return ParseStatus::BadFormat;
    }

    params.seq = static_cast<quint64>(
        obj.value(QStringLiteral("seq")).toDouble(0.0));
    params.writtenAt = obj.value(QStringLiteral("writtenAt")).toString();
    params.source = obj.value(QStringLiteral("source")).toString();
    if (out) *out = params;
    if (error) *error = QString();
    return ParseStatus::Ok;
}

ParseStatus parseParamsFile(const QString &path, Params *out, QString *error)
{
    if (out) *out = Params{};
    if (error) *error = QString();
    QFile file(path);
    if (!QFileInfo::exists(path))
        return ParseStatus::FileMissing;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开参数文件");
        return ParseStatus::ReadFailed;
    }
    return parseParamsJson(file.readAll(), out, error);
}

QString totalAlinesRuleKey(TotalAlinesRule rule)
{
    switch (rule) {
    case TotalAlinesRule::PerEnabledChannel:
        return QStringLiteral("perEnabledChannel");
    case TotalAlinesRule::PerPeriod:
        return QStringLiteral("perPeriod");
    case TotalAlinesRule::DirectionOnly:
        return QStringLiteral("directionOnly");
    }
    return QStringLiteral("perEnabledChannel");
}

TotalAlinesRule totalAlinesRuleFromKey(const QString &key)
{
    if (key == QStringLiteral("perPeriod"))
        return TotalAlinesRule::PerPeriod;
    if (key == QStringLiteral("directionOnly"))
        return TotalAlinesRule::DirectionOnly;
    return TotalAlinesRule::PerEnabledChannel;   // 未知键回退默认
}

int deriveTotalAlines(TotalAlinesRule rule, int xStepsNum,
                      int xStepsPerPeriod, int enabledChannelCount)
{
    if (xStepsNum <= 0)
        return 0;
    qint64 total = 0;
    switch (rule) {
    case TotalAlinesRule::PerEnabledChannel:
        if (enabledChannelCount <= 0)
            return 0;
        total = qint64(xStepsNum) * enabledChannelCount;
        break;
    case TotalAlinesRule::PerPeriod:
        if (xStepsPerPeriod <= 0)
            return 0;
        total = qint64(xStepsNum) * xStepsPerPeriod;
        break;
    case TotalAlinesRule::DirectionOnly:
        return 0;
    }
    return total <= std::numeric_limits<int>::max() ? static_cast<int>(total) : 0;
}

bool totalAlinesConsistent(int totalAlines, int enabledChannelCount)
{
    if (totalAlines <= 0 || enabledChannelCount <= 0)
        return false;
    const int divisor = enabledChannelCount * 2;   // 启用通道数 × 激发光波长数
    if (totalAlines % divisor != 0)
        return false;
    return totalAlines / divisor >= 2;
}

QString describeDirection(bool ccw)
{
    return ccw ? QStringLiteral("逆时针(逆)")
               : QStringLiteral("顺时针(顺)");
}

} // namespace motor_scan_linkage

// ============================================================
// MotorScanLinkageMonitor
// ============================================================

MotorScanLinkageMonitor::MotorScanLinkageMonitor(Settings settings, QObject *parent)
    : QObject(parent), m_settings(std::move(settings))
{
    m_lastStatus = m_settings.enabled ? QStringLiteral("联动状态：等待参数文件")
                                     : QStringLiteral("电机联动未启用");
    if (m_settings.pollMs < 100)
        m_settings.pollMs = 100;
    m_timer.setInterval(m_settings.pollMs);
    // tick() 为公开测试缝：生产路径经 QTimer 驱动，单测直接调用。
    connect(&m_timer, &QTimer::timeout, this, &MotorScanLinkageMonitor::tick);
}

void MotorScanLinkageMonitor::start()
{
    if (m_settings.enabled) m_timer.start();
}

void MotorScanLinkageMonitor::setApplyGate(bool open)
{
    // 触发闩锁关闭时保持旧行为：闸门入口为空操作。
    if (!m_settings.triggerLatch)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_applyMutex);
    m_applyGateOpen.store(open, std::memory_order_relaxed);
}

void MotorScanLinkageMonitor::onPhysicalRoundEvent(const paimage::PhysicalRoundEvent &event)
{
    using Kind = paimage::PhysicalRoundEvent::Kind;
    if (event.kind == Kind::RoundStarted) setApplyGate(false);
    else if (event.kind == Kind::CountBoundary || event.kind == Kind::TimeoutBoundary)
        setApplyGate(true);
}

void MotorScanLinkageMonitor::retryPendingApply(const QString &reason)
{
    m_pendingApply = true;
    m_applyError = reason;
}

void MotorScanLinkageMonitor::setStatus(const QString &status)
{
    if (status == m_lastStatus)
        return;
    m_lastStatus = status;
    emit linkageStatusChanged(status);
}

void MotorScanLinkageMonitor::tick()
{
    if (!m_settings.enabled) return;
    using motor_scan_linkage::Params;
    using motor_scan_linkage::ParseStatus;

    Params params;
    QString error;
    const ParseStatus status = motor_scan_linkage::parseParamsFile(
        m_settings.paramsFile, &params, &error);
    if (status != ParseStatus::Ok) {
        if (status == ParseStatus::FileMissing) {
            // 文件缺失不算错误状态（LabVIEW 桥接可能尚未启动），
            // 只做状态提示，保持上次已联动的参数不变；重置变化检测，
            // 文件重新出现时（即使内容相同）会再次发出一次联动参数。
            m_haveParams = false;
            setStatus(QStringLiteral("未联动（参数文件不存在）"));
        } else {
            setStatus(QStringLiteral("参数文件无效：%1").arg(error));
        }
        return;
    }

    // 写入方布尔语义反接（可选）：directionInverted 只翻转方向，
    // 不影响步数。
    if (m_settings.directionInverted && params.hasDirection)
        params.directionCcw = !params.directionCcw;

    m_fileSeen = true;
    const QString dirText = params.hasDirection
        ? motor_scan_linkage::describeDirection(params.directionCcw)
        : QStringLiteral("未提供");
    const QString statusText = QStringLiteral("已联动：%1 | XStepsNum=%2 | seq=%3")
        .arg(dirText)
        .arg(params.hasXStepsNum
                 ? QString::number(params.xStepsNum) : QStringLiteral("未提供"))
        .arg(params.seq);
    const bool observedChanged = !m_haveParams
        || params.hasDirection != m_lastParams.hasDirection
        || params.directionCcw != m_lastParams.directionCcw
        || params.hasXStepsNum != m_lastParams.hasXStepsNum
        || params.xStepsNum != m_lastParams.xStepsNum
        || params.xStepsPerPeriod != m_lastParams.xStepsPerPeriod;
    m_lastParams = params;
    m_haveParams = true;
    if (observedChanged)
        m_pendingApply = true;
    // 信号在主线程同步消费。锁覆盖 gate 检查与下发事务；采集侧 RoundStarted
    // 在返回/交付首触发前完成关闸，不能在检查与下发之间穿过。
    {
        std::lock_guard<std::recursive_mutex> lock(m_applyMutex);
        if (m_pendingApply && applyGateOpen()) {
            m_pendingApply = false;
            m_applyError.clear();
            emit linkageParamsChanged(params);
        }
    }
    setStatus(!m_applyError.isEmpty()
        ? QStringLiteral("联动待应用：%1").arg(m_applyError)
        : !applyGateOpen()
            ? QStringLiteral("%1（本轮已闩锁，改动下一轮生效）").arg(statusText)
            : statusText);
}
