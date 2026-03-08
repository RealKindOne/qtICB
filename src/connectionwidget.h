#ifndef CONNECTIONWIDGET_H
#define CONNECTIONWIDGET_H

#include <QStringList>
#include <QWidget>
#include "chatdisplay.h"
#include "historylineedit.h"
#include "icbsession.h"

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPushButton;
class QListWidget;
class QGroupBox;
class QSplitter;
QT_END_NAMESPACE

class UserList;

// ConnectionWidget is a self-contained tab for one ICB server connection.
//
// Layout (top to bottom):
//   ┌─ Connection group box ────────────────────────────────────────┐
//   │  Server  [edit]  Port  [edit]  Nickname  [edit]  Group [edit] [Connect] │
//   └───────────────────────────────────────────────────────────────┘
//   ┌─ Chat display (ChatDisplay) ──────┬─ User list (QListWidget) ─┐
//   │  scrollable chat history          │  sorted nicks (@mod first) │
//   └───────────────────────────────────┴────────────────────────────┘
//   ┌─ Input line (HistoryLineEdit) ────────────────────────────────┐
//   │  Up/Down history, Tab nick-completion, PageUp/Down scrolling  │
//   └───────────────────────────────────────────────────────────────┘
//
// The widget owns an ICBSession and is responsible for:
//   - Connecting to / disconnecting from the server
//   - Routing user input to the session (commands or plain messages)
//   - Displaying incoming public, private, and system messages
//   - Keeping the user-list sidebar in sync with ICBSession::userList()
//   - Emitting signals for MainWindow to handle tab labels, title bar,
//     and private chat tab creation
class ConnectionWidget : public QWidget {
    Q_OBJECT

  public:
    explicit ConnectionWidget(QWidget* parent = nullptr);
    ~ConnectionWidget();

    // Gracefully closes the current connection (if any).
    void disconnectFromServer();

    // Returns the current TCP connection state (Disconnected / Connecting /
    // LoggedIn / etc.).
    ICBClient::ConnectionState connectionState() const;

    // Direct access to the underlying session, used by MainWindow to wire up
    // the private-chat and title-bar signals.
    ICBSession* connection() const { return m_session; }

    // Appends a plain system message without logging it.
    void appendSystemMessage(const QString& message);

  public slots:
    // (none currently needed from outside)

  signals:
    // Emitted when the ICB group name changes (join, rename) so MainWindow
    // can update the tab label.
    void groupNameChanged(const QString& group);

    // Emitted when group flags or topic change, so MainWindow can update the
    // window title bar.  Carries the raw flags string "(m)" and topic text.
    void groupInfoChanged(const QString& flags, const QString& topic);

    // Emitted whenever a public group message arrives so MainWindow can mark
    // this tab red if it is in the background.
    void messageActivity();

    // Forwarded from ICBSession::personalMessageReceived so MainWindow can
    // ensure a PrivateChat tab exists for the sender.
    void privateMessageReceived(const QString& from, const QString& message);

    // Forwarded from ICBSession::selfPrivateMessageSent so MainWindow can
    // ensure a PrivateChat tab exists for the recipient.
    void privateMessageSent(const QString& to, const QString& message);

    // Emitted when the user runs /query <nick> so MainWindow can open and
    // focus the appropriate PrivateChat tab.
    void openPrivateChat(const QString& nick);

  protected:
    // Intercepts events on m_messageDisplay and m_messageInput:
    //   - Click on display  --> focus the input line
    //   - PageUp/Down on input --> scroll the chat display
    bool eventFilter(QObject* obj, QEvent* event) override;

  private slots:
    // Toggle between connect and disconnect when the button is clicked.
    void onConnectButtonClicked();

    // Update button labels and input availability as the connection state
    // machine transitions between Disconnected / Connecting / LoggedIn / etc.
    void onConnectionStateChanged(ICBClient::ConnectionState state);

    // Display an incoming public group message.
    void onMessageReceived(const QString& sender, const QString& message);

    // Display an incoming server/status message (prefixed with "* ").
    void onSystemMessageReceived(const QString& message);

    // Display a connection or socket error in red.
    void onErrorOccurred(const QString& error);

    // Forward an incoming private message up to MainWindow.
    void onPersonalMessageReceived(const QString& from, const QString& message);

    // After login, emit groupNameChanged so MainWindow can replace the
    // "Disconnected" placeholder text on the tab.
    void onLoggedIn();

    // Route text from the input line: handle /clear locally, pass everything
    // else to ICBSession::processUserInput.
    void processInput(const QString& text);

  private:
    // Connect all ICBSession signals to our slots.  Called once from the
    // constructor to keep construction code readable.
    void setupConnections();

    // Connection form
    QLineEdit*   m_serverEdit;
    QLineEdit*   m_portEdit;
    QLineEdit*   m_nicknameEdit;
    QLineEdit*   m_groupEdit;
    QPushButton* m_connectButton;

    // Chat area
    ChatDisplay*     m_messageDisplay;  // read-only scrollable history
    QListWidget*     m_userList;        // sidebar showing current group members
    HistoryLineEdit* m_messageInput;    // single-line input with history + completion

    // The session layer that handles all ICB protocol communication.
    ICBSession* m_session;
};

#endif  // CONNECTIONWIDGET_H
