#include "RingConfigDialog.h"
#include "ImagingController.h"
#include "RingTimevarLowpassSettings.h"
#include "PaimageAcquisition/SettingsPath.h"
#include <QApplication>
#include <QCheckBox>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QTimer>
#include <QMessageBox>
#include <QTabWidget>
#include <QFile>
#include <QFontDatabase>
#include <QGroupBox>
#include <iostream>
#include <stdexcept>

void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
template<class T>T* widget(QObject& d,const char* name){auto *w=d.findChild<T*>(name);check(w,name);return w;}
void click(RingConfigDialog& d,const QString& name) {
    for(auto* b:d.findChildren<QPushButton*>())if(b->text()==name){b->click();return;}
    throw std::runtime_error("button not found");
}
void dismissMessage(){QTimer::singleShot(10,[]{for(auto* w:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(w))box->accept();});}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei",9));
    try {
        // Dedicated executable directory: never shares the production INI path.
        check(QCoreApplication::applicationDirPath().endsWith("ui-isolated"),"unsafe settings path");
        QFile::remove(paimageSettingsPath());
        ImagingController controller;
        RingConfigDialog dlg(&controller);dlg.setAcquisitionParams(4,20000);
        check(!dlg.timevarConfig().enabled,"factory enabled");
        check(!widget<QSpinBox>(dlg,"timevarP1")->isEnabled(),"OFF controls enabled");
        for(auto* w:dlg.findChildren<QCheckBox*>())check(!w->text().contains("补偿")&&!w->text().contains("Plot"),"old controls");
        auto advanced=[&](bool accept,bool invalid=false) {
            QTimer::singleShot(0,[&,accept,invalid] {
                auto* d=dlg.findChild<QDialog*>("timevarAdvancedDialog");check(d,"advanced dialog");
                widget<QSpinBox>(*d,"timevarWindowLength")->setValue(129);
                widget<QDoubleSpinBox>(*d,"timevarOverlapRatio")->setValue(.625);
                widget<QCheckBox>(*d,"timevarFftAuto")->setChecked(false);
                widget<QSpinBox>(*d,"timevarFftLength")->setValue(invalid?128:192);
                widget<QDoubleSpinBox>(*d,"timevarFilterOrder")->setValue(2.5);
                widget<QComboBox>(*d,"timevarTransition")->setCurrentText("linear");
                auto* buttons=d->findChild<QDialogButtonBox*>();
                if(invalid) {
                    dismissMessage();
                    QTimer::singleShot(10,d,&QDialog::reject);
                }
                buttons->button(accept?QDialogButtonBox::Ok:QDialogButtonBox::Cancel)->click();
            });
            widget<QPushButton>(dlg,"timevarAdvanced")->click();
        };
        advanced(false);check(dlg.timevarConfig().windowLength==128,"advanced cancel changed draft");
        advanced(true,true);check(dlg.timevarConfig().windowLength==128,"invalid advanced accepted");
        advanced(true);check(dlg.timevarConfig().fftLength==192,"advanced OK missing");
        check(controller.ringTimevarConfig().fftLength==0,"advanced prematurely applied");
        dlg.reject();check(dlg.timevarConfig().fftLength==0,"parent cancel retained draft");
        advanced(true);widget<QCheckBox>(dlg,"timevarEnabled")->setChecked(true);
        widget<QDoubleSpinBox>(dlg,"timevarFcMaxMhz")->setValue(42);
        widget<QDoubleSpinBox>(dlg,"timevarFcMinMhz")->setValue(2);
        widget<QSpinBox>(dlg,"timevarP1")->setValue(700);widget<QSpinBox>(dlg,"timevarP2")->setValue(2100);
        check(dlg.applyConfig(),"apply rejected");const auto accepted=dlg.timevarConfig();
        check(ring_timevar::toJson(accepted)==ring_timevar::toJson(controller.ringTimevarConfig()),"controller snapshot differs");
        widget<QSpinBox>(dlg,"timevarP2")->setValue(2200);dlg.reject();
        check(dlg.timevarConfig().p2==2100,"cancel lost applied snapshot");
        QSettings ini(paimageSettingsPath(),QSettings::IniFormat);ini.setValue("RingConfigDialog/Defaults/enhanceFreq",true);ini.sync();
        click(dlg,"设为默认");
        RingConfigDialog restarted(&controller);restarted.setAcquisitionParams(4,20000);
        check(ring_timevar::toJson(restarted.timevarConfig())==ring_timevar::toJson(accepted),"startup lost defaults");
        widget<QSpinBox>(restarted,"timevarP2")->setValue(2300);click(restarted,"恢复默认");
        check(restarted.timevarConfig().p2==2100,"restore defaults differs");
        check(restarted.applyConfig(),"first imaging config rejected");
        ini.sync();check(!ini.contains("RingConfigDialog/Defaults/enhanceFreq"),"legacy key not cleaned");
        widget<QSpinBox>(restarted,"timevarP2")->setValue(6000);dismissMessage();click(restarted,"设为默认");
        ini.sync();check(ini.value("RingConfigDialog/Defaults/timevarLpP2").toInt()==2100,"invalid draft saved");
        dismissMessage();check(!restarted.applyConfig(),"invalid config applied");
        check(controller.ringTimevarConfig().p2==2100,"invalid partially applied");
        restarted.reject();restarted.show();restarted.findChild<QTabWidget*>()->setCurrentIndex(1);app.processEvents();
        restarted.grab().save(QCoreApplication::applicationDirPath()+"/ring-timevar-ui.png");
        widget<QCheckBox>(restarted,"timevarEnabled")->parentWidget()->grab().save(
            QCoreApplication::applicationDirPath()+"/ring-timevar-controls.png");
        // A bad saved snapshot falls back atomically and is never overwritten on load.
        ini.setValue("RingConfigDialog/Defaults/timevarLpP2","broken");ini.sync();
        dismissMessage();
        RingConfigDialog badDefaults(&controller);app.processEvents();
        check(!badDefaults.timevarConfig().enabled&&badDefaults.timevarConfig().p2==2000,
              "bad saved snapshot partly enabled");
        ini.sync();check(ini.value("RingConfigDialog/Defaults/timevarLpP2").toString()=="broken",
                         "load rewrote bad defaults");
        ini.beginGroup("RingConfigDialog/Defaults");auto deep=accepted;deep.p2=5000;
        ring_timevar::saveSettings(ini,deep);ini.endGroup();ini.sync();
        RingConfigDialog deepDefaults(&controller);deepDefaults.setAcquisitionParams(4,30000);
        check(deepDefaults.timevarConfig().enabled&&deepDefaults.timevarConfig().p2==5000,
              "lazy UI validated saved defaults against placeholder depth");
        check(deepDefaults.applyConfig(),"deep startup config failed");
        deepDefaults.setAcquisitionParams(4,16000);dismissMessage();
        check(!deepDefaults.applyConfig()&&deepDefaults.timevarConfig().p2==5000,
              "depth change silently clamped configuration");
        QFile::remove(paimageSettingsPath());
        std::cout<<"PASS UI advanced/parent cancel, apply, defaults, restart, invalid draft isolation\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
