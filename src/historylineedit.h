#ifndef HISTORYLINEEDIT_H
#define HISTORYLINEEDIT_H

#include <QLineEdit>
#include <QStringList>

// HistoryLineEdit is a single-line text input for the Qt GUI chat tabs.
//
// It extends QLineEdit with two features:
//
//   1. Command / message history
//      Up and Down arrow keys cycle through previously sent lines.  The
//      current in-progress line is saved when history browsing starts and
//      restored when the user presses Down past the most recent entry.
//      History is capped at 100 entries; consecutive duplicates are not added.
//
//   2. Inline cycling nick tab-completion that matches ncurses behavior
//      Pressing Tab scans backwards from the cursor to find the current word,
//      then cycles through all nicks that start with that prefix on successive
//      Tab presses.  Any other key resets the cycle so the next Tab starts
//      fresh.  No popup is shown — the completion is applied directly in the
//      input field, just like in the ncurses UI.
//
// When the user presses Enter, sendRequested(text) is emitted and the field
// is cleared.  The parent widget (ConnectionWidget) connects to this signal
// to route the text to ICBSession.
class HistoryLineEdit : public QLineEdit {
    Q_OBJECT

  public:
    explicit HistoryLineEdit(QWidget* parent = nullptr);

    // Replaces the nick list used for tab-completion.
    // Called by ConnectionWidget whenever UserList::listChanged fires.
    void setUserList(const QStringList& users);

  signals:
    // Emitted when the user presses Enter with non-empty text.
    // The field is cleared and the history index is reset before this fires.
    void sendRequested(const QString& text);

  protected:
    // event() is overridden specifically to intercept Tab before Qt's focus-chain
    // machinery claims it.  Qt processes Tab as a focus-change key at the QWidget
    // level, before keyPressEvent() is ever called, so overriding keyPressEvent()
    // alone is not sufficient to suppress it.  We intercept QKeyEvent here, handle
    // Tab ourselves, and return true (consumed).  All other events are forwarded to
    // the base class unchanged.
    bool event(QEvent* event) override;

    // Handles Up/Down (history) and all other non-Tab keys (which reset the
    // completion cycle before being passed to QLineEdit).
    // Tab is handled in event() above and never reaches this function.
    void keyPressEvent(QKeyEvent* event) override;

  private:
    // History state
    QStringList m_history;      // previously sent lines, oldest first
    int         m_historyIndex; // index into m_history while browsing, or -1
    QString     m_savedInput;   // in-progress text saved before browsing starts

    // Tab-completion state
    QStringList m_userList;          // current nick list, updated by setUserList()
    QString     m_completionPrefix;  // word prefix that was typed before the first Tab
    QString     m_lastInserted;      // text placed in the buffer by the most recent Tab
    QStringList m_completions;       // nicks that start with m_completionPrefix
    int         m_completionIndex;   // which candidate to insert on the next Tab
    bool        m_inCompletion;      // true while cycling; reset by any non-Tab key

    // Perform one step of tab-completion against the current cursor position.
    void tryComplete();

    // Reset completion state.  Called whenever a key other than Tab is pressed.
    void resetCompletion();
};

#endif  // HISTORYLINEEDIT_H
