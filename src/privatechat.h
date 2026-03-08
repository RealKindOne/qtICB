#ifndef PRIVATECHAT_H
#define PRIVATECHAT_H

#include <QObject>
#include <QStringList>
#include "icbsession.h"

// ---------------------------------------------------------------------------
// PrivateChatLogic  (shared by both the Qt GUI and the ncurses UI)
// ---------------------------------------------------------------------------
// PrivateChatLogic manages the state and message routing for a private
// (one-to-one) conversation with another user.
//
// It connects to the parent ICBSession and filters incoming/outgoing personal
// messages to those that match the other party's nick.  It also watches for
// events that should be reported inside the chat window:
//   - The other user leaving the group or being booted
//   - The other user changing their nick
//
// This class contains no UI code so it can be used by both the Qt widget UI
// (PrivateChat below) and the ncurses UI (NcursesUI).
class PrivateChatLogic : public QObject {
    Q_OBJECT

  public:
    explicit PrivateChatLogic(ICBSession* session, const QString& otherNick,
                              QObject* parent = nullptr);
    ~PrivateChatLogic();

    ICBSession* session()   const { return m_session; }
    QString     otherNick() const { return m_otherNick; }

    // Send a message to the other party.  Slash-commands are routed through
    // ICBSession::processUserInput; plain text is sent as a private message.
    void sendMessage(const QString& text);

  signals:
    // A message arrived from the other party.
    void incomingMessage(const QString& from, const QString& message);

    // We sent a message to the other party.
    void outgoingMessage(const QString& to, const QString& message);

    // A status event worth displaying in the chat window "bob has left"
    void systemMessage(const QString& message);

    // The other user changed their nick.  Carries both old and new values so
    // the UI can update tab labels and its own state.
    void otherNickChanged(const QString& oldNick, const QString& newNick);

  private slots:
    // Filter ICBSession::personalMessageReceived to this conversation's nick.
    void onPersonalMessageReceived(const QString& from, const QString& message);

    // Filter ICBSession::selfPrivateMessageSent to this conversation's nick.
    void onSelfPrivateMessageSent(const QString& to, const QString& message);

    // ICBSession was destroyed - clear our pointer so we don't use it after free.
    void onConnectionDestroyed();

    // Emit a status message if the other user leaves or is booted.
    void onUserLeft(const QString& nick, const QString& reason);
    void onUserBooted(const QString& nick, const QString& by);

    // Track nick changes by the other party and emit otherNickChanged.
    void onUserChangedNick(const QString& oldNick, const QString& newNick);

  private:
    ICBSession* m_session;   // may become nullptr if the session is destroyed
    QString     m_otherNick; // updated when the other user renames themselves
};

// ---------------------------------------------------------------------------
// PrivateChat  (Qt GUI only - compiled only when QT_GUI_LIB is defined)
// ---------------------------------------------------------------------------
// PrivateChat is the Qt widget that wraps PrivateChatLogic and provides a
// visible tab containing a chat history display and a message input line.
//
// Layout:
//   ┌─ Chat display (ChatDisplay) ─────────────────────────────────┐
//   │  incoming messages in purple, outgoing in dark green         │
//   └──────────────────────────────────────────────────────────────┘
//   ┌─ Input line (HistoryLineEdit) ───────────────────────────────┐
//   │  Up/Down history, PageUp/Down scroll, /clear supported       │
//   └──────────────────────────────────────────────────────────────┘
#ifdef QT_GUI_LIB
#include <QWidget>
#include "chatdisplay.h"
#include "historylineedit.h"

class PrivateChat : public QWidget {
    Q_OBJECT

  public:
    explicit PrivateChat(ICBSession* session, const QString& otherNick,
                         QWidget* parent = nullptr);
    ~PrivateChat();

    // Convenience accessors that delegate to the underlying logic object.
    ICBSession* session()   const { return m_logic ? m_logic->session()   : nullptr; }
    QString     otherNick() const { return m_logic ? m_logic->otherNick() : QString(); }

    // Append an incoming message from the other party (shown in purple).
    void appendIncomingMessage(const QString& from, const QString& message);

    // Append a message we sent (shown in dark green).
    void appendOutgoingMessage(const QString& to, const QString& message);

    // Move keyboard focus to the input line after the tab is activated.
    void setInputFocus();

    // Append a plain status/system message "bob has left", day separator.
    void appendSystemMessage(const QString& message);

  signals:
    // Emitted when any message arrives so MainWindow can mark the tab red if
    // it is not currently active.
    void messageActivity(PrivateChat* sender);

    // Emitted when the other party changes their nick so MainWindow can update
    // the tab label.
    void nickChanged(const QString& newNick);

  protected:
    // Click on the display area --> focus the input line.
    bool eventFilter(QObject* obj, QEvent* event) override;

  private slots:
    // Handles Enter in the input line: route to PrivateChatLogic::sendMessage.
    void onSendMessage(const QString& text);

  private:
    PrivateChatLogic* m_logic;    // session binding and message routing
    HistoryLineEdit*  m_input;    // single-line text input with history
    ChatDisplay*      m_display;  // read-only scrollable message history
};

#endif  // QT_GUI_LIB

#endif  // PRIVATECHAT_H
