#ifndef LOGGER_H
#define LOGGER_H

#include <QDate>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QRegularExpression>

// Logger writes ICB chat history to plain-text log files on disk.
//
// File layout:
//   <basePath>/logs/<sanitized_name>_<yyyyMMdd>.log
//
// A new file is opened automatically whenever the date rolls over midnight,
// so each calendar day gets its own file per channel.  The basePath is
// normally QCoreApplication::applicationDirPath() so logs land next to the
// binary, making them easy to find.
//
// Two kinds of channels are logged independently:
//
//   Group log
//     All public messages and system/status lines for the current ICB group.
//     Controlled by setGroup() - only one group is active at a time.
//     Written by logGroupMessage() and logSystemMessage().
//
//   Private logs
//     One file per contact nick, shared between sent and received messages.
//     Written by logPrivateMessage().  The direction parameter ("in"/"out")
//     determines which nick names the file: incoming --> from, outgoing --> to.
//
// Each line written to disk is prefixed with a "[hh:mm:ss]" timestamp by
// writeToFile().  The caller passes the already-formatted message body.
//
// Channel files are kept open for the lifetime of the day to avoid the
// overhead of re-opening on every message.  All open files are flushed and
// closed by closeAll() (called on disconnect or destruction).
class Logger : public QObject {
    Q_OBJECT

  public:
    // basePath is the directory under which the "logs/" sub-directory is
    // created.  Typically QCoreApplication::applicationDirPath().
    explicit Logger(const QString& basePath, QObject* parent = nullptr);

    // Closes all open log files.
    ~Logger();

    // Group logging

    // Switch the active group channel.  If a different group was previously
    // active its log file is closed; the new group's file is opened on the
    // first message.  Calling with the same group name is a no-op.
    void setGroup(const QString& group);

    // Append a public message line to the current group log.
    // 'message' should be pre-formatted "<nick> text".
    void logGroupMessage(const QString& message);

    // Append a system/status line to the current group log, prefixed with "* ".
    // Examples: "* Connected to server", "* alice joined".
    void logSystemMessage(const QString& message);

    // Private (per-contact) logging

    // Append a private message to the per-contact log file.
    //   direction - "in"  --> file is named after 'from' (incoming message)
    //               "out" --> file is named after 'to'   (outgoing message)
    // The line is formatted as "<from> message".
    void logPrivateMessage(const QString& direction, const QString& from,
                           const QString& to, const QString& message);

    // Close and flush all open log files.  Called on disconnect so the OS
    // doesn't hold file handles open unnecessarily.
    void closeAll();

  private:
    // One open log file plus associated metadata.
    struct LogChannel {
        QFile*  file    = nullptr; // open file handle (nullptr if not yet opened)
        QDate   date;              // calendar date this file covers
        QString name;              // channel key (group name or contact nick)
        bool    isGroup;           // true for group logs, false for private logs
    };

    // Return the open QFile* for 'channel', opening a new file if needed
    // (first use, or the date has rolled over since last use).
    // Returns nullptr if the file could not be opened.
    QFile* getChannelFile(const QString& channel, bool isGroup);

    // Close and remove the channel entry by name.
    void closeChannel(const QString& channel);

    // Close and free the file handle inside a LogChannel struct.
    void closeChannel(LogChannel& channel);

    // Replace any character that is not alphanumeric, '_', or '-' with '_'
    // so channel names are safe to use as filesystem filenames.
    QString sanitizeName(const QString& name) const;

    // Write "[hh:mm:ss] line\n" to the already-open file and flush.
    void writeToFile(QFile* file, const QString& line);

    QString m_basePath;                      // root directory for "logs/" sub-folder
    QString m_currentGroup;                  // name of the currently active group channel
    QHash<QString, LogChannel> m_channels;   // all currently open channels, keyed by name
};

#endif  // LOGGER_H
