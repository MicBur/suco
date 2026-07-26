#include <QApplication>
#include <QIcon>
#include "main_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("SUCO Control Center");
    app.setOrganizationName("SUCO");
    app.setQuitOnLastWindowClosed(false);

    QIcon appIcon("resources/app_icon.ico");
    if (appIcon.isNull()) appIcon = QIcon("app_icon.ico");
    app.setWindowIcon(appIcon);

    MainWindow window;
    window.setWindowIcon(appIcon);
    window.show();

    return app.exec();
}
