#pragma once
#include "FilteredDasConfig.h"
#include <QJsonValue>
#include <QJsonObject>
#include <QSettings>
namespace filtered_das {
QJsonObject toJson(const FilteredDasConfig&);
// Missing values use factory fields; invalid present values reject the whole snapshot.
bool fromJson(const QJsonValue&, double fs, int samples, FilteredDasConfig&,
              QString* error = nullptr, bool depthKnown = true);
bool loadSettings(QSettings&, double fs, int samples, FilteredDasConfig&,
                  QString* error = nullptr, bool depthKnown = true);
void saveSettings(QSettings&, const FilteredDasConfig&);
}
