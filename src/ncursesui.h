#ifndef NCURSESUI_H
#define NCURSESUI_H

// clang-format off
#include <QHash>
#include <QObject>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include "commandhandler.h"
#include "icbsession.h"
#include "privatechat.h"
#include "userlist.h"

// _XOPEN_SOURCE_EXTENDED must be defined before ncurses.h is included.
// Without it, ncurses omits the wide-character function declarations
// (get_wch, waddwstr, mvwaddwstr, etc.) that qtICB uses for correct
// Unicode display.  The matching wide-character library is -lncursesw
// (see the LIBS line in qtICBv2.pro).
#ifndef _XOPEN_SOURCE_EXTENDED
#  define _XOPEN_SOURCE_EXTENDED
#endif
#include <ncurses.h>
// clang-format on

class DayChangeNotifier;

// Convenience macro: converts a letter to its Ctrl+letter key code.
#define KEY_CTRL(x) ((x)&0x1F)

// NcursesUI is the full-screen terminal front-end for qtICB.
//
// It uses ncurses for rendering and integrates with Qt's event loop via
// QSocketNotifier (to watch stdin) and QTimer (to tick the info bar clock).
//
// Screen layout (top to bottom):
//   ┌─ Topic bar (1 line, blue background) ─────────────────────┐
//   │  Current group topic, or "Private chat with <nick>"       │
//   ├─ Chat area ─────────────────────────┬─ User list (opt.) ──┤
//   │  Scrollable message history         │  Sorted nick list   │
//   │  (most of the screen height)        │  (20 cols wide)     │
//   ├─ Info bar (1 line, blue background) ┴─────────────────────┤
//   │  [hh:mm:ss] [nick] [N:group (flags)]       [Act: 2,3]     │
//   └─ Input line (1 line) ─────────────────────────────────────┘
//
// Buffer model:
//   m_buffers is an ordered list of QObject pointers; each entry is either
//   an ICBSession* (group buffer) or a PrivateChatLogic* (private chat).
//   m_currentBuffer points to whichever buffer is currently displayed.
//   Each buffer has a BufferData entry tracking its message history, scroll
//   position, "following" mode, and unread flag.
//
// Dirty-flag rendering:
//   Instead of redrawing everything on every event, five boolean "dirty"
//   flags track which screen regions need updating.  flush() checks each
//   flag and calls only the necessary redraw function(s), then calls
//   doupdate() once to push all changes to the physical terminal.
//
// Key bindings:
//   Enter           Send the current input line
//   Up / Down       Command history
//   Left / Right    Move cursor in the input line
//   Backspace       Delete character before cursor
//   Tab             Nick tab-completion (cycles through matches)
//   PageUp / Down   Scroll the chat area
//   F2 / Ctrl+P     Switch to previous buffer
//   F3 / Ctrl+N     Switch to next buffer
//   Ctrl+A          Jump to the next buffer with unread activity
//   Ctrl+L          Toggle the user-list sidebar
//   ESC + 1–9       Switch directly to buffer N
//   /exit           Quit the application
//   /clear          Clear the current buffer's message history
//   /close          Close the current private-chat buffer
//   /nicklist       Toggle the user-list sidebar (same as Ctrl+L)
class NcursesUI : public QObject {
    Q_OBJECT

  public:
    explicit NcursesUI(QObject* parent = nullptr);
    ~NcursesUI();

    // Initialise ncurses, set up the QSocketNotifier for stdin, create the
    // ncurses sub-windows, and start the info-bar clock timer.
    // Must be called once before the Qt event loop starts.
    void start();

  public slots:
    // Register a new ICBSession with this UI.  Connects all relevant signals
    // and adds the session to m_buffers.  Safe to call multiple times with
    // different sessions to support multi-server use.
    void addSession(ICBSession* session);

  private slots:
    // Called by QSocketNotifier when stdin has data ready.  Drains all
    // available key events via get_wch() and dispatches each to handleInput().
    void onStdinReady();

    // Appends a date-separator line to every open buffer once per calendar day.
    void onDayChanged(const QString& message, const QDate& newDate);

    // Ticks once per second to keep the clock in the info bar up to date.
    void updateInfoBar();

    // Called when ICBSession emits whoInfoReceived (after /who or a live topic
    // change).  Stores the new flags and topic for the session and redraws the
    // topic bar and info bar if this is the current buffer.
    void onSessionWhoInfo(ICBSession* session, const QString& flags, const QString& topic);

    // Called when a public group message arrives.
    void onSessionMessage(ICBSession* session, const QString& sender, const QString& text);

    // Called when a server/status message arrives.
    void onSessionSystemMessage(ICBSession* session, const QString& text);

    // Called when an incoming private message arrives.  Ensures a
    // PrivateChatLogic buffer exists (PrivateChatLogic itself handles display).
    void onSessionPrivateMessage(ICBSession* session, const QString& from, const QString& text);

    // Called when the user switches group.  A whoInfoReceived with empty
    // strings will follow, which resets the topic/flags display.
    void onSessionGroupChanged(ICBSession* session, const QString& newGroup);

    // Called when the user-list contents change for any session.
    // Redraws the sidebar if the changed session is the current buffer.
    void redrawForUserListSender();

    // Called when a user's entry is updated (nick change, moderator change).
    void onUserUpdated(const QString& oldNick, const UserInfo& newInfo);

    // Called by a PrivateChatLogic when a message arrives from the other party.
    void onPrivateChatIncoming(const QString& from, const QString& message);

    // Called by a PrivateChatLogic when we send a message.
    void onPrivateChatOutgoing(const QString& to, const QString& message);

  private:
    // Per-buffer state stored in the hash maps.
    struct BufferData {
        bool        unread     = false;  // true if there are unseen messages
        QStringList messages;            // full message history (capped at 1000)
        int         scrollLine = 0;      // index of the first visible display line
        bool        following  = true;   // if true, auto-scroll to keep newest message visible
    };

    // Ordered list of all open buffers.  Each entry is either an ICBSession*
    // or a PrivateChatLogic*.  Buffers are numbered 1–N in the info bar.
    QList<QObject*> m_buffers;
    QObject*        m_currentBuffer = nullptr;

    // Separate data maps for each buffer type.
    QHash<ICBSession*,      BufferData> m_sessionData;
    QHash<PrivateChatLogic*, BufferData> m_privateChatData;

    // Per-session topic strings (formatted for display " Some topic" or
    // " (No topic)").
    QHash<ICBSession*, QString> m_sessionTopics;

    // Per-session group-flags strings "(m)" for moderated.
    QHash<ICBSession*, QString> m_groupFlags;

    // ncurses sub-windows
    WINDOW* m_topicWin = nullptr;  // 1-line topic bar at the very top
    WINDOW* m_chatWin  = nullptr;  // main scrollable message area
    WINDOW* m_userWin  = nullptr;  // optional 20-column user list on the right
    WINDOW* m_infoWin  = nullptr;  // 1-line status bar near the bottom
    WINDOW* m_inputWin = nullptr;  // 1-line text input at the very bottom

    // Input line state
    QString     m_inputBuffer;            // text currently being typed
    int         m_inputPos = 0;           // cursor position within m_inputBuffer
    QStringList m_history;               // previously sent commands/messages
    int         m_historyIndex = -1;      // -1 = not browsing history
    QString     m_savedInput;             // saved current line when history browsing starts

    // Tab-completion state
    QString     m_completionPrefix;       // the prefix being completed
    QString     m_lastInserted;           // text placed in the buffer by the most recent Tab
    int         m_completionIndex = 0;    // which completion candidate to insert next
    QStringList m_completions;            // list of matching nicks
    bool        m_inCompletion = false;   // true while cycling through completions

    // Misc
    DayChangeNotifier* m_dayNotifier    = nullptr;
    QSocketNotifier*   m_stdinNotifier  = nullptr;
    QTimer*            m_infoTimer      = nullptr;  // 1-second tick for the clock
    bool               m_escapePending  = false;    // true after ESC, expecting a digit
    bool               m_showUserList   = true;     // whether the sidebar is visible

    // Dirty flags for deferred rendering
    // Setting a flag marks that region as needing a redraw.  flush() checks
    // all flags and calls only what is necessary before calling doupdate().
    bool m_topicDirty = true;
    bool m_infoDirty  = true;
    bool m_userDirty  = true;
    bool m_chatDirty  = true;
    bool m_inputDirty = true;

    // Buffer helpers
    // Returns the BufferData for the given buffer (session or private chat).
    BufferData& bufferData(QObject* buf);

    // Appends msg to m_currentBuffer's history, advances scrollLine if in
    // following mode, and respects the 1000-message cap.
    void addMessageToCurrent(const QString& msg);

    // Appends msg to an arbitrary buffer's history (used for background buffers
    // and day-separator lines).
    void appendToBuffer(QObject* buf, const QString& msg);

    // Remove a session or private chat from m_buffers and clean up its data.
    void removeSession(ICBSession* session);

    // Result struct returned by getOrCreatePrivateChat so callers know whether
    // the chat was newly created (and should be switched to).
    struct PrivateChatResult {
        PrivateChatLogic* chat;
        bool              created;
    };

    // Returns the existing PrivateChatLogic for (session, nick) or creates one.
    PrivateChatResult getOrCreatePrivateChat(ICBSession* session, const QString& nick);

    // Adds a PrivateChatLogic to m_buffers and connects its signals.
    void addPrivateChat(PrivateChatLogic* chat);

    // Removes a PrivateChatLogic from m_buffers and switches away if needed.
    void removePrivateChat(PrivateChatLogic* chat);

    // Drawing
    // (Re-)create all ncurses sub-windows to fit the current terminal size.
    void resizeWindows();

    // Redraw individual regions.  Each is guarded by a dirty flag in flush().
    void redrawTopic();    // blue topic bar at the top
    void redrawInfo();     // blue status bar near the bottom
    void redrawUserList(); // optional nick list on the right
    void redrawChat();     // main scrollable message area
    void redrawInput();    // input line at the bottom

    // Re-evaluate the scroll position after a terminal resize so content
    // doesn't jump unexpectedly.
    void adjustScrollAfterResize();

    // Check all dirty flags and call only the needed redraw functions, then
    // call doupdate() once to push changes to the physical terminal.
    void flush();

    // Force all dirty flags true and call flush() (used on startup and resize).
    void draw();

    // Input handling
    // Dispatch a single key code to the appropriate action.
    void handleInput(int ch);

    // Move to the next or previous buffer (direction = +1 or -1).
    void switchBuffer(int direction);

    // Perform or cycle tab-completion from the current input line context.
    void tryComplete();

    // Info helpers
    // Display name of the current buffer ("groupname" or "nick").
    QString     currentBufferName() const;

    // Nick list for the current buffer (empty for private-chat buffers).
    QStringList currentUserList()   const;

};

#endif  // NCURSESUI_H
