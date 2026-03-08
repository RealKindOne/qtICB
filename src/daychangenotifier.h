#ifndef DAYCHANGENOTIFIER_H
#define DAYCHANGENOTIFIER_H

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QObject>
#include <QTimer>

// DayChangeNotifier emits a signal once per calendar day, exactly at midnight,
// so the UI can insert a visible date-change banner into the chat log.
//
// Connections in practice:
//   ICBSession owns one DayChangeNotifier and connects its dayChanged signal
//   to both the Qt GUI (MainWindow inserts a line into the active ChatDisplay)
//   and the ncurses UI (NcursesUI appends a line to the active buffer).
class DayChangeNotifier : public QObject {
    Q_OBJECT

  public:
    explicit DayChangeNotifier(QObject* parent = nullptr);

  signals:
    // Emitted once each time the calendar date advances.
    //   message - human-readable string "Day changed to 08 Mar 2026"
    //   newDate - the new calendar date, for any logic that needs the raw value
    void dayChanged(const QString& message, const QDate& newDate);

  private slots:
    // Called when m_timer fires.  Checks for a date change, emits dayChanged
    // if the date has advanced, then reschedules for the next midnight.
    void onTimerTimeout();

  private:
    // Compute milliseconds until the next midnight and (re)start m_timer.
    void scheduleNextCheck();

    QTimer* m_timer;        // single-shot timer, rescheduled after each firing
    QDate   m_currentDate;  // the date at last check; updated on each real day change
};

#endif  // DAYCHANGENOTIFIER_H
