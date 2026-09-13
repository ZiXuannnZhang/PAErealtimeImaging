#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

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

} // namespace NetworkDiagnostics
