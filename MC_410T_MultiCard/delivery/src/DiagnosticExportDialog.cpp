#include "DiagnosticExportDialog.h"

#include <QDateTimeEdit>
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
#include <QTimeZone>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QtConcurrent/QtConcurrentRun>
#include <limits>

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

    const QDateTime now = QDateTime::currentDateTime();
    m_startEdit = new QDateTimeEdit(now.addSecs(-600), this);
    m_endEdit = new QDateTimeEdit(now, this);
    for (QDateTimeEdit *edit : {m_startEdit, m_endEdit}) {
        edit->setObjectName(edit == m_startEdit ? QStringLiteral("startTimeEdit")
                                                 : QStringLiteral("endTimeEdit"));
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        edit->setCalendarPopup(true);
        edit->setTimeZone(QTimeZone::systemTimeZone());
        edit->setKeyboardTracking(false);
    }
    m_endEdit->setMaximumDateTime(now);
    form->addRow(QStringLiteral("开始时间（本机）:"), m_startEdit);
    form->addRow(QStringLiteral("结束时间（本机）:"), m_endEdit);

    m_historyStatus = new QLabel(QStringLiteral("正在加载 diagnostics retention 覆盖范围…"), this);
    m_historyStatus->setObjectName(QStringLiteral("retentionStatus"));
    m_historyStatus->setWordWrap(true);
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
    if (safeRunId.isEmpty()) safeRunId = QStringLiteral("time-window");
    m_targetEdit->setText(QDir(directory).filePath(
        QStringLiteral("diagnostic-%1-%2.zip")
            .arg(safeRunId)
            .arg(now.toString(QStringLiteral("yyyyMMdd-hhmmss")))));

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *exportButton = buttons->addButton(QStringLiteral("导出"), QDialogButtonBox::AcceptRole);
    exportButton->setObjectName(QStringLiteral("exportButton"));
    buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked,
            this, &DiagnosticExportDialog::chooseTargetPath);
    connect(exportButton, &QPushButton::clicked,
            this, &DiagnosticExportDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_historyWatcher = new QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>>(this);
    connect(m_historyWatcher,
            &QFutureWatcher<QVector<DiagnosticRecorder::RunInfo>>::finished,
            this,
            [this]() { applyRetentionBounds(m_historyWatcher->result()); });
    const QString historyRoot = QFileInfo(m_currentRunDirectory).absolutePath();
    m_historyWatcher->setFuture(QtConcurrent::run([historyRoot] {
        return DiagnosticRecorder::enumerateRuns(historyRoot);
    }));
}

DiagnosticExportDialog::~DiagnosticExportDialog() = default;

DiagnosticExportDialog::Selection DiagnosticExportDialog::selection() const
{
    Selection result;
    result.startTime = m_startEdit ? m_startEdit->dateTime() : QDateTime();
    result.endTime = m_endEdit ? m_endEdit->dateTime() : QDateTime();
    result.note = m_noteEdit ? m_noteEdit->toPlainText().trimmed() : QString();
    result.targetPath = m_targetEdit ? m_targetEdit->text().trimmed() : QString();
    return result;
}

bool DiagnosticExportDialog::isValidTimeWindow(const QDateTime &start,
                                               const QDateTime &end,
                                               const QDateTime &now)
{
    return start.isValid() && end.isValid() && start <= end && end <= now;
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
    if (!m_startEdit || !m_endEdit
        || !isValidTimeWindow(m_startEdit->dateTime(), m_endEdit->dateTime())) {
        QMessageBox::warning(this, QStringLiteral("导出诊断日志"),
                             QStringLiteral("时间窗无效：开始时间必须早于或等于结束时间，且结束时间不能晚于当前时间。"));
        return;
    }
    if (!m_targetEdit || m_targetEdit->text().trimmed().isEmpty()) {
        if (m_targetEdit) m_targetEdit->setFocus();
        return;
    }
    QString normalized = m_targetEdit->text().trimmed();
    if (!normalized.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
        normalized += QStringLiteral(".zip");
    m_targetEdit->setText(QFileInfo(normalized).absoluteFilePath());
    accept();
}

void DiagnosticExportDialog::applyRetentionBounds(
    const QVector<DiagnosticRecorder::RunInfo> &runs)
{
    qint64 earliest = std::numeric_limits<qint64>::max();
    qint64 latest = std::numeric_limits<qint64>::min();
    int active = 0;
    for (const auto &run : runs) {
        const QString first = run.earliestIsoTime.isEmpty() ? run.startIsoTime : run.earliestIsoTime;
        const QString last = run.latestIsoTime.isEmpty() ? run.endIsoTime : run.latestIsoTime;
        const QDateTime firstTime = QDateTime::fromString(first, Qt::ISODateWithMs);
        const QDateTime lastTime = QDateTime::fromString(last, Qt::ISODateWithMs);
        if (firstTime.isValid()) earliest = qMin(earliest, firstTime.toMSecsSinceEpoch());
        if (lastTime.isValid()) latest = qMax(latest, lastTime.toMSecsSinceEpoch());
        if (run.active) ++active;
    }
    const QString firstText = earliest == std::numeric_limits<qint64>::max()
        ? QStringLiteral("未知")
        : QDateTime::fromMSecsSinceEpoch(earliest).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString lastText = latest == std::numeric_limits<qint64>::min()
        ? QStringLiteral("未知")
        : QDateTime::fromMSecsSinceEpoch(latest).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_historyStatus->setText(QStringLiteral("当前 retention 覆盖：%1 至 %2；run=%3，active=%4。导出会跨越所有重叠 run，包含正常、active 和 incomplete run。")
                             .arg(firstText, lastText).arg(runs.size()).arg(active));
}
