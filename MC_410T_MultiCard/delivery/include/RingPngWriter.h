#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ring_png {
constexpr int kEdge = 1600;
constexpr int kShutdownGraceMs = 4000;
constexpr std::size_t kPairBytes = 2 * kEdge * kEdge * sizeof(QRgb);
using Pair = std::array<QImage, 2>;
using Paths = std::array<QString, 2>;

struct Result {
    QString status;
    QString retainedFile;
    Paths paths{};
    bool saved() const { return status == QStringLiteral("saved"); }
};

Paths targetPaths(const QString& dir, const QString& suffix, const QString& tag);
Result commitPair(const Pair&, const Paths&);

// Checked before the application's GUI, logging and imaging initialization.
std::optional<int> runWriterMode(int argc, char** argv);

class Channel : public QObject {
public:
    using Completion = std::function<void(std::uint64_t, Result)>;
    explicit Channel(Completion completion, QObject* parent = nullptr);
    ~Channel() override;

    // Call on the owning event-loop thread, including shutdown.
    // The caller retains admission until the completion callback.
    void enqueue(std::uint64_t id, Pair images, Paths paths);
    void shutdown();
#ifdef MAINWINDOW_START_TEST_SEAM
    enum class TestMode { Normal, Block, ExitAfterFirst, FailStart };
    void setTestMode(TestMode mode);
    bool testBlocked() const;
    bool testReceiptReady() const;
    void deferTestReceiptNotification();
    void setTestExecutable(QString path);
    void releaseTestGate();
    qint64 testProcessId() const;
#endif

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ring_png
