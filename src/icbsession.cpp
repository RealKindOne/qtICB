#include "icbsession.h"
#include <QDebug>
#include <QRegularExpression>
#include <QTimer>
#include "commandhandler.h"
#include "userlist.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ICBSession::ICBSession(const QString& basePath, QObject* parent)
    : QObject(parent), m_client(this), m_logger(basePath, this), m_groupLoggingEnabled(false) {
    // Wire up every ICBClient signal to the corresponding handler here.
    // ICBClient deals with raw packet parsing; ICBSession adds state management,
    // logging, and higher-level formatting on top.
    connect(&m_client, &ICBClient::connectionStateChanged, this, &ICBSession::onClientStateChanged);
    connect(&m_client, &ICBClient::errorOccurred,          this, &ICBSession::onClientErrorOccurred);
    connect(&m_client, &ICBClient::groupChanged,           this, &ICBSession::onClientGroupChanged);
    connect(&m_client, &ICBClient::groupRenamed,           this, &ICBSession::onClientGroupRenamed);
    connect(&m_client, &ICBClient::inviteReceived,         this, &ICBSession::onClientInviteReceived);
    connect(&m_client, &ICBClient::loggedIn,               this, &ICBSession::onClientLoggedIn);
    connect(&m_client, &ICBClient::messageReceived,        this, &ICBSession::onClientMessageReceived);
    connect(&m_client, &ICBClient::moderatorGranted,       this, &ICBSession::onClientModeratorGranted);
    connect(&m_client, &ICBClient::moderatorLost,          this, &ICBSession::onClientModeratorLost);
    connect(&m_client, &ICBClient::moderatorPassed,        this, &ICBSession::onClientModeratorPassed);
    connect(&m_client, &ICBClient::personalMessageReceived,this, &ICBSession::onClientPersonalMessageReceived);
    connect(&m_client, &ICBClient::systemMessageReceived,  this, &ICBSession::onClientSystemMessageReceived);
    connect(&m_client, &ICBClient::topicChanged,           this, &ICBSession::onClientTopicChanged);
    connect(&m_client, &ICBClient::userBooted,             this, &ICBSession::onClientUserBooted);
    connect(&m_client, &ICBClient::userChangedNick,        this, &ICBSession::onClientUserChangedNick);
    connect(&m_client, &ICBClient::userJoined,             this, &ICBSession::onClientUserJoined);
    connect(&m_client, &ICBClient::userLeft,               this, &ICBSession::onClientUserLeft);
}

ICBSession::~ICBSession() {
    disconnectFromServer();
}

// ---------------------------------------------------------------------------
// Connection control
// ---------------------------------------------------------------------------

ICBClient::ConnectionState ICBSession::state() const {
    return m_client.getState();
}

// Initiates a TCP connection to the given host and port, then begins the ICB
// login handshake with the supplied nickname and initial group.
// Does nothing if a connection is already in progress or established.
void ICBSession::connectToServer(const QString& host, quint16 port,
                                 const QString& nickname, const QString& group) {
    if (m_client.getState() != ICBClient::Disconnected)
        return;

    m_host         = host;
    m_port         = port;
    m_nickname     = nickname;
    m_group        = group.isEmpty() ? "icb" : group;
    m_currentGroup = m_group;  // assume we'll join this group until told otherwise

    m_logger.setGroup(m_currentGroup);
    m_groupLoggingEnabled = false;  // will be enabled after we see the "Group:" /who line
    m_topic.clear();
    m_userList.clear();

    m_client.connectToServer(host, port, nickname, group);
}

void ICBSession::disconnectFromServer() {
    m_client.disconnectFromServer();
}

bool ICBSession::isConnected() const {
    return m_client.getState() != ICBClient::Disconnected;
}

bool ICBSession::isLoggedIn() const {
    return m_client.getState() == ICBClient::LoggedIn;
}

// ---------------------------------------------------------------------------
// Message sending
// ---------------------------------------------------------------------------

// Sends a public message to the current group, logs it, and echoes it back
// via selfMessageSent so the UI can display it immediately without waiting
// for the server to echo it back.
void ICBSession::sendMessage(const QString& text) {
    if (m_client.getState() != ICBClient::LoggedIn || text.isEmpty())
        return;
    m_client.sendMessage(text);
    m_logger.logGroupMessage(QString("<%1> %2").arg(m_nickname, text));
    emit selfMessageSent(text);
}

// Sends a private message to 'to', logs it, and emits selfPrivateMessageSent
// so the UI can reflect the sent message in the private chat window.
void ICBSession::sendPrivateMessage(const QString& to, const QString& text) {
    if (m_client.getState() != ICBClient::LoggedIn || to.isEmpty() || text.isEmpty())
        return;
    m_client.sendPrivateMessage(to, text);
    m_logger.logPrivateMessage("out", m_nickname, to, text);
    emit selfPrivateMessageSent(to, text);
}

// Sends a raw ICB command packet like "w" for /who, "topic" for /topic.
// 'manual' marks whether the command came from the user (true) or was
// generated automatically by the session layer (false).
void ICBSession::sendCommand(const QString& command, const QString& arg, bool manual) {
    if (m_client.getState() != ICBClient::LoggedIn)
        return;
    m_client.sendCommand(command, arg, manual);
}

// ---------------------------------------------------------------------------
// User input routing
// ---------------------------------------------------------------------------

// Parses a line typed by the user.  CommandHandler handles slash-commands
// (/msg, /topic, /query, /quit, etc.).  Plain text goes out as a public
// group message.
void ICBSession::processUserInput(const QString& input) {
    CommandHandler::Callbacks cb;

    cb.sendPrivate = [this](const QString& to, const QString& msg) {
        sendPrivateMessage(to, msg);
    };
    cb.sendRaw = [this](const QString& cmd, const QString& arg) {
        sendCommand(cmd, arg, true);
    };
    cb.openPrivateChat = [this](const QString& nick) {
        emit openPrivateChatRequested(nick);
    };
    cb.disconnect = [this]() {
        disconnectFromServer();
    };
    cb.quitApplication = []() {};  // handled by the UI layer

    if (!CommandHandler::handle(input, cb)) {
        // Not a recognized command - send as a public group message.
        sendMessage(input.trimmed());
    }
}

// ---------------------------------------------------------------------------
// ICBClient signal handlers
// ---------------------------------------------------------------------------

// Connection state changed.  On disconnect, tear down per-session state so
// it isn't accidentally displayed if the user reconnects later.
void ICBSession::onClientStateChanged(ICBClient::ConnectionState state) {
    if (state == ICBClient::Disconnected) {
        m_userList.clear();
        m_groupLoggingEnabled = false;
        m_pendingModerators.clear();
        m_logger.closeAll();
    }
    emit connectionStateChanged(state);
}

void ICBSession::onClientMessageReceived(const QString& sender, const QString& message) {
    m_logger.logGroupMessage(QString("<%1> %2").arg(sender, message));
    emit messageReceived(sender, message);
}

// All server/status text passes through handleSystemMessage, which does
// special-case parsing before calling emit systemMessageReceived.
void ICBSession::onClientSystemMessageReceived(const QString& message) {
    handleSystemMessage(message);
}

void ICBSession::onClientErrorOccurred(const QString& error) {
    emit errorOccurred(error);
}

void ICBSession::onClientPersonalMessageReceived(const QString& from, const QString& message) {
    m_logger.logPrivateMessage("in", from, QString(), message);
    emit personalMessageReceived(from, message);
}

// Login was acknowledged by the server.  Send an automatic /who for the
// current group so both UIs receive the initial group flags, topic, and user
// list without each UI having to issue it independently.
void ICBSession::onClientLoggedIn() {
    m_client.sendCommand("w", m_currentGroup, false);
    emit loggedIn();
}

// A new user appeared in the group.  Add them to the user list, then apply
// any pending moderator status that arrived before the join packet.
void ICBSession::onClientUserJoined(const QString& nick, const QString& ident,
                                    const QString& host, bool isModerator) {
#ifdef QT_DEBUG
    qDebug() << "onClientUserJoined:" << nick << "ident=" << ident
             << "host=" << host << "isModerator=" << isModerator;
#endif
    m_userList.addUser(nick, ident, host, isModerator);

    // The moderatorGranted packet sometimes races ahead of the join packet.
    // If that happened, we stashed the nick in m_pendingModerators; apply now.
    if (m_pendingModerators.contains(nick)) {
#ifdef QT_DEBUG
        qDebug() << "Applying pending moderator for" << nick;
#endif
        m_userList.setModerator(nick, true);
        m_pendingModerators.remove(nick);
    }

    emit userJoined(nick, ident, host, isModerator);
}

void ICBSession::onClientUserLeft(const QString& nick, const QString& reason) {
    m_userList.removeUser(nick);
    emit userLeft(nick, reason);
}

// A user changed their nick. Preserve their moderator status across the
// rename, and update our own stored nickname if it was us.
void ICBSession::onClientUserChangedNick(const QString& oldNick, const QString& newNick) {
    bool wasModerator = false;
    for (const UserInfo& u : m_userList.users()) {
        if (u == oldNick) {
            wasModerator = u.isModerator;
            break;
        }
    }
    m_userList.updateUser(oldNick, newNick, wasModerator);
    emit userChangedNick(oldNick, newNick);

    if (oldNick == m_nickname)
        m_nickname = newNick;
}

// A live topic change packet arrived (not from /who output).
// Update m_topic and emit whoInfoReceived so both UIs update their topic
// display through the same code path used for /who output.
void ICBSession::onClientTopicChanged(const QString& nick, const QString& topic) {
    m_topic = topic;
    emit whoInfoReceived(m_groupFlags, m_topic);
    emit topicChanged(nick, topic);
}

void ICBSession::onClientInviteReceived(const QString& group, const QString& inviter) {
    emit inviteReceived(group, inviter);
}

void ICBSession::onClientModeratorPassed(const QString& from, const QString& to) {
    m_userList.setModerator(from, false);
    m_userList.setModerator(to, true);
    emit moderatorPassed(from, to);
}

// A moderator-granted packet arrived.  If the user is already in the list,
// mark them immediately; otherwise stash the nick until the join arrives.
void ICBSession::onClientModeratorGranted(const QString& nick) {
#ifdef QT_DEBUG
    qDebug() << "onClientModeratorGranted:" << nick
             << "userList contains?" << m_userList.contains(nick);
#endif
    if (m_userList.contains(nick)) {
        m_userList.setModerator(nick, true);
    } else {
#ifdef QT_DEBUG
        qDebug() << "Adding to pendingModerators:" << nick;
#endif
        m_pendingModerators.insert(nick);
    }
    emit moderatorGranted(nick);
}

// A group rename packet arrived.
// Both the old and new names are logged, a human-readable message is emitted
// via systemMessageReceived (so the UIs don't have to format it themselves),
// and groupRenamed is emitted for any listeners that need the raw names.
void ICBSession::onClientGroupRenamed(const QString& oldName, const QString& newName,
                                      const QString& byNick) {
    m_currentGroup = newName;

    // Log the event under both the old and new group names so the history
    // files on disk are consistent.
    m_logger.setGroup(oldName);
    m_logger.logSystemMessage(
        QString("Group renamed to: %1 by %2").arg(newName, byNick));
    m_logger.setGroup(newName);
    m_logger.logSystemMessage(
        QString("Group renamed from: %1 by %2").arg(oldName, byNick));

    // Build a display-ready string.  Both UIs previously built this
    // independently; now it happens once here.
    QString msg = byNick.isEmpty()
        ? QString("Group renamed: %1 -> %2").arg(oldName, newName)
        : QString("Group renamed by %1: %2 -> %3").arg(byNick, oldName, newName);
    emit systemMessageReceived(msg);
    emit groupRenamed(oldName, newName, byNick);
}

void ICBSession::onClientUserBooted(const QString& nick, const QString& by) {
    m_userList.removeUser(nick);
    emit userBooted(nick, by);
}

// The user joined a new group (or the server moved them).
// Clear all group-specific state and emit whoInfoReceived with empty strings
// so both UIs immediately blank their flags/topic display before the next
// /who response arrives with fresh data.
void ICBSession::onClientGroupChanged(const QString& newGroup) {
    m_currentGroup = newGroup;
    m_groupFlags.clear();
    m_topic.clear();
    m_userList.clear();
    m_logger.setGroup(newGroup);
    m_groupLoggingEnabled = true;
    emit whoInfoReceived(m_groupFlags, m_topic);  // empty strings reset the UI
    emit groupChanged(newGroup);
}

void ICBSession::onClientModeratorLost(const QString& nick) {
    m_userList.setModerator(nick, false);
    emit moderatorLost(nick);
}

// ---------------------------------------------------------------------------
// System-message parsing
// ---------------------------------------------------------------------------

// Called for every system message before it is forwarded to the UIs.
//
// Special case: the "Group: groupname (flags) Topic: ..." line that appears
// in /who output.  This is the single point where group flags and topic are
// extracted; the results are stored on the session and broadcast via
// whoInfoReceived so neither UI duplicates the regex logic.
//
// All other messages are logged (if logging is active) and forwarded as-is.
void ICBSession::handleSystemMessage(const QString& message) {
    if (message.startsWith("Group:")) {
        // Three regex's cover the three pieces of data on this line:
        //   "Group: icb (m) Topic: Welcome to ICB"
        //   ─────   ───  ───        ────────────────
        //   name    flags (opt)     topic (opt)
        static QRegularExpression reGroupLine("^Group:\\s+(\\S+)");
        static QRegularExpression reFlags("Group:\\s+\\S+\\s+(\\([^)]+\\))");
        static QRegularExpression reTopic("Topic:\\s*(.*?)\\s*$");

        // On initial login the logger hasn't been pointed at the right file
        // yet. Use the group name from this line to enable logging.
        if (!m_groupLoggingEnabled) {
            QRegularExpressionMatch groupMatch = reGroupLine.match(message);
            if (groupMatch.hasMatch()) {
                m_currentGroup = groupMatch.captured(1);
                m_logger.setGroup(m_currentGroup);
                m_groupLoggingEnabled = true;
            }
        }

        QRegularExpressionMatch flagMatch = reFlags.match(message);
        m_groupFlags = flagMatch.hasMatch() ? flagMatch.captured(1) : QString();

        QRegularExpressionMatch topicMatch = reTopic.match(message);
        if (topicMatch.hasMatch())
            m_topic = topicMatch.captured(1);

        // Notify both UIs with the parsed values.
        emit whoInfoReceived(m_groupFlags, m_topic);
        // Fall through to also log and emit the raw "Group: ..." line below.
    }

    // Log system messages once logging is active (i.e. after the first
    // "Group:" line has been seen), then forward to the UI.
    if (m_groupLoggingEnabled)
        m_logger.logSystemMessage(message);

    emit systemMessageReceived(message);
}
