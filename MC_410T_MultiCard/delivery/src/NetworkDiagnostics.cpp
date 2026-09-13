#include "NetworkDiagnostics.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

namespace {

QString isoNow()
{
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

QString unknownText()
{
    return QStringLiteral("unknown");
}

QJsonValue unknownValue()
{
    return QJsonValue(unknownText());
}

#ifdef _WIN32

QString ipv4Text(const IN_ADDR &address)
{
    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr)
        return unknownText();
    return QString::fromLatin1(text);
}

QString ipv4Text(const SOCKADDR_INET &address)
{
    if (address.si_family != AF_INET) return unknownText();
    return ipv4Text(address.Ipv4.sin_addr);
}

QString ipv4Text(const SOCKADDR *address)
{
    if (!address || address->sa_family != AF_INET) return unknownText();
    const auto *ipv4 = reinterpret_cast<const SOCKADDR_IN *>(address);
    return ipv4Text(ipv4->sin_addr);
}

QString prefixMask(quint8 prefixLength)
{
    if (prefixLength > 32) return unknownText();
    const quint32 mask = prefixLength == 0
        ? 0U
        : (0xffffffffU << (32U - static_cast<quint32>(prefixLength)));
    IN_ADDR address{};
    address.S_un.S_addr = htonl(mask);
    return ipv4Text(address);
}

QString macText(const BYTE *bytes, ULONG length)
{
    if (!bytes || length == 0) return unknownText();
    QStringList parts;
    parts.reserve(static_cast<int>(length));
    for (ULONG i = 0; i < length; ++i) {
        parts.append(QStringLiteral("%1").arg(bytes[i], 2, 16, QLatin1Char('0')).toUpper());
    }
    return parts.join(QLatin1Char(':'));
}

QString operationalStatus(IF_OPER_STATUS status)
{
    switch (status) {
    case IfOperStatusUp: return QStringLiteral("up");
    case IfOperStatusDown: return QStringLiteral("down");
    case IfOperStatusTesting: return QStringLiteral("testing");
    case IfOperStatusUnknown: return unknownText();
    case IfOperStatusDormant: return QStringLiteral("dormant");
    case IfOperStatusNotPresent: return QStringLiteral("notPresent");
    case IfOperStatusLowerLayerDown: return QStringLiteral("lowerLayerDown");
    default: return unknownText();
    }
}

QString neighborState(NL_NEIGHBOR_STATE state)
{
    switch (state) {
    case NlnsUnreachable: return QStringLiteral("unreachable");
    case NlnsIncomplete: return QStringLiteral("incomplete");
    case NlnsProbe: return QStringLiteral("probe");
    case NlnsDelay: return QStringLiteral("delay");
    case NlnsStale: return QStringLiteral("stale");
    case NlnsReachable: return QStringLiteral("reachable");
    case NlnsPermanent: return QStringLiteral("permanent");
    default: return unknownText();
    }
}

QJsonObject unknownFailure(const QString &api, ULONG errorCode)
{
    QJsonObject object;
    object.insert(QStringLiteral("api"), api);
    object.insert(QStringLiteral("code"), static_cast<double>(errorCode));
    object.insert(QStringLiteral("status"), unknownText());
    return object;
}

#endif

} // namespace

namespace NetworkDiagnostics {

QString formatHex(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex().toUpper());
}

QString formatEndpoint(const QString &address, quint16 port)
{
    const QString trimmed = address.trimmed();
    if (trimmed.contains(QLatin1Char(':')) && !trimmed.startsWith(QLatin1Char('['))) {
        return QStringLiteral("[%1]:%2").arg(trimmed).arg(port);
    }
    return QStringLiteral("%1:%2").arg(trimmed).arg(port);
}

QJsonObject collectSnapshot(const QStringList &targetIPs)
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("formatVersion"), 1);
    snapshot.insert(QStringLiteral("capturedAt"), isoNow());
    snapshot.insert(QStringLiteral("platform"),
#ifdef _WIN32
                    QStringLiteral("windows")
#else
                    QStringLiteral("nonWindows")
#endif
    );

    QJsonArray interfaces;
    QJsonArray routes;
    QJsonArray neighbors;
    QJsonArray errors;

    QHash<QString, bool> localAddresses;
    QHash<QString, bool> gateways;
    bool interfacesKnown = false;
    bool routesKnown = false;
    bool neighborsKnown = false;

#ifdef _WIN32
    // GetAdaptersAddresses may require more than one retry when an interface
    // changes while the snapshot is being collected. The retry is local and
    // does not send any packet.
    ULONG addressBufferSize = 16 * 1024;
    std::vector<BYTE> addressBuffer(addressBufferSize);
    ULONG addressResult = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && addressResult == ERROR_BUFFER_OVERFLOW; ++attempt) {
        addressBuffer.resize(addressBufferSize);
        addressResult = GetAdaptersAddresses(AF_INET,
                                             GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
                                             nullptr,
                                             reinterpret_cast<PIP_ADAPTER_ADDRESSES>(addressBuffer.data()),
                                             &addressBufferSize);
    }

    if (addressResult == NO_ERROR) {
        interfacesKnown = true;
        for (PIP_ADAPTER_ADDRESSES adapter =
                 reinterpret_cast<PIP_ADAPTER_ADDRESSES>(addressBuffer.data());
             adapter != nullptr;
             adapter = adapter->Next) {
            QJsonObject interfaceObject;
            interfaceObject.insert(QStringLiteral("index"), static_cast<double>(adapter->IfIndex));
            interfaceObject.insert(QStringLiteral("name"),
                                   adapter->FriendlyName
                                       ? QString::fromWCharArray(adapter->FriendlyName)
                                       : unknownText());
            interfaceObject.insert(QStringLiteral("description"),
                                   adapter->Description
                                       ? QString::fromWCharArray(adapter->Description)
                                       : unknownText());
            interfaceObject.insert(QStringLiteral("operationalStatus"),
                                   operationalStatus(adapter->OperStatus));
            interfaceObject.insert(QStringLiteral("mac"),
                                   macText(adapter->PhysicalAddress,
                                           adapter->PhysicalAddressLength));

            QJsonArray ipv4;
            for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
                 unicast != nullptr;
                 unicast = unicast->Next) {
                const QString address = ipv4Text(unicast->Address.lpSockaddr);
                if (address == unknownText()) continue;
                const quint8 prefixLength = unicast->OnLinkPrefixLength;
                QJsonObject addressObject;
                addressObject.insert(QStringLiteral("address"), address);
                addressObject.insert(QStringLiteral("prefixLength"), prefixLength);
                addressObject.insert(QStringLiteral("netmask"), prefixMask(prefixLength));
                ipv4.append(addressObject);
                localAddresses.insert(address, true);
            }
            interfaceObject.insert(QStringLiteral("ipv4"), ipv4);

            QJsonArray adapterGateways;
            for (const IP_ADAPTER_GATEWAY_ADDRESS_LH *gateway = adapter->FirstGatewayAddress;
                 gateway != nullptr;
                 gateway = gateway->Next) {
                const QString address = ipv4Text(gateway->Address.lpSockaddr);
                if (address == unknownText()) continue;
                adapterGateways.append(address);
                gateways.insert(address, true);
            }
            interfaceObject.insert(QStringLiteral("gateways"), adapterGateways);
            interfaces.append(interfaceObject);
        }
    } else {
        errors.append(unknownFailure(QStringLiteral("GetAdaptersAddresses"), addressResult));
    }

    PMIB_IPFORWARD_TABLE2 routeTable = nullptr;
    const ULONG routeResult = GetIpForwardTable2(AF_INET, &routeTable);
    if (routeResult == NO_ERROR && routeTable != nullptr) {
        routesKnown = true;
        for (ULONG i = 0; i < routeTable->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2 &row = routeTable->Table[i];
            QJsonObject routeObject;
            routeObject.insert(QStringLiteral("interfaceIndex"),
                               static_cast<double>(row.InterfaceIndex));
            routeObject.insert(QStringLiteral("destination"),
                               QStringLiteral("%1/%2")
                                   .arg(ipv4Text(row.DestinationPrefix.Prefix))
                                   .arg(row.DestinationPrefix.PrefixLength));
            const QString nextHop = ipv4Text(row.NextHop);
            routeObject.insert(QStringLiteral("nextHop"), nextHop);
            routeObject.insert(QStringLiteral("metric"),
                               static_cast<double>(row.Metric));
            routes.append(routeObject);
            if (nextHop != QStringLiteral("0.0.0.0") && nextHop != unknownText()) {
                gateways.insert(nextHop, true);
            }
        }
    } else {
        errors.append(unknownFailure(QStringLiteral("GetIpForwardTable2"), routeResult));
    }
    if (routeTable != nullptr) FreeMibTable(routeTable);

    PMIB_IPNET_TABLE2 neighborTable = nullptr;
    const ULONG neighborResult = GetIpNetTable2(AF_INET, &neighborTable);
    if (neighborResult == NO_ERROR && neighborTable != nullptr) {
        neighborsKnown = true;
        for (ULONG i = 0; i < neighborTable->NumEntries; ++i) {
            const MIB_IPNET_ROW2 &row = neighborTable->Table[i];
            QJsonObject neighborObject;
            neighborObject.insert(QStringLiteral("interfaceIndex"),
                                  static_cast<double>(row.InterfaceIndex));
            neighborObject.insert(QStringLiteral("address"), ipv4Text(row.Address));
            neighborObject.insert(QStringLiteral("mac"),
                                  macText(row.PhysicalAddress,
                                          row.PhysicalAddressLength));
            neighborObject.insert(QStringLiteral("state"), neighborState(row.State));
            neighborObject.insert(QStringLiteral("isRouter"), row.IsRouter != FALSE);
            neighbors.append(neighborObject);
        }
    } else {
        errors.append(unknownFailure(QStringLiteral("GetIpNetTable2"), neighborResult));
    }
    if (neighborTable != nullptr) FreeMibTable(neighborTable);
#else
    Q_UNUSED(targetIPs);
    QJsonObject failure;
    failure.insert(QStringLiteral("api"), QStringLiteral("WindowsNetworkTables"));
    failure.insert(QStringLiteral("code"), 0);
    failure.insert(QStringLiteral("status"), unknownText());
    errors.append(failure);
#endif

    snapshot.insert(QStringLiteral("interfacesStatus"),
                    interfacesKnown ? QStringLiteral("ok") : unknownText());
    snapshot.insert(QStringLiteral("routesStatus"),
                    routesKnown ? QStringLiteral("ok") : unknownText());
    snapshot.insert(QStringLiteral("neighborsStatus"),
                    neighborsKnown ? QStringLiteral("ok") : unknownText());
    snapshot.insert(QStringLiteral("interfaces"), interfaces);
    snapshot.insert(QStringLiteral("routes"), routes);
    snapshot.insert(QStringLiteral("neighbors"), neighbors);

    QJsonArray relations;
    const bool gatewayEvidenceKnown = interfacesKnown || routesKnown;
    for (const QString &target : targetIPs) {
        const QString normalized = target.trimmed();
        QJsonObject relation;
        relation.insert(QStringLiteral("targetIP"), normalized);
        if (interfacesKnown) {
            const bool local = localAddresses.contains(normalized);
            relation.insert(QStringLiteral("localAddress"), local);
            relation.insert(QStringLiteral("isLocalAddress"), local);
        } else {
            relation.insert(QStringLiteral("localAddress"), unknownValue());
            relation.insert(QStringLiteral("isLocalAddress"), unknownValue());
        }
        if (gatewayEvidenceKnown) {
            relation.insert(QStringLiteral("isGateway"), gateways.contains(normalized));
        } else {
            relation.insert(QStringLiteral("isGateway"), unknownValue());
        }
        relations.append(relation);
    }
    snapshot.insert(QStringLiteral("targetRelationsStatus"),
                    targetIPs.isEmpty()
                        ? QStringLiteral("notRequested")
                        : ((interfacesKnown || gatewayEvidenceKnown)
                               ? QStringLiteral("ok")
                               : unknownText()));
    snapshot.insert(QStringLiteral("targetRelations"), relations);
    snapshot.insert(QStringLiteral("errors"), errors);
    snapshot.insert(QStringLiteral("status"), errors.isEmpty() ? QStringLiteral("ok") : unknownText());
    return snapshot;
}

} // namespace NetworkDiagnostics
