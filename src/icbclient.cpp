#include "icbclient.h"
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include "color.h"
#include "formatting.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ICBClient::ICBClient(QObject* parent)
    : QObject(parent),
      m_socket(new QTcpSocket(this)),
      m_pingTimer(new QTimer(this)),
      m_state(Disconnected),
      m_autoReconnect(false),  // TODO: reconnect logic not yet implemented
      m_expectedLength(-1),
      m_requestedWhoGroup() {

    // Socket signals
    connect(m_socket, &QTcpSocket::connected,    this, &ICBClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &ICBClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &ICBClient::onReadyRead);

    // QTcpSocket renamed the error signal in Qt 5.15; support both.
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this, &ICBClient::onError);
#else
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, &ICBClient::onError);
#endif

    // Keep-alive ping
    // ICB servers close idle connections.  Send a no-op 'n' packet every 30
    // seconds to keep the session alive without producing visible output.
    m_pingTimer->setInterval(30000);
    connect(m_pingTimer, &QTimer::timeout, this, [this]() {
        if (m_state == LoggedIn) {
            QByteArray ping = buildPacket('n');
            sendRawPacket(ping);
        }
    });

    // Protocol-info timeout
    // After the TCP connection is established, the server should immediately
    // send a 'j' packet with protocol/host/server information.  If it doesn't
    // arrive within 5 seconds we treat that as an error and disconnect.
    m_protoTimer = new QTimer(this);
    m_protoTimer->setSingleShot(true);
    connect(m_protoTimer, &QTimer::timeout, this, [this]() {
        if (m_state == Connected) {
            emit errorOccurred("Timeout waiting for protocol info from server");
            disconnectFromServer();
        }
    });

    // /who response timeout
    // /who results arrive as a stream of 'i wl' packets terminated by an
    // 'i co Total:' line.  If the server stops mid-stream, this 500 ms timer
    // fires so we can print whatever we received and clean up.
    m_whoTimeout = new QTimer(this);
    m_whoTimeout->setSingleShot(true);
    connect(m_whoTimeout, &QTimer::timeout, this, &ICBClient::onWhoTimeout);
}

ICBClient::~ICBClient() {
    disconnectFromServer();
}

// ---------------------------------------------------------------------------
// Connection control
// ---------------------------------------------------------------------------

// Begin a new connection.  Sets up credentials, transitions to Connecting, and
// initiates the async TCP handshake.  Does nothing if already connected.
void ICBClient::connectToServer(const QString& host, quint16 port,
                                const QString& nickname, const QString& group) {
    if (m_state != Disconnected) return;

    m_host         = host;
    m_port         = port;
    m_nickname     = nickname;
    m_group        = group.isEmpty() ? "icb" : group;
    m_currentGroup = m_group;  // optimistic initial value; corrected by server
    m_state        = Connecting;
    m_buffer.clear();
    m_expectedLength = -1;

    emit connectionStateChanged(m_state);
#ifdef QT_DEBUG
    qDebug() << "Connecting to" << host << ":" << port;
#endif
    m_socket->connectToHost(host, port);
}

// Initiates a graceful TCP close.  The actual state transition happens in
// onDisconnected() once the socket confirms the close.
void ICBClient::disconnectFromServer() {
    if (m_state == Disconnected) return;
    m_pingTimer->stop();
    m_protoTimer->stop();
    m_socket->disconnectFromHost();
}

// ---------------------------------------------------------------------------
// Packet building helpers
// ---------------------------------------------------------------------------

// Constructs a single ICB packet.
//
// Wire layout:   [length][command][arg0]\x01[arg1]\x01...[argN]\x00
//
// 'length' is a single byte counting everything after itself: the command
// byte, all argument bytes, \x01 separators, and the trailing \x00.
//
// Returns an empty QByteArray and logs a critical error if the payload would
// exceed 255 bytes - callers must chunk large payloads before calling here.
QByteArray ICBClient::buildPacket(char command, const QList<QByteArray>& args) {
    QByteArray data;
    data.append(command);
    for (int i = 0; i < args.size(); ++i) {
        data.append(args[i]);
        if (i < args.size() - 1)
            data.append('\001');  // \x01 field separator
    }
    data.append('\0');  // required trailing NUL

    if (data.size() > 255) {
        qCritical() << "buildPacket: payload" << data.size()
                    << "bytes exceeds 255. Caller must split. Packet dropped.";
        return QByteArray();
    }

    QByteArray packet;
    packet.append(static_cast<char>(data.size() & 0xFF));  // length prefix
    packet.append(data);
    return packet;
}

// Splits a packet body (everything after the command byte) on \x01 separators
// into individual field byte arrays.  The final field is always appended,
// even if it is empty, so an explicit empty trailing argument is preserved.
QList<QByteArray> ICBClient::parseArgs(const QByteArray& data) {
    QList<QByteArray> args;
    QByteArray current;

    for (int i = 0; i < data.size(); ++i) {
        if (data[i] == '\001') {
            args.append(current);
            current.clear();
        } else {
            current.append(data[i]);
        }
    }
    args.append(current);  // always include the last field
    return args;
}

// Writes a fully-formed packet (length prefix already included) to the socket.
// Silently no-ops on empty input (rejected by buildPacket) or a closed socket.
void ICBClient::sendRawPacket(const QByteArray& packet) {
    if (packet.isEmpty()) return;
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(packet);
#ifdef QT_DEBUG
        qDebug() << "SENT - RAW DATA HEX  :" << packet.toHex();
        qDebug() << "SENT - RAW DATA ASCII:" << packet;
#endif
    }
}

// ---------------------------------------------------------------------------
// Login packet
// ---------------------------------------------------------------------------

// Sends the 'a' (login) packet immediately after the server's 'j' greeting.
// The ICB protocol uses the same value for LoginID and Nickname; the password,
// GroupStatus, and ProtocolLevel fields are left empty.
void ICBClient::sendLoginPacket() {
    QList<QByteArray> args;
    args.append(m_nickname.toUtf8());  // LoginID  (we just reuse the nickname)
    args.append(m_nickname.toUtf8());  // Nickname
    args.append(m_group.toUtf8());     // DefaultGroup
    args.append("login");              // Command
    args.append("");                   // Password  (not used)
    args.append("");                   // GroupStatus
    args.append("");                   // ProtocolLevel

    sendRawPacket(buildPacket('a', args));
#ifdef QT_DEBUG
    qDebug() << "Sent login packet";
#endif
}

// ---------------------------------------------------------------------------
// Outgoing messages
// ---------------------------------------------------------------------------

// Sends a public message to the current group.
//
// The ICB 'b' packet has this layout:
//   [length]['b'][message text][NUL]
// With 1 byte for 'b' and 1 for NUL, the maximum message payload per packet
// is 244 bytes (246 - 2).  Longer messages are automatically chunked.
void ICBClient::sendMessage(const QString& message) {
    if (m_state != LoggedIn || message.isEmpty()) return;

    QByteArray utf8      = message.toUtf8();
    const int MAX_DATA   = 246;        // length byte can hold at most 255 --> keep some margin
    const int MAX_MSG    = MAX_DATA - 2; // minus 'b' command byte and trailing NUL

    int pos = 0;
    while (pos < utf8.size()) {
        int chunkSize = qMin(utf8.size() - pos, MAX_MSG);
        QByteArray chunk = utf8.mid(pos, chunkSize);

        // Build manually rather than via buildPacket to avoid the 255-byte check
        // on individual chunks (the loop already ensures each is in-range).
        QByteArray packet;
        packet.append(char(0));               // placeholder - overwritten below
        packet.append('b');                   // command: open message
        packet.append(chunk);
        packet.append('\0');                  // required trailing NUL
        packet[0] = char(packet.size() - 1); // actual length (excludes the length byte itself)
        sendRawPacket(packet);

        pos += chunkSize;
    }
}

// Sends a private message to 'to'.
//
// The ICB 'h m' private-message packet layout:
//   [length]['h']['m'][SOH][recipient][ ][message text][NUL]
// Note: private messages use a space (' ') between recipient and text,
// NOT a \x01 separator as used in other 'h' subcommands.
// The header occupies: 1('h') + 1('m') + 1(SOH) + len(recipient) + 1(' ') bytes,
// leaving MAX_TOTAL - HEADER_SIZE - 1(NUL) bytes for the message text.
void ICBClient::sendPrivateMessage(const QString& to, const QString& message) {
    if (m_state != LoggedIn || message.isEmpty() || to.isEmpty()) return;

    QByteArray recipient   = to.toUtf8();
    QByteArray msg         = message.toUtf8();
    const int HEADER_SIZE  = 3 + recipient.size() + 1; // 'h'+'m'+SOH + recipient + ' '
    const int MAX_TOTAL    = 246;
    const int MAX_MSG      = MAX_TOTAL - HEADER_SIZE - 1; // reserve for trailing NUL

    int pos = 0;
    while (pos < msg.size()) {
        int chunkSize  = qMin(msg.size() - pos, MAX_MSG);
        QByteArray chunk = msg.mid(pos, chunkSize);

        QByteArray packet;
        packet.append(char(0));    // placeholder length
        packet.append('h');        // command: ICB command/private
        packet.append('m');        // subcommand: private message
        packet.append('\001');     // SOH separator before recipient
        packet.append(recipient);
        packet.append(' ');        // space separates recipient from text (ICB convention)
        packet.append(chunk);
        packet.append('\0');       // required trailing NUL
        packet[0] = char(packet.size() - 1);
        sendRawPacket(packet);

        pos += chunkSize;
    }
}

// Sends an arbitrary ICB command packet ('h' with a subcommand name).
// Common subcommands: "w" (/who), "topic" (/topic), "name" (/nick),
// "boot" (/boot), "invite" (/invite), "pass" (/pass).
//
// When command == "w", sets up /who tracking state so arriving 'wl' packets
// are accumulated and displayed once the "Total:" terminator arrives.
// 'manual' controls whether the results are displayed (true = user-initiated)
// or silently used to populate the user list (false = automatic on login/join).
void ICBClient::sendCommand(const QString& command, const QString& arg, bool manual) {
    if (m_state != LoggedIn) return;

    if (command == "w") {
        // Record state before starting the 500 ms collection window.
        m_manualWhoRequest  = manual;
        m_whoEntries.clear();
        m_requestedWhoGroup = arg.isEmpty() ? m_currentGroup : arg;
        m_whoTimeout->start(500);
#ifdef QT_DEBUG
        static int whoSeq = 0;
        qDebug() << ">>> SEND WHO [seq=" << ++whoSeq << "] group="
                 << m_requestedWhoGroup << "manual=" << manual;
#endif
    }

    QByteArray cmd      = command.toUtf8();
    QByteArray argument = arg.toUtf8();

    // ICB 'h' packet: ['h'][subcommand][\x01][argument][\x00]
    QByteArray data;
    data.append('h');
    data.append(cmd);
    if (!argument.isEmpty()) {
        data.append('\001');
        data.append(argument);
    }
    data.append('\0');

    if (data.size() > 255) {
        qCritical() << "sendCommand: payload" << data.size()
                    << "bytes for command" << command << "- dropped.";
        return;
    }

    QByteArray packet;
    packet.append(static_cast<char>(data.size()));
    packet.append(data);
    sendRawPacket(packet);
}

// ---------------------------------------------------------------------------
// Socket event handlers
// ---------------------------------------------------------------------------

// TCP handshake complete.  We cannot log in yet - ICB requires us to wait for
// the server's 'j' protocol-info packet first.  Start a 5-second safety timer
// in case the server is silent.
void ICBClient::onConnected() {
#ifdef QT_DEBUG
    qDebug() << "Connected to server";
#endif
    m_state = Connected;
    emit connectionStateChanged(m_state);
    emit systemMessageReceived("Connected to server");
    m_protoTimer->start(5000);
}

// TCP connection closed (by us or by the server).  Reset all runtime state.
void ICBClient::onDisconnected() {
#ifdef QT_DEBUG
    qDebug() << "Disconnected from server";
#endif
    m_state = Disconnected;
    m_pingTimer->stop();
    emit connectionStateChanged(m_state);
    emit systemMessageReceived("Disconnected from server");

    // Auto-reconnect stub - not implemented yet.
    if (m_autoReconnect) {
        QTimer::singleShot(5000, this, [this]() {
            connectToServer(m_host, m_port, m_nickname, m_group);
        });
    }

    m_requestedWhoGroup.clear();
    m_whoTimeout->stop();
}

// New bytes arrived from the server.  Appends them to the rolling buffer and
// extracts complete packets using the length-prefixed framing scheme.
void ICBClient::onReadyRead() {
    QByteArray newData = m_socket->readAll();
#ifdef QT_DEBUG
    // Keep spacing in HEX for formatting
    qDebug() << "RECV - RAW DATA HEX  :" << newData.toHex();
    qDebug() << "RECV - RAW DATA ASCII:" << newData;
#endif

    m_buffer.append(newData);

    // Loop until the buffer doesn't contain another complete packet.
    while (m_buffer.size() > 0) {
        // Phase 1: read the length byte if we don't have it yet.
        if (m_expectedLength < 0) {
            if (m_buffer.size() < 1) return;  // need at least 1 byte
            unsigned char lengthByte = static_cast<unsigned char>(m_buffer[0]);
            m_expectedLength = lengthByte;
            m_buffer = m_buffer.mid(1);       // consume the length byte
#ifdef QT_DEBUG
            qDebug() << "Expected packet length:" << m_expectedLength;
#endif
        }

        // Phase 2: wait until we have the full payload.
        if (m_buffer.size() < m_expectedLength) return;

        // Extract exactly m_expectedLength bytes and dispatch.
        QByteArray packetData = m_buffer.left(m_expectedLength);
        m_buffer = m_buffer.mid(m_expectedLength);
        processPacket(packetData);

        m_expectedLength = -1;  // ready for the next packet's length byte
    }
}

// A socket-level error occurred (connection refused, network unreachable, etc.).
void ICBClient::onError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    emit errorOccurred(m_socket->errorString());
}

// ---------------------------------------------------------------------------
// Packet dispatcher
// ---------------------------------------------------------------------------

// Interprets one complete packet (length byte already stripped) and emits
// the corresponding high-level signal(s).
//
// ICB command bytes used here:
//   'a' - login OK
//   'b' - open (public group) message
//   'c' - personal (private) message
//   'd' - status message (sub-typed: Arrive, Depart, Name, Boot, Pass, …)
//   'e' - error message
//   'f' - important message
//   'g' - server exit
//   'i' - command output  (sub-typed: wl = who-list entry, co = who summary)
//   'j' - protocol info   (sent by server immediately after TCP connect)
//   'k' - beep
//   'l' - ping from server (we must reply with 'm')
//   'm' - pong (server's reply to our 'n' keep-alive)
//   'n' - no-op
void ICBClient::processPacket(const QByteArray& packetData) {
#ifdef QT_DEBUG
    qDebug() << ">>> PROCESS PACKET: cmd="
             << (packetData.isEmpty() ? -1 : (int)packetData[0])
             << "hex=" << packetData.toHex();
#endif

    if (packetData.isEmpty()) return;

    // Strip the trailing NUL (if present) before parsing.
    QByteArray cleanData = packetData;
    if (!cleanData.isEmpty() && cleanData[cleanData.size() - 1] == '\0')
        cleanData.chop(1);

    char command        = cleanData[0];
    QByteArray data     = cleanData.mid(1);
    QList<QByteArray> args = parseArgs(data);

#ifdef QT_DEBUG
    qDebug() << "Received packet - Command:" << command
             << "Data length:" << data.size();
    for (int i = 0; i < args.size(); ++i)
        qDebug() << "  Arg" << i << ":" << args[i];
#endif

    switch (command) {
        // 'a' - Login acknowledged.  Start the keep-alive timer.
        case 'a':
            if (m_state != LoggedIn) {
                m_state = LoggedIn;
                m_pingTimer->start();
                m_manualWhoRequest = false;
                emit connectionStateChanged(m_state);
                emit loggedIn();
            }
            // Some servers include a welcome message in args[0].
            if (args.size() > 0 && !args[0].isEmpty())
                emit systemMessageReceived("Server: " + QString::fromUtf8(args[0]));
            break;

        // 'b' - Open (public group) message.
        // Two args: sender nick and message text.  Single-arg form is used for
        // server MOTD / welcome lines.  Our own echoed messages are filtered out
        // because the session layer already displays them on send.
        case 'b':
            if (args.size() >= 2) {
                QString sender  = QString::fromUtf8(args[0]);
                QString message = QString::fromUtf8(args[1]);
                if (sender.compare(m_nickname, Qt::CaseInsensitive) != 0)
                    emit messageReceived(sender, message);
            } else if (args.size() >= 1) {
                // Single-arg 'b' - server welcome or MOTD line.
                emit systemMessageReceived(QString::fromUtf8(args[0]));
            }
            break;

        // 'c' - Personal (private) message.  args[0]=from, args[1]=text.
        case 'c':
            if (args.size() >= 2)
                emit personalMessageReceived(QString::fromUtf8(args[0]),
                                             QString::fromUtf8(args[1]));
            break;

        // 'd' - Status message.  args[0] is the sub-type string, args[1] is the
        // human-readable status text.  Each sub-type maps to one or more signals.
        case 'd':
            if (args.size() >= 2) {
                QString statusType = QString::fromUtf8(args[0]);
                QString statusMsg  = QString::fromUtf8(args[1]);

                // "You are now in group X" - group switch confirmed by server.
                // Emit groupChanged and schedule an automatic /who to repopulate
                // the user list (delayed 100 ms to let the server settle).
                if (statusType == "Status" &&
                    statusMsg.startsWith("You are now in group ")) {
                    const int prefixLen =
                        static_cast<int>(QString("You are now in group ").length());
                    int end = statusMsg.indexOf(' ', prefixLen);
                    if (end == -1) end = statusMsg.length();
                    QString newGroup = statusMsg.mid(prefixLen, end - prefixLen);

                    if (newGroup != m_currentGroup) {
                        m_currentGroup = newGroup;
                        emit groupChanged(m_currentGroup);
                        emit systemMessageReceived(
                            QString("Joined group: %1").arg(m_currentGroup));
                        // Repopulate user list after a short delay.
                        QTimer::singleShot(100, this, [this]() {
                            if (m_state == LoggedIn)
                                sendCommand("w", m_currentGroup, false);
                        });
                    }
                }
                // "Arrive" / "Sign-on" - a user joined the group.
                else if (statusType == "Arrive" || statusType == "Sign-on") {
                    static const QRegularExpression re(
                        "^([^ ]+) \\(([^@]+)@([^)]+)\\)");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit userJoined(match.captured(1), match.captured(2),
                                        match.captured(3), false);
                    emit systemMessageReceived(statusMsg);
                }
                // "Depart" - a user left the group voluntarily.
                else if (statusType == "Depart") {
                    static const QRegularExpression re(
                        "^([^ ]+) \\(([^@]+)@([^)]+)\\)");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit userLeft(match.captured(1), QString());
                    emit systemMessageReceived(statusMsg);
                }
                // "Sign-off" - a user disconnected (quit the server entirely).
                // Parse the nick and optional reason from the status text.
                else if (statusType == "Sign-off") {
                    int space = statusMsg.indexOf(' ');
                    QString nick   = (space != -1) ? statusMsg.left(space) : statusMsg;
                    QString reason;
                    if (space != -1) {
                        int parenOpen  = statusMsg.indexOf('(', space);
                        int parenClose = statusMsg.indexOf(')', parenOpen > -1 ? parenOpen : space);
                        if (parenOpen != -1 && parenClose != -1) {
                            reason = statusMsg.mid(parenClose + 1).trimmed();
                            if (reason.endsWith('.')) reason.chop(1);
                        }
                    }
                    emit userLeft(nick, reason);
                    emit systemMessageReceived(statusMsg);
                }
                // "Name" - a user changed their nickname.
                // Format: "oldnick changed nickname to newnick"
                else if (statusType == "Name") {
                    QStringList parts = statusMsg.split(' ');
                    if (parts.size() >= 5 && parts[1] == "changed" &&
                        parts[2] == "nickname" && parts[3] == "to") {
                        QString oldNick = parts[0];
                        QString newNick = parts[4];
                        emit userChangedNick(oldNick, newNick);
                        if (oldNick == m_nickname)
                            m_nickname = newNick;  // keep our own nick up to date
                    }
                    emit systemMessageReceived(statusMsg);
                }
                // "Boot" - a user was removed from the group by the moderator.
                // Format: "nick was booted." or "nick was booted by moderator."
                else if (statusType == "Boot") {
                    static const QRegularExpression re(
                        "^(\\S+) was booted(?:\\.| by (\\S+)\\.?)$");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch()) {
                        emit userBooted(match.captured(1), match.captured(2));
                        emit userLeft(match.captured(1), "booted");
                    }
                    emit systemMessageReceived(statusMsg);
                }
                // "Pass" - moderator privilege transferred or granted.
                // Two formats:
                //   "A has passed moderation to B"  --> moderatorPassed
                //   "A is now mod."                 --> moderatorGranted
                else if (statusType == "Pass") {
                    static const QRegularExpression re1(
                        "^(\\S+) has passed moderation to (\\S+)$");
                    static const QRegularExpression re2(
                        "^(\\S+) is now mod\\.?$");
                    QRegularExpressionMatch match;
                    if ((match = re1.match(statusMsg)).hasMatch())
                        emit moderatorPassed(match.captured(1), match.captured(2));
                    else if ((match = re2.match(statusMsg)).hasMatch())
                        emit moderatorGranted(match.captured(1));
                    emit systemMessageReceived(statusMsg);
                }
                // "Change" - the group was renamed.
                // Two formats:
                //   "Group is now named newname"       (server-initiated)
                //   "nick renamed group to newname"    (moderator-initiated)
                // Note: some servers append a spurious '.' to the new name;
                // strip it if present.
                else if (statusType == "Change") {
                    static const QRegularExpression re1(
                        "^Group is now named (\\S+)$");
                    static const QRegularExpression re2(
                        "^(\\S+) renamed group to (\\S+)$");
                    QRegularExpressionMatch match;
                    QString oldName = m_currentGroup;
                    if ((match = re1.match(statusMsg)).hasMatch()) {
                        QString newName = match.captured(1);
                        if (newName.endsWith('.')) newName.chop(1);
                        m_currentGroup = newName;
                        emit groupRenamed(oldName, newName, QString());
                    } else if ((match = re2.match(statusMsg)).hasMatch()) {
                        QString newName = match.captured(2);
                        if (newName.endsWith('.')) newName.chop(1);
                        m_currentGroup = newName;
                        emit groupRenamed(oldName, newName, match.captured(1));
                    } else {
                        emit systemMessageReceived(statusMsg);
                    }
                }
                // "Topic" - group topic changed.
                // Format: "nick changed the topic to \"new topic\""
                else if (statusType == "Topic") {
                    static const QRegularExpression re(
                        "^(\\S+) changed the topic to \"(.*)\"$");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit topicChanged(match.captured(1), match.captured(2));
                    emit systemMessageReceived(statusMsg);
                }
                // "RSVP" - we received an invitation to join another group.
                // Format: "You are invited to group X [by Y]."
                else if (statusType == "RSVP") {
                    static const QRegularExpression re(
                        "^You are invited to group (\\S+)(?: by (\\S+))?\\.?$");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit inviteReceived(match.captured(1), match.captured(2));
                    emit systemMessageReceived(statusMsg);
                }
                // "Idle-Mod" - moderator removed due to idling (the "piano" event).
                // Format: "A piano suddenly falls on nick, dislodging moderatorship…"
                else if (statusType == "Idle-Mod") {
                    static const QRegularExpression re("falls on (\\S+),");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit moderatorLost(match.captured(1));
                    emit systemMessageReceived(statusMsg);
                }
                // "Timeout" - a user became moderator after the previous one timed out.
                // Format: "nick is now mod."
                else if (statusType == "Timeout") {
                    static const QRegularExpression re("^(\\S+) is now mod\\.?$");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    if (match.hasMatch())
                        emit moderatorGranted(match.captured(1));
                    emit systemMessageReceived(statusMsg);
                }
                // "Mod" - direct moderator status message.
                // Try a clean regex first; fall back to the first word.
                else if (statusType == "Mod") {
                    static const QRegularExpression re(
                        "^(\\S+) is the active moderator");
                    QRegularExpressionMatch match = re.match(statusMsg);
                    QString nick = match.hasMatch()
                        ? match.captured(1)
                        : statusMsg.section(' ', 0, 0);  // fallback: first word
                    if (!nick.isEmpty())
                        emit moderatorGranted(nick);
                    emit systemMessageReceived(statusMsg);
                }
                // Anything else: forward as a plain system message.
                else {
                    emit systemMessageReceived(
                        QString("%1: %2").arg(statusType, statusMsg));
                }
            }
            break;

        // 'e' - Server error message.  Display in red via errorOccurred.
        case 'e':
            if (args.size() >= 1)
                emit errorOccurred(QString::fromUtf8(args[0]));
            break;

        // 'f' - Important (broadcast) message from the server.
        case 'f':
            if (args.size() >= 2)
                emit systemMessageReceived(
                    QString("IMPORTANT: %1: %2")
                        .arg(QString::fromUtf8(args[0]),
                             QString::fromUtf8(args[1])));
            break;

        // 'g' - Server is shutting down.
        case 'g':
            emit systemMessageReceived("Server exit");
            disconnectFromServer();
            break;

        // 'i' - Command output.  Used for /who results and other responses.
        //
        // Sub-types (args[0]):
        //   "wl"  - one who-list entry; accumulate into m_whoEntries and emit userJoined
        //           for the current group so the sidebar stays in sync.
        //   "co"  - command output line; a "Group:" line is always forwarded as a
        //           system message; a "Total:" line terminates the /who response.
        case 'i': {
            QString cmd = QString::fromUtf8(args[0]);
#ifdef QT_DEBUG
            qDebug() << "   ICB i-command:" << cmd << "argc=" << args.size();
#endif
            if (cmd == "wl") {
                // who-list entry: args indices:
                //   [0]=cmd  [1]=flags  [2]=nick  [3]=idle  [4]=?
                //   [5]=signon  [6]=ident  [7]=host
                if (args.size() >= 8) {
                    WhoEntry entry;
                    entry.isModerator  = QString::fromUtf8(args[1]).contains('m');
                    entry.nick         = QString::fromUtf8(args[2]);
                    entry.idleSeconds  = QString::fromUtf8(args[3]).toInt();
                    entry.signonTime   = QString::fromUtf8(args[5]).toLongLong();
                    entry.ident        = QString::fromUtf8(args[6]);
                    entry.host         = QString::fromUtf8(args[7]);
                    m_whoEntries.append(entry);

                    // Emit userJoined for the current group so the sidebar is
                    // populated in real time.  Skip for other groups (foreign /who).
                    if (m_requestedWhoGroup == m_currentGroup)
                        emit userJoined(entry.nick, entry.ident, entry.host,
                                        entry.isModerator);
                }
            } else if (cmd == "co") {
                // Command output line (one per args[1..n]).
                for (int i = 1; i < args.size(); ++i) {
                    QString line = QString::fromUtf8(args[i]);
                    if (line.startsWith("Group:", Qt::CaseInsensitive)) {
                        // Always forward the "Group:" header line so ICBSession
                        // can parse flags and topic from it.
                        emit systemMessageReceived(line);
                    } else if (line.startsWith("Total:", Qt::CaseInsensitive)) {
                        // "Total:" is the last line of every /who response.
                        // Stop the timeout timer, display the accumulated table,
                        // and emit the Total line itself.
                        m_whoTimeout->stop();
                        printWhoTable();
                        m_whoEntries.clear();
                        emit systemMessageReceived(line);
                    } else if (!line.trimmed().isEmpty()) {
                        emit systemMessageReceived(line);
                    }
                }
            }
#ifdef QT_DEBUG
            else {
                qDebug() << "   >>> unknown i-subcommand:" << cmd;
            }
#endif
            break;
        }

        // 'j' - Protocol info sent by the server immediately after TCP connect.
        // args[0]=protocol, args[1]=host, args[2]=server name.
        // Receiving this is the signal to send our login packet.
        case 'j':
            m_protoTimer->stop();
            if (args.size() >= 3) {
                emit systemMessageReceived(
                    QString("Protocol: %1, Host: %2, Server: %3")
                        .arg(QString::fromUtf8(args[0]),
                             QString::fromUtf8(args[1]),
                             QString::fromUtf8(args[2])));
                if (m_state == Connected) {
                    sendLoginPacket();
                    m_state = LoggingIn;
                    emit connectionStateChanged(m_state);
                    emit systemMessageReceived("Logging in...");
                }
            }
            break;

        // 'k' - Beep from another user.
        case 'k':
            if (args.size() >= 1)
                emit systemMessageReceived(
                    "Beep from: " + QString::fromUtf8(args[0]));
            break;

        // 'l' - Server ping.  Must reply with an 'm' pong containing the same payload.
        case 'l':
            if (args.size() >= 1) {
                QList<QByteArray> pongArgs;
                pongArgs.append(args[0]);
                sendRawPacket(buildPacket('m', pongArgs));
            }
            break;

        // 'm' - Pong response to our 'n' keep-alive.  No user-visible output needed.
        case 'm':
#ifdef QT_DEBUG
            qDebug() << "Received pong:"
                     << (args.size() >= 1 ? args[0] : QByteArray());
#endif
            break;

        // 'n' - No-op.  The server may send these; we simply ignore them.
        case 'n':
            break;

        default:
#ifdef QT_DEBUG
            qDebug() << "Unknown command:" << command
                     << "data:" << data.toHex();
#endif
            emit systemMessageReceived(
                QString("Unknown command: %1").arg(command));
            break;
    }
}

// ---------------------------------------------------------------------------
// /who helpers
// ---------------------------------------------------------------------------

// Formats a duration in seconds as "1d2h3m4s".  Returns "0s" for zero.
QString ICBClient::formatIdle(int seconds) const {
    if (seconds < 0) seconds = 0;
    int days    = seconds / 86400;
    int hours   = (seconds % 86400) / 3600;
    int minutes = (seconds % 3600)  / 60;
    int secs    = seconds % 60;

    QString result;
    if (days    > 0) result += QString::number(days)    + "d";
    if (hours   > 0) result += QString::number(hours)   + "h";
    if (minutes > 0) result += QString::number(minutes) + "m";
    if (secs    > 0) result += QString::number(secs)    + "s";
    if (result.isEmpty()) result = "0s";
    return result;
}

// Converts a Unix timestamp into a human-readable local date/time string.
QString ICBClient::formatSignon(qint64 timestamp) const {
    return QDateTime::fromSecsSinceEpoch(timestamp)
        .toLocalTime()
        .toString("yyyy-MM-dd hh:mm:ss");
}

// Called when the /who timeout fires (no "Total:" line received within 500 ms).
// If the request was manual, print whatever entries arrived; otherwise discard
// them silently (the automatic /who on login/join only needs userJoined signals,
// which were emitted per-entry as they arrived).
void ICBClient::onWhoTimeout() {
#ifdef QT_DEBUG
    qDebug() << ">>> WHO TIMEOUT manual=" << m_manualWhoRequest
             << "entries=" << m_whoEntries.size();
#endif
    if (!m_whoEntries.isEmpty()) {
        if (m_manualWhoRequest)
            printWhoTable();  // also clears m_whoEntries and resets the flag
        else
            m_whoEntries.clear();
    } else {
        m_manualWhoRequest = false;
    }
}

// Formats m_whoEntries as a column-aligned table and emits each row as a
// systemMessageReceived line so it appears in the chat history.
// Column widths are computed dynamically to fit the longest value in each column.
// This copies the ICBM client /who output format:
//
//   Nickname   Idle  Sign-On             Account
//   ──────────────────────────────────────────────
//   @moderator 0s    2026-03-07 09:00:00 mod@host
//     alice    5m    2026-03-07 10:30:00 alice@example.com
void ICBClient::printWhoTable() {
    if (m_whoEntries.isEmpty()) return;

    int maxNick = 0, maxIdle = 0, maxSignOn = 0, maxAccount = 0;
    QList<QStringList> rows;

    for (const WhoEntry& e : m_whoEntries) {
        // Moderators get "@nick"; others get "  nick" (two spaces of indent).
        QString displayNick = e.isModerator ? "  @" + e.nick : "  " + e.nick;
        QString idleStr     = formatIdle(e.idleSeconds);
        QString signonStr   = formatSignon(e.signonTime);
        QString account     = e.ident + "@" + e.host;

        maxNick    = qMax(maxNick,    displayNick.length());
        maxIdle    = qMax(maxIdle,    idleStr.length());
        maxSignOn  = qMax(maxSignOn,  signonStr.length());
        maxAccount = qMax(maxAccount, account.length());

        rows.append({displayNick, idleStr, signonStr, account});
    }

    // Header and separator.
    QString header = QString("%1 %2 %3 %4")
        .arg("  Nickname", -maxNick)
        .arg("Idle",       -maxIdle)
        .arg("Sign-On",    -maxSignOn)
        .arg("Account",    -maxAccount);
    emit systemMessageReceived(header);
    emit systemMessageReceived(QString("-").repeated(header.length()));

    for (const QStringList& row : rows) {
        emit systemMessageReceived(
            QString("%1 %2 %3 %4")
                .arg(row[0], -maxNick)
                .arg(row[1], -maxIdle)
                .arg(row[2], -maxSignOn)
                .arg(row[3], -maxAccount));
    }

    m_whoEntries.clear();
    m_manualWhoRequest = false;
}
