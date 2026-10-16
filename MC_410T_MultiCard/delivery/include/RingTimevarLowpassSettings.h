#pragma once
#include "RingTimevarLowpassConfig.h"
#include <QJsonValue>
#include <QJsonObject>
#include <QSettings>
namespace ring_timevar {
QJsonObject toJson(const RingTimevarLowpassConfig&);
// Missing values use factory fields; invalid present values reject the whole snapshot.
bool fromJson(const QJsonValue&, double fs, int samples, RingTimevarLowpassConfig&,
              QString* error = nullptr, bool depthKnown = true);
bool loadSettings(QSettings&, double fs, int samples, RingTimevarLowpassConfig&,
                  QString* error = nullptr, bool depthKnown = true);
void saveSettings(QSettings&, const RingTimevarLowpassConfig&);
}
