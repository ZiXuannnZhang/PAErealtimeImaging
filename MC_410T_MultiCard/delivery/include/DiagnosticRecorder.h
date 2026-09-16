#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <future>
#include <functional>
#include <memory>

// Thread-safe, process-local diagnostic recorder used by the receiver UI and
// network workers.  The recorder owns one directory per process run.  Logging
// calls only copy their arguments into a bounded queue; disk I/O is performed
// by the recorder worker thread.
class DiagnosticRecorder final
{
public:
    using BundleSink=std::function<bool(const QString&,const QByteArray&)>;
    using BundleExtension=std::function<bool(const BundleSink&,QString*)>;
    using BundleCapture=std::function<BundleExtension(qint64,qint64)>;
    static void setBundleCapture(BundleCapture);
    enum class Severity {
        Trace = 0,
        Debug,
        Info,
        Warning,
        Error,
        Critical
    };

    struct Options {
        // Empty means QStandardPaths::AppLocalDataLocation/diagnostics.
        QString rootDirectory;
        int maxQueueEntries = 8192;
        int maxFallbackEntries = 2048;
        int noiseWindowMs = 1000;
        int noiseBurst = 100;
        int maxNoiseBuckets = 4096;
        int maxSnapshotEntries = 4096;
        int maxRetainedRuns = 30;
        qint64 maxRetainedBytes = 500LL * 1024LL * 1024LL;
    };

    struct Event {
        quint64 sequence = 0;
        QString runId;
        QString isoTime;
        qint64 monotonicMs = 0;
        QString category;
        QString message;
        Severity severity = Severity::Info;
        QJsonObject fields;
    };

    struct Snapshot {
        quint64 sequence = 0;
        QString runId;
        QString isoTime;
        qint64 monotonicMs = 0;
        QJsonObject data;
    };

    struct CardSnapshot {
        quint64 sequence = 0;
        QString runId;
        QString isoTime;
        qint64 monotonicMs = 0;
        int card = 0;
        QString ip;
        QString state;
        QJsonObject fields;
    };

    struct DropCounters {
        quint64 queueDropped = 0;
        quint64 criticalQueueDropped = 0;
        quint64 noiseDropped = 0;
        quint64 writeFallbackDropped = 0;
    };

    struct Status {
        bool active = false;
        bool writeError = false;
        QString writeErrorText;
        quint64 nextSequence = 1;
        quint64 writtenSequence = 0;
        DropCounters drops;
        int queuedEntries = 0;
        int fallbackEntries = 0;
    };

    struct RunInfo {
        QString runId;
        QString directory;
        QString startIsoTime;
        QString endIsoTime;
        QString earliestIsoTime;
        QString latestIsoTime;
        bool active = false;
        quint64 lastSequence = 0;
        qint64 bytes = 0;
        bool manifestComplete = false;
    };

    // A request is an immutable, bounded description of an export cut.  It
    // contains the sequence boundary and any events/snapshots not yet safely
    // represented by the on-disk files, so a later worker cannot accidentally
    // include state recorded after the button click.
    struct ExportRequest {
        BundleExtension bundleExtension;
        QString runId;
        QString sourceDirectory;
        QString startIsoTime;
        QString endIsoTime;
        quint64 boundarySequence = 0;
        bool sourceWasActive = false;
        QString note;
        QVector<Event> fallbackEvents;
        QVector<Snapshot> networkSnapshots;
        QVector<Snapshot> settingsSnapshots;
        QVector<CardSnapshot> cardSnapshots;
        DropCounters drops;
        bool writeError = false;
        QString writeErrorText;
        bool snapshotsTruncated = false;
        bool capturedLive = false;
        // Executed only by the exporter, never by the UI capture operation.
        std::function<bool()> flushBeforeExport;
    };

    struct TimeWindowRequest {
        BundleExtension bundleExtension;
        QString requestedStartTime;
        QString requestedEndTime;
        QString exportCapturedAt;
        QString note;
        QString rootDirectory;
        QString validationError;
        QVector<ExportRequest> sources;
    };

    struct ExportResult {
        bool success = false;
        QString targetPath;
        QString error;
        quint64 boundarySequence = 0;
        QStringList missingFiles;
        QStringList truncationReasons;
        bool noRecords = false;
        int sourceRunCount = 0;
        QJsonObject formalWindowRecordCounts;
        QJsonObject boundaryContextCounts;
    };

    static DiagnosticRecorder *instance();
    static DiagnosticRecorder *initialize(const Options &options,
                                          QString *error = nullptr);
    static DiagnosticRecorder *initialize(QString *error = nullptr);
    static void shutdown();

    DiagnosticRecorder();
    explicit DiagnosticRecorder(const Options &options);
    ~DiagnosticRecorder();

    DiagnosticRecorder(const DiagnosticRecorder &) = delete;
    DiagnosticRecorder &operator=(const DiagnosticRecorder &) = delete;

    bool isActive() const;
    QString runId() const;
    QString runDirectory() const;
    Status status() const;

    // Non-blocking from the caller's point of view.  A return value of zero
    // means the recorder has stopped accepting events; nonzero values are
    // monotonically increasing even when a low-priority event is rate-limited
    // or discarded because the bounded queue is full.
    quint64 recordEvent(const QString &category,
                        const QString &message,
                        Severity severity = Severity::Info,
                        const QJsonObject &fields = QJsonObject());
    quint64 logText(const QString &message,
                    Severity severity = Severity::Info,
                    const QJsonObject &fields = QJsonObject());

    // Snapshot methods are also thread-safe and enqueue persistence work.  The
    // caller should put listenId/configId and the canonical per-card fields in
    // the supplied JSON.  Each update is versioned by the same sequence space
    // as events and is retained in the run history rather than overwritten.
    quint64 recordNetworkSnapshot(const QJsonObject &snapshot);
    quint64 recordSettingsSnapshot(const QJsonObject &snapshot);
    quint64 setCardSnapshot(int card,
                            const QString &ip,
                            const QString &state,
                            const QJsonObject &fields = QJsonObject());

    // requestFlush() only signals the worker and is suitable for hot network
    // or UI paths.  flush() is a bounded barrier for tests and export workers.
    void requestFlush();
    bool flush(int timeoutMs = 5000);

    quint64 captureBoundary() const;
    ExportRequest captureExportRequest(quint64 boundarySequence = 0,
                                       const QString &note = QString()) const;

    TimeWindowRequest captureTimeWindowRequest(const QDateTime &startLocal,
                                               const QDateTime &endLocal,
                                               const QString &note = QString()) const;

    ExportResult exportRun(const QString &targetZipPath,
                           quint64 boundarySequence = 0,
                           const QString &note = QString());
    std::future<ExportResult> exportRunAsync(const QString &targetZipPath,
                                             quint64 boundarySequence = 0,
                                             const QString &note = QString());
    static ExportResult exportRequest(const ExportRequest &request,
                                      const QString &targetZipPath);
    static ExportResult exportTimeWindow(const TimeWindowRequest &request,
                                         const QString &targetZipPath);
    std::future<ExportResult> exportTimeWindowAsync(const TimeWindowRequest &request,
                                                    const QString &targetZipPath) const;

    static QVector<RunInfo> enumerateRuns(const QString &rootDirectory = QString());

    static QString defaultRootDirectory();
    static QString severityName(Severity severity);

private:
    struct State;
    std::shared_ptr<State> m_state;
};
