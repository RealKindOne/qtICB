#include "mainwindow.h"
#include <QCloseEvent>
#include <QDebug>
#include <QMessageBox>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include "connectionwidget.h"
#include "icbclient.h"
#include "privatechat.h"
#include "ui_mainwindow.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MainWindow::~MainWindow() {
    // Set the guard flag before deleting the UI.  Any QObject::destroyed()
    // signals fired during widget teardown will check this flag and bail out
    // early rather than touching m_privateChats on a half-destroyed object.
    m_destroying = true;
    delete ui;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_destroying(false), m_dayNotifier(nullptr) {
    ui->setupUi(this);

    QTabWidget* tabWidget = ui->tabWidget;
    tabWidget->setTabsClosable(true);
    tabWidget->setMovable(true);

    // Permanent "+" tab
    // We add a disabled dummy QWidget as the last tab so we can embed a "+"
    // QToolButton in its tab-bar entry.  The tab itself is disabled (non-
    // clickable); only the button widget is interactive.
    QWidget* dummy = new QWidget(this);
    int newTabIndex = tabWidget->addTab(dummy, QString());
    tabWidget->setTabEnabled(newTabIndex, false);

    QToolButton* newTabButton = new QToolButton(this);
    newTabButton->setText("+");
    newTabButton->setToolTip("Open a new connection tab");
    newTabButton->setAutoRaise(true);   // flat style matching the tab bar
    newTabButton->setFixedSize(24, 24);

    // Attach the button to the right side of the dummy tab's bar entry, and
    // remove the left-side close button that QTabWidget adds automatically.
    tabWidget->tabBar()->setTabButton(newTabIndex, QTabBar::RightSide, newTabButton);
    tabWidget->tabBar()->setTabButton(newTabIndex, QTabBar::LeftSide, nullptr);

    connect(newTabButton, &QToolButton::clicked, this, &MainWindow::addNewTab);
    connect(tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabWidget, &QTabWidget::currentChanged,    this, &MainWindow::onTabCurrentChanged);

    // Open the first connection tab immediately on startup.
    addNewTab();

    // DayChangeNotifier emits dayChanged() once per calendar day so every
    // open buffer can display a  "Day changed to DD MONTH YYYY" separator line.
    m_dayNotifier = new DayChangeNotifier(this);
    connect(m_dayNotifier, &DayChangeNotifier::dayChanged, this, &MainWindow::onDayChanged);
}

// ---------------------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------------------

// Creates a new ConnectionWidget, inserts it just before the "+" tab, and
// connects all the signals it needs to interact with MainWindow.
void MainWindow::addNewTab() {
    QTabWidget* tabWidget = ui->tabWidget;

    // count()-1 is always the index of the "+" tab, so inserting at that
    // position keeps the "+" pinned to the right end.
    int specialIndex = tabWidget->count() - 1;
    ConnectionWidget* newConn = new ConnectionWidget(this);
    int index = tabWidget->insertTab(specialIndex, newConn, "Disconnected");
    tabWidget->setCurrentIndex(index);

    // Keep the tab label in sync with the ICB group name.
    connect(newConn, &ConnectionWidget::groupNameChanged, this, [=](const QString& group) {
        int idx = tabWidget->indexOf(newConn);
        if (idx != -1) tabWidget->setTabText(idx, group);
    });

    // Rebuild the window title bar whenever group flags or topic change, but
    // only when this connection's tab is the one currently on screen.
    connect(newConn, &ConnectionWidget::groupInfoChanged, this, [=](const QString& flags, const QString& topic) {
        if (ui->tabWidget->currentWidget() == newConn) {
            ICBSession* s = newConn->connection();
            if (s->state() == ICBClient::Disconnected) {
                // No active connection yet - show the bare application name.
                setWindowTitle("qtICB");
            } else {
                // Format: "qtICB - groupname (flags) | topic"
                QString title = s->currentGroup();
                if (!flags.isEmpty()) title += " " + flags;
                if (!topic.isEmpty()) title += " | " + topic;
                setWindowTitle("qtICB - " + title);
            }
        }
    });

    // Highlight this tab red when a new public message arrives and the tab
    // isn't currently in the foreground.
    connect(newConn, &ConnectionWidget::messageActivity, this, &MainWindow::onTabMessageActivity);

    // Incoming private message: ensure the PrivateChat tab exists.
    // The actual message display is handled by PrivateChatLogic (which is
    // already connected to ICBSession::personalMessageReceived), so we only
    // need to guarantee the tab is present.
    connect(newConn, &ConnectionWidget::privateMessageReceived, this,
        [=](const QString& from, const QString&) {
            getOrCreatePrivateChat(newConn->connection(), from);
        });

    // Outgoing private message: same - ensure the PrivateChat tab exists.
    connect(newConn, &ConnectionWidget::privateMessageSent, this,
        [=](const QString& to, const QString&) {
            getOrCreatePrivateChat(newConn->connection(), to);
        });

    // /query command or equivalent: open the PrivateChat tab and focus it.
    connect(newConn, &ConnectionWidget::openPrivateChat, this, [=](const QString& nick) {
        PrivateChat* chat = getOrCreatePrivateChat(newConn->connection(), nick);
        ui->tabWidget->setCurrentWidget(chat);
        chat->setInputFocus();
    });

    // When this ConnectionWidget is destroyed (tab closed), remove all its
    // associated PrivateChat entries from m_privateChats.
    // We capture the ICBSession pointer now because newConn will already be
    // invalid by the time the destroyed() lambda runs.
    ICBSession* coreSession = newConn->connection();
    connect(newConn, &QObject::destroyed, this, [=]() {
        if (m_destroying) return;  // whole window is being torn down; skip cleanup
        m_privateChats.remove(coreSession);
    });
}

// Handles the close button on a tab.
// Rules:
//   - The "+" tab can never be closed.
//   - The last remaining ConnectionWidget tab cannot be closed.
//   - Closing a ConnectionWidget also closes all its PrivateChat tabs.
void MainWindow::closeTab(int index) {
    QTabWidget* tabWidget = ui->tabWidget;

    // count()-1 is always the "+" tab; protect it.
    if (index == tabWidget->count() - 1) return;

    QWidget* widget = tabWidget->widget(index);
    int oldCurrent  = tabWidget->currentIndex();

    if (ConnectionWidget* conn = qobject_cast<ConnectionWidget*>(widget)) {
        // count() includes the "+" tab, so <= 2 means only one real tab remains.
        if (tabWidget->count() <= 2) {
            QMessageBox::information(this, "Cannot close",
                                     "At least one connection tab must remain open.");
            return;
        }

        if (QMessageBox::question(this, "Close Tab",
                                  "Are you sure you want to close this tab?",
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;

        // Close every PrivateChat tab that belongs to this connection before
        // removing the ConnectionWidget itself.
        ICBSession* core = conn->connection();
        auto chats = m_privateChats.value(core);
        for (PrivateChat* chat : chats) {
            int chatIndex = tabWidget->indexOf(chat);
            if (chatIndex != -1) {
                tabWidget->removeTab(chatIndex);
                chat->deleteLater();
            }
        }
        m_privateChats.remove(core);

        conn->disconnectFromServer();
        conn->deleteLater();
        tabWidget->removeTab(index);

    } else if (PrivateChat* chat = qobject_cast<PrivateChat*>(widget)) {
        // Remove just this private chat entry and close its tab.
        ICBSession* core = chat->session();
        if (core) {
            QString user = chat->otherNick();
            m_privateChats[core].remove(user);
            if (m_privateChats[core].isEmpty())
                m_privateChats.remove(core);
        }
        tabWidget->removeTab(index);
        chat->deleteLater();
    }

    // After removal, return focus to the best available tab.
    // If the tab that was closed was at or before the previously active index,
    // shift left by one and skip the "+" tab if we land on it.
    int newCount     = tabWidget->count();
    int specialIndex = newCount - 1;
    if (index <= oldCurrent) {
        int target = oldCurrent - 1;
        if (target == specialIndex) target--;  // skip the "+" tab
        tabWidget->setCurrentIndex((target >= 0 && target < specialIndex) ? target : 0);
    }
}

// ---------------------------------------------------------------------------
// Private chat management
// ---------------------------------------------------------------------------

// Returns the existing PrivateChat for (session, nick), or creates a new tab.
// Nick comparison is case-insensitive because ICB does not distinguish case.
PrivateChat* MainWindow::getOrCreatePrivateChat(ICBSession* session, const QString& user) {
    // Search the existing map before creating anything.
    if (m_privateChats.contains(session)) {
        const auto& chats = m_privateChats[session];
        for (auto it = chats.begin(); it != chats.end(); ++it) {
            if (it.key().compare(user, Qt::CaseInsensitive) == 0)
                return it.value();
        }
    }

    // Create a new PrivateChat and insert its tab before the "+" tab.
    PrivateChat* chat = new PrivateChat(session, user);
    int specialIndex  = ui->tabWidget->count() - 1;
    ui->tabWidget->insertTab(specialIndex, chat, user);

    // If the other user changes their nick, keep the tab label up to date.
    connect(chat, &PrivateChat::nickChanged, this, [this, chat](const QString& newNick) {
        int index = ui->tabWidget->indexOf(chat);
        if (index != -1)
            ui->tabWidget->setTabText(index, newNick);
    });

    connect(chat, &PrivateChat::messageActivity, this, &MainWindow::onPrivateChatActivity);
    m_privateChats[session][user] = chat;
    return chat;
}

// ---------------------------------------------------------------------------
// Window-level event handlers
// ---------------------------------------------------------------------------

// Intercept the window close event.  If any ConnectionWidget tab is still
// connected, ask the user to confirm before allowing the application to exit.
void MainWindow::closeEvent(QCloseEvent* event) {
    QTabWidget* tabWidget = ui->tabWidget;
    int count = tabWidget->count();

    for (int i = 0; i < count - 1; ++i) {  // skip the "+" tab at count()-1
        ConnectionWidget* conn = qobject_cast<ConnectionWidget*>(tabWidget->widget(i));
        if (conn && conn->connectionState() != ICBClient::Disconnected) {
            QMessageBox::StandardButton reply = QMessageBox::question(
                this, "Confirm Exit",
                "There are active connections. Are you sure you want to quit?",
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::No) {
                event->ignore();
                return;
            }
            break;  // only ask once, even if multiple tabs are connected
        }
    }

    event->accept();
}

// ---------------------------------------------------------------------------
// Unread / activity indicator helpers
// ---------------------------------------------------------------------------

// Colors the tab for 'widget' red to signal unread activity.
// Does nothing if 'widget' is the currently visible tab.
void MainWindow::markTabUnread(QWidget* widget) {
    int index = ui->tabWidget->indexOf(widget);
    if (index != -1 && index != ui->tabWidget->currentIndex())
        ui->tabWidget->tabBar()->setTabTextColor(index, Qt::red);
}

// Slot connected to ConnectionWidget::messageActivity.
// Called when a public group message arrives on a tab that isn't active.
void MainWindow::onTabMessageActivity() {
#ifdef QT_DEBUG
    qDebug() << "onTabMessageActivity received from sender" << sender();
#endif
    ConnectionWidget* widget = qobject_cast<ConnectionWidget*>(sender());
    if (!widget) {
#ifdef QT_DEBUG
        qDebug() << "onTabMessageActivity: sender is not a ConnectionWidget";
#endif
        return;
    }
    markTabUnread(widget);
}

// Slot connected to QTabWidget::currentChanged.
// Clears the red unread color on the newly active tab, then rebuilds the
// window title to reflect what the user is now looking at:
//   - Connected session  -->  "qtICB - groupname (flags) | topic"
//   - Disconnected conn  -->  "qtICB"
//   - Private chat       -->  "qtICB - Private: nick"
//   - "+" tab or other   -->  "qtICB"
void MainWindow::onTabCurrentChanged(int index) {
#ifdef QT_DEBUG
    qDebug() << "Tab changed to index" << index;
#endif
    if (index >= 0 && index < ui->tabWidget->count())
        ui->tabWidget->tabBar()->setTabTextColor(index, QColor());  // reset to default

    QWidget* w = ui->tabWidget->widget(index);
    if (ConnectionWidget* conn = qobject_cast<ConnectionWidget*>(w)) {
        ICBSession* s = conn->connection();
        if (s->state() == ICBClient::Disconnected) {
            setWindowTitle("qtICB");
        } else {
            QString title = s->currentGroup();
            if (!s->groupFlags().isEmpty()) title += " " + s->groupFlags();
            if (!s->topic().isEmpty())      title += " | " + s->topic();
            setWindowTitle("qtICB - " + title);
        }
    } else if (PrivateChat* chat = qobject_cast<PrivateChat*>(w)) {
        setWindowTitle("qtICB - Private: " + chat->otherNick());
    } else {
        setWindowTitle("qtICB");
    }
}

// Slot connected to PrivateChat::messageActivity.
// Called when a private message arrives on a tab that isn't currently active.
void MainWindow::onPrivateChatActivity(PrivateChat* chat) {
    markTabUnread(chat);
}

// ---------------------------------------------------------------------------
// Day-change notification
// ---------------------------------------------------------------------------

// Fired once per calendar day by DayChangeNotifier.
// Appends a separator line "Day changed to DD MONTH YYYY" to every open
// buffer so users can see exactly when midnight occurred in their history.
void MainWindow::onDayChanged(const QString& message, const QDate& newDate) {
    Q_UNUSED(newDate);

    // Iterate all real tabs; skip the "+" tab at count()-1.
    for (int i = 0; i < ui->tabWidget->count() - 1; ++i) {
        QWidget* widget = ui->tabWidget->widget(i);
        if (ConnectionWidget* conn = qobject_cast<ConnectionWidget*>(widget))
            conn->appendSystemMessage(message);
        else if (PrivateChat* chat = qobject_cast<PrivateChat*>(widget))
            chat->appendSystemMessage(message);
    }
}
