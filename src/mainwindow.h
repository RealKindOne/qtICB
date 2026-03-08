#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCloseEvent>
#include <QDateTime>
#include <QHash>
#include <QMainWindow>
#include "connectionwidget.h"
#include "daychangenotifier.h"
#include "icbclient.h"

class PrivateChat;
class DayChangeNotifier;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// MainWindow is the top-level application window.
//
// It owns a QTabWidget that holds two kinds of real tabs:
//   - ConnectionWidget  - one per server connection (chat + user list + input)
//   - PrivateChat       - one per active private conversation
//
// A permanent non-clickable "+" tab always lives at the far right end of the
// tab bar.  Clicking the button on it opens a new ConnectionWidget tab.
//
// Responsibilities:
//   - Creating and destroying tabs
//   - Tracking which PrivateChat tabs belong to which ICBSession so they can
//     be bulk-removed when that connection tab is closed
//   - Keeping the window title bar in sync with the currently visible tab
//   - Marking tabs red when unread activity arrives on a background tab
//   - Asking for confirmation before closing a live connection or quitting
//   - Broadcasting day-change separator lines to every open buffer at midnight
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

  protected:
    // Prompts for confirmation if any ConnectionWidget tab is still connected.
    void closeEvent(QCloseEvent* event) override;

  private slots:
    // Inserts a new ConnectionWidget tab just before the permanent "+" tab.
    void addNewTab();

    // Handles the close button on a tab.  Guards against closing the last
    // remaining connection tab or the "+" tab.  When a ConnectionWidget is
    // closed, all of its associated PrivateChat tabs are closed too.
    void closeTab(int index);

    // Called when a public message arrives on a ConnectionWidget that isn't
    // the currently visible tab - colors that tab red.
    void onTabMessageActivity();

    // Called when the user switches tabs.  Clears the unread (red) color on
    // the newly active tab and rebuilds the window title to match its content.
    void onTabCurrentChanged(int index);

    // Called when a private message arrives on a PrivateChat tab that isn't
    // currently visible - colors that tab red.
    void onPrivateChatActivity(PrivateChat* chat);

    // Called once per calendar day.  Appends a date-separator line to every
    // open ConnectionWidget and PrivateChat buffer so users can see in their
    // history exactly when midnight passed.
    void onDayChanged(const QString& message, const QDate& newDate);

  private:
    Ui::MainWindow* ui;

    // Two-level map: ICBSession --> (nick --> PrivateChat widget).
    // Allows O(1) lookup when routing an incoming private message, and
    // enables bulk cleanup when the parent ConnectionWidget tab is closed.
    QHash<ICBSession*, QHash<QString, PrivateChat*>> m_privateChats;

    // Set to true at the very start of destruction so that QObject::destroyed
    // lambdas captured in addNewTab() don't attempt to modify m_privateChats
    // while the object is being torn down.
    bool m_destroying;

    // Returns the existing PrivateChat for (session, nick), or creates a new
    // tab for it.  Nick comparison is case-insensitive (ICB convention).
    PrivateChat* getOrCreatePrivateChat(ICBSession* session, const QString& user);

    // Shared helper used by onTabMessageActivity and onPrivateChatActivity:
    // colors the tab for 'widget' red if it is not the currently active tab.
    void markTabUnread(QWidget* widget);

    // Fires the dayChanged signal once per calendar day.
    DayChangeNotifier* m_dayNotifier;
};

#endif  // MAINWINDOW_H
