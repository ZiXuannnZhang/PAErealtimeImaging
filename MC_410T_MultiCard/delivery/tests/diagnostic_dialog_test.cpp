#include "DiagnosticExportDialog.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
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
        auto *combo=dialog.findChild<QComboBox*>();
        auto *path=dialog.findChild<QLineEdit*>();
        auto *note=dialog.findChild<QPlainTextEdit*>();
        if(!combo || !path || !note) return 2;
        QElapsedTimer timer; timer.start();
        while(combo->count()<2 && timer.elapsed()<3000) { app.processEvents(); QThread::msleep(5); }
        if(combo->count()!=2) return 3;
        combo->setCurrentIndex(1);
        if(dialog.selection().currentRun || dialog.selection().historical.lastSequence==0) return 4;
        combo->setCurrentIndex(0);
        note->setPlainText("四张卡确认成功，第五项目标无反馈；现场测试记录。");
        path->setText(QDir(temp.path()).filePath("诊断日志.zip"));
        app.processEvents();
        if(argc>1) dialog.grab().save(QString::fromLocal8Bit(argv[1]));
        for(auto *button:dialog.findChildren<QPushButton*>())
            if(button->text()==QStringLiteral("导出")) button->click();
        const auto selection=dialog.selection();
        ok=dialog.result()==QDialog::Accepted && selection.currentRun
            && selection.note.contains("第五项") && selection.targetPath.endsWith(".zip");
    }
    QThreadPool::globalInstance()->waitForDone();
    DiagnosticRecorder::shutdown();
    QTextStream(stdout) << (ok?"PASS":"FAIL") << " diagnostic dialog history/current/note/path selection" << Qt::endl;
    return ok?0:5;
}
