#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace paimage {

// Dynamic acquisition-card discovery by CONFIG ACK only.
//
// A candidate IP becomes a card target when, during this discovery session,
// the temporary feedback socket receives a 60-byte CONFIG acknowledgement
// from that IP after at least one successful CONFIG send. ICMP/ARP
// reachability and 18-byte ready feedback are diagnostic evidence and never
// admit a card. Local IPv4 addresses (including down interfaces) are
// excluded before any CONFIG is sent.
struct DiscoveryOptions {
    QString baseIP;
    int candidateCount = 32;
    QString localBindIP;            // NetworkParams/LocalBindIP; empty = auto
    int configDurationNs = 20000;
    int delayANs = 200;
    int delayBNs = 200;
    int ackWindowMs = 2000;
    int maxAttempts = 5;
    int finalGraceMs = 500;
    int icmpTimeoutMs = 150;
    QString discoveryId;
};

enum class DiscoveryCandidateState {
    Pending = 0,
    ExcludedLocal,
    Invalid,
    SendFailed,
    Verified,
    TimedOut
};

QString discoveryCandidateStateName(DiscoveryCandidateState state);

struct DiscoveryReachability {
    bool probed = false;
    bool icmpReachable = false;
    bool arpReachable = false;
    bool icmpTimedOut = false;
    int icmpReplyCount = 0;
    int icmpReplyStatus = 0;    // reachability requires IP_SUCCESS
    int icmpError = 0;
    int arpError = 0;
};

struct DiscoveryCandidateEvidence {
    QString ip;
    DiscoveryCandidateState state = DiscoveryCandidateState::Pending;
    bool icmpReachable = false;
    bool arpReachable = false;
    bool ready18Observed = false;
    int attemptsSent = 0;
    int sendFailures = 0;
    int ack60Count = 0;
    int duplicateAck60Count = 0;
    int otherLengthCount = 0;
    int unexpectedFeedbackCount = 0;
    int lastSendError = 0;
    int verifiedRound = 0;
    qint64 firstSendNs = 0;
    qint64 lastSendNs = 0;
    qint64 firstAckNs = 0;
    qint64 lastAckNs = 0;
    QString note;
};

struct DiscoveryDatagram {
    QString sourceIp;
    std::uint16_t sourcePort = 0;
    QByteArray payload;
    qint64 receivedNs = 0;
};

struct DiscoveryResult {
    QString discoveryId;
    QVector<QString> verifiedIPs;
    QVector<DiscoveryCandidateEvidence> candidates;
    QString localBindIP;
    QString localBindBasis;     // "explicit" | "best_route"
    QString error;
    bool cancelled = false;
    qint64 totalDurationNs = 0;
};

// Transport seam. The production implementation owns the WinSock reference,
// the temporary 0.0.0.0:8000 feedback socket and the interface-bound control
// socket, all released by close() (RAII in runDiscovery).
class DiscoveryChannel {
public:
    virtual ~DiscoveryChannel() = default;
    virtual bool open(const QString& localBindIp, QString* error) = 0;
    virtual void close() = 0;
    virtual qint64 nowNs() const = 0;
    // Returns bytes sent (>0) or -1; *errorCode carries the failing error.
    virtual int sendConfig(const QString& ip,
                           const std::uint8_t* data,
                           std::size_t size,
                           int* errorCode) = 0;
    // Blocks until at least one datagram is available, the absolute monotonic
    // deadline passes, or cancel is set. Short select timeouts inside the
    // implementation are polling only and never mark a candidate failed.
    virtual std::vector<DiscoveryDatagram> waitAndReceive(
        qint64 deadlineNs, const std::atomic<bool>& cancel) = 0;
};

struct LocalAddressInfo {
    QString ip;
    QString ifIndex;    // "unknown" when a field cannot be obtained
    QString luid;
    QString name;
    QString operStatus;
};
using LocalAddressProvider =
    std::function<std::vector<LocalAddressInfo>(QString* error)>;
// Returns the Windows-selected local source IPv4 for reaching destinationIp,
// or an empty string and an error description.
using BestRouteSourceFn =
    std::function<QString(const QString& destinationIp, QString* error)>;
using ReachabilityProbe = std::function<QHash<QString, DiscoveryReachability>(
    const QStringList& ips, int timeoutMs, const std::atomic<bool>& cancel)>;
// category is the full event name such as "discovery.begin".
using DiscoveryEventSink = std::function<void(
    const QString& category, const QString& message, const QJsonObject& fields)>;

struct DiscoveryEnvironment {
    LocalAddressProvider localAddresses;
    BestRouteSourceFn bestRouteSource;
    ReachabilityProbe probe;
    DiscoveryEventSink eventSink;
};

// Bounded, synchronous, UI-free. On every return path the channel has been
// closed again, so the formal 8000 feedback socket can bind afterwards.
DiscoveryResult runDiscovery(const DiscoveryOptions& options,
                             const std::atomic<bool>& cancel,
                             DiscoveryChannel& channel,
                             const DiscoveryEnvironment& environment);

// Production convenience: Winsock channel plus real local-address
// enumeration, best-route source selection and ICMP/ARP diagnostics.
DiscoveryResult runDiscovery(const DiscoveryOptions& options,
                             const std::atomic<bool>& cancel);

// Clamps out-of-range values back to the documented defaults and reports
// every normalization in normalizationNote for diagnostic logging.
DiscoveryOptions sanitizeDiscoveryOptions(const DiscoveryOptions& options,
                                          QString* normalizationNote);

// UI-thread acceptance guard: a finished result may only be applied when the
// window is still alive and the result belongs to the still-active session.
bool canApplyDiscoveryResult(bool windowAlive,
                             const QString& activeDiscoveryId,
                             const DiscoveryResult& result);

// Numeric IPv4 ordering helper shared with tests; ok=false for invalid text.
std::uint32_t discoveryIpToHostOrder(const QString& ip, bool* ok = nullptr);

} // namespace paimage
