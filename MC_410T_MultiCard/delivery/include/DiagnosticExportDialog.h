#pragma once

#include "DiagnosticRecorder.h"

#include <QDialog>
#include <QDateTime>
#include <QVector>

class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
template <typename T>
class QFutureWatcher;

// UI for selecting a local-time interval across all retained runs. Enumeration
// is intentionally performed in QtConcurrent so opening the dialog never
// walks the diagnostics directory on the UI thread.
class DiagnosticExportDialog final : public QDialog
{
public:
    struct Selection {
        QDateTime startTime;
        QDateTime endTime;
        QString note;
        QString targetPath;
    };

    explicit DiagnosticExportDialog(const QString &currentRunId,
                                    const QString &currentRunDirectory,
                                    QWidget *parent = nullptr);
    ~DiagnosticExportDialog() override;

    Selection selection() const;
    static bool isValidTimeWindow(const QDateTime &start,
                                  const QDateTime &end,
                                  const QDateTime &now = QDateTime::currentDateTime());

private:
    void chooseTargetPath();
    void acceptSelection();
    void applyRetentionBounds(const QVector<DiagnosticRecorder::RunInfo> &runs);

    QString m_currentRunId;
    QString m_currentRunDirectory;
    QDateTimeEdit *m_startEdit = nullptr;
    QDateTimeEdit *m_endEdit = nullptr;
    QPlainTextEdit *m_noteEdit = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QLabel *m_historyStatus = nullptr;
    QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>> *m_historyWatcher = nullptr;
};
