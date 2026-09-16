#pragma once

#include <QMainWindow>
#include <QString>

#include "UdpReplaySender.h"
#include "VirtualCardEmulator.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

// UDP 数据发送模拟器主窗口：一键启动同机 UDP 联调数据发送。
class SimSenderWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit SimSenderWindow(QWidget *parent = nullptr);

signals:
    void progressUpdated(int sent, int total);
    void logLine(const QString &line);

private slots:
    void onBrowse();
    void onStart();
    void onStop();
    void onVirtualStart();
    void onVirtualStop();
    void onProgress(int sent, int total);
    void onLog(const QString &line);

private:
    void buildUi();
    UdpReplaySender m_sender;
    VirtualCardEmulator m_virtualCards;

    QLineEdit      *m_edtFile;
    QPushButton    *m_btnBrowse;
    QComboBox      *m_cmbDataset;
    QComboBox      *m_cmbBits;
    QSpinBox       *m_spnRate;
    QSpinBox       *m_spnAcqTimeNs;
    QDoubleSpinBox *m_spnSourceRateMHz;
    QDoubleSpinBox *m_spnListenerRateMHz;
    QSpinBox       *m_spnPort;
    QSpinBox       *m_spnNoise;
    QCheckBox      *m_chkChannel[8];
    QSpinBox       *m_spnVcCards;
    QLineEdit      *m_edtHostIp;
    QPushButton    *m_btnVcStart;
    QPushButton    *m_btnVcStop;
    QLabel         *m_lblVcStatus;
    QPushButton    *m_btnStart;
    QPushButton    *m_btnStop;
    QProgressBar   *m_progress;
    QLabel         *m_lblStatus;
    QPlainTextEdit *m_log;
};
