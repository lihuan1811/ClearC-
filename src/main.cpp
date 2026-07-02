#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("C DiskGlow"));
    QApplication::setOrganizationName(QStringLiteral("ClearC"));

    MainWindow window;
    window.show();
    return app.exec();
}
