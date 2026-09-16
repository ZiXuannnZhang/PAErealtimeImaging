#include "NetworkDiagnostics.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto snapshot = NetworkDiagnostics::collectSnapshot({"127.0.0.1"});
    const auto relations = snapshot.value("targetRelations").toArray();
    if (relations.size() != 1 || !relations.first().toObject().value("localAddress").toBool()) return 1;
    if (snapshot.value("interfacesStatus").toString() != "ok") return 2;
    if (snapshot.value("routesStatus").toString() != "ok") return 3;
    QTextStream(stdout) << "PASS read-only adapter/route snapshot and loopback relation" << Qt::endl;
    return 0;
}
