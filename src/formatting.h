#ifndef FORMATTING_H
#define FORMATTING_H

#include <QDateTime>
#include <QString>

// Returns the current wall-clock time as "hh:mm:ss".
// Used to prefix every chat and system message line with a timestamp in both
// ChatDisplay (Qt) and NcursesUI (ncurses).
inline QString currentTimestamp() {
    return QDateTime::currentDateTime().toString("hh:mm:ss");
}

#endif  // FORMATTING_H
