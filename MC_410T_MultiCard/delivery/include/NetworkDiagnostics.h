#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdint>

// Read-only local network inspection helpers used by NetworkController's
// diagnostic path. The snapshot APIs query the Windows networking tables;
// they do not send ICMP/ARP packets or otherwise probe the network.
namespace NetworkDiagnostics {

// Collect a point-in-time description of local IPv4 interfaces, gateways,
// routes and neighbour-table entries.  When targetIPs is non-empty, the
// returned object also contains a targetRelations array with local-address
// and gateway membership derived from the same snapshot. A failed Windows
// table query is represented as "unknown" in the corresponding status and
// relation fields; an empty list is never used to imply that the table was
// successfully queried and contained no rows.
QJsonObject collectSnapshot(const QStringList &targetIPs = QStringList());

// Stable human-readable encoding shared by packet diagnostics and tests.
QString formatHex(const QByteArray &bytes);

// Format an IPv4 endpoint without performing any name resolution.
QString formatEndpoint(const QString &address, quint16 port);

// Counter helpers are kept pure so tests can inject deterministic samples.
// Windows counters are exported as decimal strings because QJson numbers are
// IEEE-754 doubles and cannot represent every uint64_t exactly.
quint64 counterDelta(quint64 previous,
                     quint64 current,
                     bool hasPrevious,
                     bool *resetDetected = nullptr);
QString counterString(quint64 value);

// Periodic Windows ingress sampler. It performs no network I/O and is meant
// to be called from the controller's low-frequency snapshot path, never from
// the packet receive hot path.
class IngressSampler final
{
public:
    IngressSampler() = default;

    void reset();
    QJsonObject sample(const QStringList &targetIPs);

private:
    QHash<QString, quint64> m_previousCounters;
    QHash<QString, bool> m_seenCounters;
    qint64 m_startedMonotonicMs = -1;
};

} // namespace NetworkDiagnostics
