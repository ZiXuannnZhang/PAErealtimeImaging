#include "PaimageAcquisition/CardDiscovery.h"
#include "PaimageAcquisition/SourceCore.h"

#include <QJsonObject>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace paimage;

namespace {

class FakeChannel final : public DiscoveryChannel {
public:
    qint64 now = 1000000000; // start away from zero so first-send time is set
    bool openOk = true;
    QString openErrorText;
    int openCount = 0;
    int closeCount = 0;
    std::vector<std::pair<QString, qint64>> sends;
    std::function<bool(const QString&)> sendShouldFail;
    std::function<void(const QString&)> onSend;
    std::vector<std::pair<qint64, DiscoveryDatagram>> arrivals;
    std::size_t cursor = 0;

    bool open(const QString&, QString* error) override
    {
        ++openCount;
        if (!openOk) {
            if (error) *error = openErrorText;
            return false;
        }
        return true;
    }
    void close() override { ++closeCount; }
    qint64 nowNs() const override { return now; }

    int sendConfig(const QString& ip, const std::uint8_t* data, std::size_t size,
                   int* errorCode) override
    {
        if (sendShouldFail && sendShouldFail(ip)) {
            if (errorCode) *errorCode = 10065;
            return -1;
        }
        sends.push_back({ip, now});
        if (onSend) onSend(ip);
        return int(size);
    }

    std::vector<DiscoveryDatagram> waitAndReceive(qint64 deadlineNs,
                                                  const std::atomic<bool>& cancel) override
    {
        std::vector<DiscoveryDatagram> out;
        while (cursor < arrivals.size() && arrivals[cursor].first <= deadlineNs &&
               !cancel.load()) {
            now = std::max(now, arrivals[cursor].first);
            out.push_back(arrivals[cursor].second);
            ++cursor;
        }
        if (now < deadlineNs) now = deadlineNs;
        return out;
    }

    void addArrival(qint64 atNs, const QString& ip, int length)
    {
        DiscoveryDatagram dgram;
        dgram.sourceIp = ip;
        dgram.sourcePort = 50000;
        dgram.payload = QByteArray(length, char(0x5a));
        dgram.receivedNs = atNs;
        arrivals.push_back({atNs, std::move(dgram)});
        std::sort(arrivals.begin(), arrivals.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }
};

struct RecordedEvent {
    QString category;
    QJsonObject fields;
};

struct Fixture {
    FakeChannel channel;
    std::vector<QString> localIps = {QStringLiteral("192.168.0.100")};
    QHash<QString, DiscoveryReachability> reach;
    std::vector<RecordedEvent> events;
    std::atomic<bool> cancel{false};

    DiscoveryEnvironment environment()
    {
        DiscoveryEnvironment env;
        env.localAddresses = [this](QString*) {
            std::vector<LocalAddressInfo> out;
            for (const QString& ip : localIps)
                out.push_back({ip, QStringLiteral("1"), QStringLiteral("2"),
                               QStringLiteral("fake-if"), QStringLiteral("down")});
            return out;
        };
        env.bestRouteSource = [](const QString&, QString*) {
            return QStringLiteral("192.168.0.100");
        };
        env.probe = [this](const QStringList& ips, int, const std::atomic<bool>&) {
            QHash<QString, DiscoveryReachability> out;
            for (const QString& ip : ips) {
                DiscoveryReachability r = reach.value(ip);
                r.probed = true;
                out.insert(ip, r);
            }
            return out;
        };
        env.eventSink = [this](const QString& category, const QString&, const QJsonObject& fields) {
            events.push_back({category, fields});
        };
        return env;
    }

    int sendCount(const QString& ip) const
    {
        int count = 0;
        for (const auto& send : channel.sends)
            if (send.first == ip) ++count;
        return count;
    }

    const DiscoveryCandidateEvidence* evidence(const DiscoveryResult& result,
                                               const QString& ip) const
    {
        for (const auto& c : result.candidates)
            if (c.ip == ip) return &c;
        return nullptr;
    }

    std::vector<RecordedEvent> eventsNamed(const QString& category) const
    {
        std::vector<RecordedEvent> out;
        for (const auto& e : events)
            if (e.category == category) out.push_back(e);
        return out;
    }
};

DiscoveryOptions baseOptions(int count = 4)
{
    DiscoveryOptions options;
    options.baseIP = QStringLiteral("192.168.0.2");
    options.candidateCount = count;
    options.ackWindowMs = 500;   // sanitized minimum; fake clock makes it instant
    options.maxAttempts = 5;
    options.finalGraceMs = 0;
    options.icmpTimeoutMs = 50;
    options.discoveryId = QStringLiteral("disco-1");
    return options;
}

} // namespace

static void require(bool condition, const char* what)
{
    if (!condition) throw std::runtime_error(what);
}

static void requireState(const DiscoveryResult& result, const QString& ip,
                         DiscoveryCandidateState state, const char* what)
{
    for (const auto& c : result.candidates)
        if (c.ip == ip) require(c.state == state, what);
}

int main()
{
    // 1. Local address .10 (ICMP success) is excluded and never receives CONFIG.
    {
        Fixture f;
        f.localIps = {QStringLiteral("192.168.0.10")};
        DiscoveryReachability up;
        up.icmpReachable = true;
        f.reach.insert(QStringLiteral("192.168.0.10"), up);
        const DiscoveryResult result = runDiscovery(baseOptions(9), f.cancel, f.channel, f.environment());
        requireState(result, QStringLiteral("192.168.0.10"), DiscoveryCandidateState::ExcludedLocal, "local excluded");
        require(f.sendCount(QStringLiteral("192.168.0.10")) == 0, "no CONFIG to local address");
        require(result.verifiedIPs.isEmpty(), "no cards without ACK");
        require(!f.eventsNamed(QStringLiteral("discovery.local_address_excluded")).empty(), "exclusion logged");
    }

    // 2. ICMP-reachable host without 60-byte ACK never becomes a card.
    {
        Fixture f;
        DiscoveryReachability up;
        up.icmpReachable = true;
        f.reach.insert(QStringLiteral("192.168.0.2"), up);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "icmp success is not identity");
        requireState(result, QStringLiteral("192.168.0.2"), DiscoveryCandidateState::TimedOut, "icmp host timed out");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->icmpReachable, "icmp evidence recorded");
    }

    // 3. ICMP-timeout candidate with a 60-byte ACK becomes a card.
    {
        Fixture f;
        DiscoveryReachability down;
        down.icmpTimedOut = true;
        f.reach.insert(QStringLiteral("192.168.0.3"), down);
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.3"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs == QVector<QString>{QStringLiteral("192.168.0.3")}, "slow icmp host verified");
        requireState(result, QStringLiteral("192.168.0.3"), DiscoveryCandidateState::Verified, "verified state");
        require(!f.eventsNamed(QStringLiteral("discovery.card_verified")).empty(), "verified event logged");
    }

    // 4. First-round timeout, third-round ACK is accepted.
    {
        Fixture f;
        const qint64 round3SendNs = f.channel.now + 2 * qint64(500) * 1000000;
        f.channel.addArrival(round3SendNs + 100000000, QStringLiteral("192.168.0.2"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.size() == 1, "round-3 ack accepted");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->verifiedRound == 3, "verified in round 3");
        require(evidence && evidence->attemptsSent == 3, "three attempts sent");
    }

    // 5. Duplicate 60-byte ACKs produce one card and correct duplicate count.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.2"), 60);
        f.channel.addArrival(f.channel.now + 150000000, QStringLiteral("192.168.0.2"), 60);
        f.channel.addArrival(f.channel.now + 200000000, QStringLiteral("192.168.0.2"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.size() == 1, "duplicate acks yield one card");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->ack60Count == 1, "one accepted ack");
        require(evidence && evidence->duplicateAck60Count == 2, "two duplicates counted");
    }

    // 6. Only 18-byte ready feedback never confirms a card.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.2"), 18);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "18-byte is not identity");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->ready18Observed, "ready18 observed");
        requireState(result, QStringLiteral("192.168.0.2"), DiscoveryCandidateState::TimedOut, "ready18 timed out");
    }

    // 7. 59/61-byte feedback is ignored.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.2"), 59);
        f.channel.addArrival(f.channel.now + 150000000, QStringLiteral("192.168.0.2"), 61);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "wrong lengths never confirm");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->otherLengthCount == 2, "other lengths counted");
    }

    // 8. 60-byte ACK from a non-candidate IP is recorded and ignored.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.77"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "non-candidate ack ignored");
        const auto unexpected = f.eventsNamed(QStringLiteral("discovery.feedback_received"));
        require(!unexpected.empty() &&
                unexpected.front().fields.value(QStringLiteral("acceptResult")).toString() ==
                    QStringLiteral("unexpected_feedback"),
                "unexpected feedback logged");
    }

    // 9. A 60-byte ACK that precedes any successful CONFIG send is ignored.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now - 500000000, QStringLiteral("192.168.0.2"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "pre-send ack ignored");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.2"));
        require(evidence && evidence->unexpectedFeedbackCount == 1, "pre-send ack counted");
    }

    // 10. Persistent sendto failure ends as SendFailed; other cards unaffected.
    {
        Fixture f;
        f.channel.sendShouldFail = [](const QString& ip) {
            return ip == QStringLiteral("192.168.0.5");
        };
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.2"), 60);
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.3"), 60);
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.4"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        requireState(result, QStringLiteral("192.168.0.5"), DiscoveryCandidateState::SendFailed, "send failed state");
        const auto* evidence = f.evidence(result, QStringLiteral("192.168.0.5"));
        require(evidence && evidence->attemptsSent == 5 && evidence->sendFailures == 5,
                "all five attempts failed");
        require(result.verifiedIPs.size() == 3, "other candidates verified");
    }

    // 11. All candidates time out: empty list, nothing verified.
    {
        Fixture f;
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty(), "no candidates verified");
        for (const auto& c : result.candidates)
            require(c.state == DiscoveryCandidateState::TimedOut, "all timed out");
        require(f.channel.closeCount >= 1, "channel closed");
    }

    // 12. Out-of-order confirmations still map to .2,.3,.4,.5.
    {
        Fixture f;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.5"), 60);
        f.channel.addArrival(f.channel.now + 120000000, QStringLiteral("192.168.0.3"), 60);
        f.channel.addArrival(f.channel.now + 140000000, QStringLiteral("192.168.0.4"), 60);
        f.channel.addArrival(f.channel.now + 160000000, QStringLiteral("192.168.0.2"), 60);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs ==
                    (QVector<QString>{QStringLiteral("192.168.0.2"), QStringLiteral("192.168.0.3"),
                                      QStringLiteral("192.168.0.4"), QStringLiteral("192.168.0.5")}),
                "numeric ordering");
    }

    // 13. Cancellation exits promptly and closes the channel.
    {
        Fixture f;
        f.cancel.store(true);
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.cancelled, "cancelled flag");
        require(result.verifiedIPs.isEmpty(), "no cards on cancel");
        require(f.channel.closeCount >= 1, "channel closed on cancel");
        require(f.channel.sends.empty(), "no sends after pre-cancel");
    }
    {
        Fixture f;
        f.channel.onSend = [&](const QString&) { f.cancel.store(true); };
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.cancelled, "mid-wait cancel honoured");
        require(f.channel.closeCount >= 1, "channel closed after mid-wait cancel");
    }

    // Expected-count early exit: once four cards carry a verified 60-byte ACK,
    // discovery closes in round 1 — later rounds never re-send CONFIG and the
    // final grace never admits a later fifth card (documented trade-off).
    {
        Fixture f;
        DiscoveryOptions options = baseOptions(5);
        options.finalGraceMs = 500;
        options.expectedCardCount = 4;
        const qint64 start = f.channel.now;
        f.channel.addArrival(start + 100000000, QStringLiteral("192.168.0.2"), 60);
        f.channel.addArrival(start + 120000000, QStringLiteral("192.168.0.3"), 60);
        f.channel.addArrival(start + 140000000, QStringLiteral("192.168.0.4"), 60);
        f.channel.addArrival(start + 160000000, QStringLiteral("192.168.0.5"), 60);
        // A fifth card answers late, inside the would-be final grace window.
        f.channel.addArrival(start + 5 * qint64(500) * 1000000 + 100000000,
                             QStringLiteral("192.168.0.6"), 60);
        const DiscoveryResult result = runDiscovery(options, f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.size() == 4, "expected four cards close discovery");
        require(result.expectedCountReached, "early exit flagged");
        require(result.roundsUsed == 1, "closed in round 1");
        require(f.channel.sends.size() == 5, "one CONFIG per candidate, no re-sends");
        for (const auto& c : result.candidates)
            require(c.attemptsSent == 1, "later rounds skipped after expectation met");
        requireState(result, QStringLiteral("192.168.0.6"),
                     DiscoveryCandidateState::TimedOut, "late fifth card not admitted");
    }

    // Expected count not reached: one ACK against expected=2 keeps the full
    // multi-round budget and returns the fallback result.
    {
        Fixture f;
        DiscoveryOptions options = baseOptions();
        options.expectedCardCount = 2;
        f.channel.addArrival(f.channel.now + 100000000, QStringLiteral("192.168.0.2"), 60);
        const DiscoveryResult result = runDiscovery(options, f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.size() == 1, "single card still accepted");
        require(!result.expectedCountReached, "expectation not flagged");
        require(result.roundsUsed == options.maxAttempts, "full budget used");
        requireState(result, QStringLiteral("192.168.0.2"),
                     DiscoveryCandidateState::Verified, "verified state");
    }

    // expectedCardCount=0 keeps the historical full-budget behaviour.
    {
        Fixture f;
        DiscoveryOptions options = baseOptions();
        const DiscoveryResult result = runDiscovery(options, f.cancel, f.channel, f.environment());
        require(!result.expectedCountReached, "early exit disabled");
        require(result.roundsUsed == options.maxAttempts, "full budget used");
        require(result.verifiedIPs.isEmpty(), "no cards without ACK");
    }

    // 14. Stale discovery results cannot override the active session.
    {
        DiscoveryResult stale;
        stale.discoveryId = QStringLiteral("disco-old");
        require(!canApplyDiscoveryResult(true, QStringLiteral("disco-new"), stale), "stale id rejected");
        require(!canApplyDiscoveryResult(false, QStringLiteral("disco-old"), stale), "dead window rejected");
        stale.discoveryId = QStringLiteral("disco-new");
        require(canApplyDiscoveryResult(true, QStringLiteral("disco-new"), stale), "current id accepted");
    }

    // Binding failures surface as errors and never send CONFIG.
    {
        Fixture f;
        f.channel.openOk = false;
        f.channel.openErrorText = QStringLiteral("bind 8000 failed");
        const DiscoveryResult result = runDiscovery(baseOptions(), f.cancel, f.channel, f.environment());
        require(result.verifiedIPs.isEmpty() && !result.error.isEmpty(), "open failure surfaced");
        require(f.channel.sends.empty(), "no CONFIG after bind failure");
        require(!f.eventsNamed(QStringLiteral("discovery.complete")).empty(), "complete event with error");
    }

    // Options normalization clamps and reports.
    {
        DiscoveryOptions options = baseOptions();
        options.ackWindowMs = 10;
        options.maxAttempts = 99;
        options.finalGraceMs = 99999;
        options.expectedCardCount = 99;
        QString note;
        const DiscoveryOptions normalized = sanitizeDiscoveryOptions(options, &note);
        require(normalized.ackWindowMs == 2000 && normalized.maxAttempts == 5 &&
                    normalized.finalGraceMs == 500,
                "defaults restored");
        require(normalized.expectedCardCount == 0,
                "untrustworthy expectation falls back to disabled");
        require(note.contains(QStringLiteral("AckWindowMs")) && note.contains(QStringLiteral("MaxAttempts")) &&
                    note.contains(QStringLiteral("ExpectedCardCount")),
                "normalization note recorded");
        DiscoveryOptions valid = baseOptions();
        valid.expectedCardCount = 4;
        QString validNote;
        const DiscoveryOptions kept = sanitizeDiscoveryOptions(valid, &validNote);
        require(kept.expectedCardCount == 4, "valid expectation preserved");
    }

    std::cout << "PASS discovery policy: local exclusion, 60-byte-only admission, slow retries, ordering, cancel, stale sessions" << std::endl;
    return 0;
}
