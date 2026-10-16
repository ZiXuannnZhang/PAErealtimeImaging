#include "RingTimevarLowpassSettings.h"
#include <cmath>
#include <limits>

namespace ring_timevar {
QJsonObject toJson(const RingTimevarLowpassConfig& c) {
    return {{"enabled",c.enabled},{"fcMaxMhz",c.fcMaxMhz},{"fcMinMhz",c.fcMinMhz},
        {"p1",c.p1},{"p2",c.p2},{"windowLength",c.windowLength},
        {"overlapRatio",c.overlapRatio},{"fftLength",c.fftLength},
        {"filterOrder",c.filterOrder},{"transition",QString::fromStdString(c.transition)}};
}
bool fromJson(const QJsonValue& v, double fs, int samples, RingTimevarLowpassConfig& out,
              QString* error, bool depthKnown) {
    auto fail=[&](const QString& e){if(error)*error=e; return false;};
    RingTimevarLowpassConfig c;
    if (!v.isUndefined() && !v.isObject()) return fail("timevarLowpass must be an object");
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
    if(!number("fcMaxMhz",c.fcMaxMhz)||!number("fcMinMhz",c.fcMinMhz)||
       !integer("p1",c.p1)||!integer("p2",c.p2)||!integer("windowLength",c.windowLength)||
       !number("overlapRatio",c.overlapRatio)||!integer("fftLength",c.fftLength)||
       !number("filterOrder",c.filterOrder)) return fail("invalid numeric field in timevarLowpass");
    if(o.contains("transition")) {
        if(!o["transition"].isString()) return fail("transition must be string");
        c.transition=o["transition"].toString().toStdString();
    }
    std::string e;
    if(!validate(c,fs,samples,&e,depthKnown)) return fail(QString::fromStdString(e));
    out=c; return true;
}

static QString iniKey(QString key) {
    key[0]=key[0].toUpper(); return "timevarLp"+key;
}
bool loadSettings(QSettings& s,double fs,int samples,RingTimevarLowpassConfig& out,QString* error,bool depthKnown) {
    QJsonObject o;
    const auto defaults=toJson(RingTimevarLowpassConfig{});
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
void saveSettings(QSettings& s,const RingTimevarLowpassConfig& c) {
    const auto o=toJson(c);
    for(auto it=o.begin();it!=o.end();++it) s.setValue(iniKey(it.key()),it.value().toVariant());
    for(const char* key:{"enhanceBipolar","enhanceFreq","enhanceFreqFcMhz","enhanceFreqHmax","enhanceFreqOrder"}) s.remove(key);
}
}
