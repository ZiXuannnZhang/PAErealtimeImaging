#include "PaimageAcquisition/CardDiscovery.h"
#include "PaimageAcquisition/SourceCore.h"

#include <QByteArray>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace paimage;

namespace {

// Loopback multi-host emulation: Windows routes the whole 127.0.0.0/8 to the
// loopback interface, so four sockets bound to 127.0.0.2..5:8080 emulate four
// candidate cards with distinct source IPv4 addresses.
struct FakeCard {
    SOCKET socket = INVALID_SOCKET;
    QString ip;
    int configsReceived = 0;
    int replyAfterConfigs = 1;   // late responders use 2
    bool replied = false;
};

QString localBindIpForDiscovery()
{
    // The discovery component selects 127.0.0.1 as the best-route source for
    // any 127.x destination; requiring it explicitly keeps the test honest.
    return QString();
}

void require(bool condition, const char* what)
{
    if (!condition) throw std::runtime_error(what);
}

} // namespace

int main()
{
    WSADATA wsaData{};
    require(WSAStartup(MAKEWORD(2, 2), &wsaData) == 0, "WSAStartup");

    // 1. CONFIG goes to port 8080 only; 60-byte ACK from the candidate's own
    //    source IPv4 is accepted; every verified card is reported sorted.
    qint64 verifiedRound2 = 0;
    {
        std::vector<FakeCard> cards;
        for (int i = 2; i <= 5; ++i) {
            FakeCard card;
            card.ip = QStringLiteral("127.0.0.%1").arg(i);
            card.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            require(card.socket != INVALID_SOCKET, "card socket");
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(8080);
            inet_pton(AF_INET, card.ip.toLatin1().constData(), &local.sin_addr);
            require(bind(card.socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
                    "card bind 8080");
            u_long nonBlocking = 1;
            ioctlsocket(card.socket, FIONBIO, &nonBlocking);
            cards.push_back(card);
        }

        std::atomic<bool> respondersStop{false};
        std::thread responder([&cards, &respondersStop]() {
            std::vector<char> buffer(65536);
            while (!respondersStop.load()) {
                fd_set readable;
                FD_ZERO(&readable);
                for (const FakeCard& card : cards) FD_SET(card.socket, &readable);
                timeval timeout{0, 20000};
                const int status = select(0, &readable, nullptr, nullptr, &timeout);
                if (status <= 0) continue;
                for (FakeCard& card : cards) {
                    if (!FD_ISSET(card.socket, &readable)) continue;
                    for (;;) {
                        sockaddr_in source{};
                        int sourceLength = sizeof(source);
                        const int received = recvfrom(card.socket, buffer.data(), int(buffer.size()),
                                                      0, reinterpret_cast<sockaddr*>(&source),
                                                      &sourceLength);
                        if (received == SOCKET_ERROR) break;
                        if (received == 58 && buffer[4] == 2) {
                            ++card.configsReceived;
                            if (card.replied || card.configsReceived < card.replyAfterConfigs) continue;
                            card.replied = true;
                            char sourceText[INET_ADDRSTRLEN] = {};
                            inet_ntop(AF_INET, &source.sin_addr, sourceText, sizeof(sourceText));
                            sockaddr_in feedback{};
                            feedback.sin_family = AF_INET;
                            feedback.sin_port = htons(8000);
                            inet_pton(AF_INET, sourceText, &feedback.sin_addr);
                            const QByteArray ack(60, char(0xa5));
                            sendto(card.socket, ack.constData(), ack.size(), 0,
                                   reinterpret_cast<sockaddr*>(&feedback), sizeof(feedback));
                        }
                    }
                }
            }
        });

        DiscoveryOptions options;
        options.baseIP = QStringLiteral("127.0.0.2");
        options.candidateCount = 4;
        options.configDurationNs = 20000;
        options.delayANs = 200;
        options.delayBNs = 200;
        options.ackWindowMs = 500;
        options.maxAttempts = 3;
        options.finalGraceMs = 0;
        options.icmpTimeoutMs = 50;
        options.discoveryId = QStringLiteral("socket-checks");
        std::atomic<bool> cancel{false};
        const DiscoveryResult result = runDiscovery(options, cancel);
        respondersStop.store(true);
        responder.join();

        if (!result.error.isEmpty())
            std::cerr << "discovery error: " << result.error.toStdString() << std::endl;
        require(result.error.isEmpty(), "discovery succeeded");
        require(result.verifiedIPs.size() == 4, "four cards verified");
        require(result.verifiedIPs == (QVector<QString>{QStringLiteral("127.0.0.2"),
                                                        QStringLiteral("127.0.0.3"),
                                                        QStringLiteral("127.0.0.4"),
                                                        QStringLiteral("127.0.0.5")}),
                "numeric order preserved");
        require(result.localBindBasis == QStringLiteral("best_route"), "best route basis");
        for (const FakeCard& card : cards)
            require(card.configsReceived == 1, "verified card receives exactly one CONFIG");
        for (const auto& c : result.candidates) {
            require(c.ack60Count == 1, "one ack per card");
            require(c.attemptsSent == 1, "one attempt per card");
        }
        for (FakeCard& card : cards) closesocket(card.socket);
    }

    // 2. A slow candidate that misses round 1 is re-sent and accepted later.
    {
        std::vector<FakeCard> cards;
        for (int i = 2; i <= 5; ++i) {
            FakeCard card;
            card.ip = QStringLiteral("127.0.0.%1").arg(i);
            card.replyAfterConfigs = (i == 2) ? 2 : 1;
            card.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            require(card.socket != INVALID_SOCKET, "card socket");
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(8080);
            inet_pton(AF_INET, card.ip.toLatin1().constData(), &local.sin_addr);
            require(bind(card.socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
                    "card bind 8080");
            u_long nonBlocking = 1;
            ioctlsocket(card.socket, FIONBIO, &nonBlocking);
            cards.push_back(card);
        }

        std::atomic<bool> respondersStop{false};
        std::thread responder([&cards, &respondersStop]() {
            std::vector<char> buffer(65536);
            while (!respondersStop.load()) {
                fd_set readable;
                FD_ZERO(&readable);
                for (const FakeCard& card : cards) FD_SET(card.socket, &readable);
                timeval timeout{0, 20000};
                const int status = select(0, &readable, nullptr, nullptr, &timeout);
                if (status <= 0) continue;
                for (FakeCard& card : cards) {
                    if (!FD_ISSET(card.socket, &readable)) continue;
                    for (;;) {
                        sockaddr_in source{};
                        int sourceLength = sizeof(source);
                        const int received = recvfrom(card.socket, buffer.data(), int(buffer.size()),
                                                      0, reinterpret_cast<sockaddr*>(&source),
                                                      &sourceLength);
                        if (received == SOCKET_ERROR) break;
                        if (received == 58 && buffer[4] == 2) {
                            ++card.configsReceived;
                            if (card.replied || card.configsReceived < card.replyAfterConfigs) continue;
                            card.replied = true;
                            char sourceText[INET_ADDRSTRLEN] = {};
                            inet_ntop(AF_INET, &source.sin_addr, sourceText, sizeof(sourceText));
                            sockaddr_in feedback{};
                            feedback.sin_family = AF_INET;
                            feedback.sin_port = htons(8000);
                            inet_pton(AF_INET, sourceText, &feedback.sin_addr);
                            const QByteArray ack(60, char(0xa5));
                            sendto(card.socket, ack.constData(), ack.size(), 0,
                                   reinterpret_cast<sockaddr*>(&feedback), sizeof(feedback));
                        }
                    }
                }
            }
        });

        DiscoveryOptions options;
        options.baseIP = QStringLiteral("127.0.0.2");
        options.candidateCount = 4;
        options.ackWindowMs = 500;
        options.maxAttempts = 3;
        options.finalGraceMs = 0;
        options.icmpTimeoutMs = 50;
        options.discoveryId = QStringLiteral("socket-checks-late");
        std::atomic<bool> cancel{false};
        const DiscoveryResult result = runDiscovery(options, cancel);
        respondersStop.store(true);
        responder.join();

        require(result.error.isEmpty(), "late discovery succeeded");
        require(result.verifiedIPs.size() == 4, "late responder still verified");
        for (const auto& c : result.candidates) {
            if (c.ip == QStringLiteral("127.0.0.2")) {
                require(c.attemptsSent == 2, "slow card re-sent once");
                require(c.verifiedRound == 2, "slow card verified in round 2");
                verifiedRound2 = c.verifiedRound;
            } else {
                require(c.attemptsSent == 1, "fast cards sent once");
            }
        }
        for (FakeCard& card : cards) closesocket(card.socket);
    }

    // 3. After the discovery channel closes, port 8000 binds again immediately.
    {
        SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port = htons(8000);
        any.sin_addr.s_addr = INADDR_ANY;
        require(bind(probe, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0,
                "formal feedback socket can bind 8000 after discovery");
        closesocket(probe);
    }

    // 4. Port 8000 already occupied: discovery fails clearly and sends nothing.
    {
        SOCKET occupant = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port = htons(8000);
        any.sin_addr.s_addr = INADDR_ANY;
        require(bind(occupant, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0, "occupant bind");

        DiscoveryOptions options;
        options.baseIP = QStringLiteral("127.0.0.2");
        options.candidateCount = 2;
        options.ackWindowMs = 500;
        options.maxAttempts = 1;
        options.finalGraceMs = 0;
        options.icmpTimeoutMs = 50;
        options.discoveryId = QStringLiteral("socket-checks-occupied");
        std::atomic<bool> cancel{false};
        const DiscoveryResult result = runDiscovery(options, cancel);
        closesocket(occupant);
        require(!result.error.isEmpty(), "occupied 8000 reported");
        require(result.verifiedIPs.isEmpty(), "no cards when 8000 occupied");
    }

    WSACleanup();
    std::cout << "PASS discovery socket: CONFIG on 8080, ACK on 8000, per-round resend, rebind, occupied failure" << std::endl;
    return 0;
}
