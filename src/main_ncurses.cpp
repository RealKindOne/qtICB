// main_ncurses.cpp - entry point for the ncurses (console) build of qtICB.
//
// Build with:  qmake CONFIG+=console && make
// Run with:    qtICB-console -s irc.example.com -n mynick -g icb

#include <signal.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include "icbsession.h"
#include "ncursesui.h"

// SIGINT handler - intentionally empty.
//
// Without this, Ctrl-C would raise the default SIGINT action and kill the
// process immediately, leaving the terminal in raw/ncurses mode (no echo,
// cursor invisible, etc.). By installing a no-op handler we let the Qt event
// loop keep running so NcursesUI::stop() can restore the terminal before exit.
// The user can still quit cleanly via the "/quit" command or 'q' keybinding.
static void sigintHandler(int) {
    // Intentionally empty - see comment above.
}

int main(int argc, char* argv[]) {
    // QCoreApplication provides the Qt event loop and meta-object system.
    // We do NOT use QApplication here because ncurses owns the terminal and
    // a platform GUI plugin would conflict with it.
    QCoreApplication app(argc, argv);
    app.setApplicationName("qtICB-console");

    // Install the no-op SIGINT handler before ncurses takes over the terminal.
    signal(SIGINT, sigintHandler);

    // Parse command-line arguments.
    // Defaults match the traditional ICB well-known server and port.
    QCommandLineParser parser;
    parser.setApplicationDescription("ICB console client");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption({"s", "server"}, "Server address", "host",  "default.icb.net"));
    parser.addOption(QCommandLineOption({"p", "port"},   "Port",           "port",  "7326"));
    parser.addOption(QCommandLineOption({"n", "nick"},   "Nickname",       "nick",  "guest"));
    parser.addOption(QCommandLineOption({"g", "group"},  "Initial group",  "group", "icb"));
    parser.process(app);

    QString host  = parser.value("server");
    quint16 port  = parser.value("port").toUShort();
    QString nick  = parser.value("nick");
    QString group = parser.value("group");

    // Initialize ncurses and start the QSocketNotifier that bridges stdin
    // key events into the Qt event loop.
    NcursesUI ui;
    ui.start();

    // Create the session with the application directory as the log base path.
    ICBSession* session = new ICBSession(QCoreApplication::applicationDirPath(), &ui);

    // Defer addSession + connectToServer until the event loop is running so
    // that any signals emitted during connection setup are delivered normally.
    // QueuedConnection ensures the lambda runs on the next event loop iteration
    // rather than synchronously inside main().
    QMetaObject::invokeMethod(
        &ui,
        [&ui, session, host, port, nick, group]() {
            ui.addSession(session);
            session->connectToServer(host, port, nick, group);
        },
        Qt::QueuedConnection);

    return app.exec();
}
