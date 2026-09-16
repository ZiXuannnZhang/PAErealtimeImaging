#pragma once

#include "RoundIdentity.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdint>

// JSON has a JavaScript-compatible number representation in many consumers.
// Keep the two physical-round fields as decimal strings so every uint64_t bit
// is preserved across Qt, JSON and ZMQ without relying on double conversion.
namespace ring_round_identity {

inline constexpr const char *kMeasurementSession = "measurement_session";
inline constexpr const char *kRoundGeneration = "round_generation";

struct ParseResult {
    paimage::RoundIdentity identity;
    bool present = false; // at least one identity field was present
    bool valid = false;   // both fields were present and strictly valid
    QString error;
};

inline bool parseDecimalUint64(const QJsonValue &value, std::uint64_t &out)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty()) return false;
    for (const QChar ch : text) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    }
    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok, 10);
    if (!ok) return false;
    out = static_cast<std::uint64_t>(parsed);
    return true;
}

inline void add(QJsonObject &object, const paimage::RoundIdentity &identity)
{
    object[QString::fromLatin1(kMeasurementSession)] =
        QString::number(static_cast<qulonglong>(identity.measurementSession));
    object[QString::fromLatin1(kRoundGeneration)] =
        QString::number(static_cast<qulonglong>(identity.roundGeneration));
}

inline ParseResult parse(const QJsonObject &object)
{
    ParseResult result;
    const QJsonValue session = object.value(QString::fromLatin1(kMeasurementSession));
    const QJsonValue generation = object.value(QString::fromLatin1(kRoundGeneration));
    result.present = !session.isUndefined() || !generation.isUndefined();
    if (!result.present) {
        result.error = QStringLiteral("missing measurement_session/round_generation");
        return result;
    }

    if (!parseDecimalUint64(session, result.identity.measurementSession) ||
        !parseDecimalUint64(generation, result.identity.roundGeneration)) {
        result.error = QStringLiteral("identity fields must be decimal uint64 strings");
        return result;
    }
    if (!result.identity.valid()) {
        result.error = QStringLiteral("measurement_session must be non-zero");
        return result;
    }
    result.valid = true;
    return result;
}

inline QJsonObject makeSnapshotReady(std::uint32_t seq,
                                     std::uint64_t submitIndex,
                                     const paimage::RoundIdentity &identity,
                                     bool roundComplete = false)
{
    QJsonObject object;
    object[QStringLiteral("cmd")] = QStringLiteral("ring_snapshot_ready");
    object[QStringLiteral("seq")] = static_cast<qint64>(seq);
    object[QStringLiteral("submit_index")] = static_cast<qint64>(submitIndex);
    add(object, identity);
    object[QStringLiteral("round_complete")] = roundComplete;
    return object;
}

} // namespace ring_round_identity
