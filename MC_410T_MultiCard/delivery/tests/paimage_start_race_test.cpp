#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "PaimageAcquisition/ControlSocket.h"
#include "PaimageAcquisition/ControlState.h"
#include "PaimageAcquisition/SocketReceiver.h"
#include "PaimageAcquisition/TraceWriter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using namespace paimage;

constexpr int kCards = 4;
constexpr int kPackets = 8;
constexpr int kPrefixPackets = 3;
// Keep the loopback topology isolated from the live diagnostics process that
// normally owns 8000..8004 on this workstation.
constexpr int kDataPort = 28001;
constexpr int kControlPort = 28080;
constexpr int kSamples = 1440;
constexpr int kSessionsPerScenario = 20;
constexpr auto kWait = std::chrono::seconds(5);

enum class Scenario { WholeTriggerGated, PrefixGated, NormalAfterStart };

const char* scenarioName(Scenario scenario) {
    switch (scenario) {
    case Scenario::WholeTriggerGated: return "whole_trigger_gated";
    case Scenario::PrefixGated: return "prefix_gated_partial";
    case Scenario::NormalAfterStart: return "normal_after_start";
    }
    return "unknown";
}

struct Plan {
    std::uint64_t session = 0;
    Scenario scenario = Scenario::WholeTriggerGated;
    std::uint16_t firstTrigger = 0;
    std::uint16_t secondTrigger = 0;
    int prePackets = 0;
};

struct FrameEvent {
    std::uint64_t session = 0;
    int card = -1;
    std::uint16_t trigger = 0;
    std::uint16_t basePacket = 0;
    std::uint32_t actual = 0;
    std::uint32_t unique = 0;
    std::uint32_t expected = 0;
    bool complete = false;
    Decision reason = Decision::Accepted;
    std::uint64_t firstIngressId = 0;
};

struct SessionState {
    std::mutex mutex;
    std::condition_variable changed;
    Plan plan;
    bool postGo = false;
    std::array<bool, kCards> startReceived{};
    std::array<bool, kCards> preSent{};
    std::array<int, kCards> preProcessed{};
    std::array<bool, kCards> postSent{};
    std::array<int, kCards> startOrder{};
    int startOrderCount = 0;
    int preIngress = 0;
    int disabledPre = 0;
    int acceptedPost = 0;
    std::vector<FrameEvent> frames;
};

struct SessionResult {
    Plan plan;
    std::vector<int> startOrder;
    std::vector<FrameEvent> frames;
    int preIngress = 0;
    int disabledPre = 0;
    int acceptedPost = 0;
};

struct TraceEvidence {
    int startSends = 0;
    bool startOrderCorrect = false;
    std::int64_t startSpanNs = -1;
    int rawPre = 0;
    int disabledPre = 0;
    int acceptedPre = 0;
    int acceptedPost = 0;
    std::vector<TraceRecord> startRecords;
    std::vector<TraceRecord> rawPreRecords;
};

struct ScenarioSummary {
    Scenario scenario = Scenario::WholeTriggerGated;
    int sessionCount = 0;
    int expectedPrePackets = 0;
    int expectedFramesPerSession = 0;
    int expectedPartialPerSession = 0;
    int rawIngress = 0;
    int disabled = 0;
    int acceptedPre = 0;
    int acceptedPost = 0;
    int completeFrames = 0;
    int partialFrames = 0;
    bool passed = true;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

std::vector<std::filesystem::path> traceFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    const auto& traceRoot = root;
    if (!std::filesystem::exists(traceRoot)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(traceRoot)) {
        if (entry.is_regular_file() && entry.path().filename().string().rfind("trace-", 0) == 0 &&
            entry.path().extension() == ".bin") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<TraceRecord> readTrace(const std::filesystem::path& root) {
    std::vector<TraceRecord> records;
    for (const auto& path : traceFiles(root)) {
        std::ifstream input(path, std::ios::binary);
        require(bool(input), "cannot read trace file " + path.string());
        TraceRecord record{};
        while (input.read(reinterpret_cast<char*>(&record), sizeof(record))) records.push_back(record);
        require(input.eof(), "short read in trace file " + path.string());
    }
    return records;
}

bool startSuccess(const TraceRecord& record) {
    return record.length == sizeof(Command) && record.value == 0;
}

class RaceHarness {
public:
    explicit RaceHarness(std::filesystem::path root)
        : root_(std::move(root)), trace_(root_) {
        std::filesystem::create_directories(root_);
        const std::vector<std::string> targets = {
            "127.0.0.2", "127.0.0.3", "127.0.0.4", "127.0.0.5"};
        for (int card = 0; card < kCards; ++card) openCard(card, targets[card]);

        std::string error;
        require(control_.open("127.0.0.1", targets, kControlPort, error), "control open: " + error);
        std::vector<SocketReceiver::Endpoint> endpoints;
        for (int card = 0; card < kCards; ++card)
            endpoints.push_back({static_cast<std::uint16_t>(kDataPort + card), "127.0.0.1"});
        receiver_ = std::make_unique<SocketReceiver>(
            Config{kCards, kSamples, 32, 0}, endpoints, SocketReceiver::Endpoint{0, {}}, targets,
            &trace_, nullptr, nullptr,
            [this](Frame frame) { onFrame(std::move(frame)); },
            [](std::uint16_t, const std::vector<Frame>&, bool) {});
        receiver_->ingressSink = [this](int card, const TraceRecord& record) { onIngress(card, record); };
        receiver_->observationSink = [this](const Observation& observation) { onObservation(observation); };

        controlState_ = std::make_unique<ControlState>(
            kCards,
            [this](const Command& command, const std::vector<int>& cards) {
                return control_.send(command, cards, [this](const Command& bytes, const ControlSocket::SendResult& sent) {
                    if (bytes[4] != 3 || bytes[57] != 1) return;
                    TraceRecord record{};
                    record.monotonicNs = SocketReceiver::now();
                    {
                        std::lock_guard<std::mutex> lock(state_.mutex);
                        record.session = state_.plan.session;
                    }
                    record.threadId = GetCurrentThreadId();
                    record.card = static_cast<std::int16_t>(sent.card);
                    record.sourceIPv4 = sent.targetIPv4;
                    record.localPort = control_.localPort();
                    record.sourcePort = kControlPort;
                    record.length = sent.bytes < 0 ? 0 : static_cast<std::uint16_t>(sent.bytes);
                    record.value = static_cast<std::uint32_t>(sent.error);
                    record.packet = bytes[57];
                    record.stage = 6;
                    record.reason = bytes[4];
                    trace_.push(record);
                });
            },
            SocketReceiver::now);
    }

    ~RaceHarness() { shutdown(); }

    void start() {
        std::string error;
        require(receiver_->start(error), "receiver start: " + error);
        for (auto& card : cards_) card.thread = std::thread(&RaceHarness::cardLoop, this, card.index);
        controlState_->setListening(true);
        require(controlState_->configure(configCommand(20000, 1000, 1000), SocketReceiver::now(), false),
                "CONFIG send failed");
        waitFor("all cards receive CONFIG", [this] {
            for (const bool received : configReceived_) if (!received) return false;
            return true;
        });
#ifdef PAIMAGE_SOCKET_TEST_SEAM
        control_.setTestSendHook([this](const Command& command, const ControlSocket::SendResult& sent) {
            if (command[4] == 3 && command[57] == 1) onStartSend(sent);
        });
#else
        throw std::runtime_error("PAIMAGE_SOCKET_TEST_SEAM is required for deterministic race test");
#endif
    }

    std::vector<SessionResult> runScenarios() {
        std::vector<SessionResult> results;
        std::uint64_t session = 1000;
        for (const Scenario scenario : {Scenario::WholeTriggerGated, Scenario::PrefixGated,
                                        Scenario::NormalAfterStart}) {
            for (int repetition = 0; repetition < kSessionsPerScenario; ++repetition) {
                Plan plan;
                plan.session = session++;
                plan.scenario = scenario;
                plan.firstTrigger = static_cast<std::uint16_t>(
                    scenario == Scenario::WholeTriggerGated ? 100 + repetition * 2
                    : scenario == Scenario::PrefixGated ? 200 + repetition * 2
                    : 300 + repetition * 2);
                plan.secondTrigger = static_cast<std::uint16_t>(plan.firstTrigger + 1);
                plan.prePackets = scenario == Scenario::WholeTriggerGated ? kPackets
                    : scenario == Scenario::PrefixGated ? kPrefixPackets : 0;
                prepare(plan);
                require(controlState_->start(
                            [this, plan] { receiver_->prepareStart(plan.session); },
                            [this](bool success) { receiver_->completeStart(success); }),
                        "START transaction failed for session " + std::to_string(plan.session));
                waitFor("all cards receive START for session " + std::to_string(plan.session), [this] {
                    for (const bool received : state_.startReceived) if (!received) return false;
                    return true;
                });
                releasePost(plan);
                const int expectedFrames = scenario == Scenario::PrefixGated ? kCards * 2 :
                    scenario == Scenario::WholeTriggerGated ? kCards : kCards * 2;
                waitFor("post packets and outputs for session " + std::to_string(plan.session), [this, plan, expectedFrames] {
                    bool allCardsSent = true;
                    for (const bool sent : state_.postSent) allCardsSent = allCardsSent && sent;
                    int frames = 0;
                    for (const auto& event : state_.frames) if (event.session == plan.session) ++frames;
                    return allCardsSent && state_.acceptedPost >= expectedPostAccepted(plan) &&
                        frames >= expectedFrames;
                });
                results.push_back(snapshot(plan));
            }
        }
        return results;
    }

    void shutdown() {
        if (shutdown_) return;
        shutdown_ = true;
        stopping_ = true;
        state_.changed.notify_all();
        if (receiver_) receiver_->stop();
        for (auto& card : cards_) if (card.thread.joinable()) card.thread.join();
        for (auto& card : cards_) if (card.socket != INVALID_SOCKET) {
            closesocket(card.socket);
            card.socket = INVALID_SOCKET;
        }
        if (controlState_) controlState_->setListening(false);
        controlState_.reset();
        control_.close();
        trace_.stop();
    }

    const std::filesystem::path& root() const { return root_; }
    std::uint64_t receiverHardErrors() const { return receiver_ ? receiver_->hardErrors() : 0; }
    std::uint64_t emulatorSendErrors() const { return emulatorSendErrors_.load(); }
    int emulatorLastError() const { return emulatorLastError_.load(); }
    std::uint64_t traceQueueDropped() const { return trace_.dropped(); }
    bool traceIncomplete() const { return trace_.incomplete(); }

private:
    struct CardSocket {
        int index = -1;
        SOCKET socket = INVALID_SOCKET;
        std::thread thread;
    };

    void openCard(int card, const std::string& ip) {
        auto& target = cards_[card];
        target.index = card;
        target.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(target.socket != INVALID_SOCKET, "card socket create failed");
        DWORD timeout = 100;
        setsockopt(target.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kControlPort);
        require(inet_pton(AF_INET, ip.c_str(), &address.sin_addr) == 1, "card IP conversion failed");
        require(bind(target.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "card control bind failed for " + ip);
    }

    template <class Predicate>
    void waitFor(const std::string& label, Predicate predicate) {
        std::unique_lock<std::mutex> lock(state_.mutex);
        const bool completed = state_.changed.wait_for(lock, kWait, [&] {
            return predicate() || emulatorSendErrors_.load() != 0 || stopping_.load();
        });
        if (!completed) throw std::runtime_error("timeout: " + label + " session=" + std::to_string(state_.plan.session));
        if (emulatorSendErrors_.load() != 0)
            throw std::runtime_error("card emulator socket error=" + std::to_string(emulatorLastError_.load()) +
                                     " while waiting for " + label);
        require(!stopping_.load(), "race harness stopped while waiting for " + label);
        require(predicate(), "incomplete condition: " + label);
    }

    void prepare(const Plan& plan) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.plan = plan;
        state_.postGo = false;
        state_.startReceived.fill(false);
        state_.preSent.fill(plan.scenario == Scenario::NormalAfterStart);
        state_.preProcessed.fill(0);
        state_.postSent.fill(false);
        state_.startOrder.fill(-1);
        state_.startOrderCount = 0;
        state_.preIngress = 0;
        state_.disabledPre = 0;
        state_.acceptedPost = 0;
        state_.frames.clear();
    }

    void releasePost(const Plan& plan) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        require(state_.plan.session == plan.session, "session changed before post release");
        state_.postGo = true;
        state_.changed.notify_all();
    }

    SessionResult snapshot(const Plan& plan) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        SessionResult result;
        result.plan = plan;
        for (int i = 0; i < state_.startOrderCount; ++i) result.startOrder.push_back(state_.startOrder[i]);
        result.preIngress = state_.preIngress;
        result.disabledPre = state_.disabledPre;
        result.acceptedPost = state_.acceptedPost;
        for (const auto& frame : state_.frames) if (frame.session == plan.session) result.frames.push_back(frame);
        return result;
    }

    void onStartSend(const ControlSocket::SendResult& sent) {
        Plan plan;
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            plan = state_.plan;
            require(sent.card >= 0 && sent.card < kCards, "invalid START card in test hook");
            require(startSuccess(TraceRecord{0, 0, plan.session, 0, 0, 0, static_cast<std::uint32_t>(sent.error),
                                             0, 0, static_cast<std::uint16_t>(sent.bytes < 0 ? 0 : sent.bytes), 0, 1,
                                             static_cast<std::int16_t>(sent.card), 6, 3, {}, 2}),
                    "START send failed for card " + std::to_string(sent.card));
            require(state_.startOrderCount < kCards, "duplicate START send in session");
            state_.startOrder[state_.startOrderCount++] = sent.card;
            state_.changed.notify_all();
        }
        if (plan.scenario == Scenario::NormalAfterStart) return;
        waitFor("card " + std::to_string(sent.card) + " pre-trigger send", [this, sent] {
            return state_.preSent[sent.card];
        });
        if (sent.card == kCards - 1) {
            const int expected = kCards * plan.prePackets;
            waitFor("all pre-trigger raw ingress", [this, expected] { return state_.preIngress >= expected; });
        }
        waitFor("card " + std::to_string(sent.card) + " pre-trigger decisions", [this, sent, plan] {
            return state_.preProcessed[sent.card] >= plan.prePackets;
        });
    }

    void cardLoop(int card) {
        try {
            while (!stopping_.load()) {
            std::array<std::uint8_t, sizeof(Command)> command{};
            sockaddr_in source{};
            int sourceSize = sizeof(source);
            const int count = recvfrom(cards_[card].socket, reinterpret_cast<char*>(command.data()),
                                       static_cast<int>(command.size()), 0,
                                       reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (count == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
                emulatorLastError_ = error;
                if (!stopping_.load()) emulatorSendErrors_.fetch_add(1);
                continue;
            }
            if (count != static_cast<int>(sizeof(Command))) continue;
            if (command[4] == 2) {
                std::lock_guard<std::mutex> lock(state_.mutex);
                configReceived_[card] = true;
                state_.changed.notify_all();
                continue;
            }
            if (command[4] != 3 || command[57] != 1) continue;

            Plan plan;
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                plan = state_.plan;
                state_.startReceived[card] = true;
                state_.changed.notify_all();
            }
            if (plan.scenario == Scenario::WholeTriggerGated) {
                sendPackets(card, plan.firstTrigger, 0, kPackets);
                markPreSent(card);
            } else if (plan.scenario == Scenario::PrefixGated) {
                sendPackets(card, plan.firstTrigger, 0, kPrefixPackets);
                markPreSent(card);
            } else {
                markPreSent(card);
            }

            waitFor("post release for card " + std::to_string(card), [this, plan] {
                return state_.plan.session == plan.session && state_.postGo;
            });
            if (plan.scenario == Scenario::WholeTriggerGated) {
                sendPackets(card, plan.secondTrigger, 0, kPackets);
            } else if (plan.scenario == Scenario::PrefixGated) {
                sendPackets(card, plan.firstTrigger, kPrefixPackets, kPackets);
                sendPackets(card, plan.secondTrigger, 0, kPackets);
            } else {
                sendPackets(card, plan.firstTrigger, 0, kPackets);
                sendPackets(card, plan.secondTrigger, 0, kPackets);
            }
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                state_.postSent[card] = true;
                state_.changed.notify_all();
            }
            }
        } catch (const std::exception&) {
            if (!stopping_.load()) emulatorSendErrors_.fetch_add(1);
            state_.changed.notify_all();
        }
    }

    void markPreSent(int card) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.preSent[card] = true;
        state_.changed.notify_all();
    }

    void sendPackets(int card, std::uint16_t trigger, int begin, int end) {
        std::vector<std::uint8_t> packet(4 + 1440, 0);
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<std::uint16_t>(kDataPort + card));
        inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
        for (int number = begin; number < end; ++number) {
            packet[0] = static_cast<std::uint8_t>(number & 0xff);
            packet[1] = static_cast<std::uint8_t>((number >> 8) & 0xff);
            packet[2] = static_cast<std::uint8_t>(trigger & 0xff);
            packet[3] = static_cast<std::uint8_t>((trigger >> 8) & 0xff);
            const int sent = sendto(cards_[card].socket, reinterpret_cast<const char*>(packet.data()),
                                    static_cast<int>(packet.size()), 0,
                                    reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
            if (sent != static_cast<int>(packet.size())) {
                emulatorLastError_ = WSAGetLastError();
                emulatorSendErrors_.fetch_add(1);
            }
        }
    }

    void onIngress(int card, const TraceRecord& record) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        const Plan& plan = state_.plan;
        if (record.session != plan.session || card < 0 || card >= kCards) return;
        if (plan.scenario == Scenario::WholeTriggerGated && record.trigger == plan.firstTrigger &&
            record.packet < kPackets) ++state_.preIngress;
        if (plan.scenario == Scenario::PrefixGated && record.trigger == plan.firstTrigger &&
            record.packet < kPrefixPackets) ++state_.preIngress;
        state_.changed.notify_all();
    }

    void onObservation(const Observation& observation) {
        std::lock_guard<std::mutex> lock(state_.mutex);
        const Plan& plan = state_.plan;
        if (plan.scenario == Scenario::WholeTriggerGated && observation.trigger == plan.firstTrigger &&
            observation.packet < kPackets && observation.decision == Decision::Disabled) {
            ++state_.disabledPre;
            if (observation.card >= 0 && observation.card < kCards) ++state_.preProcessed[observation.card];
        }
        if (plan.scenario == Scenario::PrefixGated && observation.trigger == plan.firstTrigger &&
            observation.packet < kPrefixPackets && observation.decision == Decision::Disabled) {
            ++state_.disabledPre;
            if (observation.card >= 0 && observation.card < kCards) ++state_.preProcessed[observation.card];
        }
        if (observation.decision == Decision::Accepted && isPostPacket(plan, observation)) ++state_.acceptedPost;
        state_.changed.notify_all();
    }

    static bool isPostPacket(const Plan& plan, const Observation& observation) {
        if (plan.scenario == Scenario::WholeTriggerGated)
            return observation.trigger == plan.secondTrigger && observation.packet < kPackets;
        if (plan.scenario == Scenario::PrefixGated)
            return (observation.trigger == plan.firstTrigger && observation.packet >= kPrefixPackets &&
                    observation.packet < kPackets) ||
                (observation.trigger == plan.secondTrigger && observation.packet < kPackets);
        return (observation.trigger == plan.firstTrigger || observation.trigger == plan.secondTrigger) &&
            observation.packet < kPackets;
    }

    static int expectedPostAccepted(const Plan& plan) {
        if (plan.scenario == Scenario::WholeTriggerGated) return kCards * kPackets;
        if (plan.scenario == Scenario::PrefixGated)
            return kCards * ((kPackets - kPrefixPackets) + kPackets);
        return kCards * kPackets * 2;
    }

    void onFrame(Frame frame) {
        if (!frame) return;
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.frames.push_back({frame->measurementSession, frame->card, frame->trigger, frame->basePacket,
                                 frame->actual, frame->unique, frame->expected, frame->complete,
                                 frame->reason, frame->firstIngressId});
        state_.changed.notify_all();
    }

    std::filesystem::path root_;
    TraceWriter trace_;
    ControlSocket control_;
    std::unique_ptr<SocketReceiver> receiver_;
    std::unique_ptr<ControlState> controlState_;
    SessionState state_;
    std::array<CardSocket, kCards> cards_{};
    std::array<bool, kCards> configReceived_{};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> emulatorSendErrors_{0};
    std::atomic<int> emulatorLastError_{0};
    bool shutdown_ = false;
};

TraceEvidence analyzeSession(const SessionResult& session, const std::vector<TraceRecord>& records) {
    TraceEvidence result;
    for (const auto& record : records) {
        if (record.session == session.plan.session && record.stage == 6 && record.reason == 3)
            result.startRecords.push_back(record);
    }
    std::sort(result.startRecords.begin(), result.startRecords.end(), [](const TraceRecord& left, const TraceRecord& right) {
        return std::tie(left.monotonicNs, left.sequence) < std::tie(right.monotonicNs, right.sequence);
    });
    result.startSends = static_cast<int>(result.startRecords.size());
    result.startOrderCorrect = result.startSends == kCards;
    if (result.startOrderCorrect) {
        for (int card = 0; card < kCards; ++card) {
            if (result.startRecords[card].card != card || !startSuccess(result.startRecords[card]))
                result.startOrderCorrect = false;
        }
    }
    if (!result.startRecords.empty())
        result.startSpanNs = static_cast<std::int64_t>(result.startRecords.back().monotonicNs - result.startRecords.front().monotonicNs);

    std::vector<TraceRecord> stage2;
    for (const auto& record : records)
        if (record.session == session.plan.session && record.stage == 2) stage2.push_back(record);
    auto matchingDecision = [&](const TraceRecord& ingress, std::uint8_t reason) {
        return std::any_of(stage2.begin(), stage2.end(), [&](const TraceRecord& decision) {
            return decision.card == ingress.card && decision.correlation == ingress.correlation &&
                   decision.trigger == ingress.trigger && decision.packet == ingress.packet && decision.reason == reason;
        });
    };
    auto acceptedFor = [&](const TraceRecord& ingress) {
        return matchingDecision(ingress, static_cast<std::uint8_t>(Decision::Accepted));
    };
    auto mappedCard = [](const TraceRecord& ingress) {
        if (ingress.card >= 0) return static_cast<int>(ingress.card);
        if (ingress.localPort >= kDataPort && ingress.localPort < kDataPort + kCards)
            return static_cast<int>(ingress.localPort - kDataPort);
        return -1;
    };
    const auto first = session.plan.firstTrigger;
    for (const auto& record : records) {
        const int card = mappedCard(record);
        if (record.session != session.plan.session || record.stage != 1 || card < 0 || card >= kCards)
            continue;
        TraceRecord mapped = record;
        mapped.card = static_cast<std::int16_t>(card);
        if (mapped.trigger == first && mapped.packet < session.plan.prePackets) {
            ++result.rawPre;
            result.rawPreRecords.push_back(mapped);
            if (matchingDecision(mapped, static_cast<std::uint8_t>(Decision::Disabled))) ++result.disabledPre;
            if (matchingDecision(mapped, static_cast<std::uint8_t>(Decision::Accepted))) ++result.acceptedPre;
        }
        const bool postTrigger = mapped.trigger == session.plan.secondTrigger ||
            (session.plan.scenario == Scenario::NormalAfterStart && mapped.trigger == first) ||
            (session.plan.scenario == Scenario::PrefixGated && mapped.trigger == first &&
             mapped.packet >= kPrefixPackets);
        if (postTrigger && acceptedFor(mapped)) ++result.acceptedPost;
    }
    return result;
}

ScenarioSummary summarize(Scenario scenario, const std::vector<SessionResult>& sessions,
                          const std::vector<TraceEvidence>& evidence) {
    ScenarioSummary summary;
    summary.scenario = scenario;
    summary.expectedPrePackets = scenario == Scenario::WholeTriggerGated ? kCards * kPackets
        : scenario == Scenario::PrefixGated ? kCards * kPrefixPackets : 0;
    summary.expectedFramesPerSession = scenario == Scenario::PrefixGated ? kCards * 2 :
        scenario == Scenario::WholeTriggerGated ? kCards : kCards * 2;
    summary.expectedPartialPerSession = scenario == Scenario::PrefixGated ? kCards : 0;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].plan.scenario != scenario) continue;
        ++summary.sessionCount;
        summary.rawIngress += evidence[i].rawPre;
        summary.disabled += evidence[i].disabledPre;
        summary.acceptedPre += evidence[i].acceptedPre;
        summary.acceptedPost += evidence[i].acceptedPost;
        for (const auto& frame : sessions[i].frames) {
            if (frame.complete) ++summary.completeFrames;
            else ++summary.partialFrames;
        }
        const int expectedRaw = summary.expectedPrePackets;
        const int expectedDisabled = summary.expectedPrePackets;
        const int expectedComplete = summary.expectedFramesPerSession - summary.expectedPartialPerSession;
        const int expectedAccepted = scenario == Scenario::WholeTriggerGated ? kCards * kPackets
            : scenario == Scenario::PrefixGated ? kCards * (kPackets - kPrefixPackets + kPackets)
            : kCards * (kPackets + kPackets);
        if (evidence[i].rawPre != expectedRaw || evidence[i].disabledPre != expectedDisabled ||
            evidence[i].acceptedPre != 0 ||
            evidence[i].acceptedPost != expectedAccepted || !evidence[i].startOrderCorrect ||
            static_cast<int>(sessions[i].frames.size()) != summary.expectedFramesPerSession)
            summary.passed = false;
        if (scenario == Scenario::WholeTriggerGated) {
            if (std::any_of(sessions[i].frames.begin(), sessions[i].frames.end(), [&](const FrameEvent& frame) {
                    return frame.trigger == sessions[i].plan.firstTrigger || !frame.complete;
                }) || summary.completeFrames < 0 || expectedComplete != kCards) summary.passed = false;
        } else if (scenario == Scenario::PrefixGated) {
            int partial = 0;
            int complete = 0;
            for (const auto& frame : sessions[i].frames) {
                if (frame.trigger == sessions[i].plan.firstTrigger && !frame.complete &&
                    frame.reason == Decision::TriggerSwitch) ++partial;
                if (frame.trigger == sessions[i].plan.secondTrigger && frame.complete) ++complete;
            }
            if (partial != kCards || complete != kCards) summary.passed = false;
        } else {
            for (const auto& frame : sessions[i].frames)
                if (!frame.complete || frame.trigger == sessions[i].plan.firstTrigger - 1) summary.passed = false;
        }
    }
    return summary;
}

std::string timelineExample(const std::vector<TraceEvidence>& evidence) {
    if (evidence.empty() || evidence.front().startRecords.empty() || evidence.front().rawPreRecords.empty())
        return "unavailable";
    const auto& start = evidence.front().startRecords;
    const auto& raw = evidence.front().rawPreRecords.front();
    std::ostringstream text;
    text << "session=" << raw.session
         << " card=" << raw.card << " START send ns=" << start.front().monotonicNs
         << " order=" << start.front().card << ",...," << start.back().card
         << " raw ingress id=" << raw.correlation << " trigger=" << raw.trigger
         << " packet=" << raw.packet << " ns=" << raw.monotonicNs;
    for (const auto& candidate : evidence.front().startRecords) {
        if (candidate.card == raw.card) {
            text << "; last START card=" << candidate.card << " ns=" << candidate.monotonicNs;
            break;
        }
    }
    text << "; stage2 decision=Disabled is correlated by ingress id=" << raw.correlation;
    return text.str();
}

void writeRunConfig(const std::filesystem::path& root) {
    std::ofstream output(root / "run-config.json");
    output << "{\"schemaVersion\":2,\"cards\":4,\"dataPort\":" << kDataPort
           << ",\"feedbackPort\":0,\"samples\":1440,\"bits\":32,\"packetsPerTrig\":8}\n";
}

void writeResult(const std::filesystem::path& root, const std::vector<SessionResult>& sessions,
                 const std::vector<TraceEvidence>& evidence, const std::vector<ScenarioSummary>& summaries,
                 std::uint64_t hardErrors, std::uint64_t emulatorErrors, std::uint64_t queueDropped,
                 bool traceIncomplete, bool passed) {
    std::ofstream output(root / "result.json");
    output << "{\n  \"testScope\":\"real WinSock UDP control + receiver concurrency; test-only ControlSocket barrier\",\n"
           << "  \"sessionCount\":" << sessions.size() << ",\n  \"cardCount\":4,\n"
           << "  \"scenarioSummaries\":[\n";
    for (std::size_t i = 0; i < summaries.size(); ++i) {
        const auto& summary = summaries[i];
        output << "    {\"scenario\":\"" << scenarioName(summary.scenario)
               << "\",\"sessionCount\":" << summary.sessionCount
               << ",\"expectedPrePacketsPerSession\":" << summary.expectedPrePackets
               << ",\"rawIngress\":" << summary.rawIngress
               << ",\"disabled\":" << summary.disabled
               << ",\"acceptedPre\":" << summary.acceptedPre
               << ",\"acceptedPost\":" << summary.acceptedPost
               << ",\"completeFrames\":" << summary.completeFrames
               << ",\"partialFrames\":" << summary.partialFrames
               << ",\"passed\":" << (summary.passed ? "true" : "false") << "}";
        if (i + 1 != summaries.size()) output << ',';
        output << '\n';
    }
    output << "  ],\n  \"timelineExample\":\"" << jsonEscape(timelineExample(evidence)) << "\",\n"
           << "  \"hardSocketErrors\":" << hardErrors
           << ",\"emulatorSendErrors\":" << emulatorErrors
           << ",\"traceQueueDropped\":" << queueDropped
           << ",\"traceIncomplete\":" << (traceIncomplete ? "true" : "false")
           << ",\"passed\":" << (passed ? "true" : "false") << "\n}\n";
}

std::filesystem::path outputRoot(int argc, char** argv) {
    std::filesystem::path parent = "start-race-validation-artifacts";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--output") parent = argv[i + 1];
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return parent / ("run-" + std::to_string(stamp));
}

} // namespace

int main(int argc, char** argv) {
    const auto root = outputRoot(argc, argv);
    WSADATA winsock{};
    bool winsockStarted = false;
    RaceHarness* harness = nullptr;
    try {
        require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WSAStartup failed");
        winsockStarted = true;
        RaceHarness instance(root);
        harness = &instance;
        writeRunConfig(root);
        instance.start();
        const auto sessions = instance.runScenarios();
        instance.shutdown();
        const auto records = readTrace(root);
        std::vector<TraceEvidence> evidence;
        evidence.reserve(sessions.size());
        for (const auto& session : sessions) evidence.push_back(analyzeSession(session, records));
        std::vector<ScenarioSummary> summaries;
        for (const Scenario scenario : {Scenario::WholeTriggerGated, Scenario::PrefixGated,
                                        Scenario::NormalAfterStart})
            summaries.push_back(summarize(scenario, sessions, evidence));
        bool passed = instance.receiverHardErrors() == 0 && instance.emulatorSendErrors() == 0 &&
            instance.traceQueueDropped() == 0 && !instance.traceIncomplete();
        for (const auto& summary : summaries) passed = passed && summary.passed;
        writeResult(root, sessions, evidence, summaries, instance.receiverHardErrors(),
                    instance.emulatorSendErrors(), instance.traceQueueDropped(), instance.traceIncomplete(), passed);
        for (const auto& summary : summaries) {
            std::cout << scenarioName(summary.scenario) << " sessions=" << summary.sessionCount
                      << " rawIngress=" << summary.rawIngress << " disabled=" << summary.disabled
                      << " acceptedPre=" << summary.acceptedPre
                      << " acceptedPost=" << summary.acceptedPost << " complete=" << summary.completeFrames
                      << " partial=" << summary.partialFrames << " passed=" << (summary.passed ? "true" : "false") << '\n';
        }
        std::cout << "artifact=" << root.string() << " hardSocketErrors=" << instance.receiverHardErrors()
                  << " emulatorSendErrors=" << instance.emulatorSendErrors()
                  << " emulatorLastError=" << instance.emulatorLastError()
                  << " traceQueueDropped=" << instance.traceQueueDropped()
                  << " traceIncomplete=" << (instance.traceIncomplete() ? "true" : "false") << '\n';
        std::cout << (passed ? "PASS deterministic paimage START-race UDP integration (60 sessions)\n"
                             : "FAIL deterministic paimage START-race UDP integration\n");
        if (winsockStarted) WSACleanup();
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        if (harness) harness->shutdown();
        std::cerr << "FAIL " << error.what() << "\n";
        if (winsockStarted) WSACleanup();
        return 1;
    }
}
