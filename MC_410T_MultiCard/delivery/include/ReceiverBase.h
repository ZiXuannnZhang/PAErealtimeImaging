#pragma once
#include <QThread>

// ============================================================
// ReceiverBase  接收线程公共基类
//
// MultiPortReceiver（WinSock 路线）继承此类。
// ============================================================
class ReceiverBase : public QThread {
    Q_OBJECT
public:
    explicit ReceiverBase(QObject* parent = nullptr) : QThread(parent) {}
    ~ReceiverBase() override = default;

    virtual void requestStop() = 0;
    virtual bool isActive()    const = 0;

signals:
    void statusMessage(const QString& msg);
    void errorOccurred(const QString& error);
};
