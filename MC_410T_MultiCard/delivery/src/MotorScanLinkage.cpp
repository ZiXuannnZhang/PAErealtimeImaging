#include "MotorScanLinkage.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

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
    switch (rule) {
    case TotalAlinesRule::PerEnabledChannel:
        if (enabledChannelCount <= 0)
            return 0;
        return xStepsNum * enabledChannelCount;
    case TotalAlinesRule::PerPeriod:
        if (xStepsPerPeriod <= 0)
            return 0;
        return xStepsNum * xStepsPerPeriod;
    case TotalAlinesRule::DirectionOnly:
        return 0;
    }
    return 0;
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
    if (m_settings.pollMs < 100)
        m_settings.pollMs = 100;
    m_timer.setInterval(m_settings.pollMs);
    // tick() 为公开测试缝：生产路径经 QTimer 驱动，单测直接调用。
    connect(&m_timer, &QTimer::timeout, this, &MotorScanLinkageMonitor::tick);
}

void MotorScanLinkageMonitor::start()
{
    m_timer.start();
}

void MotorScanLinkageMonitor::setApplyGate(bool open)
{
    // 触发闩锁关闭时保持旧行为：闸门入口为空操作。
    if (!m_settings.triggerLatch)
        return;
    m_applyGateOpen.store(open, std::memory_order_relaxed);
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
    // 触发闩锁冻结期间照常显示文件当前值，但标注“本轮不生效”，
    // 避免把“文件已改”误读为“已下发”。
    setStatus(applyGateOpen()
                  ? statusText
                  : QStringLiteral("%1（本轮已闩锁，改动下一轮生效）").arg(statusText));

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
    // 触发闩锁：闸门关闭时只记“待应用”，不发出；开闸后的下一次 tick 补发一次
    // （否则冻结期间操作员改的参数会永久丢失）。发出前重新读一次闸门，
    // 尽量缩小“本轮刚触发、闸门刚关”与本次发出之间的窗口。
    if (m_pendingApply && applyGateOpen()) {
        m_pendingApply = false;
        emit linkageParamsChanged(params);
    }
}
