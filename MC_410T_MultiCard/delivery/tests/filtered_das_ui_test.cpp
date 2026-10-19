#include "RingConfigDialog.h"
#include "ImagingController.h"
#include "RingTimevarLowpassSettings.h"
#include "FilteredDasSettings.h"
#include <QLabel>
#include <QLineEdit>
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
    QApplication app(argc,argv);QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");app.setFont(QFont("Microsoft YaHei",9));
    try {
        check(QCoreApplication::applicationDirPath().endsWith("filtered-ui-isolated"),"unsafe settings path");
        QFile::remove(paimageSettingsPath());ImagingController controller;RingConfigDialog dlg(&controller);dlg.setAcquisitionParams(4,20000);
        const QJsonObject factory{{"enabled",true},{"order",.5},{"referenceMhz",5.0},
            {"passMhz",40.0},{"stopMhz",60.0},{"padFactor",4.0},{"batchColumns",64}};
        check(filtered_das::toJson(dlg.filteredDasConfig())==factory,"entry factory snapshot");
        check(widget<QCheckBox>(dlg,"filteredDasEnabled")->text()=="滤波反投影","Chinese checkbox label");
        auto advanced=[&](bool accept,bool invalid=false) {
            QTimer::singleShot(0,[&,accept,invalid] {
                auto* d=dlg.findChild<QDialog*>("filteredDasAdvancedDialog");check(d,"separate advanced dialog");
                check(d->windowTitle()=="滤波反投影高级参数","advanced title");
                check(d->findChildren<QDoubleSpinBox*>().size()==1&&d->findChildren<QSpinBox*>().size()==1,"advanced controls scope");
                widget<QDoubleSpinBox>(*d,"filteredDasPadFactor")->setValue(4.25);
                widget<QSpinBox>(*d,"filteredDasBatchColumns")->setValue(invalid?2147483647:7);
                if(invalid){dismissMessage();QTimer::singleShot(30,d,&QDialog::reject);}
                d->findChild<QDialogButtonBox*>()->button(accept?QDialogButtonBox::Ok:QDialogButtonBox::Cancel)->click();
            });widget<QPushButton>(dlg,"filteredDasAdvanced")->click();
        };
        advanced(false);check(filtered_das::toJson(dlg.filteredDasConfig())==factory,"advanced cancel mutated parent");
        advanced(true,true);check(dlg.filteredDasConfig().batchColumns==64,"invalid resource draft accepted");
        advanced(true);check(dlg.filteredDasConfig().batchColumns==7&&controller.filteredDasConfig().batchColumns==64,"advanced applied early");
        dlg.reject();check(filtered_das::toJson(dlg.filteredDasConfig())==factory,"parent cancel failed");
        advanced(true);widget<QDoubleSpinBox>(dlg,"filteredDasOrder")->setValue(.75);
        widget<QDoubleSpinBox>(dlg,"filteredDasReferenceMhz")->setValue(6);
        widget<QDoubleSpinBox>(dlg,"filteredDasPassMhz")->setValue(35);
        widget<QDoubleSpinBox>(dlg,"filteredDasStopMhz")->setValue(100);
        widget<QCheckBox>(dlg,"timevarEnabled")->setChecked(true);
        check(dlg.applyConfig(),"apply failed");const auto accepted=dlg.filteredDasConfig();const auto acceptedTv=dlg.timevarConfig();
        check(filtered_das::toJson(controller.filteredDasConfig())==filtered_das::toJson(accepted),"controller snapshot");
        check(!widget<QLabel>(dlg,"filteredDasSamplingHint")->text().isEmpty(),"spatial sampling advisory absent");
        widget<QDoubleSpinBox>(dlg,"filteredDasOrder")->setValue(.25);dlg.reject();check(dlg.filteredDasConfig().order==.75,"cancel lost accepted");
        click(dlg,"设为默认");
        RingConfigDialog restart(&controller);restart.setAcquisitionParams(4,20000);
        check(filtered_das::toJson(restart.filteredDasConfig())==filtered_das::toJson(accepted),"restart/defaults incomplete");
        check(ring_timevar::toJson(restart.timevarConfig())==ring_timevar::toJson(acceptedTv),"new defaults overwrote timevar");
        widget<QCheckBox>(restart,"filteredDasEnabled")->setChecked(false);
        for(const char* name:{"filteredDasOrder","filteredDasReferenceMhz","filteredDasPassMhz","filteredDasStopMhz"})
            check(!widget<QDoubleSpinBox>(restart,name)->isEnabled(),"OFF main parameter enabled");
        check(widget<QPushButton>(restart,"filteredDasAdvanced")->isEnabled(),"OFF cannot edit advanced draft");
        check(restart.applyConfig(),"OFF apply");click(restart,"设为默认");
        widget<QCheckBox>(restart,"filteredDasEnabled")->setChecked(true);click(restart,"恢复默认");
        check(!restart.filteredDasConfig().enabled&&restart.filteredDasConfig().batchColumns==7,"complete disabled defaults");
        QSettings ini(paimageSettingsPath(),QSettings::IniFormat);
        ini.setValue("RingConfigDialog/Defaults/filteredDasEnabled",true);
        ini.setValue("RingConfigDialog/Defaults/filteredDasOrder","broken");ini.sync();dismissMessage();
        RingConfigDialog bad(&controller);app.processEvents();auto safe=FilteredDasConfig{};safe.enabled=false;
        check(filtered_das::toJson(bad.filteredDasConfig())==filtered_das::toJson(safe),"bad INI not complete disabled fallback");
        check(bad.timevarConfig().enabled,"bad FilteredDAS INI damaged timevar");
        ini.sync();check(ini.value("RingConfigDialog/Defaults/filteredDasOrder").toString()=="broken","bad INI overwritten");
        auto exact=accepted;exact.order=.5000000000000001;exact.referenceMhz=5.123456789123;exact.padFactor=4.250000000000001;
        ini.beginGroup("RingConfigDialog/Defaults");filtered_das::saveSettings(ini,exact);ini.endGroup();ini.sync();
        RingConfigDialog precision(&controller);precision.setAcquisitionParams(4,20000);check(precision.applyConfig(),"precision apply");
        check(filtered_das::toJson(controller.filteredDasConfig())==filtered_das::toJson(exact),"display quantized saved values");
        widget<QDoubleSpinBox>(precision,"filteredDasStopMhz")->setValue(125);dismissMessage();
        check(!precision.applyConfig(),"invalid time Nyquist applied");dismissMessage();click(precision,"设为默认");ini.sync();
        check(ini.value("RingConfigDialog/Defaults/filteredDasStopMhz").toDouble()==exact.stopMhz,"invalid draft partially saved");
        precision.reject();precision.show();precision.findChild<QTabWidget*>()->setCurrentIndex(1);app.processEvents();
        widget<QCheckBox>(precision,"filteredDasEnabled")->parentWidget()->grab().save(QCoreApplication::applicationDirPath()+"/filtered-das-controls.png");
        RingConfigDialog sampling(&controller);sampling.setAcquisitionParams(4,20000);
        widget<QLineEdit>(sampling,"soundSpeedRadii")->setText("0");
        widget<QLineEdit>(sampling,"soundSpeeds")->setText("1540,1490");
        widget<QDoubleSpinBox>(sampling,"filteredDasStopMhz")->setValue(76);
        check(sampling.applyConfig()&&widget<QLabel>(sampling,"filteredDasSamplingHint")->text().isEmpty(),
              "single-speed advisory used inactive second speed");
        widget<QLineEdit>(sampling,"soundSpeedRadii")->setText("7.5");
        check(sampling.applyConfig()&&!widget<QLabel>(sampling,"filteredDasSamplingHint")->text().isEmpty(),
              "layered-speed advisory ignored active slow layer");
        QFile::remove(paimageSettingsPath());std::cout<<"PASS FilteredDAS UI factory/draft/advanced/defaults/restart/precision/INI/independence\n";return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
