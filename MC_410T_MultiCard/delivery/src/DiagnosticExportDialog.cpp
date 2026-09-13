#include "DiagnosticExportDialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

DiagnosticExportDialog::DiagnosticExportDialog(const QString &currentRunId,
                                               const QString &currentRunDirectory,
                                               QWidget *parent)
    : QDialog(parent)
    , m_currentRunId(currentRunId)
    , m_currentRunDirectory(currentRunDirectory)
{
    setWindowTitle(QStringLiteral("导出诊断日志"));
    setModal(true);
    setMinimumWidth(620);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_runCombo = new QComboBox(this);
    m_runCombo->setObjectName(QStringLiteral("runCombo"));
    const QString liveLabel = m_currentRunId.isEmpty()
        ? QStringLiteral("当前运行")
        : QStringLiteral("当前运行（%1）").arg(m_currentRunId);
    m_runCombo->addItem(liveLabel);
    m_runCombo->setToolTip(QStringLiteral("导出点击保存时当前运行的日志边界"));
    form->addRow(QStringLiteral("导出范围:"), m_runCombo);

    m_historyStatus = new QLabel(QStringLiteral("正在加载历史运行…"), this);
    m_historyStatus->setStyleSheet(QStringLiteral("color: #888888;"));
    form->addRow(QString(), m_historyStatus);

    m_noteEdit = new QPlainTextEdit(this);
    m_noteEdit->setObjectName(QStringLiteral("noteEdit"));
    m_noteEdit->setPlaceholderText(QStringLiteral("可选：填写现场现象、复现步骤或提供给维护人员的说明"));
    m_noteEdit->setMaximumHeight(84);
    form->addRow(QStringLiteral("现场说明:"), m_noteEdit);

    auto *targetRow = new QWidget(this);
    auto *targetLayout = new QHBoxLayout(targetRow);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    m_targetEdit = new QLineEdit(targetRow);
    m_targetEdit->setObjectName(QStringLiteral("targetEdit"));
    m_targetEdit->setPlaceholderText(QStringLiteral("选择 ZIP 保存位置"));
    auto *browseButton = new QPushButton(QStringLiteral("选择…"), targetRow);
    targetLayout->addWidget(m_targetEdit, 1);
    targetLayout->addWidget(browseButton);
    form->addRow(QStringLiteral("ZIP 文件:"), targetRow);

    layout->addLayout(form);

    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString directory = documents.isEmpty() ? QDir::currentPath() : documents;
    QString safeRunId = m_currentRunId;
    safeRunId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    if (safeRunId.isEmpty()) safeRunId = QStringLiteral("current");
    m_targetEdit->setText(QDir(directory).filePath(
        QStringLiteral("diagnostic-%1-%2.zip")
            .arg(safeRunId)
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")))));

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *exportButton = buttons->addButton(QStringLiteral("导出"), QDialogButtonBox::AcceptRole);
    exportButton->setObjectName(QStringLiteral("exportButton"));
    QPushButton *cancelButton = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    cancelButton->setObjectName(QStringLiteral("cancelButton"));
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked,
            this, &DiagnosticExportDialog::chooseTargetPath);
    connect(exportButton, &QPushButton::clicked,
            this, &DiagnosticExportDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // The worker captures no dialog state.  Only the finished signal touches
    // widgets, and the QObject context disconnects it if the dialog closes.
    m_historyWatcher = new QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>>(this);
    connect(m_historyWatcher,
            &QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>>::finished,
            this,
            [this]() {
                applyHistoricalRuns(m_historyWatcher->result());
                m_historyStatus->setText(m_historicalRuns.isEmpty()
                    ? QStringLiteral("没有可选的历史运行")
                    : QStringLiteral("已加载 %1 个历史运行").arg(m_historicalRuns.size()));
            });
    const QString historyRoot = QFileInfo(m_currentRunDirectory).absolutePath();
    m_historyWatcher->setFuture(QtConcurrent::run([historyRoot] {
        return DiagnosticRecorder::enumerateRuns(historyRoot);
    }));
}

DiagnosticExportDialog::~DiagnosticExportDialog() = default;

DiagnosticExportDialog::Selection DiagnosticExportDialog::selection() const
{
    Selection result;
    result.currentRun = m_runCombo && m_runCombo->currentIndex() == 0;
    if (!result.currentRun) {
        const int historyIndex = m_runCombo->currentIndex() - 1;
        if (historyIndex >= 0 && historyIndex < m_historicalRuns.size())
            result.historical = m_historicalRuns.at(historyIndex);
    }
    result.note = m_noteEdit ? m_noteEdit->toPlainText().trimmed() : QString();
    result.targetPath = m_targetEdit ? m_targetEdit->text().trimmed() : QString();
    return result;
}

void DiagnosticExportDialog::chooseTargetPath()
{
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("选择诊断日志 ZIP 保存位置"),
        m_targetEdit ? m_targetEdit->text() : QString(),
        QStringLiteral("ZIP 文件 (*.zip)"));
    if (path.isEmpty() || !m_targetEdit) return;
    QString normalized = path;
    if (!normalized.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
        normalized += QStringLiteral(".zip");
    m_targetEdit->setText(normalized);
}

void DiagnosticExportDialog::acceptSelection()
{
    if (!m_targetEdit || m_targetEdit->text().trimmed().isEmpty()) {
        m_targetEdit->setFocus();
        return;
    }
    QString normalized = m_targetEdit->text().trimmed();
    if (!normalized.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
        normalized += QStringLiteral(".zip");
    m_targetEdit->setText(QFileInfo(normalized).absoluteFilePath());
    accept();
}

void DiagnosticExportDialog::applyHistoricalRuns(
    const QVector<DiagnosticRecorder::RunInfo> &runs)
{
    m_historicalRuns.clear();
    for (const auto &run : runs) {
        if (run.directory.isEmpty() || run.runId.isEmpty()) continue;
        if (!m_currentRunDirectory.isEmpty()
            && QFileInfo(run.directory).absoluteFilePath()
                == QFileInfo(m_currentRunDirectory).absoluteFilePath())
            continue;
        m_historicalRuns.append(run);
        const QString time = run.startIsoTime.isEmpty()
            ? QStringLiteral("时间未知") : run.startIsoTime;
        const QString state = run.active
            ? QStringLiteral("进行中")
            : (run.manifestComplete ? QStringLiteral("已完成") : QStringLiteral("未完整结束"));
        const QString id = run.runId.isEmpty() ? QFileInfo(run.directory).fileName() : run.runId;
        m_runCombo->addItem(QStringLiteral("历史：%1 · %2 · %3").arg(time, state, id));
    }
}
