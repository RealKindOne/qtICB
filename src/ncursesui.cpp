// clang-format off
#include <locale.h>
#include <signal.h>
#include <unistd.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include "daychangenotifier.h"
#include "icbsession.h"
#include "ncursesui.h"
#include <QTimer>
#include "formatting.h"
// clang-format on

// Number of spaces used for the hanging indent on wrapped lines.
// A message like "<alice> hello world" that wraps will indent the continuation
// by this many spaces so the nick label stays clearly attached to the first line.
const int HANGING_INDENT = 4;

// ---------------------------------------------------------------------------
// Text layout helpers
// ---------------------------------------------------------------------------

// Splits 'text' into lines of at most 'width' characters.  The first line
// uses the full width; continuation lines are indented by 'hangingIndent'
// spaces so wrapped messages remain visually grouped.
static QStringList wrapText(const QString& text, int width, int hangingIndent = 0) {
    QStringList lines;
    int pos = 0;
    int len = text.length();
    bool firstLine = true;

    while (pos < len) {
        int lineWidth = firstLine ? width : width - hangingIndent;
        if (lineWidth <= 0) lineWidth = 1;
        int take = qMin(lineWidth, len - pos);
        QString line = text.mid(pos, take);
        if (!firstLine)
            line = QString(hangingIndent, ' ') + line;
        lines.append(line);
        pos += take;
        firstLine = false;
    }
    return lines;
}

// Returns the total number of terminal display lines that 'messages' will
// occupy when rendered inside a window of 'width' columns.  Used to calculate
// scroll limits and to decide whether "following" mode should auto-scroll.
static int totalDisplayLines(const QStringList& messages, int width, int hangingIndent = 0) {
    int total = 0;
    for (const QString& msg : messages) {
        int remaining = msg.length();
        bool firstLine = true;
        while (remaining > 0) {
            int lineWidth = firstLine ? width : width - hangingIndent;
            if (lineWidth <= 0) lineWidth = 1;
            remaining -= lineWidth;
            firstLine = false;
            total++;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

NcursesUI::NcursesUI(QObject* parent)
    : QObject(parent) {
    // The info timer ticks every second to keep the HH:MM clock in the
    // status bar current without blocking the event loop.
    m_infoTimer = new QTimer(this);
    connect(m_infoTimer, &QTimer::timeout, this, &NcursesUI::updateInfoBar);
}

NcursesUI::~NcursesUI() {
    // Free ncurses windows in reverse-creation order, then restore the
    // terminal to its normal state.
    if (m_topicWin) delwin(m_topicWin);
    if (m_infoWin)  delwin(m_infoWin);
    if (m_stdinNotifier) {
        m_stdinNotifier->setEnabled(false);
        delete m_stdinNotifier;
    }
    endwin();
}

// Initialize ncurses and Qt integration.  Must be called once before the
// Qt event loop starts.
void NcursesUI::start() {
    setlocale(LC_ALL, "");  // must precede initscr() to enable UTF-8 I/O
    initscr();
    if (stdscr == nullptr) {
        qCritical() << "Failed to initialize ncurses";
        return;
    }
    raw();                   // pass all key codes directly (including Ctrl keys)
    noecho();                // don't echo typed characters automatically
    keypad(stdscr, TRUE);    // enable arrow keys and function keys
    nodelay(stdscr, TRUE);   // make getch() non-blocking (we use QSocketNotifier)
    curs_set(1);             // show the hardware cursor at the input position

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE,   COLOR_BLUE);   // topic + info bars
        init_pair(2, COLOR_MAGENTA, COLOR_BLACK);  // unread-activity indicator
    }

    // QSocketNotifier watches stdin (fd 0) and fires activated() when a key
    // is available, letting ncurses coexist with Qt's event loop.
    m_stdinNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_stdinNotifier, &QSocketNotifier::activated, this, &NcursesUI::onStdinReady);

    // One day-change notification for all buffers.
    m_dayNotifier = new DayChangeNotifier(this);
    connect(m_dayNotifier, &DayChangeNotifier::dayChanged, this, &NcursesUI::onDayChanged);

    resizeWindows();
    draw();
    m_infoTimer->start(1000);
}

// ---------------------------------------------------------------------------
// Buffer lookup
// ---------------------------------------------------------------------------

// Returns a reference to the BufferData for 'buf', which may be either an
// ICBSession or a PrivateChatLogic.  Returns a throwaway static instance for
// unknown pointer types (should never happen in practice).
NcursesUI::BufferData& NcursesUI::bufferData(QObject* buf) {
    if (ICBSession* session = qobject_cast<ICBSession*>(buf))
        return m_sessionData[session];
    if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(buf))
        return m_privateChatData[chat];
    static BufferData dummy;
    return dummy;
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

// Register a new ICBSession with this UI.  This adds the session to m_buffers,
// allocates its BufferData, and connects all the signals needed to display
// messages and keep the screen up to date.
void NcursesUI::addSession(ICBSession* session) {
    if (!session || m_buffers.contains(session))
        return;

    m_buffers.append(session);
    m_sessionData.insert(session, BufferData());
    m_sessionTopics.insert(session, " (No topic)");
    m_groupFlags.insert(session, QString());

    // Group messages
    connect(session, &ICBSession::groupChanged, this,
        [this, session](const QString& newGroup) {
            onSessionGroupChanged(session, newGroup);
        });
    connect(session, &ICBSession::messageReceived, this,
        [this, session](const QString& sender, const QString& text) {
            onSessionMessage(session, sender, text);
        });
    connect(session, &ICBSession::personalMessageReceived, this,
        [this, session](const QString& from, const QString& text) {
            onSessionPrivateMessage(session, from, text);
        });
    connect(session, &ICBSession::systemMessageReceived, this,
        [this, session](const QString& text) {
            onSessionSystemMessage(session, text);
        });

    // Flags and topic from /who output or live Topic packets.
    // ICBSession parses the "Group:" line once and emits this signal;
    // both the topic bar and info bar are updated here.
    connect(session, &ICBSession::whoInfoReceived, this,
        [this, session](const QString& flags, const QString& topic) {
            onSessionWhoInfo(session, flags, topic);
        });

    // Clean up when the session object is destroyed.
    connect(session, &ICBSession::destroyed, this,
        [this, session]() { removeSession(session); });

    // /query or incoming private message: open the private chat buffer and
    // switch to it immediately so the user sees the new conversation.
    connect(session, &ICBSession::openPrivateChatRequested, this,
        [this, session](const QString& nick) {
            auto result = getOrCreatePrivateChat(session, nick);
            if (result.created) {
                m_currentBuffer = result.chat;
                bufferData(m_currentBuffer).scrollLine = 0;
                bufferData(m_currentBuffer).unread     = false;
                bufferData(m_currentBuffer).following  = true;
                m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
            }
            flush();
        });

    // Echo our own sent public messages into the current buffer.
    connect(session, &ICBSession::selfMessageSent, this,
        [this, session](const QString& msg) {
            QString formatted = "<" + session->nickname() + "> " + msg;
            if (m_currentBuffer == session) {
                addMessageToCurrent(formatted);
                m_chatDirty = true;
            } else {
                // We're looking at a different buffer — append with timestamp
                // and mark this buffer as having unread activity.
                QString timestamped = "[" + currentTimestamp() + "] " + formatted;
                appendToBuffer(session, timestamped);
                bufferData(session).unread = true;
                m_infoDirty = true;
            }
            flush();
        });

    // Ensure a private chat buffer exists for any outgoing private message.
    connect(session, &ICBSession::selfPrivateMessageSent, this,
        [this, session](const QString& to, const QString& /*msg*/) {
            getOrCreatePrivateChat(session, to);
        });

    // User-list signals: listChanged covers joins, leaves, and nick changes;
    // userUpdated covers moderator-status changes.
    UserList* userList = session->userList();
    connect(userList, &UserList::listChanged, this, &NcursesUI::redrawForUserListSender);
    connect(userList, &UserList::userUpdated, this, &NcursesUI::onUserUpdated);

    // If this is the very first session, make it the active buffer.
    if (m_buffers.size() == 1) {
        m_currentBuffer = session;
        m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
    }

    flush();
}

// Remove a session and all its data.  Switches to another buffer if needed.
void NcursesUI::removeSession(ICBSession* session) {
    m_buffers.removeAll(session);
    m_sessionData.remove(session);
    m_sessionTopics.remove(session);

    if (m_currentBuffer == session) {
        m_currentBuffer = m_buffers.isEmpty() ? nullptr : m_buffers.first();
        if (m_currentBuffer)
            bufferData(m_currentBuffer).scrollLine = 0;
        m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
    }
    flush();
}

// ---------------------------------------------------------------------------
// Private chat management
// ---------------------------------------------------------------------------

// Returns the existing PrivateChatLogic for (session, nick) or creates one.
// Nick comparison is case-insensitive (ICB convention).
NcursesUI::PrivateChatResult NcursesUI::getOrCreatePrivateChat(ICBSession* session,
                                                                const QString& nick) {
    for (QObject* buf : m_buffers) {
        if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(buf)) {
            if (chat->session() == session &&
                chat->otherNick().compare(nick, Qt::CaseInsensitive) == 0)
                return {chat, false};
        }
    }
    PrivateChatLogic* chat = new PrivateChatLogic(session, nick, this);
    addPrivateChat(chat);
    return {chat, true};
}

// Register a new PrivateChatLogic buffer, allocate its BufferData, and
// connect its message signals.
void NcursesUI::addPrivateChat(PrivateChatLogic* chat) {
    if (!chat || m_buffers.contains(chat))
        return;

    m_buffers.append(chat);
    m_privateChatData.insert(chat, BufferData());

    connect(chat, &PrivateChatLogic::incomingMessage,
            this, &NcursesUI::onPrivateChatIncoming);
    connect(chat, &PrivateChatLogic::outgoingMessage,
            this, &NcursesUI::onPrivateChatOutgoing);

    // System messages from a private chat "bob has left" always get a
    // timestamp and are appended to the appropriate buffer.
    connect(chat, &PrivateChatLogic::systemMessage, this,
        [this, chat](const QString& msg) {
            QString timestamped = "[" + currentTimestamp() + "] * " + msg;
            appendToBuffer(chat, timestamped);
            if (m_currentBuffer != chat) {
                bufferData(chat).unread = true;
                m_infoDirty = true;
            }
            flush();
        });

    connect(chat, &QObject::destroyed, this,
        [this, chat]() { removePrivateChat(chat); });

    flush();
}

// Remove a private chat buffer.  Switches the current buffer if needed.
void NcursesUI::removePrivateChat(PrivateChatLogic* chat) {
    m_buffers.removeAll(chat);
    m_privateChatData.remove(chat);

    if (m_currentBuffer == chat) {
        m_currentBuffer = m_buffers.isEmpty() ? nullptr : m_buffers.first();
        if (m_currentBuffer)
            bufferData(m_currentBuffer).scrollLine = 0;
        m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
    }
    flush();
}

// ---------------------------------------------------------------------------
// Session event handlers
// ---------------------------------------------------------------------------

// A public group message arrived.  If this session is the current buffer,
// add it with addMessageToCurrent (which also advances the scroll position
// in following mode); otherwise append it with a timestamp and mark unread.
void NcursesUI::onSessionMessage(ICBSession* session, const QString& sender,
                                 const QString& text) {
    QString msg        = QString("<%1> %2").arg(sender, text);
    QString timestamped = "[" + currentTimestamp() + "] " + msg;

    if (m_currentBuffer == session) {
        addMessageToCurrent(msg);
        m_chatDirty = true;
    } else {
        appendToBuffer(session, timestamped);
        bufferData(session).unread = true;
        m_infoDirty = true;
    }
    flush();
}

// A server or status message arrived (join/leave, /who output, group notices).
void NcursesUI::onSessionSystemMessage(ICBSession* session, const QString& text) {
    QString msg = QString("* %1").arg(text);

    if (m_currentBuffer == session) {
        addMessageToCurrent(msg);
        m_chatDirty = true;
    } else {
        QString timestamped = "[" + currentTimestamp() + "] " + msg;
        appendToBuffer(session, timestamped);
        bufferData(session).unread = true;
        m_infoDirty = true;
    }
    flush();
}

// An incoming personal message arrived for this session.
// We ensure the private chat buffer exists here.  If the buffer is newly
// created its PrivateChatLogic missed the personalMessageReceived signal that
// triggered this call (the connection didn't exist yet), so we append the
// message directly.  For an already-existing buffer, PrivateChatLogic is
// already connected and will emit incomingMessage → onPrivateChatIncoming,
// so we do nothing extra — handling it here too would display it twice.
void NcursesUI::onSessionPrivateMessage(ICBSession* session, const QString& from,
                                        const QString& text) {
    auto result = getOrCreatePrivateChat(session, from);
    if (result.created) {
        // Manually replicate what onPrivateChatIncoming would do, since
        // PrivateChatLogic wasn't connected when the signal fired.
        QString timestamped = "[" + currentTimestamp() + "] <" + from + "> " + text;
        appendToBuffer(result.chat, timestamped);
        if (m_currentBuffer != result.chat) {
            bufferData(result.chat).unread = true;
            m_infoDirty = true;
        }
        flush();
    }
}

// ICBSession parsed a "Group: name (flags) Topic: ..." line from /who output,
// or a live Topic packet arrived.  Update our stored values and redraw the
// topic bar and info bar if this is the currently visible buffer.
void NcursesUI::onSessionWhoInfo(ICBSession* session, const QString& flags,
                                 const QString& topic) {
    m_groupFlags[session]    = flags;
    m_sessionTopics[session] = topic.isEmpty() ? " (No topic)" : " " + topic;

    if (m_currentBuffer == session) {
        m_topicDirty = true;
        m_infoDirty  = true;
        flush();
    }
}

// The user switched to a new group.  The session layer emits whoInfoReceived
// with empty strings immediately after this, which will call onSessionWhoInfo
// and blank the topic/flags display.  Nothing extra needed here.
void NcursesUI::onSessionGroupChanged(ICBSession* session, const QString& newGroup) {
    Q_UNUSED(session);
    Q_UNUSED(newGroup);
}

// ---------------------------------------------------------------------------
// User-list event handlers
// ---------------------------------------------------------------------------

// The user list for some session changed.  Find which session owns the list
// (by comparing UserList pointers) and redraw the sidebar if it's current.
void NcursesUI::redrawForUserListSender() {
    UserList* list = qobject_cast<UserList*>(sender());
    if (!list) return;

    for (ICBSession* session : m_sessionData.keys()) {
        if (session->userList() == list) {
            if (m_currentBuffer == session) {
                m_userDirty = true;
                flush();
            }
            break;
        }
    }
}

// A user's entry was updated (nick change or moderator status change).
// Delegate to redrawForUserListSender which checks whether a redraw is needed.
void NcursesUI::onUserUpdated(const QString& oldNick, const UserInfo& newInfo) {
#ifdef QT_DEBUG
    qDebug() << "onUserUpdated:" << oldNick << "->" << newInfo.nick
             << "moderator=" << newInfo.isModerator;
#else
    Q_UNUSED(oldNick);
    Q_UNUSED(newInfo);
#endif
    redrawForUserListSender();
}

// ---------------------------------------------------------------------------
// Private chat event handlers
// ---------------------------------------------------------------------------

// A message arrived from the other party in a private chat.
void NcursesUI::onPrivateChatIncoming(const QString& from, const QString& message) {
    PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(sender());
    if (!chat) return;

    QString msg        = QString("<%1> %2").arg(from, message);
    QString timestamped = "[" + currentTimestamp() + "] " + msg;

    if (m_currentBuffer == chat) {
        addMessageToCurrent(msg);
        m_chatDirty = true;
    } else {
        appendToBuffer(chat, timestamped);
        bufferData(chat).unread = true;
        m_infoDirty = true;
    }
    flush();
}

// We sent a message in a private chat.
void NcursesUI::onPrivateChatOutgoing(const QString& to, const QString& message) {
    Q_UNUSED(to);
    PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(sender());
    if (!chat) return;

    QString myNick     = chat->session() ? chat->session()->nickname() : "you";
    QString msg        = QString("<%1> %2").arg(myNick, message);
    QString timestamped = "[" + currentTimestamp() + "] " + msg;

    if (m_currentBuffer == chat) {
        addMessageToCurrent(msg);
        m_chatDirty = true;
    } else {
        appendToBuffer(chat, timestamped);
        bufferData(chat).unread = true;
        m_infoDirty = true;
    }
    flush();
}

// ---------------------------------------------------------------------------
// Day-change notification
// ---------------------------------------------------------------------------

// Append a date-separator line to every open buffer once per calendar day.
void NcursesUI::onDayChanged(const QString& message, const QDate& newDate) {
    Q_UNUSED(newDate);
    for (QObject* buf : m_buffers)
        appendToBuffer(buf, "* " + message);
    m_chatDirty = true;
    m_infoDirty = true;
    flush();
}

// ---------------------------------------------------------------------------
// Keyboard input
// ---------------------------------------------------------------------------

// Called by QSocketNotifier when stdin has bytes available.  Drains all
// available key events via get_wch() and dispatches each one to handleInput().
void NcursesUI::onStdinReady() {
    wint_t ch;
    while (get_wch(&ch) != ERR)
        handleInput(static_cast<int>(ch));
}

// Dispatch a single key event to the appropriate action.
void NcursesUI::handleInput(int ch) {
    // ESC sequence: ESC followed by a digit switches to buffer N
    if (m_escapePending) {
        m_escapePending = false;
        if (ch >= '0' && ch <= '9') {
            int win = ch - '0';
            if (win >= 1 && win <= m_buffers.size()) {
                m_currentBuffer = m_buffers[win - 1];
                bufferData(m_currentBuffer).scrollLine = 0;
                bufferData(m_currentBuffer).unread     = false;
                bufferData(m_currentBuffer).following  = true;
                m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
            }
            flush();
            return;
        }
        // Non-digit after ESC: fall through and treat 'ch' as a normal key.
    }

    // Enter: submit the input line
    if (ch == '\n' || ch == KEY_ENTER) {
        if (!m_inputBuffer.isEmpty()) {
            QString text    = m_inputBuffer;
            QString trimmed = text.trimmed();

            // Add to history (avoid duplicates of the last entry).
            if (m_history.isEmpty() || m_history.last() != text)
                m_history.append(text);
            m_historyIndex = -1;
            m_savedInput.clear();

            if (trimmed.startsWith('/')) {
                QString cmd = trimmed.mid(1).toLower();

                if (cmd == "exit") {
                    QCoreApplication::quit();
                    return;
                }

                if (cmd == "clear") {
                    // Clear the current buffer's message history.
                    if (m_currentBuffer) {
                        bufferData(m_currentBuffer).messages.clear();
                        bufferData(m_currentBuffer).scrollLine = 0;
                        bufferData(m_currentBuffer).following  = true;
                        m_chatDirty = true;
                    }
                    m_inputBuffer.clear();
                    m_inputPos   = 0;
                    m_inputDirty = true;
                    flush();
                    return;
                }

                if (cmd == "close") {
                    // Close the current private-chat buffer (not valid for sessions).
                    if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(m_currentBuffer)) {
                        m_inputBuffer.clear();
                        m_inputPos = 0;
                        removePrivateChat(chat);
                        chat->deleteLater();
                        return;
                    }
                    // Not a private chat buffer — fall through to clear input.
                }

                if (cmd == "nicklist") {
                    // Toggle the user-list sidebar.
                    m_showUserList = !m_showUserList;
                    resizeWindows();
                    adjustScrollAfterResize();
                    m_userDirty = m_chatDirty = m_topicDirty = m_infoDirty = m_inputDirty = true;
                    m_inputBuffer.clear();
                    m_inputPos = 0;
                    flush();
                    return;
                }
            }

            // Any tab-completion in progress is cancelled when the user sends.
            m_inCompletion = false; m_lastInserted.clear();

            // Route the text to the appropriate handler.
            if (ICBSession* session = qobject_cast<ICBSession*>(m_currentBuffer))
                session->processUserInput(text);
            else if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(m_currentBuffer))
                chat->sendMessage(text);

            m_inputBuffer.clear();
            m_inputPos   = 0;
            m_inputDirty = true;
            flush();
        }

    // Ctrl+A: jump to next unread buffer
    } else if (ch == 1) {
        for (int i = 0; i < m_buffers.size(); ++i) {
            QObject* buf = m_buffers[i];
            if (buf == m_currentBuffer) continue;
            if (bufferData(buf).unread) {
                m_currentBuffer = buf;
                bufferData(m_currentBuffer).scrollLine = 0;
                bufferData(m_currentBuffer).unread     = false;
                bufferData(m_currentBuffer).following  = true;
                m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
                flush();
                break;
            }
        }

    // Ctrl+L: toggle user-list sidebar
    } else if (ch == 12) {
        m_showUserList = !m_showUserList;
        resizeWindows();
        adjustScrollAfterResize();
        m_userDirty = m_chatDirty = m_topicDirty = m_infoDirty = m_inputDirty = true;
        flush();

    // ESC: start a two-key buffer-switch sequence (ESC + digit)
    } else if (ch == 27) {
        m_escapePending = true;
        return;

    // Backspace
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (m_inputPos > 0) {
            m_inputBuffer.remove(m_inputPos - 1, 1);
            m_inputPos--;
            m_inCompletion = false; m_lastInserted.clear();
            m_inputDirty   = true;
            flush();
        }

    // Up arrow: go back in command history
    } else if (ch == KEY_UP) {
        if (!m_history.isEmpty()) {
            if (m_historyIndex == -1) {
                // Save whatever is currently typed before entering history mode.
                m_historyIndex = m_history.size() - 1;
                m_savedInput   = m_inputBuffer;
            } else if (m_historyIndex > 0) {
                m_historyIndex--;
            }
            m_inputBuffer  = m_history[m_historyIndex];
            m_inputPos     = m_inputBuffer.length();
            m_inCompletion = false; m_lastInserted.clear();
            m_inputDirty   = true;
            flush();
        }

    // Down arrow: go forward in command history
    } else if (ch == KEY_DOWN) {
        if (m_historyIndex != -1) {
            if (m_historyIndex < m_history.size() - 1) {
                m_historyIndex++;
                m_inputBuffer = m_history[m_historyIndex];
            } else {
                // Reached the end of history — restore the saved in-progress text.
                m_historyIndex = -1;
                m_inputBuffer  = m_savedInput;
                m_savedInput.clear();
            }
            m_inputPos     = m_inputBuffer.length();
            m_inCompletion = false; m_lastInserted.clear();
            m_inputDirty   = true;
            flush();
        }

    // Left / Right arrow: move cursor in input line
    } else if (ch == KEY_LEFT) {
        if (m_inputPos > 0) m_inputPos--;
        m_inCompletion = false; m_lastInserted.clear();
        m_inputDirty   = true;
        flush();
    } else if (ch == KEY_RIGHT) {
        if (m_inputPos < m_inputBuffer.length()) m_inputPos++;
        m_inCompletion = false; m_lastInserted.clear();
        m_inputDirty   = true;
        flush();

    // Tab: nick tab-completion
    } else if (ch == 9) {
        tryComplete();
        m_inputDirty = true;
        flush();

    // PageUp / PageDown: scroll the chat area
    } else if (ch == KEY_PPAGE) {
        if (m_currentBuffer) {
            BufferData& bd   = bufferData(m_currentBuffer);
            int pageStep     = getmaxy(m_chatWin) - 1;
            bd.scrollLine    = qMax(bd.scrollLine - pageStep, 0);
            int width        = getmaxx(m_chatWin);
            int total        = totalDisplayLines(bd.messages, width, HANGING_INDENT);
            int maxScroll    = qMax(total - getmaxy(m_chatWin), 0);
            bd.following     = (bd.scrollLine == maxScroll);
            m_chatDirty      = true;
            flush();
        }
    } else if (ch == KEY_NPAGE) {
        if (m_currentBuffer) {
            BufferData& bd   = bufferData(m_currentBuffer);
            int width        = getmaxx(m_chatWin);
            int total        = totalDisplayLines(bd.messages, width, HANGING_INDENT);
            int pageStep     = getmaxy(m_chatWin) - 1;
            int maxScroll    = qMax(total - getmaxy(m_chatWin), 0);
            bd.scrollLine    = qMin(bd.scrollLine + pageStep, maxScroll);
            bd.following     = (bd.scrollLine == maxScroll);
            m_chatDirty      = true;
            flush();
        }

    // F2 / Ctrl+P: switch to previous buffer
    } else if (ch == KEY_F(2) || ch == KEY_CTRL('P')) {
        switchBuffer(-1);

    // F3 / Ctrl+N: switch to next buffer
    } else if (ch == KEY_F(3) || ch == KEY_CTRL('N')) {
        switchBuffer(1);

    // Terminal resize event
    } else if (ch == KEY_RESIZE) {
        resizeWindows();
        adjustScrollAfterResize();
        m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
        flush();

    // Printable character: insert into the input buffer
    } else if (ch >= 32 && (ch > 126 || ch < KEY_MIN)) {
        // Accept printable ASCII (32–126) and all Unicode code points above 127.
        // ncurses KEY_* constants start at KEY_MIN (typically 0x101), so anything
        // below KEY_MIN that is not a C0 control character is safe to insert.
        m_inputBuffer.insert(m_inputPos, QChar(ch));
        m_inputPos++;
        m_inCompletion = false; m_lastInserted.clear();
        m_inputDirty   = true;
        flush();
    }
}

// ---------------------------------------------------------------------------
// Buffer navigation
// ---------------------------------------------------------------------------

// Cycle m_currentBuffer by 'direction' positions (+1 = next, -1 = previous),
// wrapping around the ends of m_buffers.
void NcursesUI::switchBuffer(int direction) {
    if (m_buffers.isEmpty()) return;

    int idx = m_buffers.indexOf(m_currentBuffer);
    if (idx == -1) idx = 0;
    idx = (idx + direction + m_buffers.size()) % m_buffers.size();

    m_currentBuffer = m_buffers[idx];
    bufferData(m_currentBuffer).unread = false;

    // Recalculate whether the new buffer is in "following" mode based on
    // where the scroll position currently sits relative to the bottom.
    BufferData& bd   = bufferData(m_currentBuffer);
    int width        = getmaxx(m_chatWin);
    int visibleLines = getmaxy(m_chatWin);
    int total        = totalDisplayLines(bd.messages, width, HANGING_INDENT);
    int maxScroll    = qMax(total - visibleLines, 0);
    bd.following     = (bd.scrollLine == maxScroll);

    m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
    flush();
}

// ---------------------------------------------------------------------------
// Buffer helpers
// ---------------------------------------------------------------------------

// Append a timestamped message to m_currentBuffer's history.
// Advances scrollLine if "following" mode is active, and enforces the
// 1000-message cap to prevent unbounded memory growth.
void NcursesUI::addMessageToCurrent(const QString& msg) {
    if (!m_currentBuffer) return;
    QString timestamp = "[" + currentTimestamp() + "] ";
    BufferData& bd    = bufferData(m_currentBuffer);
    int width         = getmaxx(m_chatWin);
    int visibleLines  = getmaxy(m_chatWin);

    bd.messages.append(timestamp + msg);
    if (bd.messages.size() > 1000)
        bd.messages.removeFirst();

    // In following mode, keep the scroll position pinned to the bottom.
    if (bd.following) {
        int totalAfter = totalDisplayLines(bd.messages, width, HANGING_INDENT);
        bd.scrollLine  = qMax(totalAfter - visibleLines, 0);
    }
}

// Append a pre-formatted message to an arbitrary buffer (used for background
// sessions and day-separator lines).  Also enforces the 1000-message cap.
void NcursesUI::appendToBuffer(QObject* buf, const QString& msg) {
    BufferData& bd = bufferData(buf);
    bd.messages.append(msg);
    if (bd.messages.size() > 1000)
        bd.messages.removeFirst();
}

// After a terminal resize, clamp the current buffer's scroll position to the
// new valid range.  If in following mode, jump to the new bottom.
void NcursesUI::adjustScrollAfterResize() {
    if (!m_currentBuffer) return;
    BufferData& bd   = bufferData(m_currentBuffer);
    int width        = getmaxx(m_chatWin);
    int visibleLines = getmaxy(m_chatWin);
    int total        = totalDisplayLines(bd.messages, width, HANGING_INDENT);
    int maxScroll    = qMax(total - visibleLines, 0);

    if (bd.following)
        bd.scrollLine = maxScroll;
    else
        bd.scrollLine = qBound(0, bd.scrollLine, maxScroll);
}

// ---------------------------------------------------------------------------
// Info / name helpers
// ---------------------------------------------------------------------------

// Returns the display name for the current buffer:
//   "#groupname"  for a session buffer
//   "nick"        for a private-chat buffer
QString NcursesUI::currentBufferName() const {
    if (ICBSession* session = qobject_cast<ICBSession*>(m_currentBuffer))
        return "#" + session->currentGroup();
    if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(m_currentBuffer))
        return chat->otherNick();
    return QString();
}

// Returns the sorted nick list for the current group buffer, or an empty
// list for private-chat buffers (they have no group user list).
QStringList NcursesUI::currentUserList() const {
    if (ICBSession* session = qobject_cast<ICBSession*>(m_currentBuffer))
        return session->userList()->displayNames();
    return QStringList();
}

// ---------------------------------------------------------------------------
// Tab completion
// ---------------------------------------------------------------------------

// Complete the partial nick at the cursor position.  On the first Tab press,
// build the candidate list from the current group's nick list.  On subsequent
// Tab presses without intervening keystrokes, cycle through the candidates.
void NcursesUI::tryComplete() {
    // Tab-completion only makes sense in a group session buffer.
    ICBSession* session = qobject_cast<ICBSession*>(m_currentBuffer);
    if (!session) return;

    // Strip the "@" moderator prefix so completions work regardless of status.
    QStringList users = currentUserList();
    for (QString& u : users) {
        if (u.startsWith('@')) u = u.mid(1);
    }

    if (!m_inCompletion) {
        // First Tab: scan backward from the cursor to find the start of the
        // word being typed, collect all nicks that start with that prefix.
        int pos = m_inputPos;
        while (pos > 0 && m_inputBuffer[pos - 1].isLetterOrNumber()) --pos;
        m_completionPrefix = m_inputBuffer.mid(pos, m_inputPos - pos);
        if (m_completionPrefix.isEmpty()) return;

        m_completions = users.filter(
            QRegularExpression("^" + QRegularExpression::escape(m_completionPrefix),
                               QRegularExpression::CaseInsensitiveOption));
        if (m_completions.isEmpty()) return;

        m_completionIndex = 0;
        m_inCompletion    = true;
        // Seed m_lastInserted with the typed prefix so the first replacement
        // removes exactly what the user typed.  On wrap-around m_completionIndex
        // returns to 0, but m_lastInserted will hold the previous candidate's
        // length — avoiding the "exampleexample..." accumulation bug.
        m_lastInserted    = m_completionPrefix;
    }

    if (!m_completions.isEmpty()) {
        // Replace whatever we put in the buffer last time with the next candidate.
        // Using m_lastInserted.length() (not m_completionPrefix.length()) is the
        // critical fix: on wrap-around m_completionIndex is 0 again but a full
        // nick is already in the buffer, so we must remove that nick, not the prefix.
        int start = m_inputPos - static_cast<int>(m_lastInserted.length());
        m_inputBuffer.remove(start, m_lastInserted.length());
        m_inputBuffer.insert(start, m_completions[m_completionIndex]);
        m_inputPos        = start + m_completions[m_completionIndex].length();
        m_lastInserted    = m_completions[m_completionIndex];
        m_completionIndex = (m_completionIndex + 1) % m_completions.size();
    }
}

// ---------------------------------------------------------------------------
// Window management
// ---------------------------------------------------------------------------

// Destroy and recreate all ncurses sub-windows to fit the current terminal
// dimensions.  Called on startup and in response to KEY_RESIZE events.
void NcursesUI::resizeWindows() {
    int height, width;
    getmaxyx(stdscr, height, width);

    // Free old windows before creating new ones at the updated size.
    if (m_topicWin) delwin(m_topicWin);
    if (m_infoWin)  delwin(m_infoWin);
    if (m_chatWin)  delwin(m_chatWin);
    if (m_userWin)  delwin(m_userWin);
    if (m_inputWin) delwin(m_inputWin);

    int userWidth = m_showUserList ? 20 : 0;

    // Row 0         : topic bar (full width)
    // Rows 1..h-3   : chat area (left) + optional user list (right)
    // Row h-2       : info bar (full width)
    // Row h-1       : input line (full width)
    m_topicWin = subwin(stdscr, 1, width,          0,         0);
    m_infoWin  = subwin(stdscr, 1, width,          height-2,  0);
    m_inputWin = subwin(stdscr, 1, width,          height-1,  0);
    m_chatWin  = subwin(stdscr, height-3, width-userWidth, 1, 0);

    if (m_showUserList)
        m_userWin = subwin(stdscr, height-3, userWidth, 1, width-userWidth);
    else
        m_userWin = nullptr;

    scrollok(m_chatWin, TRUE);  // allow ncurses to scroll the chat window
    wmove(m_inputWin, 0, 0);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Redraw the one-line topic bar at the top of the screen.
// Shows the current group topic for session buffers, or "Private chat with
// <nick>" for private-chat buffers.
void NcursesUI::redrawTopic() {
    if (!m_currentBuffer) {
        werase(m_topicWin);
        wnoutrefresh(m_topicWin);
        return;
    }
    wbkgd(m_topicWin, COLOR_PAIR(1));
    werase(m_topicWin);

    QString topic;
    if (ICBSession* session = qobject_cast<ICBSession*>(m_currentBuffer)) {
        topic = m_sessionTopics.value(session, "  (No topic)");
    } else if (PrivateChatLogic* chat = qobject_cast<PrivateChatLogic*>(m_currentBuffer)) {
        topic = " Private chat with " + chat->otherNick();
    }

    // Truncate with "..." if the topic is too long for the terminal width.
    int width = getmaxx(m_topicWin);
    if (topic.length() > width) topic = topic.left(width - 3) + "...";

    mvwaddwstr(m_topicWin, 0, 0, topic.toStdWString().c_str());
    wnoutrefresh(m_topicWin);
}

// Redraw the one-line info bar near the bottom of the screen.
// Left side:  "[hh:mm:ss] [nick] [N:groupname (flags)]"
// Right side: "[Act: 2,3]"  (indices of buffers with unread activity)
void NcursesUI::redrawInfo() {
    if (!m_currentBuffer) return;

    QString timestamp = currentTimestamp();
    QString nick, location;

    ICBSession*      currentSession = qobject_cast<ICBSession*>(m_currentBuffer);
    PrivateChatLogic* currentChat   = qobject_cast<PrivateChatLogic*>(m_currentBuffer);

    if (currentSession) {
        nick = currentSession->nickname();
        int currentIdx    = m_buffers.indexOf(currentSession) + 1;
        QString groupName = currentSession->currentGroup();
        QString flags     = m_groupFlags.value(currentSession, QString());
        if (!flags.isEmpty()) flags = " " + flags;
        location = QString("[%1:%2%3]").arg(currentIdx).arg(groupName).arg(flags);
    } else if (currentChat) {
        nick = currentChat->session() ? currentChat->session()->nickname() : "?";
        int currentIdx = m_buffers.indexOf(currentChat) + 1;
        location = QString("[%1:Private:%2]").arg(currentIdx).arg(currentChat->otherNick());
    }

    QString left = QString("[%1] [%2] %3").arg(timestamp, nick, location);

    // Build the activity indicator listing all background buffers with unread messages.
    QStringList actIndices;
    for (int i = 0; i < m_buffers.size(); ++i) {
        QObject* buf = m_buffers[i];
        if (buf != m_currentBuffer && bufferData(buf).unread)
            actIndices << QString::number(i + 1);
    }
    QString right = actIndices.isEmpty() ? "" : "[Act: " + actIndices.join(",") + "]";

    // Fit left and right strings into the available width, truncating left if
    // necessary.
    int width    = getmaxx(m_infoWin);
    int leftLen  = left.length();
    int rightLen = right.length();

    if (leftLen + rightLen + 1 > width) {
        int available = width - rightLen - 1;
        if (available < 0) {
            right   = right.left(width - 1);
            left    = "";
            leftLen = 0;
        } else {
            left    = left.left(available);
            leftLen = available;
        }
    }

    wbkgd(m_infoWin, COLOR_PAIR(1));
    werase(m_infoWin);
    mvwaddwstr(m_infoWin, 0, 0, left.toStdWString().c_str());
    if (!right.isEmpty())
        mvwaddwstr(m_infoWin, 0, leftLen + 1, right.toStdWString().c_str());
    wnoutrefresh(m_infoWin);
}

// Redraw the optional user-list sidebar on the right.
// If the sidebar is hidden (m_showUserList == false), erase the window and return.
void NcursesUI::redrawUserList() {
    if (!m_showUserList || !m_userWin) {
        if (m_userWin) {
            werase(m_userWin);
            wnoutrefresh(m_userWin);
        }
        return;
    }

    werase(m_userWin);
    QStringList users = currentUserList();
    int height, width;
    getmaxyx(m_userWin, height, width);

    for (int i = 0; i < users.size() && i < height; ++i) {
        QString name = users[i];
        if (name.length() >= width) name = name.left(width - 1);  // prevent overflow
        mvwaddwstr(m_userWin, i, 0, name.toStdWString().c_str());
    }
    wnoutrefresh(m_userWin);
}

// Redraw the main scrollable chat area.
// Only renders the lines that fall within the visible [scrollLine, scrollLine+height)
// range.  Messages that span multiple terminal lines (due to wrapping) are handled
// correctly by wrapText().
void NcursesUI::redrawChat() {
    if (!m_currentBuffer) return;
    BufferData& bd = bufferData(m_currentBuffer);
    werase(m_chatWin);

    int height, width;
    getmaxyx(m_chatWin, height, width);

    int currentLine  = 0;
    int visibleStart = bd.scrollLine;
    int visibleEnd   = visibleStart + height - 1;

    for (const QString& msg : bd.messages) {
        QStringList lines    = wrapText(msg, width, HANGING_INDENT);
        int         msgLines = lines.size();
        int         msgStart = currentLine;
        int         msgEnd   = currentLine + msgLines - 1;

        // Only process this message if any of its display lines fall in the
        // visible range.
        if (msgEnd >= visibleStart && msgStart <= visibleEnd) {
            int printStart = qMax(visibleStart - msgStart, 0);
            int printEnd   = qMin(visibleEnd   - msgStart, msgLines - 1);
            for (int i = printStart; i <= printEnd; ++i) {
                int winLine = msgStart + i - visibleStart;
                mvwaddwstr(m_chatWin, winLine, 0, lines[i].toStdWString().c_str());
            }
        }

        currentLine += msgLines;
        if (currentLine > visibleEnd) break;  // no more visible content
    }
    wnoutrefresh(m_chatWin);
}

// Redraw the one-line input area and reposition the hardware cursor.
void NcursesUI::redrawInput() {
    werase(m_inputWin);
    mvwaddwstr(m_inputWin, 0, 0, m_inputBuffer.toStdWString().c_str());
    wmove(m_inputWin, 0, m_inputPos);
    wnoutrefresh(m_inputWin);
}

// ---------------------------------------------------------------------------
// Flush / draw
// ---------------------------------------------------------------------------

// Check all dirty flags, call only the necessary redraw functions, then
// call doupdate() once to push accumulated changes to the physical terminal.
// The input window is always refreshed last so the hardware cursor ends up
// at the correct position.
void NcursesUI::flush() {
    if (m_topicDirty) { redrawTopic();    m_topicDirty = false; }
    if (m_infoDirty)  { redrawInfo();     m_infoDirty  = false; }
    if (m_userDirty)  { redrawUserList(); m_userDirty  = false; }
    if (m_chatDirty)  { redrawChat();     m_chatDirty  = false; }
    if (m_inputDirty) { redrawInput();    m_inputDirty = false; }

    // Always reposition the cursor and refresh the input window last so the
    // terminal cursor visually tracks the user's typing position.
    wmove(m_inputWin, 0, m_inputPos);
    wnoutrefresh(m_inputWin);

    doupdate();  // single physical write to the terminal
}

// Force a full redraw of all regions (used on startup and after resize).
void NcursesUI::draw() {
    m_topicDirty = m_infoDirty = m_userDirty = m_chatDirty = m_inputDirty = true;
    flush();
}

// Tick handler for the 1-second info-bar clock.
void NcursesUI::updateInfoBar() {
    if (m_currentBuffer) {
        m_infoDirty = true;
        flush();
    }
}
