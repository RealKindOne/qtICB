// main.cpp - entry point for the Qt GUI (widget) build of qtICB.
//
// Build note: after compiling on Windows, run
//   windeployqt qtICB.exe
// to copy the required Qt DLLs and platform plugins alongside the binary so
// it can be distributed without a Qt installation.

#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    // QApplication must be constructed before any QWidget.
    // It initializes the platform plugin, event loop, and font/palette systems.
    QApplication app(argc, argv);
    app.setApplicationName("qtICB");
    app.setOrganizationName("github.com/RealKindOne");

    // MainWindow creates the tab bar and the DayChangeNotifier; it is the
    // root of the entire widget hierarchy.
    MainWindow window;
    window.show();

    // Hand control to the Qt event loop.  Returns when the last window closes.
    return app.exec();
}
