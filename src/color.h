#ifndef CHATCOULORS_H
#define CHATCOULORS_H

// color.h - centralized color constants for the Qt GUI chat display.
//
// All message-coloring decisions in ConnectionWidget, PrivateChat, and
// ChatDisplay should reference these macros rather than hard-coding QColor
// or Qt::GlobalColor values inline.  This makes it easy to retheme the
// application by changing values in one place.
//
// The file is guarded by QT_GUI_LIB so it can be included from translation
// units that are compiled for both the GUI and ncurses targets - the macros
// simply won't expand to anything usable in the ncurses build, but the
// include won't cause a compile error either.
//
// Naming convention: COLOR_QT_<NAME>
//   - Names ending in a plain color word (BLACK, GRAY, …) are direct
//     Qt::GlobalColor aliases.
//   - Names containing DARK/ORANGE etc. are custom QColor(r,g,b) values
//     because Qt's built-in palette doesn't include them.

#ifdef QT_GUI_LIB
#include <QColor>
#endif

// clang-format off

// Qt::GlobalColor aliases
#define COLOR_QT_BLACK      Qt::black
#define COLOR_QT_GRAY       Qt::gray
#define COLOR_QT_RED        Qt::red
#define COLOR_QT_GREEN      Qt::green
#define COLOR_QT_BLUE       Qt::blue
#define COLOR_QT_DARKGREEN  Qt::darkGreen

// Qt::darkCyan is the closest built-in to a classic IRC teal.
#define COLOR_QT_TEAL       Qt::darkCyan

// Custom QColor values (no Qt::GlobalColor equivalent)
#define COLOR_QT_PURPLE     QColor(128, 0, 128)   // incoming private messages
#define COLOR_QT_ORANGE     QColor(255, 165, 0)   // general highlights
#define COLOR_QT_DARKORANGE QColor(255, 140, 0)   // stronger highlight variant

// clang-format on

#endif  // CHATCOLORS_H
