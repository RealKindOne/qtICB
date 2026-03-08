#include "privatechat.h"
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include "color.h"

// ===========================================================================
// PrivateChatLogic  (shared, no UI dependencies)
// ===========================================================================

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PrivateChatLogic::PrivateChatLogic(ICBSession* session, const QString& otherNick,
                                   QObject* parent)
    : QObject(parent), m_session(session), m_otherNick(otherNick) {
    if (m_session) {
        // Route incoming personal messages to our filter slot.
        connect(m_session, &ICBSession::personalMessageReceived,
                this, &PrivateChatLogic::onPersonalMessageReceived);

        // Route our own sent private messages so the UI can echo them.
        connect(m_session, &ICBSession::selfPrivateMessageSent,
                this, &PrivateChatLogic::onSelfPrivateMessageSent);

        // Watch for events we want to report inside the chat window.
        connect(m_session, &ICBSession::userLeft,
                this, &PrivateChatLogic::onUserLeft);
        connect(m_session, &ICBSession::userBooted,
                this, &PrivateChatLogic::onUserBooted);
        connect(m_session, &ICBSession::userChangedNick,
                this, &PrivateChatLogic::onUserChangedNick);

        // Clear our session pointer if the session is torn down before us.
        connect(m_session, &QObject::destroyed,
                this, &PrivateChatLogic::onConnectionDestroyed);
    }
}

PrivateChatLogic::~PrivateChatLogic() = default;

// ---------------------------------------------------------------------------
// Message sending
// ---------------------------------------------------------------------------

// Route text entered by the user.  Slash-commands are passed to
// ICBSession::processUserInput (so /nick, /topic, etc. still work from inside
// a private chat window).  Plain text is sent as a private message.
void PrivateChatLogic::sendMessage(const QString& text) {
    if (!m_session) return;
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;

    if (trimmed.startsWith('/')) {
        m_session->processUserInput(trimmed);
    } else {
        m_session->sendPrivateMessage(m_otherNick, trimmed);
    }
}

// ---------------------------------------------------------------------------
// ICBSession signal handlers
// ---------------------------------------------------------------------------

// Filter incoming personal messages to those from the nick we're chatting with.
// All other personal messages (for different open private chats) are ignored here;
// the appropriate PrivateChatLogic instance for the other nick handles them.
void PrivateChatLogic::onPersonalMessageReceived(const QString& from, const QString& message) {
    if (from.compare(m_otherNick, Qt::CaseInsensitive) == 0)
        emit incomingMessage(from, message);
}

// Filter outgoing private messages to the nick we're chatting with.
void PrivateChatLogic::onSelfPrivateMessageSent(const QString& to, const QString& message) {
    if (to.compare(m_otherNick, Qt::CaseInsensitive) == 0)
        emit outgoingMessage(to, message);
}

// The ICBSession was destroyed while this logic object is still alive.
// Disconnect all signals and null the pointer so we don't access freed memory.
void PrivateChatLogic::onConnectionDestroyed() {
    disconnect(m_session, nullptr, this, nullptr);
    m_session = nullptr;
}

// The other user left the group - emit a status message for display.
void PrivateChatLogic::onUserLeft(const QString& nick, const QString& reason) {
    if (nick.compare(m_otherNick, Qt::CaseInsensitive) == 0) {
        QString msg = reason.isEmpty()
            ? QString("%1 has left.").arg(nick)
            : QString("%1 has left (%2).").arg(nick, reason);
        emit systemMessage(msg);
    }
}

// The other user was booted - emit a status message for display.
void PrivateChatLogic::onUserBooted(const QString& nick, const QString& by) {
    if (nick.compare(m_otherNick, Qt::CaseInsensitive) == 0) {
        QString msg = by.isEmpty()
            ? QString("%1 was booted.").arg(nick)
            : QString("%1 was booted by %2.").arg(nick, by);
        emit systemMessage(msg);
    }
}

// The other user changed their nick.  Update our stored nick so subsequent
// messages are still matched correctly, then notify the UI.
void PrivateChatLogic::onUserChangedNick(const QString& oldNick, const QString& newNick) {
    if (oldNick.compare(m_otherNick, Qt::CaseInsensitive) == 0) {
        QString old = m_otherNick;
        m_otherNick = newNick;
        emit otherNickChanged(old, newNick);
        emit systemMessage(QString("%1 is now known as %2.").arg(oldNick, newNick));
    }
}

// ===========================================================================
// PrivateChat  (Qt GUI widget - compiled only when QT_GUI_LIB is defined)
// ===========================================================================

#ifdef QT_GUI_LIB
#include <QKeyEvent>
#include <QLineEdit>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PrivateChat::PrivateChat(ICBSession* session, const QString& otherNick, QWidget* parent)
    : QWidget(parent) {
    // Create the shared logic object, which handles all ICB protocol
    // interactions and owns the nick-matching state.
    m_logic = new PrivateChatLogic(session, otherNick, this);

    // Chat history display
    // Read-only; an event filter redirects mouse clicks to the input line.
    m_display = new ChatDisplay(this);
    m_display->setReadOnly(true);
    m_display->setFont(QFont("FixedSys", 9));
    m_display->installEventFilter(this);

    // Message input
    // HistoryLineEdit provides Up/Down command history.
    m_input = new HistoryLineEdit(this);
    m_input->installEventFilter(this);

    // Stack: display (stretches) above input.
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(m_display);
    layout->addWidget(m_input);
    setLayout(layout);

    // Wire up Enter in the input line.
    connect(m_input, &HistoryLineEdit::sendRequested,
            this, &PrivateChat::onSendMessage);

    // Reflect logic events in the display.
    connect(m_logic, &PrivateChatLogic::incomingMessage,
            this, &PrivateChat::appendIncomingMessage);
    connect(m_logic, &PrivateChatLogic::outgoingMessage,
            this, &PrivateChat::appendOutgoingMessage);
    connect(m_logic, &PrivateChatLogic::systemMessage,
            this, &PrivateChat::appendSystemMessage);

    // When the other user renames themselves, forward the new nick upward to
    // MainWindow so it can update the tab label.
    connect(m_logic, &PrivateChatLogic::otherNickChanged,
            this, [this](const QString& /*old*/, const QString& newNick) {
                emit nickChanged(newNick);
            });

    // Clicking the display area re-focuses the input line.
    connect(m_display, &ChatDisplay::clicked, this, [this]() {
        m_input->setFocus();
    });
}

PrivateChat::~PrivateChat() {
#ifdef QT_DEBUG
    qDebug() << "PrivateChat for" << (m_logic ? m_logic->otherNick() : "") << "destroyed";
#endif
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Incoming message from the other party - shown in purple and triggers the
// messageActivity signal so MainWindow can mark the tab red if it's in the
// background.
void PrivateChat::appendIncomingMessage(const QString& from, const QString& message) {
    m_display->appendUserMessage(from, message, COLOR_QT_PURPLE);
    emit messageActivity(this);
}

// Our own sent message - shown in dark green.  We use our own nick as the
// sender label, falling back to "you" if the session is gone.
void PrivateChat::appendOutgoingMessage(const QString& to, const QString& message) {
    Q_UNUSED(to);
    QString myNick = (m_logic && m_logic->session())
        ? m_logic->session()->nickname()
        : "you";
    m_display->appendUserMessage(myNick, message, COLOR_QT_DARKGREEN);
}

// Status / system message - disconnection-related messages are highlighted in
// red so they stand out; everything else appears in gray.
void PrivateChat::appendSystemMessage(const QString& message) {
    if (message.contains("Disconnected", Qt::CaseInsensitive))
        m_display->appendSystemMessage(message, COLOR_QT_RED);
    else
        m_display->appendSystemMessage(message, COLOR_QT_GRAY);
}

void PrivateChat::setInputFocus() {
    m_input->setFocus();
}

// ---------------------------------------------------------------------------
// Slot implementations
// ---------------------------------------------------------------------------

// Text submitted from the input line.  "/clear" is handled locally; all other
// input is forwarded to PrivateChatLogic which routes it to the session.
void PrivateChat::onSendMessage(const QString& text) {
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;

    if (trimmed == "/clear") {
        m_display->clear();
        return;
    }

    if (m_logic)
        m_logic->sendMessage(trimmed);
}

// ---------------------------------------------------------------------------
// Event filter
// ---------------------------------------------------------------------------

// Clicking anywhere on the chat display moves focus to the input line so the
// user can start typing without clicking on the input field explicitly.
bool PrivateChat::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_display && event->type() == QEvent::MouseButtonPress)
        m_input->setFocus();
    return QWidget::eventFilter(obj, event);
}

#endif  // QT_GUI_LIB
