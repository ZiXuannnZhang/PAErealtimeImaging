#pragma once

#include "DiagnosticRecorder.h"

#include <QDialog>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
template <typename T>
class QFutureWatcher;

// UI for choosing the live recorder run or a persisted historical run and
// collecting the optional operator note and ZIP destination.  Enumeration is
// intentionally started by the dialog in QtConcurrent so opening the dialog
// never walks the diagnostics directory on the UI thread.
class DiagnosticExportDialog final : public QDialog
{
public:
    struct Selection {
        bool currentRun = true;
        DiagnosticRecorder::RunInfo historical;
        QString note;
        QString targetPath;
    };

    explicit DiagnosticExportDialog(const QString &currentRunId,
                                    const QString &currentRunDirectory,
                                    QWidget *parent = nullptr);
    ~DiagnosticExportDialog() override;

    Selection selection() const;

private:
    void chooseTargetPath();
    void acceptSelection();
    void applyHistoricalRuns(const QVector<DiagnosticRecorder::RunInfo> &runs);

    QString m_currentRunId;
    QString m_currentRunDirectory;
    QComboBox *m_runCombo = nullptr;
    QPlainTextEdit *m_noteEdit = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QLabel *m_historyStatus = nullptr;
    QVector<DiagnosticRecorder::RunInfo> m_historicalRuns;
    QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>> *m_historyWatcher = nullptr;
};
