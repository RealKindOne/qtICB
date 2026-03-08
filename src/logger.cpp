#include "logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QTextStream>
#include "formatting.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

// Create the Logger and ensure the logs/ sub-directory exists.
// mkpath() is a no-op if the directory already exists, so this is safe to
// call unconditionally every time the application starts.
Logger::Logger(const QString& basePath, QObject* parent)
    : QObject(parent), m_basePath(basePath) {
    QDir dir(m_basePath + "/logs");
    if (!dir.exists())
        dir.mkpath(".");
}

// Close all open file handles before the object is destroyed.
Logger::~Logger() {
    closeAll();
}

// ---------------------------------------------------------------------------
// Group channel management
// ---------------------------------------------------------------------------

// Switch the active group.  The old group's file is closed immediately so we
// don't hold open a handle for a group we are no longer in.  The new group's
// file is opened lazily on the first message to avoid creating empty log files
// for groups the user only briefly passes through.
void Logger::setGroup(const QString& group) {
    if (m_currentGroup == group) return;
    if (!m_currentGroup.isEmpty())
        closeChannel(m_currentGroup);
    m_currentGroup = group;
}

// ---------------------------------------------------------------------------
// Filename helpers
// ---------------------------------------------------------------------------

// Replace every character that is not [a-zA-Z0-9_-] with '_'.
// This makes group names like "#icb" and nicks like "user@host" safe to use
// as part of a filesystem filename on any platform.
QString Logger::sanitizeName(const QString& name) const {
    QString safe = name;
    safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    return safe;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// Write a single timestamped line to an already-open file.
// Format: "[hh:mm:ss] line\n"
// QTextStream::flush() is called immediately so the file is readable even if
// the application crashes or is killed without a clean shutdown.
void Logger::writeToFile(QFile* file, const QString& line) {
    if (!file || !file->isOpen()) return;
    QTextStream out(file);
    out << "[" << currentTimestamp() << "] " << line << "\n";
    out.flush();
}

// Return the open QFile* for 'channel', creating and opening a new file if:
//   (a) this channel has never been logged this session, or
//   (b) the date has rolled over since the file was last opened.
//
// Files are named:  <basePath>/logs/<sanitized_channel>_<yyyyMMdd>.log
// They are opened in Append mode so existing content is preserved across
// application restarts on the same day.
//
// Returns nullptr if the file cannot be opened.
QFile* Logger::getChannelFile(const QString& channel, bool isGroup) {
    if (channel.isEmpty()) return nullptr;

    QDate today = QDate::currentDate();

    // Check for an existing open handle for today.
    auto it = m_channels.find(channel);
    if (it != m_channels.end()) {
        LogChannel& ch = it.value();
        if (ch.date == today)
            return ch.file;  // reuse today's open handle
        // The date rolled over: close the old file and fall through to open a new one.
        m_channels.erase(it);
    }

    // Build the filename and open in append mode.
    QString fileName = QString("%1/logs/%2_%3.log")
        .arg(m_basePath)
        .arg(sanitizeName(channel))
        .arg(today.toString("yyyyMMdd"));

    QFile* file = new QFile(fileName, this);
    if (file->open(QIODevice::Append | QIODevice::Text)) {
#ifdef QT_DEBUG
        qDebug() << "Logger: opened file" << fileName;
#endif
        LogChannel ch;
        ch.file    = file;
        ch.date    = today;
        ch.name    = channel;
        ch.isGroup = isGroup;
        m_channels.insert(channel, ch);
        return file;
    } else {
        qWarning() << "Logger: failed to open" << fileName
                   << "-" << file->errorString();
        delete file;
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Close helpers
// ---------------------------------------------------------------------------

// Close and remove the channel entry identified by name.
void Logger::closeChannel(const QString& channel) {
    auto it = m_channels.find(channel);
    if (it != m_channels.end()) {
        closeChannel(it.value());
        m_channels.erase(it);
    }
}

// Close and free the file handle inside a LogChannel struct.
// Sets file to nullptr so double-close is harmless.
void Logger::closeChannel(LogChannel& channel) {
    if (channel.file && channel.file->isOpen()) {
        channel.file->close();
        delete channel.file;
        channel.file = nullptr;
    }
}

// Close every open channel file and reset all state.
// Called on disconnect and from the destructor.
void Logger::closeAll() {
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it)
        closeChannel(it.value());
    m_channels.clear();
    m_currentGroup.clear();
}

// ---------------------------------------------------------------------------
// Public logging API
// ---------------------------------------------------------------------------

// Write a line to the current group log.
// 'message' should already be formatted "<nick> text" or "* status".
// Silently no-ops if no group is active (not yet connected to a group).
void Logger::logGroupMessage(const QString& message) {
    if (m_currentGroup.isEmpty()) return;
    QFile* file = getChannelFile(m_currentGroup, true);
    if (file) writeToFile(file, message);
}

// Write a system/status line to the current group log, prefixed with "* ".
// This matches the visual convention used in the chat display (system messages
// are shown without a nick, distinguished by the asterisk).
void Logger::logSystemMessage(const QString& message) {
    logGroupMessage("* " + message);
}

// Write a private message to a per-contact log file.
//
// The file is named after the other party:
//   direction == "in"  --> other party is 'from' (they sent to us)
//   direction == "out" --> other party is 'to'   (we sent to them)
//
// Both sides of the conversation go into the same file so the full exchange
// is readable in one place.  The line is formatted as "<from> message" so
// the direction is apparent from the nick.
void Logger::logPrivateMessage(const QString& direction, const QString& from,
                                const QString& to, const QString& message) {
    // Determine which nick names the per-contact file.
    QString other;
    if      (direction == "in")  other = from;
    else if (direction == "out") other = to;
    else
        return;  // unknown direction - silently ignore

    if (other.isEmpty()) return;

    QFile* file = getChannelFile(other, false);
    if (!file) return;

    writeToFile(file, QString("<%1> %2").arg(from, message));
}
