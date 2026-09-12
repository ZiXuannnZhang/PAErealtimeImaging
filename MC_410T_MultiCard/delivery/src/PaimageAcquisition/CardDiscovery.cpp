#include "PaimageAcquisition/CardDiscovery.h"

#include "PaimageAcquisition/SourceCore.h"
#include "DiagnosticRecorder.h"

#include <QJsonArray>

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windows.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace paimage {

namespace {

constexpr std::uint16_t kDiscoveryControlPort = 8080;   // formal control link port
constexpr std::uint16_t kDiscoveryFeedbackPort = 8000;  // formal feedback port

} // namespace

std::uint32_t discoveryIpToHostOrder(const QString& ip, bool* ok)
{
    const QStringList parts = ip.split('.');
    if (ok) *ok = false;
    if (parts.size() != 4) return 0;
    std::uint32_t value = 0;
    for (const QString& part : parts) {
        bool valid = false;
        const int octet = part.toInt(&valid);
        if (!valid || octet < 0 || octet > 255) return 0;
        value = (value << 8) | static_cast<std::uint32_t>(octet);
    }
    if (ok) *ok = true;
    return value;
}

QString discoveryCandidateStateName(DiscoveryCandidateState state)
{
    switch (state) {
        case DiscoveryCandidateState::Pending: return QStringLiteral("pending");
        case DiscoveryCandidateState::ExcludedLocal: return QStringLiteral("excluded_local");
        case DiscoveryCandidateState::Invalid: return QStringLiteral("invalid");
        case DiscoveryCandidateState::SendFailed: return QStringLiteral("send_failed");
        case DiscoveryCandidateState::Verified: return QStringLiteral("verified");
        case DiscoveryCandidateState::TimedOut: return QStringLiteral("timed_out");
    }
    return QStringLiteral("unknown");
}

DiscoveryOptions sanitizeDiscoveryOptions(const DiscoveryOptions& options,
                                          QString* normalizationNote)
{
    DiscoveryOptions normalized = options;
    QStringList notes;
    auto clamp = [&](int& value, int low, int high, int fallback, const char* name) {
        if (value < low || value > high) {
            notes << QStringLiteral("%1 %2->%3").arg(QLatin1String(name)).arg(value).arg(fallback);
            value = fallback;
        }
    };
    clamp(normalized.ackWindowMs, 500, 5000, 2000, "AckWindowMs");
    clamp(normalized.maxAttempts, 1, 10, 5, "MaxAttempts");
    clamp(normalized.finalGraceMs, 0, 2000, 500, "FinalGraceMs");
    clamp(normalized.icmpTimeoutMs, 50, 1000, 150, "IcmpTimeoutMs");
    clamp(normalized.candidateCount, 1, 254, 32, "CandidateCount");
    if (normalized.configDurationNs <= 0) {
        notes << QStringLiteral("ConfigDurationNs %1->20000").arg(normalized.configDurationNs);
        normalized.configDurationNs = 20000;
    }
    if (normalized.delayANs < 0) {
        notes << QStringLiteral("DelayANs %1->200").arg(normalized.delayANs);
        normalized.delayANs = 200;
    }
    if (normalized.delayBNs < 0) {
        notes << QStringLiteral("DelayBNs %1->200").arg(normalized.delayBNs);
        normalized.delayBNs = 200;
    }
    if (normalizationNote) *normalizationNote = notes.join(QStringLiteral("; "));
    return normalized;
}

bool canApplyDiscoveryResult(bool windowAlive,
                             const QString& activeDiscoveryId,
                             const DiscoveryResult& result)
{
    if (!windowAlive) return false;
    if (activeDiscoveryId.isEmpty() || result.discoveryId.isEmpty()) return false;
    return activeDiscoveryId == result.discoveryId;
}

namespace {

struct DiscoverySession {
    const DiscoveryOptions& options;
    DiscoveryChannel& channel;
    const DiscoveryEnvironment& environment;
    DiscoveryResult& result;
    std::vector<DiscoveryCandidateEvidence> candidates;
    QHash<QString, int> indexByIp;
    std::vector<qint64> firstSuccessSendNs;
    int round = 0;
    qint64 startedNs = 0;

    void emitEvent(const QString& category,
                   const QString& message,
                   QJsonObject fields) const
    {
        fields.insert(QStringLiteral("discoveryId"), options.discoveryId);
        fields.insert(QStringLiteral("monotonicNs"),
                      QString::number(channel.nowNs()));
        if (environment.eventSink) environment.eventSink(category, message, fields);
    }
};

QJsonObject candidateSummary(const DiscoveryCandidateEvidence& c)
{
    QJsonObject object;
    object.insert(QStringLiteral("ip"), c.ip);
    object.insert(QStringLiteral("state"), discoveryCandidateStateName(c.state));
    object.insert(QStringLiteral("icmpReachable"), c.icmpReachable);
    object.insert(QStringLiteral("arpReachable"), c.arpReachable);
    object.insert(QStringLiteral("ready18Observed"), c.ready18Observed);
    object.insert(QStringLiteral("attemptsSent"), c.attemptsSent);
    object.insert(QStringLiteral("sendFailures"), c.sendFailures);
    object.insert(QStringLiteral("ack60Count"), c.ack60Count);
    object.insert(QStringLiteral("duplicateAck60Count"), c.duplicateAck60Count);
    object.insert(QStringLiteral("otherLengthCount"), c.otherLengthCount);
    object.insert(QStringLiteral("unexpectedFeedbackCount"), c.unexpectedFeedbackCount);
    object.insert(QStringLiteral("lastSendError"), c.lastSendError);
    object.insert(QStringLiteral("verifiedRound"), c.verifiedRound);
    if (!c.note.isEmpty()) object.insert(QStringLiteral("note"), c.note);
    return object;
}

void emitComplete(DiscoverySession& session, const QString& error, bool cancelled)
{
    QJsonArray states;
    for (const auto& c : session.candidates) states.append(candidateSummary(c));
    QJsonArray verified;
    for (const QString& ip : session.result.verifiedIPs) verified.append(ip);
    session.result.totalDurationNs = session.channel.nowNs() - session.startedNs;
    session.emitEvent(QStringLiteral("discovery.complete"),
                      error.isEmpty() && !cancelled
                          ? QStringLiteral("发现完成")
                          : (cancelled ? QStringLiteral("发现已取消")
                                       : QStringLiteral("发现失败")),
                      QJsonObject{{"candidateStates", states},
                                  {"verifiedIPs", verified},
                                  {"verifiedCount", session.result.verifiedIPs.size()},
                                  {"totalDurationMs",
                                   (session.result.totalDurationNs + 500000) / 1000000},
                                  {"cancelled", cancelled},
                                  {"complete", error.isEmpty() && !cancelled},
                                  {"error", error}});
}

// Admits a datagram only under the full 60-byte CONFIG ACK contract.
void processDatagram(DiscoverySession& session, const DiscoveryDatagram& dgram)
{
    const int index = session.indexByIp.value(dgram.sourceIp, -1);
    QJsonObject fields{{"sourceIp", dgram.sourceIp},
                       {"sourcePort", dgram.sourcePort},
                       {"length", dgram.payload.size()},
                       {"receivedMonotonicNs", QString::number(dgram.receivedNs)}};
    if (index < 0) {
        fields.insert(QStringLiteral("acceptResult"), QStringLiteral("unexpected_feedback"));
        fields.insert(QStringLiteral("ignoreReason"), QStringLiteral("not_a_candidate"));
        session.emitEvent(QStringLiteral("discovery.feedback_received"),
                          QStringLiteral("收到非候选地址反馈，已忽略"), fields);
        return;
    }
    DiscoveryCandidateEvidence& c = session.candidates[index];
    if (c.state == DiscoveryCandidateState::ExcludedLocal ||
        c.state == DiscoveryCandidateState::Invalid) {
        fields.insert(QStringLiteral("acceptResult"), QStringLiteral("unexpected_feedback"));
        fields.insert(QStringLiteral("ignoreReason"),
                      c.state == DiscoveryCandidateState::ExcludedLocal
                          ? QStringLiteral("local_address")
                          : QStringLiteral("invalid_candidate"));
        c.unexpectedFeedbackCount++;
        session.emitEvent(QStringLiteral("discovery.feedback_received"),
                          QStringLiteral("收到本机/无效候选反馈，已忽略"), fields);
        return;
    }
    if (session.firstSuccessSendNs[index] == 0 ||
        dgram.receivedNs < session.firstSuccessSendNs[index]) {
        fields.insert(QStringLiteral("acceptResult"), QStringLiteral("unexpected_feedback"));
        fields.insert(QStringLiteral("ignoreReason"),
                      QStringLiteral("before_first_successful_send"));
        c.unexpectedFeedbackCount++;
        session.emitEvent(QStringLiteral("discovery.feedback_received"),
                          QStringLiteral("收到先于本次发送的反馈，已忽略"), fields);
        return;
    }
    if (dgram.payload.size() == 60) {
        if (c.ack60Count == 0) {
            c.ack60Count = 1;
            c.firstAckNs = dgram.receivedNs;
            c.lastAckNs = dgram.receivedNs;
            c.state = DiscoveryCandidateState::Verified;
            c.verifiedRound = session.round;
            QJsonObject verified{{"ip", c.ip},
                                 {"verifiedRound", session.round},
                                 {"firstSendToAckMs",
                                  (c.firstAckNs - c.firstSendNs + 500000) / 1000000},
                                 {"lastSendToAckMs",
                                  (c.firstAckNs - c.lastSendNs + 500000) / 1000000},
                                 {"receivedMonotonicNs", QString::number(dgram.receivedNs)}};
            session.emitEvent(QStringLiteral("discovery.card_verified"),
                              QStringLiteral("60 字节 CONFIG 确认，采集卡身份已协议确认"),
                              verified);
        } else {
            c.duplicateAck60Count++;
            c.lastAckNs = dgram.receivedNs;
            fields.insert(QStringLiteral("acceptResult"), QStringLiteral("duplicate_ack60"));
            session.emitEvent(QStringLiteral("discovery.feedback_received"),
                              QStringLiteral("同一采集卡重复确认，仅计数"), fields);
        }
        return;
    }
    if (dgram.payload.size() == 18) {
        c.ready18Observed = true;
        fields.insert(QStringLiteral("acceptResult"), QStringLiteral("ready18_not_admitting"));
        session.emitEvent(QStringLiteral("discovery.feedback_received"),
                          QStringLiteral("18 字节就绪反馈不作为身份确认"), fields);
        return;
    }
    c.otherLengthCount++;
    fields.insert(QStringLiteral("acceptResult"), QStringLiteral("ignored_length"));
    session.emitEvent(QStringLiteral("discovery.feedback_received"),
                      QStringLiteral("非 60/18 字节反馈，已忽略"), fields);
}

// Drains the feedback socket until the absolute deadline; short select
// timeouts inside the channel are polling only and never fail a candidate.
void drainUntil(DiscoverySession& session,
                qint64 deadlineNs,
                const std::atomic<bool>& cancel)
{
    while (!cancel.load()) {
        std::vector<DiscoveryDatagram> dgrams =
            session.channel.waitAndReceive(deadlineNs, cancel);
        for (const DiscoveryDatagram& dgram : dgrams) processDatagram(session, dgram);
        if (session.channel.nowNs() >= deadlineNs) return;
    }
}

DiscoveryResult runDiscoveryCore(const DiscoveryOptions& rawOptions,
                                 const std::atomic<bool>& cancel,
                                 DiscoveryChannel& channel,
                                 const DiscoveryEnvironment& environment)
{
    const DiscoveryOptions options = sanitizeDiscoveryOptions(rawOptions, nullptr);
    DiscoveryResult result;
    result.discoveryId = options.discoveryId;
    const qint64 startedNs = channel.nowNs();

    DiscoverySession session{options, channel, environment, result, {}, {}, {}, startedNs};

    // 1. Candidate generation along ScanBaseIP/ScanIPCount, 1..254 bounded.
    const QStringList parts = options.baseIP.split('.');
    bool parsed = parts.size() == 4;
    int baseOctet = 0;
    if (parsed) baseOctet = parts[3].toInt(&parsed);
    if (!parsed) {
        result.error = QStringLiteral("ScanBaseIP 不是合法 IPv4：%1").arg(options.baseIP);
        emitComplete(session, result.error, false);
        return result;
    }
    const QString prefix = parts[0] + '.' + parts[1] + '.' + parts[2] + '.';
    for (int i = 0; i < options.candidateCount; ++i) {
        const int octet = baseOctet + i;
        if (octet < 1 || octet > 254) break;
        DiscoveryCandidateEvidence evidence;
        evidence.ip = prefix + QString::number(octet);
        session.indexByIp.insert(evidence.ip, static_cast<int>(session.candidates.size()));
        session.candidates.push_back(evidence);
    }
    if (session.candidates.empty()) {
        result.error = QStringLiteral("候选范围为空（%1 起 %2 个）")
                           .arg(options.baseIP).arg(options.candidateCount);
        emitComplete(session, result.error, false);
        return result;
    }
    session.firstSuccessSendNs.assign(session.candidates.size(), 0);

    // 2. Local IPv4 enumeration; every local address (including down
    // interfaces and loopback) is excluded before any CONFIG is sent.
    QString localError;
    std::vector<LocalAddressInfo> locals;
    if (environment.localAddresses)
        locals = environment.localAddresses(&localError);
    if (locals.empty()) {
        result.error = localError.isEmpty()
                           ? QStringLiteral("本机 IPv4 枚举结果为空，无法选择发送接口")
                           : localError;
        emitComplete(session, result.error, false);
        return result;
    }
    QHash<QString, LocalAddressInfo> localByIp;
    for (const LocalAddressInfo& local : locals) localByIp.insert(local.ip, local);
    for (DiscoveryCandidateEvidence& c : session.candidates) {
        const auto it = localByIp.constFind(c.ip);
        if (it == localByIp.constEnd()) continue;
        c.state = DiscoveryCandidateState::ExcludedLocal;
        c.note = QStringLiteral("interface=%1 index=%2 luid=%3 status=%4")
                     .arg(it->name, it->ifIndex, it->luid, it->operStatus);
        session.emitEvent(QStringLiteral("discovery.local_address_excluded"),
                          QStringLiteral("候选地址属于本机，已排除且不发送 CONFIG"),
                          QJsonObject{{"ip", c.ip},
                                      {"ifIndex", it->ifIndex},
                                      {"luid", it->luid},
                                      {"interfaceName", it->name},
                                      {"operStatus", it->operStatus}});
    }

    // 3. Optional ICMP/ARP reachability: diagnostics only, never identity.
    QStringList probeList;
    for (const DiscoveryCandidateEvidence& c : session.candidates)
        if (c.state == DiscoveryCandidateState::Pending) probeList.append(c.ip);
    if (environment.probe && !probeList.isEmpty() && !cancel.load()) {
        const QHash<QString, DiscoveryReachability> reach =
            environment.probe(probeList, options.icmpTimeoutMs, cancel);
        for (DiscoveryCandidateEvidence& c : session.candidates) {
            if (c.state != DiscoveryCandidateState::Pending) continue;
            const auto it = reach.constFind(c.ip);
            if (it == reach.constEnd()) continue;
            c.icmpReachable = it->icmpReachable;
            c.arpReachable = it->arpReachable;
            session.emitEvent(QStringLiteral("discovery.reachability"),
                              QStringLiteral("ICMP/ARP 可达性仅为诊断证据，不确认采集卡身份"),
                              QJsonObject{{"ip", c.ip},
                                          {"icmpReachable", c.icmpReachable},
                                          {"arpReachable", c.arpReachable},
                                          {"icmpReplyCount", it->icmpReplyCount},
                                          {"icmpReplyStatus", it->icmpReplyStatus},
                                          {"icmpError", it->icmpError},
                                          {"icmpTimedOut", it->icmpTimedOut},
                                          {"arpError", it->arpError},
                                          {"identityEvidence", false}});
        }
    }

    // 4. Local send interface: explicit LocalBindIP or Windows best route.
    QString bindIp;
    QString bindBasis;
    if (!options.localBindIP.isEmpty()) {
        if (!localByIp.contains(options.localBindIP)) {
            result.error = QStringLiteral("NetworkParams/LocalBindIP=%1 不是本机 IPv4")
                               .arg(options.localBindIP);
            emitComplete(session, result.error, false);
            return result;
        }
        if (session.indexByIp.contains(options.localBindIP)) {
            result.error = QStringLiteral("LocalBindIP=%1 与候选目标相同")
                               .arg(options.localBindIP);
            emitComplete(session, result.error, false);
            return result;
        }
        bindIp = options.localBindIP;
        bindBasis = QStringLiteral("explicit");
    } else {
        if (!environment.bestRouteSource) {
            result.error = QStringLiteral("未提供最佳路由源地址选择器");
            emitComplete(session, result.error, false);
            return result;
        }
        QString routeError;
        const QString source = environment.bestRouteSource(options.baseIP, &routeError);
        if (source.isEmpty()) {
            result.error = routeError.isEmpty()
                               ? QStringLiteral("未能获得可靠的本地源地址")
                               : routeError;
            emitComplete(session, result.error, false);
            return result;
        }
        if (!localByIp.contains(source)) {
            result.error = QStringLiteral("最佳路由源地址 %1 不是本机 IPv4").arg(source);
            emitComplete(session, result.error, false);
            return result;
        }
        if (session.indexByIp.contains(source)) {
            result.error = QStringLiteral("最佳路由源地址 %1 与候选目标相同").arg(source);
            emitComplete(session, result.error, false);
            return result;
        }
        bindIp = source;
        bindBasis = QStringLiteral("best_route");
    }
    result.localBindIP = bindIp;
    result.localBindBasis = bindBasis;

    QJsonArray candidateArray;
    for (const DiscoveryCandidateEvidence& c : session.candidates)
        candidateArray.append(c.ip);
    QJsonArray localArray;
    for (const LocalAddressInfo& local : locals) {
        QJsonObject entry{{"address", local.ip},
                          {"ifIndex", local.ifIndex},
                          {"luid", local.luid},
                          {"name", local.name},
                          {"operStatus", local.operStatus}};
        localArray.append(entry);
    }
    session.emitEvent(QStringLiteral("discovery.begin"),
                      QStringLiteral("CONFIG-ACK 发现开始"),
                      QJsonObject{{"baseIP", options.baseIP},
                                  {"candidateCount", options.candidateCount},
                                  {"candidates", candidateArray},
                                  {"localIPv4", localArray},
                                  {"localBindIP", bindIp},
                                  {"localBindBasis", bindBasis},
                                  {"configDurationNs", options.configDurationNs},
                                  {"delayANs", options.delayANs},
                                  {"delayBNs", options.delayBNs},
                                  {"ackWindowMs", options.ackWindowMs},
                                  {"maxAttempts", options.maxAttempts},
                                  {"finalGraceMs", options.finalGraceMs},
                                  {"icmpTimeoutMs", options.icmpTimeoutMs},
                                  {"controlPort", kDiscoveryControlPort},
                                  {"feedbackPort", kDiscoveryFeedbackPort}});

    // 5. Open the temporary channel; RAII closes it on every return path so
    // the formal 8000 feedback socket can bind immediately afterwards.
    QString openError;
    if (!channel.open(bindIp, &openError)) {
        result.error = openError;
        emitComplete(session, result.error, false);
        return result;
    }
    struct ChannelGuard {
        DiscoveryChannel* channel;
        ~ChannelGuard() { if (channel) channel->close(); }
    } channelGuard{&channel};

    const Command config = configCommand(options.configDurationNs,
                                         options.delayANs,
                                         options.delayBNs);

    // 6. Bounded multi-round CONFIG probing; only unverified candidates with
    // remaining send budget are re-sent each round.
    bool cancelled = false;
    for (session.round = 1; session.round <= options.maxAttempts; ++session.round) {
        if (cancel.load()) { cancelled = true; break; }
        for (std::size_t i = 0; i < session.candidates.size(); ++i) {
            DiscoveryCandidateEvidence& c = session.candidates[i];
            if (c.state != DiscoveryCandidateState::Pending) continue;
            if (c.attemptsSent >= options.maxAttempts) continue;
            int sendError = 0;
            const int sent = channel.sendConfig(c.ip, config.data(), config.size(), &sendError);
            const qint64 nowNs = channel.nowNs();
            c.attemptsSent++;
            c.lastSendNs = nowNs;
            QJsonObject fields{{"ip", c.ip},
                               {"attempt", session.round},
                               {"sendtoBytes", sent},
                               {"sendError", sent > 0 ? 0 : sendError},
                               {"sendMonotonicNs", QString::number(nowNs)}};
            if (sent > 0) {
                if (c.firstSendNs == 0) c.firstSendNs = nowNs;
                if (session.firstSuccessSendNs[i] == 0) session.firstSuccessSendNs[i] = nowNs;
                fields.insert(QStringLiteral("sent"), true);
            } else {
                c.sendFailures++;
                c.lastSendError = sendError;
                fields.insert(QStringLiteral("sent"), false);
            }
            session.emitEvent(QStringLiteral("discovery.config_send"),
                               sent > 0 ? QStringLiteral("CONFIG 已发送（仅代表协议栈接受发送）")
                                        : QStringLiteral("CONFIG 发送失败"),
                               fields);
        }
        drainUntil(session, channel.nowNs() + qint64(options.ackWindowMs) * 1000000, cancel);
        if (cancel.load()) { cancelled = true; break; }
    }
    // 7. Final grace window keeps accepting late 60-byte confirmations.
    if (!cancelled && !cancel.load()) {
        drainUntil(session, channel.nowNs() + qint64(options.finalGraceMs) * 1000000, cancel);
        if (cancel.load()) cancelled = true;
    }

    // 8. Final classification; only Verified candidates become card targets.
    if (!cancelled) {
        for (DiscoveryCandidateEvidence& c : session.candidates) {
            if (c.state != DiscoveryCandidateState::Pending) continue;
            c.state = (c.attemptsSent > 0 && c.sendFailures == c.attemptsSent)
                          ? DiscoveryCandidateState::SendFailed
                          : DiscoveryCandidateState::TimedOut;
            session.emitEvent(QStringLiteral("discovery.candidate_timeout"),
                               QStringLiteral("候选地址未在发现窗口内返回合格确认"),
                               QJsonObject{{"ip", c.ip},
                                           {"attemptsSent", c.attemptsSent},
                                           {"sendFailures", c.sendFailures},
                                           {"waitedMs",
                                            (channel.nowNs() - (c.firstSendNs ? c.firstSendNs : startedNs)
                                             + 500000) / 1000000},
                                           {"lastSendError", c.lastSendError},
                                           {"finalState", discoveryCandidateStateName(c.state)}});
        }
    }

    QVector<QPair<std::uint32_t, QString>> verifiedPairs;
    for (DiscoveryCandidateEvidence& c : session.candidates) {
        if (c.state != DiscoveryCandidateState::Verified) continue;
        bool ok = false;
        const std::uint32_t hostOrder = discoveryIpToHostOrder(c.ip, &ok);
        if (ok) verifiedPairs.append({hostOrder, c.ip});
    }
    std::sort(verifiedPairs.begin(), verifiedPairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& pair : verifiedPairs)
        if (!result.verifiedIPs.contains(pair.second)) result.verifiedIPs.append(pair.second);

    for (const DiscoveryCandidateEvidence& c : session.candidates)
        result.candidates.append(c);
    result.cancelled = cancelled;
    result.totalDurationNs = channel.nowNs() - startedNs;
    emitComplete(session, QString(), cancelled);
    return result;
}

#ifdef _WIN32

QString sockaddrToIpv4(const sockaddr* address)
{
    if (!address || address->sa_family != AF_INET) return QString();
    const auto* in = reinterpret_cast<const sockaddr_in*>(address);
    char text[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) return QString();
    return QString::fromLatin1(text);
}

QString operStatusText(IF_OPER_STATUS status)
{
    switch (status) {
        case IfOperStatusUp: return QStringLiteral("up");
        case IfOperStatusDown: return QStringLiteral("down");
        case IfOperStatusTesting: return QStringLiteral("testing");
        case IfOperStatusUnknown: return QStringLiteral("unknown");
        case IfOperStatusDormant: return QStringLiteral("dormant");
        case IfOperStatusNotPresent: return QStringLiteral("not_present");
        case IfOperStatusLowerLayerDown: return QStringLiteral("lower_layer_down");
    }
    return QStringLiteral("unknown");
}

// Real local IPv4 enumeration: every adapter (up or down, connected or not)
// contributes its unicast IPv4 addresses; loopback is guaranteed present.
std::vector<LocalAddressInfo> enumerateLocalIpv4Addresses(QString* error)
{
    std::vector<LocalAddressInfo> out;
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> storage(bytes);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        storage.resize(bytes);
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                                      reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()),
                                      &bytes);
    }
    if (status != NO_ERROR) {
        if (error) *error = QStringLiteral("GetAdaptersAddresses 失败，错误码 %1").arg(int(status));
        return out;
    }
    bool loopbackSeen = false;
    for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data());
         adapter != nullptr; adapter = adapter->Next) {
        const QString name = adapter->FriendlyName
                                 ? QString::fromWCharArray(adapter->FriendlyName)
                                 : QStringLiteral("unknown");
        const QString operStatus = operStatusText(adapter->OperStatus);
        for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const QString address = sockaddrToIpv4(unicast->Address.lpSockaddr);
            if (address.isEmpty()) continue;
            LocalAddressInfo info;
            info.ip = address;
            info.ifIndex = QString::number(adapter->IfIndex);
            info.luid = QString::number(adapter->Luid.Value);
            info.name = name;
            info.operStatus = operStatus;
            if (address == QStringLiteral("127.0.0.1")) loopbackSeen = true;
            out.push_back(info);
        }
    }
    if (!loopbackSeen) {
        out.push_back({QStringLiteral("127.0.0.1"), QStringLiteral("unknown"),
                       QStringLiteral("unknown"), QStringLiteral("synthetic-loopback"),
                       QStringLiteral("unknown")});
    }
    return out;
}

QString bestRouteSourceIp(const QString& destinationIp, QString* error)
{
    SOCKADDR_INET destination{};
    destination.si_family = AF_INET;
    const QByteArray encoded = destinationIp.toLatin1();
    if (InetPtonA(AF_INET, encoded.constData(), &destination.Ipv4.sin_addr) != 1) {
        if (error) *error = QStringLiteral("GetBestRoute2 目标地址非法：%1").arg(destinationIp);
        return QString();
    }
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET source{};
    const ULONG status = GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source);
    if (status != NO_ERROR) {
        if (error) *error = QStringLiteral("GetBestRoute2 失败，错误码 %1").arg(int(status));
        return QString();
    }
    if (source.si_family != AF_INET || source.Ipv4.sin_addr.s_addr == 0) {
        if (error) *error = QStringLiteral("GetBestRoute2 未返回有效 IPv4 源地址");
        return QString();
    }
    return sockaddrToIpv4(reinterpret_cast<const sockaddr*>(&source));
}

DiscoveryReachability probeOne(const QString& ip, int timeoutMs)
{
    DiscoveryReachability result;
    result.probed = true;
    const QByteArray encoded = ip.toLatin1();
    const IPAddr dest = inet_addr(encoded.constData());
    HANDLE icmp = IcmpCreateFile();
    if (icmp != INVALID_HANDLE_VALUE) {
        const unsigned char payload[4] = {0x70, 0x61, 0x69, 0x6e};
        const DWORD replySize = sizeof(ICMP_ECHO_REPLY) + 32;
        std::vector<unsigned char> reply(replySize);
        const DWORD replies = IcmpSendEcho(icmp, dest, const_cast<unsigned char*>(payload),
                                           sizeof(payload), nullptr, reply.data(), replySize,
                                           DWORD(timeoutMs));
        result.icmpReplyCount = int(replies);
        if (replies > 0) {
            const auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
            result.icmpReplyStatus = int(echo->Status);
            result.icmpReachable = echo->Status == IP_SUCCESS;
        } else {
            result.icmpError = int(GetLastError());
            result.icmpTimedOut = result.icmpError == IP_REQ_TIMED_OUT;
        }
        IcmpCloseHandle(icmp);
    } else {
        result.icmpError = int(GetLastError());
    }
    // ARP fallback mirrors the historical scan criterion: only when ICMP
    // errored without timing out. Never used as identity evidence.
    if (!result.icmpReachable && !result.icmpTimedOut) {
        ULONG mac[2] = {};
        ULONG macLength = sizeof(mac);
        result.arpError = int(SendARP(dest, 0, mac, &macLength));
        result.arpReachable = result.arpError == NO_ERROR && macLength >= 6;
    }
    return result;
}

QHash<QString, DiscoveryReachability> probeReachability(const QStringList& ips,
                                                        int timeoutMs,
                                                        const std::atomic<bool>& cancel)
{
    QHash<QString, DiscoveryReachability> out;
    if (ips.isEmpty()) return out;
    std::vector<DiscoveryReachability> probeResults(ips.size());
    const int threadCount = int(std::min<size_t>(16, ips.size()));
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([&ips, &probeResults, t, threadCount, timeoutMs, &cancel]() {
            for (int i = t; i < int(ips.size()); i += threadCount) {
                if (cancel.load()) return;
                probeResults[i] = probeOne(ips[i], timeoutMs);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (int i = 0; i < int(ips.size()); ++i) out.insert(ips[i], probeResults[i]);
    return out;
}

// Production channel: owns the WinSock reference, the temporary
// 0.0.0.0:8000 feedback socket and the interface-bound control socket.
class WinsockDiscoveryChannel final : public DiscoveryChannel {
public:
    ~WinsockDiscoveryChannel() override { close(); }

    bool open(const QString& localBindIp, QString* error) override
    {
        close();
        WSADATA data{};
        const int wsaStatus = WSAStartup(MAKEWORD(2, 2), &data);
        if (wsaStatus != 0) {
            if (error) *error = QStringLiteral("WSAStartup 失败，错误码 %1").arg(wsaStatus);
            return false;
        }
        wsa_ = true;

        feedback_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (feedback_ == INVALID_SOCKET) {
            fail(error, QStringLiteral("创建反馈 socket 失败"));
            return false;
        }
        DWORD receiveBuffer = 64 * 1024 * 1024;
        if (setsockopt(feedback_, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer))) {
            fail(error, QStringLiteral("设置反馈接收缓冲失败"));
            return false;
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port = htons(kDiscoveryFeedbackPort);
        any.sin_addr.s_addr = INADDR_ANY;
        if (bind(feedback_, reinterpret_cast<sockaddr*>(&any), sizeof(any))) {
            fail(error, QStringLiteral("绑定临时反馈端口 8000 失败"));
            return false;
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(feedback_, FIONBIO, &nonBlocking)) {
            fail(error, QStringLiteral("设置反馈 socket 非阻塞失败"));
            return false;
        }

        control_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (control_ == INVALID_SOCKET) {
            fail(error, QStringLiteral("创建控制 socket 失败"));
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        const QByteArray encoded = localBindIp.toLatin1();
        if (inet_pton(AF_INET, encoded.constData(), &local.sin_addr) != 1 ||
            local.sin_addr.s_addr == 0) {
            fail(error, QStringLiteral("本地绑定地址非法：%1").arg(localBindIp));
            return false;
        }
        if (bind(control_, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
            fail(error, QStringLiteral("控制 socket 绑定 %1 失败").arg(localBindIp));
            return false;
        }
        DWORD sendTimeout = 500;
        setsockopt(control_, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
        return true;
    }

    void close() override
    {
        if (feedback_ != INVALID_SOCKET) { closesocket(feedback_); feedback_ = INVALID_SOCKET; }
        if (control_ != INVALID_SOCKET) { closesocket(control_); control_ = INVALID_SOCKET; }
        if (wsa_) { WSACleanup(); wsa_ = false; }
    }

    qint64 nowNs() const override
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int sendConfig(const QString& ip, const std::uint8_t* data, std::size_t size,
                   int* errorCode) override
    {
        if (control_ == INVALID_SOCKET) {
            if (errorCode) *errorCode = WSAENOTSOCK;
            return -1;
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(kDiscoveryControlPort);
        const QByteArray encoded = ip.toLatin1();
        if (inet_pton(AF_INET, encoded.constData(), &target.sin_addr) != 1) {
            if (errorCode) *errorCode = WSAEADDRNOTAVAIL;
            return -1;
        }
        const int sent = sendto(control_, reinterpret_cast<const char*>(data),
                                int(size), 0, reinterpret_cast<sockaddr*>(&target),
                                sizeof(target));
        if (sent == int(size)) return sent;
        if (errorCode) *errorCode = WSAGetLastError();
        return -1;
    }

    std::vector<DiscoveryDatagram> waitAndReceive(qint64 deadlineNs,
                                                  const std::atomic<bool>& cancel) override
    {
        std::vector<DiscoveryDatagram> out;
        if (feedback_ == INVALID_SOCKET) return out;
        std::vector<char> buffer(65536);
        while (!cancel.load()) {
            for (;;) {
                sockaddr_in source{};
                int sourceLength = sizeof(source);
                const int received = recvfrom(feedback_, buffer.data(), int(buffer.size()), 0,
                                              reinterpret_cast<sockaddr*>(&source),
                                              &sourceLength);
                if (received == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK && error != WSAEINTR) lastError_ = error;
                    break;
                }
                DiscoveryDatagram dgram;
                dgram.sourceIp = sockaddrToIpv4(reinterpret_cast<const sockaddr*>(&source));
                dgram.sourcePort = ntohs(source.sin_port);
                dgram.payload = QByteArray(buffer.data(), received);
                dgram.receivedNs = nowNs();
                out.push_back(std::move(dgram));
            }
            if (!out.empty()) return out;
            const qint64 now = nowNs();
            if (now >= deadlineNs) return out;
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(feedback_, &readable);
            const qint64 remainingNs = deadlineNs - now;
            const qint64 sliceNs = std::min(remainingNs, qint64(50) * 1000000);
            timeval timeout{long(sliceNs / 1000000000), long((sliceNs % 1000000000) / 1000)};
            const int status = select(0, &readable, nullptr, nullptr, &timeout);
            if (status == SOCKET_ERROR) {
                lastError_ = WSAGetLastError();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        return out;
    }

    int lastError() const { return lastError_; }

private:
    void fail(QString* error, const QString& what)
    {
        const int code = WSAGetLastError();
        if (error) *error = QStringLiteral("%1，WinSock 错误码 %2").arg(what).arg(code);
        lastError_ = code;
        close();
    }

    SOCKET feedback_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    bool wsa_ = false;
    int lastError_ = 0;
};

#endif // _WIN32

} // namespace

DiscoveryResult runDiscovery(const DiscoveryOptions& options,
                             const std::atomic<bool>& cancel,
                             DiscoveryChannel& channel,
                             const DiscoveryEnvironment& environment)
{
    return runDiscoveryCore(options, cancel, channel, environment);
}

DiscoveryResult runDiscovery(const DiscoveryOptions& options, const std::atomic<bool>& cancel)
{
#ifdef _WIN32
    DiscoveryEnvironment environment;
    environment.localAddresses = [](QString* error) {
        return enumerateLocalIpv4Addresses(error);
    };
    environment.bestRouteSource = [](const QString& destinationIp, QString* error) {
        return bestRouteSourceIp(destinationIp, error);
    };
    environment.probe = [](const QStringList& ips, int timeoutMs,
                           const std::atomic<bool>& cancelFlag) {
        return probeReachability(ips, timeoutMs, cancelFlag);
    };
    environment.eventSink = [](const QString& category, const QString& message,
                               const QJsonObject& fields) {
        if (auto* recorder = DiagnosticRecorder::instance())
            recorder->recordEvent(category, message, DiagnosticRecorder::Severity::Info, fields);
    };
    WinsockDiscoveryChannel channel;
    return runDiscoveryCore(options, cancel, channel, environment);
#else
    Q_UNUSED(options);
    Q_UNUSED(cancel);
    DiscoveryResult result;
    result.error = QStringLiteral("动态发现当前仅支持 Windows 平台");
    return result;
#endif
}

} // namespace paimage
