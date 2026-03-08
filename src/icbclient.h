#ifndef ICBCLIENT_H
#define ICBCLIENT_H

#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

// ICB packet wire format:
//   [1 byte length][command byte][field1]\x01[field2]...[fieldN]\x00
//   - The length byte counts ALL bytes that follow it (command + fields + NUL).
//   - Fields are separated by \x01 (ASCII SOH).
//   - The packet is always terminated with a \x00 NUL byte.
//   - Maximum total packet size is 255 bytes (limited by the 1-byte length).
//
// Connection state machine:
//   Disconnected --> (connectToServer) --> Connecting
//   Connecting   --> (TCP connected)  --> Connected
//   Connected    --> (recv 'j' proto) --> LoggingIn
//   LoggingIn    --> (recv 'a' login) --> LoggedIn
//   Any state    --> (error/close)    --> Disconnected
class ICBClient : public QObject {
    Q_OBJECT

  public:
    // Mirrors the connection lifecycle described above.
    enum ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        LoggingIn,
        LoggedIn
    };

    explicit ICBClient(QObject* parent = nullptr);
    ~ICBClient();

    // Connection control

    // Initiates a TCP connection to host:port and begins the ICB login
    // handshake once the server sends its protocol-info packet.
    void connectToServer(const QString& host, quint16 port,
                         const QString& nickname, const QString& group = "icb");

    // Gracefully closes the TCP connection.  The actual state change and
    // signal emission happen in onDisconnected().
    void disconnectFromServer();

    // Outgoing messages

    // Sends a public message to the current group ('b' packet).
    // Automatically splits messages longer than 244 bytes into multiple packets.
    void sendMessage(const QString& message);

    // Sends a private message to 'to' ('h m' packet).
    // Automatically splits long messages accounting for the recipient header.
    void sendPrivateMessage(const QString& to, const QString& message);

    // Sends an ICB command packet ('h' packet), like "w" for /who or
    // "topic" for /topic.  'manual' indicates whether the user typed the
    // command (true) or the session layer issued it automatically (false);
    // this affects whether /who results are displayed to the user.
    void sendCommand(const QString& command, const QString& arg = "",
                     bool manual = true);

    // Accessors
    QString         getNickname()     const { return m_nickname; }
    ConnectionState getState()        const { return m_state; }
    QString         getHost()         const { return m_host; }
    quint16         getPort()         const { return m_port; }
    QString         getCurrentGroup() const { return m_currentGroup; }

  signals:
    // Emitted whenever the connection state machine transitions.
    void connectionStateChanged(ICBClient::ConnectionState state);

    // A public group message arrived ('b' packet, two arguments).
    // Not emitted for our own echoed messages (filtered by nickname).
    void messageReceived(const QString& sender, const QString& message);

    // A server status or informational line arrived (various packet types).
    void systemMessageReceived(const QString& message);

    // A socket or protocol error occurred.
    void errorOccurred(const QString& error);

    // A private ('c') message arrived.
    void personalMessageReceived(const QString& from, const QString& message);

    // Login was acknowledged by the server ('a' packet).
    void loggedIn();

    // A user appeared in the group (from an Arrive/Sign-on status or /who).
    void userJoined(const QString& nick, const QString& ident,
                    const QString& host, bool isModerator);

    // A user left the group (Depart or Sign-off status).
    void userLeft(const QString& nick, const QString& reason);

    // A user changed their nickname (Name status).
    void userChangedNick(const QString& oldNick, const QString& newNick);

    // The group topic was changed (Topic status).
    void topicChanged(const QString& nick, const QString& topic);

    // We received an invitation to another group (RSVP status).
    void inviteReceived(const QString& group, const QString& inviter);

    // Moderator privilege was transferred between two users (Pass status).
    void moderatorPassed(const QString& from, const QString& to);

    // A user gained moderator status (Pass / Mod / Timeout status).
    void moderatorGranted(const QString& nick);

    // The group was renamed (Change status).
    void groupRenamed(const QString& oldName, const QString& newName,
                      const QString& byNick);

    // A user was booted from the group (Boot status).
    void userBooted(const QString& nick, const QString& by);

    // We switched to a new group (Status "You are now in group…").
    void groupChanged(const QString& newGroup);

    // A user lost moderator status (Idle-Mod status - piano event).
    void moderatorLost(const QString& nick);

  private slots:
    // Fired by QTcpSocket::connected - advances state to Connected and starts
    // the protocol-info timeout timer.
    void onConnected();

    // Fired by QTcpSocket::disconnected - resets state to Disconnected,
    // stops timers, and emits connectionStateChanged.
    void onDisconnected();

    // Fired by QTcpSocket::readyRead - drains the socket into m_buffer and
    // processes all complete packets that are now available.
    void onReadyRead();

    // Fired by QTcpSocket::errorOccurred (or ::error on Qt < 5.15) - emits
    // errorOccurred with the human-readable socket error string.
    void onError(QAbstractSocket::SocketError error);

    // Fired by m_whoTimeout if the server never sends the "Total:" line that
    // terminates a /who response.  Prints whatever entries arrived (if the
    // request was manual) or silently discards them (if automatic).
    void onWhoTimeout();

  private:
    // Low-level packet I/O

    // Builds a complete ICB packet from a command byte and an optional list
    // of \x01-separated argument fields.  Returns an empty QByteArray if the
    // payload would exceed 255 bytes (callers must chunk before calling this).
    QByteArray buildPacket(char command,
                           const QList<QByteArray>& args = QList<QByteArray>());

    // Splits a raw packet body (after command byte) on \x01 separators into a
    // list of field byte arrays.  Always appends the final field, even if empty.
    QList<QByteArray> parseArgs(const QByteArray& data);

    // Writes a fully-formed packet (with length prefix) to the TCP socket.
    // Silently no-ops if the socket is not in ConnectedState.
    void sendRawPacket(const QByteArray& packet);

    // Constructs and sends the login ('a') packet using the stored credentials.
    void sendLoginPacket();

    // Interprets one complete packet (length byte already stripped) and emits
    // the corresponding signal(s).
    void processPacket(const QByteArray& packet);

    // /who table helpers

    // Formats an idle-time in seconds as "1h23m45s" (or "0s" for zero).
    QString formatIdle(int seconds) const;

    // Formats a Unix timestamp as a local "yyyy-MM-dd hh:mm:ss" string.
    QString formatSignon(qint64 timestamp) const;

    // Emits all collected /who entries as formatted systemMessageReceived lines,
    // then clears m_whoEntries and resets m_manualWhoRequest.
    void printWhoTable();

    // Internal state

    QTcpSocket*     m_socket;         // the TCP connection
    QTimer*         m_pingTimer;      // fires every 30 s to send a keep-alive 'n' packet
    QString         m_host;
    quint16         m_port;
    QString         m_nickname;
    QString         m_group;          // initial group requested at login time
    QString         m_currentGroup;   // actual current group (updated on group changes)
    ConnectionState m_state;
    QByteArray      m_buffer;         // accumulates partial incoming data between readyRead calls
    bool m_autoReconnect;             // placeholder - reconnect logic is not yet implemented
    qint32          m_expectedLength; // byte count expected for the packet currently being read,
                                      // or -1 when waiting for the next length byte

    // Fires 5 seconds after TCP connect if the server has not yet sent its
    // 'j' (protocol-info) packet.
    QTimer* m_protoTimer;

    // /who response tracking
    // ICB sends /who results as a stream of 'i wl' (who-list entry) packets
    // followed by a single 'i co' packet containing a "Total:" line.  We
    // accumulate the entries here, then format and display them all at once.

    QString m_requestedWhoGroup;  // the group whose /who we are currently collecting

    // One entry per user returned by /who.
    struct WhoEntry {
        bool    isModerator;
        QString nick;
        int     idleSeconds;
        qint64  signonTime;
        QString ident;
        QString host;
    };

    QList<WhoEntry> m_whoEntries;       // accumulated entries for the current /who
    QTimer*         m_whoTimeout;       // fires 500 ms after /who if "Total:" never arrives
    bool            m_manualWhoRequest; // true if the user typed /who; false if automatic
};

#endif  // ICBCLIENT_H
