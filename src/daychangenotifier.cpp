#include <QDebug>
#include "daychangenotifier.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// Record today's date and schedule the first timer wakeup at midnight.
DayChangeNotifier::DayChangeNotifier(QObject* parent)
    : QObject(parent), m_currentDate(QDate::currentDate()) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);  // rescheduled manually after each firing
    connect(m_timer, &QTimer::timeout, this, &DayChangeNotifier::onTimerTimeout);
    scheduleNextCheck();
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------

// Called at midnight.
//
// We re-read the current date rather than assuming the timer fired precisely
// on the day boundary - OS scheduling jitter and system clock adjustments can
// cause the timer to fire a few milliseconds early.  If the date has not yet
// changed we simply reschedule and wait again; this costs at most one extra
// wakeup per midnight and keeps the implementation correct across DST changes
// and leap seconds.
void DayChangeNotifier::onTimerTimeout() {
    QDate newDate = QDate::currentDate();

    if (newDate != m_currentDate) {
        m_currentDate = newDate;

        // Format a locale-aware string "Day changed to 08 Mar 2026".
        QString message = QString("Day changed to %1")
            .arg(QLocale::system().toString(newDate, "dd MMM yyyy"));

        emit dayChanged(message, newDate);
    }

    // Always reschedule - whether or not the date changed - so the next
    // midnight wakeup is correctly computed from the current moment.
    scheduleNextCheck();
}

// ---------------------------------------------------------------------------
// Scheduling helper
// ---------------------------------------------------------------------------

// Calculate the exact number of milliseconds until 00:00:00 tomorrow and
// start the single-shot timer for that duration.
//
// Using QDateTime arithmetic rather than a fixed 86400-second interval means:
//   - A 23-hour day (spring-forward DST) fires at the right wall time.
//   - A 25-hour day (fall-back DST) also fires at the right wall time.
//   - If the system clock is adjusted forward past midnight while we sleep,
//     the timer fires immediately on the next Qt event loop iteration
//     (msecsToMidnight will be ≤ 0, clamped to 0 by QTimer).
void DayChangeNotifier::scheduleNextCheck() {
    QDateTime now         = QDateTime::currentDateTime();
    QDateTime nextMidnight = QDateTime(now.date().addDays(1), QTime(0, 0, 0));
    qint64 msecsToMidnight = now.msecsTo(nextMidnight);

    m_timer->start(static_cast<int>(msecsToMidnight));

#ifdef QT_DEBUG
    qDebug() << "DayChangeNotifier: next check in"
             << (msecsToMidnight / 1000 / 60) << "minutes";
#endif
}
