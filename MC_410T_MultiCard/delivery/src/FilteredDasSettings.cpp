#include "FilteredDasSettings.h"
#include <cmath>
#include <limits>

namespace filtered_das {
QJsonObject toJson(const FilteredDasConfig& c) {
    return {{"enabled",c.enabled},{"order",c.order},{"referenceMhz",c.referenceMhz},
        {"passMhz",c.passMhz},{"stopMhz",c.stopMhz},{"padFactor",c.padFactor},{"batchColumns",c.batchColumns}};
}
bool fromJson(const QJsonValue& v, double fs, int samples, FilteredDasConfig& out,
              QString* error, bool depthKnown) {
    auto fail=[&](const QString& e){if(error)*error=e; return false;};
    FilteredDasConfig c;
    if (!v.isUndefined() && !v.isObject()) return fail("filteredDas must be an object");
    const auto o=v.toObject();
    auto number=[&](const char* key,double& x) {
        if(!o.contains(key)) return true;
        if(!o[key].isDouble() || !std::isfinite(o[key].toDouble())) return false;
        x=o[key].toDouble(); return true;
    };
    auto integer=[&](const char* key,int& x) {
        double d=x;
        if(!number(key,d) || d!=std::floor(d) || d<0 || d>std::numeric_limits<int>::max()) return false;
        x=static_cast<int>(d); return true;
    };
    if(o.contains("enabled")) {
        if(!o["enabled"].isBool()) return fail("enabled must be boolean");
        c.enabled=o["enabled"].toBool();
    }
    if(!number("order",c.order)||!number("referenceMhz",c.referenceMhz)||
       !number("passMhz",c.passMhz)||!number("stopMhz",c.stopMhz)||
       !number("padFactor",c.padFactor)||!integer("batchColumns",c.batchColumns))
        return fail("invalid numeric field in filteredDas");
    std::string e;
    if(!validate(c,fs,samples,&e,depthKnown)) return fail(QString::fromStdString(e));
    out=c; return true;
}

static QString iniKey(QString key) {
    key[0]=key[0].toUpper(); return "filteredDas"+key;
}
bool loadSettings(QSettings& s,double fs,int samples,FilteredDasConfig& out,QString* error,bool depthKnown) {
    QJsonObject o;
    const auto defaults=toJson(FilteredDasConfig{});
    for(auto it=defaults.begin();it!=defaults.end();++it) {
        const QString key=iniKey(it.key());
        if(!s.contains(key)) continue;
        const QVariant v=s.value(key);
        // INI deserialization may return strings; parse strictly, never toDouble(default).
        if(it.value().isBool()) {
            const QString t=v.toString();
            if(t!="true" && t!="false" && t!="1" && t!="0") {
                if(error)*error=key+" must be boolean"; return false;
            }
            o[it.key()]=(t=="true"||t=="1");
        } else if(it.value().isDouble()) {
            bool ok=false; const double d=v.toString().toDouble(&ok);
            if(!ok||!std::isfinite(d)) {if(error)*error=key+" must be finite numeric";return false;}
            o[it.key()]=d;
        } else o[it.key()]=v.toString();
    }
    return fromJson(o,fs,samples,out,error,depthKnown);
}
void saveSettings(QSettings& s,const FilteredDasConfig& c) {
    const auto o=toJson(c);
    for(auto it=o.begin();it!=o.end();++it) s.setValue(iniKey(it.key()),it.value().toVariant());
}
}
