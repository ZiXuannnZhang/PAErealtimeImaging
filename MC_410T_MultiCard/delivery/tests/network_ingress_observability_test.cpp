#include "NetworkDiagnostics.h"
#include "DataTypes.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

namespace {
bool require(bool condition, const QString& message)
{
    if (!condition) QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;

    bool reset = false;
    ok = require(NetworkDiagnostics::counterDelta(0, 7, false, &reset) == 0
                     && !reset,
                 QStringLiteral("first counter sample has unavailable delta")) && ok;
    ok = require(NetworkDiagnostics::counterDelta(10, 17, true, &reset) == 7
                     && !reset,
                 QStringLiteral("counter increase delta")) && ok;
    ok = require(NetworkDiagnostics::counterDelta(17, 3, true, &reset) == 0
                     && reset,
                 QStringLiteral("counter reset does not wrap to a huge delta")) && ok;
    ok = require(NetworkDiagnostics::counterString(0xffffffffffffffffULL)
                     == QStringLiteral("18446744073709551615"),
                 QStringLiteral("uint64 counter is decimal text")) && ok;

    NetworkDiagnostics::IngressSampler sampler;
    const QJsonObject first = sampler.sample({QStringLiteral("127.0.0.1")});
    const QJsonObject second = sampler.sample({QStringLiteral("127.0.0.1")});
    ok = require(first.value(QStringLiteral("kind")).toString()
                     == QStringLiteral("runtime_ingress"),
                 QStringLiteral("runtime ingress kind")) && ok;
    ok = require(first.contains(QStringLiteral("targetMappings"))
                     && first.contains(QStringLiteral("relevantInterfaces"))
                     && first.contains(QStringLiteral("systemIpv4Counters"))
                     && first.contains(QStringLiteral("systemUdpCounters")),
                 QStringLiteral("ingress schema top-level fields")) && ok;
    ok = require(second.value(QStringLiteral("monotonicMs")).toDouble()
                     >= first.value(QStringLiteral("monotonicMs")).toDouble(),
                 QStringLiteral("monotonic sample timestamp")) && ok;

#ifdef _WIN32
    const QJsonArray mappings = first.value(QStringLiteral("targetMappings")).toArray();
    ok = require(mappings.size() == 1,
                 QStringLiteral("loopback route mapping count")) && ok;
    if (!mappings.isEmpty()) {
        const QJsonObject mapping = mappings.first().toObject();
        if (mapping.value(QStringLiteral("status")).toString() != QStringLiteral("ok")
            || !mapping.value(QStringLiteral("interfaceLuid")).isString())
            QTextStream(stderr) << QJsonDocument(mapping).toJson(QJsonDocument::Compact) << Qt::endl;
        ok = require(mapping.value(QStringLiteral("status")).toString() == QStringLiteral("ok")
                         && mapping.value(QStringLiteral("interfaceLuid")).isString(),
                     QStringLiteral("loopback route resolves interface index/luid")) && ok;
    }
    const QJsonObject udp = first.value(QStringLiteral("systemUdpCounters")).toObject();
    ok = require(udp.value(QStringLiteral("scope")).toString() == QStringLiteral("system_ipv4"),
                 QStringLiteral("UDP system scope")) && ok;
#else
    ok = require(first.value(QStringLiteral("status")).toString() == QStringLiteral("unknown"),
                 QStringLiteral("non-Windows API is structured unavailable")) && ok;
#endif

    CardStats stats;
    stats.observeRawReceive(4, 0);
    stats.observeRawReceive(4, 2);
    const auto card = stats.snapshot();
    ok = require(card.sameTriggerForwardGapEvents == 1
                     && card.sameTriggerForwardGapPackets == 1,
                 QStringLiteral("card raw sequence helper")) && ok;

    if (ok) QTextStream(stdout) << "PASS network ingress observability helpers" << Qt::endl;
    return ok ? 0 : 1;
}
