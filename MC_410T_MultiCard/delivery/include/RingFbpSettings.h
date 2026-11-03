#pragma once
#include "ring_fbp.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QJsonDocument>
#include <QString>
#include <cmath>
#include <algorithm>

struct RingFbpSettings {
    bool enabled=false, median=true, invert=true;
    double densities[8]={1000,1100,1100,1100,1100,1100,1100,1100};
    double angleOffset[8]={};
    double delayOffset[8][2]={}; // positive measured lateness, corrected before K
    QString calibrationFile;
    int angularSamples=2049;
    QJsonObject artifact; // optional per-channel/per-wavelength timing templates
};
namespace ring_fbp_settings {
inline QJsonObject toJson(const RingFbpSettings& s){
    QJsonArray density,angles,delays;for(int c=0;c<8;++c){density.append(s.densities[c]);angles.append(s.angleOffset[c]);delays.append(QJsonArray{s.delayOffset[c][0],s.delayOffset[c][1]});}
    return {{"enabled",s.enabled},{"median",s.median},{"invert",s.invert},{"densities",density},
        {"angleOffsetDeg",angles},{"delayOffsetSamples",delays},{"angularSamples",s.angularSamples},{"calibrationFile",s.calibrationFile},{"artifact",s.artifact}};
}
inline bool fromJson(const QJsonObject& o,RingFbpSettings& s,QString* e){
    auto fail=[&](const QString& t){if(e)*e=t;return false;};
    for(const char* k:{"enabled","median","invert"})if(o.contains(k)&&!o[k].isBool())return fail(QString("FBP %1 must be boolean").arg(k));
    if(o.contains("artifact")&&!o["artifact"].isObject())return fail("FBP artifact must be an object");
    if(o.contains("angularSamples")&&(!o["angularSamples"].isDouble()||o["angularSamples"].toDouble()!=o["angularSamples"].toInt()))return fail("FBP angularSamples must be integer");
    s=RingFbpSettings{};s.enabled=o.value("enabled").toBool(false);s.median=o.value("median").toBool(true);s.invert=o.value("invert").toBool(true);
    s.angularSamples=o.value("angularSamples").toInt(2049);s.calibrationFile=o.value("calibrationFile").toString();
    s.artifact=o.value("artifact").toObject();
    auto d=o.value("densities").toArray(),a=o.value("angleOffsetDeg").toArray(),t=o.value("delayOffsetSamples").toArray();
    if((o.contains("densities")&&d.size()!=8)||(o.contains("angleOffsetDeg")&&a.size()!=8)||(o.contains("delayOffsetSamples")&&t.size()!=8))return fail("FBP arrays require 8 entries");
    for(int c=0;c<8;++c){if(!d.isEmpty())s.densities[c]=d[c].toDouble(-1);if(!a.isEmpty())s.angleOffset[c]=a[c].toDouble(1e9);
        if(!t.isEmpty()){if(t[c].toArray().size()!=2)return fail("FBP delay requires 8 x 2 values");for(int w=0;w<2;++w)s.delayOffset[c][w]=t[c].toArray()[w].toDouble(1e9);}
        if(!std::isfinite(s.densities[c])||s.densities[c]<=0||!std::isfinite(s.angleOffset[c])||fabs(s.angleOffset[c])>360)return fail("Invalid FBP density or angle offset");
        for(int w=0;w<2;++w)if(!std::isfinite(s.delayOffset[c][w])||fabs(s.delayOffset[c][w])>32)return fail("FBP fractional delay must be within +/-32 samples");
    }
    return s.angularSamples>=65&&s.angularSamples<=8193?true:fail("FBP angular table must contain 65..8193 entries");
}
inline bool validate(const RingFbpSettings&s,const RingReconCudaConfig& c,const int delays[8][2],QString*e){
    RingFbpSettings parsed;if(!fromJson(toJson(s),parsed,e))return false;
    if(!s.enabled)return true;
    auto fail=[&](const char*t){if(e)*e=QString::fromUtf8(t);return false;};
    if(!s.artifact.isEmpty()){
        int start=s.artifact.value("startSample").toInt(),count=s.artifact.value("prefixPerWavelength").toInt();auto templates=s.artifact.value("templates").toArray();
        if(start<1||count<8||count>256||templates.size()!=8)return fail("FBP artifact templates require startSample>=1, prefixPerWavelength=8..256 and 8 x 2 templates");
        for(int ch=0;ch<8;++ch)if(c.enabledChannels[ch]){if(templates[ch].toArray().size()!=2)return fail("FBP templates require two wavelengths per channel");for(int w=0;w<2;++w){auto t=templates[ch].toArray()[w].toArray();
            if(t.size()<32||t.size()>1000||start-1+t.size()>c.sampDepth)return fail("FBP artifact template window outside acquired samples");
            for(auto v:t)if(!v.isDouble()||!std::isfinite(v.toDouble()))return fail("Nonfinite FBP artifact template");
        }}
        if(size_t(c.sampDepth)*c.enabledChannelCount*(2*count+c.alinesPerChannelPerBlock)*sizeof(float)>256*1024*1024)return fail("FBP timing prefix exceeds 256 MiB");
    }
    if(c.soundSpeedRadiiCount<0||c.soundSpeedRadiiCount>7||c.soundSpeedsCount<c.soundSpeedRadiiCount+1)return fail("FBP requires one speed/density per concentric layer");
    for(int i=0;i<=c.soundSpeedRadiiCount;++i){
        if(!std::isfinite(c.soundSpeeds[i])||c.soundSpeeds[i]<=0)return fail("Invalid FBP sound speed");
        if(i<c.soundSpeedRadiiCount&&(!std::isfinite(c.soundSpeedRadii[i])||c.soundSpeedRadii[i]<=0||(i&&c.soundSpeedRadii[i]<=c.soundSpeedRadii[i-1])))return fail("FBP layer radii must be positive and increasing");
    }
    if(c.alinesPerChannelPerFrame<1||c.enabledChannelCount<1||!std::isfinite(c.daqHz)||c.daqHz<=0)return fail("Invalid FBP trajectory");
    if(!std::isfinite(c.fov)||!std::isfinite(c.gridSize)||c.gridSize<=0||c.fov<=c.gridSize||ceil(c.fov/c.gridSize)>8192)return fail("FBP grid dimension must be 2..8192");
    for(int ch=0;ch<8;++ch)if(c.enabledChannels[ch]){
        if(!std::isfinite(c.radiusPerChannel[ch])||c.radiusPerChannel[ch]<=0)return fail("Invalid FBP detector radius");
        for(int w=0;w<2;++w){int nt=c.sampDepth-(c.delayCut?delays[ch][w]-1:0);
            if((c.delayCut&&(delays[ch][w]<1||delays[ch][w]>c.sampDepth))||nt<128||nt>8192)return fail("FBP effective samples must be 128..8192 for every channel/wavelength");}
    }return true;
}
inline RingFbpConfig native(const RingFbpSettings&s,const RingReconCudaConfig&c){
    RingFbpConfig f{};f.size=sizeof(f);f.version=1;std::copy(s.densities,s.densities+8,f.densities);
    f.angularStepRad=2*3.14159265358979323846/(c.enabledChannelCount*double(c.alinesPerChannelPerFrame));f.maxNt=std::min(c.sampDepth,8192);f.tableAngularSamples=s.angularSamples;return f;
}
// Calibration is an immutable snapshot read at configuration time, never during a round.
inline bool loadCalibration(RingFbpSettings& s,double fs,QString* e){
    if(!s.enabled||s.calibrationFile.isEmpty())return true;
    QFile f(s.calibrationFile);if(!f.open(QIODevice::ReadOnly)){if(e)*e="Cannot open FBP calibration: "+f.errorString();return false;}
    QJsonParseError pe;auto doc=QJsonDocument::fromJson(f.readAll(),&pe);auto o=doc.object();
    if(pe.error!=QJsonParseError::NoError||o.value("formatVersion").toInt()!=1||o.value("geometry").toString()!="concentric-ring"||o.value("daqHz").toDouble()!=fs){if(e)*e="FBP calibration requires version 1, concentric-ring geometry and matching daqHz";return false;}
    auto merged=toJson(s);for(const auto* k:{"angleOffsetDeg","delayOffsetSamples"}){if(!o.contains(k)){if(e)*e="Incomplete FBP calibration";return false;}merged[k]=o[k];}
    if(o.contains("artifact"))merged["artifact"]=o["artifact"];
    return fromJson(merged,s,e);
}
}
