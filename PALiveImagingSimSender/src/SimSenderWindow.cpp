#include "SimSenderWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTime>
#include <QVBoxLayout>

SimSenderWindow::SimSenderWindow(QWidget *parent)
    : QMainWindow(parent) {
    buildUi();
    connect(this, &SimSenderWindow::progressUpdated,
            this, &SimSenderWindow::onProgress);
    connect(this, &SimSenderWindow::logLine,
            this, &SimSenderWindow::onLog);
    setWindowTitle("PALiveImaging UDP 数据发送模拟器");
    resize(780, 680);
}

void SimSenderWindow::buildUi() {
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);

    auto *grp = new QGroupBox("发送配置");
    auto *form = new QFormLayout(grp);

    m_edtFile = new QLineEdit("D:/zzx/data/20260519/14.dat");
    m_btnBrowse = new QPushButton("浏览…");
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_edtFile, 1);
    fileRow->addWidget(m_btnBrowse);

    m_cmbDataset = new QComboBox;
    m_cmbDataset->addItems({"14.dat（360°）", "11.dat（180°）"});
    m_cmbBits = new QComboBox;
    m_cmbBits->addItems({"32 (int32)", "16 (int16)"});
    m_spnRate = new QSpinBox;
    m_spnRate->setRange(0, 1000);
    m_spnRate->setValue(40);
    m_spnRate->setSuffix(" Hz（0=最快）");
    m_spnAcqTimeNs = new QSpinBox;
    m_spnAcqTimeNs->setRange(1000, 1000000);
    m_spnAcqTimeNs->setValue(20000);   // 14.dat：200MHz × 4000 点 = 20000ns
    m_spnAcqTimeNs->setSuffix(" ns");
    m_spnAcqTimeNs->setToolTip(
        "监听程序“采集时间(ns)”。按监听采样间隔折算目标点数，"
        "超过源数据时长的部分用第51~100点底噪循环补齐。");
    m_spnSourceRateMHz = new QDoubleSpinBox;
    m_spnSourceRateMHz->setRange(50.0, 1000.0);
    m_spnSourceRateMHz->setDecimals(1);
    m_spnSourceRateMHz->setValue(200.0);
    m_spnSourceRateMHz->setSuffix(" MSa/s");
    m_spnSourceRateMHz->setToolTip("模拟数据文件自身的采样率（14.dat 为 200MSa/s）。");
    m_spnListenerRateMHz = new QDoubleSpinBox;
    m_spnListenerRateMHz->setRange(50.0, 1000.0);
    m_spnListenerRateMHz->setDecimals(1);
    m_spnListenerRateMHz->setValue(200.0);
    m_spnListenerRateMHz->setSuffix(" MSa/s");
    m_spnListenerRateMHz->setToolTip(
        "监听程序采样率。当前为手动输入；后续版本改为读取监听程序"
        "开始测量时下发的采样率并只读显示。");
    m_spnPort = new QSpinBox;
    m_spnPort->setRange(1024, 65535);
    m_spnPort->setValue(8001);

    form->addRow("数据文件", fileRow);
    form->addRow("数据集", m_cmbDataset);
    form->addRow("位宽", m_cmbBits);
    form->addRow("发送速率", m_spnRate);
    form->addRow("采集时间", m_spnAcqTimeNs);
    form->addRow("模拟数据采样率", m_spnSourceRateMHz);
    form->addRow("监听程序采样率", m_spnListenerRateMHz);
    form->addRow("起始端口", m_spnPort);
    root->addWidget(grp);

    // ── 发送通道选择 ──────────────────────────────────────────
    auto *chGrp = new QGroupBox("发送通道（未勾选通道发送噪声）");
    auto *chLayout = new QGridLayout(chGrp);
    const char *chLabels[8] = {
        "卡1-通道A", "卡1-通道B", "卡2-通道A", "卡2-通道B",
        "卡3-通道A", "卡3-通道B", "卡4-通道A", "卡4-通道B"
    };
    for (int i = 0; i < 8; ++i) {
        m_chkChannel[i] = new QCheckBox(chLabels[i]);
        m_chkChannel[i]->setChecked(true);
        chLayout->addWidget(m_chkChannel[i], i / 2, i % 2);
    }
    auto *noiseRow = new QHBoxLayout;
    m_spnNoise = new QSpinBox;
    m_spnNoise->setRange(1, 100);
    m_spnNoise->setValue(10);
    m_spnNoise->setSuffix(" %");
    noiseRow->addWidget(new QLabel("未勾选通道噪声幅度（% 信号 RMS）:"));
    noiseRow->addWidget(m_spnNoise);
    noiseRow->addStretch();
    chLayout->addLayout(noiseRow, 4, 0, 1, 2);
    root->addWidget(chGrp);

    // ── 虚拟采集卡 ────────────────────────────────────────────
    auto *vcGrp = new QGroupBox("虚拟采集卡（模拟卡片上线与配置应答）");
    auto *vcForm = new QFormLayout(vcGrp);
    m_spnVcCards = new QSpinBox;
    m_spnVcCards->setRange(1, 4);
    m_spnVcCards->setValue(4);
    m_edtHostIp = new QLineEdit("127.0.0.1");
    auto *vcBtnRow = new QHBoxLayout;
    m_btnVcStart = new QPushButton("启动虚拟采集卡");
    m_btnVcStop = new QPushButton("停止虚拟采集卡");
    m_btnVcStop->setEnabled(false);
    vcBtnRow->addWidget(m_btnVcStart);
    vcBtnRow->addWidget(m_btnVcStop);
    vcBtnRow->addStretch();
    m_lblVcStatus = new QLabel("未启动");
    vcForm->addRow("虚拟卡数量", m_spnVcCards);
    vcForm->addRow("监听程序IP", m_edtHostIp);
    vcForm->addRow("", vcBtnRow);
    vcForm->addRow("状态", m_lblVcStatus);
    root->addWidget(vcGrp);

    auto *btnRow = new QHBoxLayout;
    m_btnStart = new QPushButton("开始发送");
    m_btnStop = new QPushButton("停止");
    m_btnStop->setEnabled(false);
    btnRow->addWidget(m_btnStart);
    btnRow->addWidget(m_btnStop);
    btnRow->addStretch();
    root->addLayout(btnRow);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    m_lblStatus = new QLabel("就绪");
    root->addWidget(m_lblStatus);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    root->addWidget(m_log, 1);

    setCentralWidget(central);

    connect(m_btnBrowse, &QPushButton::clicked, this, &SimSenderWindow::onBrowse);
    connect(m_btnStart, &QPushButton::clicked, this, &SimSenderWindow::onStart);
    connect(m_btnStop, &QPushButton::clicked, this, &SimSenderWindow::onStop);
    connect(m_btnVcStart, &QPushButton::clicked, this, &SimSenderWindow::onVirtualStart);
    connect(m_btnVcStop, &QPushButton::clicked, this, &SimSenderWindow::onVirtualStop);
}

void SimSenderWindow::onBrowse() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择数据文件", m_edtFile->text(), "数据文件 (*.dat *.bin)");
    if (!path.isEmpty()) m_edtFile->setText(path);
}

void SimSenderWindow::onStart() {
    if (m_sender.running()) return;
    UdpReplaySender::Config cfg;
    cfg.dataPath = m_edtFile->text().toStdString();
    cfg.id = (m_cmbDataset->currentIndex() == 0) ? 14 : 11;
    cfg.bits = (m_cmbBits->currentIndex() == 0) ? 32 : 16;
    cfg.rateHz = m_spnRate->value();
    cfg.acqTimeNs = m_spnAcqTimeNs->value();
    cfg.sourceRateMHz = m_spnSourceRateMHz->value();
    cfg.listenerRateMHz = m_spnListenerRateMHz->value();
    cfg.portBase = m_spnPort->value();
    for (int i = 0; i < 8; ++i)
        cfg.channelEnabled.push_back(m_chkChannel[i]->isChecked());
    cfg.noisePercent = m_spnNoise->value();

    const bool ok = m_sender.start(
        cfg,
        [this](int sent, int total) {
            emit progressUpdated(sent, total);
        },
        [this](const std::string &line) {
            emit logLine(QString::fromStdString(line));
        });
    if (!ok) {
        QMessageBox::warning(this, "无法启动", "发送已在运行或数据文件为空");
        return;
    }
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_lblStatus->setText("正在发送…");
}

void SimSenderWindow::onVirtualStart() {
    if (m_virtualCards.running()) return;
    VirtualCardEmulator::Config cfg;
    cfg.nCards = m_spnVcCards->value();
    cfg.hostIP = m_edtHostIp->text().trimmed().toStdString();
    if (cfg.hostIP.empty()) cfg.hostIP = "127.0.0.1";

    const bool ok = m_virtualCards.start(
        cfg,
        [this](const std::string &line) {
            emit logLine(QString::fromStdString(line));
        });
    if (!ok) {
        QMessageBox::warning(this, "无法启动", "虚拟采集卡已在运行");
        return;
    }
    m_btnVcStart->setEnabled(false);
    m_btnVcStop->setEnabled(true);
    m_lblVcStatus->setText("运行中");
    m_log->appendPlainText(
        QString("[%1] 提示：监听程序请使用 --target-ips 127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4 启动，"
                "以便每张虚拟卡按源IP映射就绪状态")
            .arg(QTime::currentTime().toString("HH:mm:ss")));
}

void SimSenderWindow::onVirtualStop() {
    m_virtualCards.requestStop();
}

void SimSenderWindow::onStop() {
    m_sender.requestStop();
}

void SimSenderWindow::onProgress(int sent, int total) {
    m_progress->setMaximum(total);
    m_progress->setValue(sent);
    m_lblStatus->setText(QString("已发送 %1 / %2 触发").arg(sent).arg(total));
    if (sent >= total) {
        m_btnStart->setEnabled(true);
        m_btnStop->setEnabled(false);
    }
}

void SimSenderWindow::onLog(const QString &line) {
    m_log->appendPlainText(QString("[%1] %2")
                               .arg(QTime::currentTime().toString("HH:mm:ss"))
                               .arg(line));
    if (line.contains("完成") || line.contains("停止")) {
        m_btnStart->setEnabled(true);
        m_btnStop->setEnabled(false);
        m_lblStatus->setText(line);
    }
    if (line.contains("[虚拟卡] 已停止")) {
        m_btnVcStart->setEnabled(true);
        m_btnVcStop->setEnabled(false);
        m_lblVcStatus->setText("未启动");
    }
    if (line.contains("启动失败")) {
        m_btnVcStart->setEnabled(true);
        m_btnVcStop->setEnabled(false);
        m_lblVcStatus->setText("启动失败");
    }
}
