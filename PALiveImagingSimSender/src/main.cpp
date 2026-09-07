#include <QApplication>

#include "SimSenderWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    SimSenderWindow w;
    w.show();
    return app.exec();
}
