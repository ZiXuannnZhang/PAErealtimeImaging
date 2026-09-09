#pragma once
#include <QCoreApplication>
#include <QString>
#include <QSettings>
#include <QFileInfo>
#include <QDateTime>
inline QString paimageSettingsPath(){return QCoreApplication::applicationDirPath()+QStringLiteral("/PAimageAcquisitionPort.ini");}
inline bool seedPaimageSettings(){
    if(QFileInfo::exists(paimageSettingsPath()))return false;
    // Read the existing application's parameters once; all setters target only
    // this independent INI. The legacy registry namespace is never written.
    QSettings source("MC410T","MC410T_Receiver");
    QSettings target(paimageSettingsPath(),QSettings::IniFormat);
    for(const auto& key:source.allKeys())target.setValue(key,source.value(key));
    target.setValue("Migration/Source","HKCU/Software/MC410T/MC410T_Receiver (read only)");
    target.setValue("Migration/CopiedAt",QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    target.sync();return target.status()==QSettings::NoError;
}
