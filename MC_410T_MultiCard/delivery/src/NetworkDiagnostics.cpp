#include "NetworkDiagnostics.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <chrono>
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

quint64 counterDelta(quint64 previous, quint64 current, bool hasPrevious, bool *resetDetected)
{
    if (resetDetected) *resetDetected = false;
    if (!hasPrevious) return 0;
    if (current < previous) {
        if (resetDetected) *resetDetected = true;
        return 0;
    }
    return current - previous;
}

QString counterString(quint64 value)
{
    return QString::number(static_cast<qulonglong>(value));
}

void IngressSampler::reset()
{
    m_previousCounters.clear();
    m_seenCounters.clear();
    m_startedMonotonicMs = -1;
}

QJsonObject IngressSampler::sample(const QStringList &targetIPs)
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("formatVersion"), 1);
    snapshot.insert(QStringLiteral("kind"), QStringLiteral("runtime_ingress"));
    snapshot.insert(QStringLiteral("scope"), QStringLiteral("windows_ingress_observability"));
    snapshot.insert(QStringLiteral("capturedAt"), isoNow());
    const qint64 monotonicMs = static_cast<qint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (m_startedMonotonicMs < 0) m_startedMonotonicMs = monotonicMs;
    snapshot.insert(QStringLiteral("monotonicMs"),
                    static_cast<double>(monotonicMs - m_startedMonotonicMs));

    QJsonArray errors;
    QJsonArray targetMappings;
    QJsonArray relevantInterfaces;
    QJsonObject ipv4Counters;
    QJsonObject udpCounters;

#ifdef _WIN32
    struct AdapterMetadata {
        QJsonObject object;
        quint64 luid = 0;
    };
    QHash<ULONG, AdapterMetadata> adapters;

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
        for (PIP_ADAPTER_ADDRESSES adapter =
                 reinterpret_cast<PIP_ADAPTER_ADDRESSES>(addressBuffer.data());
             adapter != nullptr;
             adapter = adapter->Next) {
            AdapterMetadata metadata;
            metadata.luid = static_cast<quint64>(adapter->Luid.Value);
            metadata.object.insert(QStringLiteral("interfaceIndex"),
                                   static_cast<double>(adapter->IfIndex));
            metadata.object.insert(QStringLiteral("interfaceLuid"),
                                   counterString(metadata.luid));
            metadata.object.insert(QStringLiteral("friendlyName"),
                                   adapter->FriendlyName
                                       ? QString::fromWCharArray(adapter->FriendlyName)
                                       : unknownText());
            metadata.object.insert(QStringLiteral("description"),
                                   adapter->Description
                                       ? QString::fromWCharArray(adapter->Description)
                                       : unknownText());
            metadata.object.insert(QStringLiteral("operationalStatus"),
                                   operationalStatus(adapter->OperStatus));
            metadata.object.insert(QStringLiteral("mtu"),
                                   static_cast<double>(adapter->Mtu));
            metadata.object.insert(QStringLiteral("receiveLinkSpeed"),
                                   counterString(static_cast<quint64>(adapter->ReceiveLinkSpeed)));
            metadata.object.insert(QStringLiteral("transmitLinkSpeed"),
                                   counterString(static_cast<quint64>(adapter->TransmitLinkSpeed)));
            QJsonArray localIPv4;
            for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
                 unicast != nullptr;
                 unicast = unicast->Next) {
                const QString address = ipv4Text(unicast->Address.lpSockaddr);
                if (address != unknownText()) localIPv4.append(address);
            }
            metadata.object.insert(QStringLiteral("localIPv4"), localIPv4);
            adapters.insert(adapter->IfIndex, metadata);
        }
    } else {
        errors.append(unknownFailure(QStringLiteral("GetAdaptersAddresses"), addressResult));
    }

    QHash<ULONG, QSet<QString>> targetsByInterface;
    QHash<ULONG, quint64> routeLuidByInterface;
    for (const QString &target : targetIPs) {
        QJsonObject mapping;
        mapping.insert(QStringLiteral("targetIP"), target);
        SOCKADDR_INET destination{};
        destination.si_family = AF_INET;
        const QByteArray targetBytes = target.toLatin1();
        const int parsed = InetPtonA(AF_INET, targetBytes.constData(),
                                     &destination.Ipv4.sin_addr);
        if (parsed != 1) {
            mapping.insert(QStringLiteral("status"), unknownText());
            mapping.insert(QStringLiteral("error"), QStringLiteral("invalid_ipv4"));
            targetMappings.append(mapping);
            continue;
        }

        MIB_IPFORWARD_ROW2 route{};
        SOCKADDR_INET bestSource{};
        const ULONG routeResult = GetBestRoute2(nullptr, 0, nullptr, &destination,
                                                0, &route, &bestSource);
        if (routeResult != NO_ERROR) {
            mapping.insert(QStringLiteral("status"), unknownText());
            mapping.insert(QStringLiteral("api"), QStringLiteral("GetBestRoute2"));
            mapping.insert(QStringLiteral("errorCode"), static_cast<double>(routeResult));
            targetMappings.append(mapping);
            errors.append(unknownFailure(QStringLiteral("GetBestRoute2"), routeResult));
            continue;
        }

        const ULONG interfaceIndex = route.InterfaceIndex;
        const quint64 interfaceLuid = static_cast<quint64>(route.InterfaceLuid.Value);
        mapping.insert(QStringLiteral("status"), QStringLiteral("ok"));
        mapping.insert(QStringLiteral("interfaceIndex"), static_cast<double>(interfaceIndex));
        mapping.insert(QStringLiteral("interfaceLuid"), counterString(interfaceLuid));
        targetMappings.append(mapping);
        targetsByInterface[interfaceIndex].insert(target);
        routeLuidByInterface.insert(interfaceIndex, interfaceLuid);
    }

    auto addCounter = [this](QJsonObject *target,
                             const QString &scope,
                             const QString &name,
                             quint64 absolute) {
        if (!target) return;
        const QString key = scope + QLatin1Char('/') + name;
        const bool hasPrevious = m_seenCounters.value(key, false);
        bool resetDetected = false;
        const quint64 delta = counterDelta(m_previousCounters.value(key), absolute,
                                           hasPrevious, &resetDetected);
        m_previousCounters.insert(key, absolute);
        m_seenCounters.insert(key, true);
        QJsonObject pair;
        pair.insert(QStringLiteral("absolute"), counterString(absolute));
        pair.insert(QStringLiteral("delta"), counterString(delta));
        pair.insert(QStringLiteral("deltaStatus"),
                    !hasPrevious ? QStringLiteral("unavailable")
                                  : (resetDetected ? QStringLiteral("reset")
                                                    : QStringLiteral("ok")));
        pair.insert(QStringLiteral("deltaAvailable"), hasPrevious && !resetDetected);
        target->insert(name, pair);
    };

    QList<ULONG> interfaceIndices = targetsByInterface.keys();
    std::sort(interfaceIndices.begin(), interfaceIndices.end());
    for (const ULONG interfaceIndex : interfaceIndices) {
        QJsonObject interfaceObject = adapters.value(interfaceIndex).object;
        if (interfaceObject.isEmpty()) {
            interfaceObject.insert(QStringLiteral("interfaceIndex"),
                                   static_cast<double>(interfaceIndex));
            interfaceObject.insert(QStringLiteral("interfaceLuid"),
                                   counterString(routeLuidByInterface.value(interfaceIndex)));
            interfaceObject.insert(QStringLiteral("friendlyName"), unknownText());
            interfaceObject.insert(QStringLiteral("description"), unknownText());
            interfaceObject.insert(QStringLiteral("localIPv4"), QJsonArray());
            interfaceObject.insert(QStringLiteral("operationalStatus"), unknownText());
        }
        QJsonArray sharedTargets;
        QStringList orderedTargets = targetsByInterface.value(interfaceIndex).values();
        orderedTargets.sort();
        for (const QString &target : orderedTargets) sharedTargets.append(target);
        interfaceObject.insert(QStringLiteral("sharedByTargets"), sharedTargets);

        MIB_IF_ROW2 row{};
        row.InterfaceIndex = interfaceIndex;
        const ULONG rowResult = GetIfEntry2(&row);
        QJsonObject counters;
        if (rowResult == NO_ERROR) {
            interfaceObject.insert(QStringLiteral("counterStatus"), QStringLiteral("ok"));
            const QString interfaceScope = QStringLiteral("interface:")
                + interfaceObject.value(QStringLiteral("interfaceLuid")).toString();
            addCounter(&counters, interfaceScope, QStringLiteral("InOctets"), row.InOctets);
            addCounter(&counters, interfaceScope, QStringLiteral("InUcastPkts"), row.InUcastPkts);
            addCounter(&counters, interfaceScope, QStringLiteral("InNUcastPkts"), row.InNUcastPkts);
            addCounter(&counters, interfaceScope, QStringLiteral("InDiscards"), row.InDiscards);
            addCounter(&counters, interfaceScope, QStringLiteral("InErrors"), row.InErrors);
            addCounter(&counters, interfaceScope, QStringLiteral("InUnknownProtos"), row.InUnknownProtos);
            addCounter(&counters, interfaceScope, QStringLiteral("OutOctets"), row.OutOctets);
            addCounter(&counters, interfaceScope, QStringLiteral("OutUcastPkts"), row.OutUcastPkts);
            addCounter(&counters, interfaceScope, QStringLiteral("OutNUcastPkts"), row.OutNUcastPkts);
            addCounter(&counters, interfaceScope, QStringLiteral("OutDiscards"), row.OutDiscards);
            addCounter(&counters, interfaceScope, QStringLiteral("OutErrors"), row.OutErrors);
        } else {
            interfaceObject.insert(QStringLiteral("counterStatus"), unknownText());
            interfaceObject.insert(QStringLiteral("counterApi"), QStringLiteral("GetIfEntry2"));
            interfaceObject.insert(QStringLiteral("counterErrorCode"), static_cast<double>(rowResult));
            errors.append(unknownFailure(QStringLiteral("GetIfEntry2"), rowResult));
        }
        interfaceObject.insert(QStringLiteral("counters"), counters);
        relevantInterfaces.append(interfaceObject);
    }

    MIB_IPSTATS ipStats{};
    const ULONG ipResult = GetIpStatisticsEx(&ipStats, AF_INET);
    if (ipResult == NO_ERROR) {
        ipv4Counters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
        ipv4Counters.insert(QStringLiteral("status"), QStringLiteral("ok"));
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InReceives"), ipStats.dwInReceives);
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InHdrErrors"), ipStats.dwInHdrErrors);
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InAddrErrors"), ipStats.dwInAddrErrors);
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InUnknownProtos"), ipStats.dwInUnknownProtos);
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InDiscards"), ipStats.dwInDiscards);
        addCounter(&ipv4Counters, QStringLiteral("system_ipv4"), QStringLiteral("InDelivers"), ipStats.dwInDelivers);
    } else {
        ipv4Counters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
        ipv4Counters.insert(QStringLiteral("status"), unknownText());
        ipv4Counters.insert(QStringLiteral("api"), QStringLiteral("GetIpStatisticsEx"));
        ipv4Counters.insert(QStringLiteral("errorCode"), static_cast<double>(ipResult));
        errors.append(unknownFailure(QStringLiteral("GetIpStatisticsEx"), ipResult));
    }

    MIB_UDPSTATS udpStats{};
    const ULONG udpResult = GetUdpStatisticsEx(&udpStats, AF_INET);
    if (udpResult == NO_ERROR) {
        udpCounters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
        udpCounters.insert(QStringLiteral("status"), QStringLiteral("ok"));
        addCounter(&udpCounters, QStringLiteral("system_udp_ipv4"), QStringLiteral("InDatagrams"), udpStats.dwInDatagrams);
        addCounter(&udpCounters, QStringLiteral("system_udp_ipv4"), QStringLiteral("NoPorts"), udpStats.dwNoPorts);
        addCounter(&udpCounters, QStringLiteral("system_udp_ipv4"), QStringLiteral("InErrors"), udpStats.dwInErrors);
        addCounter(&udpCounters, QStringLiteral("system_udp_ipv4"), QStringLiteral("OutDatagrams"), udpStats.dwOutDatagrams);
    } else {
        udpCounters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
        udpCounters.insert(QStringLiteral("status"), unknownText());
        udpCounters.insert(QStringLiteral("api"), QStringLiteral("GetUdpStatisticsEx"));
        udpCounters.insert(QStringLiteral("errorCode"), static_cast<double>(udpResult));
        errors.append(unknownFailure(QStringLiteral("GetUdpStatisticsEx"), udpResult));
    }
#else
    Q_UNUSED(targetIPs);
    QJsonObject unavailable;
    unavailable.insert(QStringLiteral("status"), unknownText());
    unavailable.insert(QStringLiteral("reason"), QStringLiteral("Windows APIs unavailable"));
    errors.append(unavailable);
    ipv4Counters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
    ipv4Counters.insert(QStringLiteral("status"), unknownText());
    udpCounters.insert(QStringLiteral("scope"), QStringLiteral("system_ipv4"));
    udpCounters.insert(QStringLiteral("status"), unknownText());
#endif

    snapshot.insert(QStringLiteral("targetMappings"), targetMappings);
    snapshot.insert(QStringLiteral("relevantInterfaces"), relevantInterfaces);
    snapshot.insert(QStringLiteral("interfaceStatus"),
                    relevantInterfaces.isEmpty() && targetIPs.isEmpty()
                        ? QStringLiteral("notRequested")
                        : (relevantInterfaces.isEmpty() ? unknownText() : QStringLiteral("ok")));
    snapshot.insert(QStringLiteral("systemIpv4Counters"), ipv4Counters);
    snapshot.insert(QStringLiteral("systemUdpCounters"), udpCounters);
    snapshot.insert(QStringLiteral("errors"), errors);
    snapshot.insert(QStringLiteral("status"), errors.isEmpty() ? QStringLiteral("ok") : unknownText());
    return snapshot;
}

} // namespace NetworkDiagnostics
