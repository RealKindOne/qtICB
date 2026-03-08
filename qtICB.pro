QT = core network
CONFIG += c++11
TEMPLATE = app

# Common headers and sources
HEADERS = \
    src/commandhandler.h \
    src/daychangenotifier.h \
    src/formatting.h \
    src/icbclient.h \
    src/icbsession.h \
    src/logger.h \
    src/privatechat.h \
    src/userlist.h

SOURCES = \
    src/commandhandler.cpp \
    src/daychangenotifier.cpp \
    src/icbclient.cpp \
    src/icbsession.cpp \
    src/logger.cpp \
    src/privatechat.cpp \
    src/userlist.cpp

# Default to GUI if no configuration specified
!gui:!console: CONFIG += gui

# GUI version
gui {
    TARGET = qtICB
    QT += widgets
    HEADERS += \
        src/color.h \
        src/mainwindow.h \
        src/chatdisplay.h \
        src/connectionwidget.h \
        src/historylineedit.h
    SOURCES += \
        src/main.cpp \
        src/mainwindow.cpp \
        src/chatdisplay.cpp \
        src/connectionwidget.cpp \
        src/historylineedit.cpp
    FORMS += \
        src/mainwindow.ui
}

# Console (ncurses) version
console {
    TARGET = qtICB-console
    # QT remains core + network (no widgets)
    HEADERS += src/ncursesui.h
    SOURCES += src/main_ncurses.cpp src/ncursesui.cpp
    LIBS += -lncursesw
}