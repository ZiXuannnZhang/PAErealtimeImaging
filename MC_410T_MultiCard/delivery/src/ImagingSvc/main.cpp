#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include "ImagingSvc.h"
#include "ProcessScheduling.h"

// =====================================================================
// ImagingSvc 子进程入口
// 启动后通过 ZMQ 等待主进程下发配置和脉冲数据，
// 调用 GPU 进行光声图像重建，并将结果写回共享内存。
//
// 对应《成像功能扩展设计方案》v2.0 §3.4
// =====================================================================
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("ImagingSvc");

    const auto scheduling = ProcessScheduling::applyIngressProtectionScheduling();
    qInfo().noquote() << scheduling.logLine;

    ImagingSvc svc;
    if (!svc.initialize()) {
        qCritical() << "ImagingSvc: Initialization failed, exiting";
        return 1;
    }

    return app.exec();
}
