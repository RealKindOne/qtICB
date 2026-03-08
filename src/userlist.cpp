#include "userlist.h"
#include <QDebug>
#include <algorithm>

UserList::UserList(QObject* parent)
    : QObject(parent) {}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

// Case-insensitive linear search.  UserInfo::operator== handles the comparison.
bool UserList::contains(const QString& nick) const {
    for (const UserInfo& u : m_users) {
        if (u == nick)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

// Add a new user to the list.  If the nick is already present (possible when
// the server sends duplicate Arrive packets or overlapping /who results), the
// call is silently ignored rather than creating a duplicate entry.
void UserList::addUser(const QString& nick, const QString& ident,
                       const QString& host, bool isModerator) {
    if (contains(nick)) return;

    UserInfo u{nick, ident, host, isModerator};
    m_users.append(u);
    sort();               // maintain moderator-first alphabetical order
    emit userAdded(u);
    emit listChanged();
}

// Remove the user with the given nick (case-insensitive).
// No-ops silently if the nick is not found (the server can occasionally send
// duplicate Depart/Sign-off status messages).
void UserList::removeUser(const QString& nick) {
    auto it = std::find_if(m_users.begin(), m_users.end(),
                           [&](const UserInfo& u) { return u == nick; });
    if (it != m_users.end()) {
        m_users.erase(it);
        emit userRemoved(nick);
        emit listChanged();
    }
}

// Update the nick and moderator status of an existing user.
// Called when a "Name" status message (nick change) arrives.
// The ident and host are preserved; only nick and isModerator change.
void UserList::updateUser(const QString& oldNick, const QString& newNick,
                          bool isModerator) {
    auto it = std::find_if(m_users.begin(), m_users.end(),
                           [&](const UserInfo& u) { return u == oldNick; });
    if (it != m_users.end()) {
        it->nick        = newNick;
        it->isModerator = isModerator;
        sort();
        emit userUpdated(oldNick, *it);
        emit listChanged();
    }
}

// Set or clear the moderator flag for an existing user.
// Triggers a re-sort so the user moves to/from the top of the list.
// No-ops if the nick is not found or if the flag is already in the desired state.
void UserList::setModerator(const QString& nick, bool isModerator) {
#ifdef QT_DEBUG
    qDebug() << "UserList::setModerator" << nick << "to" << isModerator;
#endif
    auto it = std::find_if(m_users.begin(), m_users.end(),
                           [&](const UserInfo& u) { return u == nick; });
    if (it != m_users.end() && it->isModerator != isModerator) {
        it->isModerator = isModerator;
        sort();
        emit userUpdated(nick, *it);
        emit listChanged();
    }
#ifdef QT_DEBUG
    else {
        qDebug() << "UserList::setModerator: nick not found or already set";
    }
#endif
}

// Remove all entries.  Emits only listChanged (not individual userRemoved
// signals) because this is used during group switches where the entire list
// is replaced by an incoming /who response - there is no benefit in notifying
// the UI about each individual removal.
void UserList::clear() {
    if (!m_users.isEmpty()) {
        m_users.clear();
        emit listChanged();
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Returns nicks in the current sort order with "@" prepended to moderators.
// This is what gets shown in the sidebar widget and offered for tab-completion.
QStringList UserList::displayNames() const {
    QStringList names;
    for (const UserInfo& u : m_users)
        names.append(u.isModerator ? "@" + u.nick : u.nick);
    return names;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

// Sort criteria (in priority order):
//   1. Moderators before non-moderators.
//   2. Case-insensitive alphabetical order within each group.
// Called after every mutation that could affect display order.
void UserList::sort() {
    std::sort(m_users.begin(), m_users.end(),
              [](const UserInfo& a, const UserInfo& b) {
                  if (a.isModerator != b.isModerator)
                      return a.isModerator > b.isModerator;  // true > false --> mods first
                  return QString::compare(a.nick, b.nick, Qt::CaseInsensitive) < 0;
              });
}
