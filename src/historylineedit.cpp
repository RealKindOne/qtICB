#include "historylineedit.h"
#include <QEvent>
#include <QKeyEvent>
#include <QRegularExpression>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HistoryLineEdit::HistoryLineEdit(QWidget* parent)
    : QLineEdit(parent),
      m_historyIndex(-1),
      m_completionIndex(0),
      m_inCompletion(false) {

    // On Enter: record history, emit sendRequested, clear field, reset state.
    connect(this, &QLineEdit::returnPressed, this, [this]() {
        QString input = text().trimmed();
        if (!input.isEmpty()) {
            if (m_history.isEmpty() || m_history.last() != input)
                m_history.append(input);
            if (m_history.size() > 100)
                m_history.removeFirst();
            emit sendRequested(input);
        }
        clear();
        m_historyIndex = -1;
        m_savedInput.clear();
        resetCompletion();
    });
}

// ---------------------------------------------------------------------------
// Nick list
// ---------------------------------------------------------------------------

// Replaces the completion candidate pool and resets any in-progress cycle.
// Called by ConnectionWidget whenever UserList::listChanged fires.
void HistoryLineEdit::setUserList(const QStringList& users) {
    m_userList = users;
    resetCompletion();
}

// ---------------------------------------------------------------------------
// Event override — Tab interception
// ---------------------------------------------------------------------------

// Qt handles Tab as a focus-traversal key at the QWidget level, before
// keyPressEvent() is called.  The only reliable way to consume it is to
// override event(), check for Tab there, and return true to mark it handled.
bool HistoryLineEdit::event(QEvent* e) {
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Tab) {
            tryComplete();
            return true;  // event consumed — focus does not move
        }
    }
    return QLineEdit::event(e);
}

// ---------------------------------------------------------------------------
// Key event handler
// ---------------------------------------------------------------------------

// Tab is intercepted in event() above and never reaches here.
// Up/Down --> history navigation; any other key --> reset completion, then QLineEdit.
void HistoryLineEdit::keyPressEvent(QKeyEvent* event) {

    // Any non-Tab key breaks the completion cycle so the next Tab starts fresh.
    resetCompletion();

    if (event->key() == Qt::Key_Up) {
        if (m_history.isEmpty()) return;
        if (m_historyIndex == -1) {
            m_savedInput   = text();
            m_historyIndex = m_history.size() - 1;
        } else if (m_historyIndex > 0) {
            --m_historyIndex;
        }
        setText(m_history.at(m_historyIndex));
        end(false);
        return;
    }

    if (event->key() == Qt::Key_Down) {
        if (m_historyIndex == -1) return;
        if (m_historyIndex < m_history.size() - 1) {
            ++m_historyIndex;
            setText(m_history.at(m_historyIndex));
        } else {
            m_historyIndex = -1;
            setText(m_savedInput);
            m_savedInput.clear();
        }
        end(false);
        return;
    }

    QLineEdit::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// Tab-completion  (mirrors NcursesUI::tryComplete exactly)
// ---------------------------------------------------------------------------

// Each Tab press performs one step of an inline cycling completion:
//
//   First Tab  (m_inCompletion == false):
//     1. Walk left from the cursor over word characters to find the prefix.
//     2. Filter the user list for nicks that start with the prefix
//        (case-insensitive; leading '@' stripped from moderator entries).
//     3. If no matches, do nothing.
//     4. Set m_inCompletion = true, m_completionIndex = 0, fall through to insert.
//
//   Subsequent Tabs  (m_inCompletion == true):
//     Replace the previously inserted nick with the next candidate, wrapping
//     around at the end of the list.
//
// Replacement: remove (removeLen) chars before the cursor, insert the candidate,
// advance the cursor to the end of the new text.
//
//   removeLen = m_completionPrefix.length()       on the first Tab (replace typed prefix)
//             = m_completions[index-1].length()   on cycle Tabs   (replace last insertion)
void HistoryLineEdit::tryComplete() {
    if (m_userList.isEmpty()) return;

    // Strip '@' prefix from moderator entries so "al" completes "alice" whether
    // or not she is currently moderator.
    QStringList users;
    users.reserve(m_userList.size());
    for (const QString& u : m_userList)
        users.append(u.startsWith('@') ? u.mid(1) : u);

    if (!m_inCompletion) {
        // First Tab: find the word under the cursor
        int cursorPos = cursorPosition();
        int pos = cursorPos;
        while (pos > 0 && text()[pos - 1].isLetterOrNumber())
            --pos;

        m_completionPrefix = text().mid(pos, cursorPos - pos);
        if (m_completionPrefix.isEmpty()) return;

        m_completions = users.filter(
            QRegularExpression("^" + QRegularExpression::escape(m_completionPrefix),
                               QRegularExpression::CaseInsensitiveOption));
        if (m_completions.isEmpty()) return;

        m_completionIndex = 0;
        m_inCompletion    = true;
        m_lastInserted    = m_completionPrefix;  // first removal replaces the typed prefix
    }

    // Replace previous text with the next candidate
    const QString& candidate = m_completions[m_completionIndex];
    int cursorPos = cursorPosition();

    // How many characters before the cursor do we own and need to replace?
    // m_lastInserted tracks exactly what we put in the buffer last time:
    //   - Before the first insertion it equals m_completionPrefix (the typed text).
    //   - After each insertion it is updated to the candidate we just placed.
    // Using m_completionIndex == 0 for this check is wrong because that condition
    // is also true on wrap-around, where a full nick is already in the buffer.
    int removeLen = static_cast<int>(m_lastInserted.length());

    int start = qMax(0, cursorPos - removeLen);

    QString buf = text();
    buf.remove(start, removeLen);
    buf.insert(start, candidate);
    setText(buf);
    setCursorPosition(start + candidate.length());

    m_lastInserted    = candidate;
    m_completionIndex = (m_completionIndex + 1) % m_completions.size();
}

// ---------------------------------------------------------------------------
// Completion reset
// ---------------------------------------------------------------------------

void HistoryLineEdit::resetCompletion() {
    m_inCompletion    = false;
    m_completionIndex = 0;
    m_completionPrefix.clear();
    m_completions.clear();
    m_lastInserted.clear();
}
