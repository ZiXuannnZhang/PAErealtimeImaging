#include "DiagnosticExportDialog.h"
#include <QApplication>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QFontDatabase>
#include <QFont>

int main(int argc, char **argv) {
    QApplication app(argc,argv);
    const int fontId=QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if(fontId>=0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(),10));
    QTemporaryDir temp;
    DiagnosticRecorder::Options options; options.rootDirectory=temp.path();
    { DiagnosticRecorder old(options); old.logText("历史运行记录"); }
    auto *recorder=DiagnosticRecorder::initialize(options);
    recorder->logText("当前运行记录");
    bool ok=false;
    {
        DiagnosticExportDialog dialog(recorder->runId(),recorder->runDirectory());
        QFile style(QStringLiteral(DIAGNOSTIC_SOURCE_DIR "/resources/styles.qss"));
        if(style.open(QIODevice::ReadOnly)) dialog.setStyleSheet(QString::fromUtf8(style.readAll()));
        dialog.show();
        auto *start=dialog.findChild<QDateTimeEdit*>("startTimeEdit");
        auto *end=dialog.findChild<QDateTimeEdit*>("endTimeEdit");
        auto *path=dialog.findChild<QLineEdit*>();
        auto *note=dialog.findChild<QPlainTextEdit*>();
        if(!start || !end || !path || !note) return 2;
        QElapsedTimer timer; timer.start();
        while(timer.elapsed()<3000) { app.processEvents(); QThread::msleep(5); }
        const QDateTime now=QDateTime::currentDateTime();
        if(!DiagnosticExportDialog::isValidTimeWindow(now.addSecs(-10), now, now)) return 3;
        if(DiagnosticExportDialog::isValidTimeWindow(now, now.addSecs(-1), now)) return 4;
        start->setDateTime(now);
        end->setDateTime(now.addSecs(-1));
        QTimer::singleShot(0, [] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                    message->accept();
                    return;
                }
            }
        });
        for(auto *button:dialog.findChildren<QPushButton*>())
            if(button->text()==QStringLiteral("导出")) button->click();
        if(dialog.result()==QDialog::Accepted) return 5;
        start->setDateTime(now.addSecs(-30));
        end->setDateTime(now.addSecs(-1));
        note->setPlainText("四张卡确认成功，第五项目标无反馈；现场测试记录。");
        path->setText(QDir(temp.path()).filePath("诊断日志.zip"));
        app.processEvents();
        if(argc>1) dialog.grab().save(QString::fromLocal8Bit(argv[1]));
        for(auto *button:dialog.findChildren<QPushButton*>())
            if(button->text()==QStringLiteral("导出")) button->click();
        const auto selection=dialog.selection();
        ok=dialog.result()==QDialog::Accepted && selection.startTime <= selection.endTime
            && selection.note.contains("第五项") && selection.targetPath.endsWith(".zip");
    }
    QThreadPool::globalInstance()->waitForDone();
    DiagnosticRecorder::shutdown();
    QTextStream(stdout) << (ok?"PASS":"FAIL") << " diagnostic dialog time-window validation/note/path selection" << Qt::endl;
    return ok?0:5;
}
