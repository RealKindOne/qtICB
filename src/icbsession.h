#ifndef ICBSESSION_H
#define ICBSESSION_H

#include <QObject>
#include "icbclient.h"
#include "logger.h"
#include "userlist.h"

// ICBSession is the middle layer between the low-level ICBClient (raw protocol)
// and the UI layer (ConnectionWidget / NcursesUI).

class ICBSession : public QObject {
    Q_OBJECT

  public:
    explicit ICBSession(const QString& basePath, QObject* parent = nullptr);
    ~ICBSession();

    // Returns the current TCP state of the underlying ICBClient.
    ICBClient::ConnectionState state() const;

    // Initiates a new connection.  Does nothing if already connected.
    void connectToServer(const QString& host, quint16 port,
                         const QString& nickname, const QString& group = "icb");

    // Drops the current connection (if any).
    void disconnectFromServer();

    bool isConnected() const;
    bool isLoggedIn() const;

    // Accessors for current session state
    QString  nickname()     const { return m_nickname; }
    QString  currentGroup() const { return m_currentGroup; }
    QString  host()         const { return m_host; }
    quint16  port()         const { return m_port; }
    // Most recently received topic string (empty if none).
    QString  topic()        const { return m_topic; }
    // Most recently received group flags "(m)" for moderated (empty if unknown).
    QString  groupFlags()   const { return m_groupFlags; }
    // The live user list for the current group.
    UserList* userList()          { return &m_userList; }

    // Message sending
    void sendMessage(const QString& text);
    void sendPrivateMessage(const QString& to, const QString& text);
    // Sends a raw ICB command packet.  'manual' indicates whether it was
    // typed by the user (affects logging behavior).
    void sendCommand(const QString& command, const QString& arg = QString(), bool manual = true);

  public slots:
    // Parse and dispatch a line of text from the user input field.
    // Commands (starting with '/') are handled by CommandHandler; plain text
    // is sent as a public group message.
    void processUserInput(const QString& input);

  signals:
    // Connection lifecycle
    void connectionStateChanged(ICBClient::ConnectionState state);
    void loggedIn();
    void errorOccurred(const QString& error);

    // Messages
    void messageReceived(const QString& sender, const QString& message);
    void systemMessageReceived(const QString& message);
    void personalMessageReceived(const QString& from, const QString& message);

    // Our own sent messages (for echo display in the UI)
    void selfMessageSent(const QString& message);
    void selfPrivateMessageSent(const QString& to, const QString& message);

    // Group / user state changes
    void userJoined(const QString& nick, const QString& ident,
                    const QString& host, bool isModerator);
    void userLeft(const QString& nick, const QString& reason);
    void userChangedNick(const QString& oldNick, const QString& newNick);
    void topicChanged(const QString& nick, const QString& topic);
    void inviteReceived(const QString& group, const QString& inviter);
    void moderatorPassed(const QString& from, const QString& to);
    void moderatorGranted(const QString& nick);
    void moderatorLost(const QString& nick);
    void groupRenamed(const QString& oldName, const QString& newName, const QString& byNick);
    void userBooted(const QString& nick, const QString& by);
    void groupChanged(const QString& newGroup);

    // Private chat plumbing
    void openPrivateChatRequested(const QString& user);

    // Emitted when the "Group: name (flags) Topic: ..." line from a /who
    // response is parsed, OR when a live Topic packet arrives, OR when the
    // user switches group (both fields cleared to empty strings).
    // UIs listen to this single signal instead of duplicating the parsing.
    void whoInfoReceived(const QString& flags, const QString& topic);

  private slots:
    // One-to-one handlers for each ICBClient signal.  They add session-level
    // logic (user list maintenance, logging, formatting) before forwarding.
    void onClientStateChanged(ICBClient::ConnectionState state);
    void onClientMessageReceived(const QString& sender, const QString& message);
    void onClientSystemMessageReceived(const QString& message);
    void onClientErrorOccurred(const QString& error);
    void onClientPersonalMessageReceived(const QString& from, const QString& message);
    void onClientLoggedIn();
    void onClientUserJoined(const QString& nick, const QString& ident,
                            const QString& host, bool isModerator);
    void onClientUserLeft(const QString& nick, const QString& reason);
    void onClientUserChangedNick(const QString& oldNick, const QString& newNick);
    void onClientTopicChanged(const QString& nick, const QString& topic);
    void onClientInviteReceived(const QString& group, const QString& inviter);
    void onClientModeratorPassed(const QString& from, const QString& to);
    void onClientModeratorGranted(const QString& nick);
    void onClientGroupRenamed(const QString& oldName, const QString& newName,
                              const QString& byNick);
    void onClientUserBooted(const QString& nick, const QString& by);
    void onClientGroupChanged(const QString& newGroup);
    void onClientModeratorLost(const QString& nick);

  private:
    // Parses special system messages before forwarding them.
    // Currently handles the "Group: ..." line to extract flags and topic,
    // and manages the group-logging enable flag.
    void handleSystemMessage(const QString& message);

    ICBClient m_client;   // raw ICB protocol layer
    Logger    m_logger;   // per-group and per-private-chat log files
    UserList  m_userList; // live list of nicks in the current group

    QString  m_host;
    quint16  m_port;
    QString  m_nickname;
    QString  m_group;         // initial group requested at connect time
    QString  m_currentGroup;  // actual group after any joins/renames
    QString  m_topic;         // most recently known topic (empty if unknown)
    QString  m_groupFlags;    // most recently known flags, "(m)" (empty if unknown)

    // Logging starts only after the first "Group:" line is seen, to avoid
    // writing partial /who output into the wrong log file on login.
    bool m_groupLoggingEnabled;

    // Moderator packets sometimes arrive before the corresponding join packet.
    // This set holds nicks whose moderator status should be applied as soon as
    // they appear in a subsequent join.
    QSet<QString> m_pendingModerators;
};

#endif  // ICBSESSION_H
