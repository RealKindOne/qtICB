#ifndef USERLIST_H
#define USERLIST_H

#include <QList>
#include <QObject>
#include <QString>

// Holds the information ICB provides about a single group member.
// The equality operator compares case-insensitively on nick so it can be used
// directly in QList::find_if lambdas and in PrivateChatLogic nick matching.
struct UserInfo {
    QString nick;
    QString ident;    // User ident/username part
    QString host;     // User hostname
    bool isModerator; // true when the nick has the 'm' flag in the ICB who-list

    // Case-insensitive comparison against a plain nick string.
    bool operator==(const QString& otherNick) const {
        return nick.compare(otherNick, Qt::CaseInsensitive) == 0;
    }
};

// UserList maintains the live set of users currently in an ICB group.
//
// Entries are kept in a sorted order (moderators first, then alphabetically
// by nick) so that displayNames() returns a consistently ordered list for
// both the Qt sidebar widget and the ncurses sidebar.
//
// All mutating operations emit signals so connected UIs can react:
//   - userAdded / userRemoved / userUpdated  - fine-grained change events
//   - listChanged                            - coarse "something changed" event
//     (used by UIs that only need to refresh without caring what changed)
//
// ICBSession is the primary owner; one UserList exists per ICBSession.
class UserList : public QObject {
    Q_OBJECT

  public:
    explicit UserList(QObject* parent = nullptr);

    // Accessors

    // Direct read-only access to the underlying list (for /who table building
    // in ICBSession and nick-preservation in onClientUserChangedNick).
    const QList<UserInfo>& users() const { return m_users; }

    // Number of users currently in the list.
    int count() const { return m_users.size(); }

    // Returns true if a user with this nick (case-insensitive) is in the list.
    bool contains(const QString& nick) const;

    // Mutators - emit signals on change

    // Adds a new user entry.  Silently no-ops if the nick is already present
    // (guards against duplicate userJoined signals from the server).
    void addUser(const QString& nick, const QString& ident,
                 const QString& host, bool isModerator);

    // Removes the entry for 'nick' (case-insensitive).  Silently no-ops if
    // not found (guards against duplicate userLeft signals).
    void removeUser(const QString& nick);

    // Renames a user entry and updates their moderator status.  Used when
    // processing a "Name" status message (nick change).
    void updateUser(const QString& oldNick, const QString& newNick,
                    bool isModerator);

    // Sets or clears the moderator flag for an existing user.  Re-sorts the
    // list so the display order stays correct.  No-ops if the nick is not
    // found or if the flag value is already correct.
    void setModerator(const QString& nick, bool isModerator);

    // Removes all entries and emits listChanged (but not individual
    // userRemoved signals - used during group switches when the whole list
    // is being replaced by a fresh /who response).
    void clear();

    // Returns the nick list in display order, with "@" prepended to
    // moderator nicks.  Used to populate the Qt sidebar and the ncurses
    // user panel, and to feed the tab-completion candidate list.
    QStringList displayNames() const;

  signals:
    // Emitted after a user is successfully added.
    void userAdded(const UserInfo& user);

    // Emitted after a user is removed (carries the original nick string).
    void userRemoved(const QString& nick);

    // Emitted after a nick change or moderator-status change.
    // 'oldNick' is the nick before the update; 'newInfo' is the full updated entry.
    void userUpdated(const QString& oldNick, const UserInfo& newInfo);

    // Emitted after any structural change to the list (add, remove, update,
    // clear).  UIs that don't need to distinguish between change types can
    // connect only to this signal and do a full refresh.
    void listChanged();

  private:
    QList<UserInfo> m_users;

    // Re-sorts m_users: moderators before non-moderators, then
    // case-insensitive alphabetical order within each group.
    // Called after every mutation that could change sort order.
    void sort();
};

#endif  // USERLIST_H
