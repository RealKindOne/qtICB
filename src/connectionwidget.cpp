#include "connectionwidget.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include "color.h"
#include "icbclient.h"
#include "userlist.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ConnectionWidget::ConnectionWidget(QWidget* parent)
    : QWidget(parent),
      m_session(new ICBSession(QCoreApplication::applicationDirPath(), this)) {

    // Connection form fields
    m_serverEdit = new QLineEdit(this);
    m_serverEdit->setText("default.icb.net");

    m_portEdit = new QLineEdit(this);
    m_portEdit->setText("7326");           // standard ICB port

    m_nicknameEdit = new QLineEdit(this);
    m_nicknameEdit->setText("example");

    m_groupEdit = new QLineEdit(this);
    m_groupEdit->setText("foobar");

    m_connectButton = new QPushButton("Connect", this);

    // Chat display
    // Read-only rich-text area that shows all messages.  An event filter is
    // installed so that clicking anywhere on it re-focuses the input line.
    m_messageDisplay = new ChatDisplay(this);
    m_messageDisplay->setReadOnly(true);
    m_messageDisplay->setFont(QFont("FixedSys", 9));
    m_messageDisplay->document()->setDocumentMargin(0);
    m_messageDisplay->installEventFilter(this);

    // User list sidebar
    // Shows nicks of everyone currently in the group.  Moderators are
    // prefixed with "@" and sort to the top.
    m_userList = new QListWidget(this);
    m_userList->setSortingEnabled(true);
    m_userList->setMaximumWidth(100);

    // Message input
    // Disabled until we reach the LoggedIn state.
    // HistoryLineEdit provides Up/Down command history and Tab nick-completion.
    m_messageInput = new HistoryLineEdit(this);
    m_messageInput->setEnabled(false);
    m_messageInput->installEventFilter(this);

    // Layout
    // Top row: all connection fields + button inside a group box.
    QHBoxLayout* connLayout = new QHBoxLayout;
    connLayout->addWidget(new QLabel("Server:", this));
    connLayout->addWidget(m_serverEdit);
    connLayout->addWidget(new QLabel("Port:", this));
    connLayout->addWidget(m_portEdit);
    connLayout->addWidget(new QLabel("Nickname:", this));
    connLayout->addWidget(m_nicknameEdit);
    connLayout->addWidget(new QLabel("Group:", this));
    connLayout->addWidget(m_groupEdit);
    connLayout->addWidget(m_connectButton);

    QGroupBox* connGroup = new QGroupBox("Connection", this);
    connGroup->setMaximumHeight(60);
    connGroup->setLayout(connLayout);

    // Middle: chat display and user list separated by a draggable splitter.
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_messageDisplay);
    splitter->addWidget(m_userList);

    // Stack: connection form --> splitter (takes all spare vertical space) --> input.
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(connGroup);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addWidget(m_messageInput);
    setLayout(mainLayout);

    // Signal wiring
    setupConnections();   // ICBSession --> this

    // Connect button toggles between initiating a connection and dropping one.
    connect(m_connectButton, &QPushButton::clicked,
            this, &ConnectionWidget::onConnectButtonClicked);

    // Pressing Enter in the input line sends the text.
    connect(m_messageInput, &HistoryLineEdit::sendRequested,
            this, &ConnectionWidget::processInput);

    // Clicking the chat display restores keyboard focus to the input line.
    connect(m_messageDisplay, &ChatDisplay::clicked, this, [this]() {
        m_messageInput->setFocus();
    });

    // Keep the tab-completer candidate list and the sidebar in sync whenever
    // the group's user list changes (joins, leaves, nick changes, moderator
    // promotions/demotions).
    connect(m_session->userList(), &UserList::listChanged, this, [this]() {
        const QStringList names = m_session->userList()->displayNames();
        m_messageInput->setUserList(names);
        m_userList->clear();
        m_userList->addItems(names);
    });

    m_messageDisplay->appendSystemMessage(
        "qtICB Ready. Enter server details and click Connect.", COLOR_QT_BLUE);
}

ConnectionWidget::~ConnectionWidget() = default;

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Gracefully closes the current connection if one is open.
void ConnectionWidget::disconnectFromServer() {
    if (m_session->state() != ICBClient::Disconnected)
        m_session->disconnectFromServer();
}

ICBClient::ConnectionState ConnectionWidget::connectionState() const {
    return static_cast<ICBClient::ConnectionState>(m_session->state());
}

// Appends a plain system message to the chat display. These are not
// logged.
void ConnectionWidget::appendSystemMessage(const QString& message) {
    m_messageDisplay->appendSystemMessage(message, COLOR_QT_GRAY);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

// Wire up all ICBSession signals to the appropriate slots on this widget.
// Kept in a separate method to avoid cluttering the constructor.
void ConnectionWidget::setupConnections() {
    // Connection lifecycle
    connect(m_session, &ICBSession::connectionStateChanged,
            this, &ConnectionWidget::onConnectionStateChanged);
    connect(m_session, &ICBSession::errorOccurred,
            this, &ConnectionWidget::onErrorOccurred);
    connect(m_session, &ICBSession::loggedIn,
            this, &ConnectionWidget::onLoggedIn);

    // Group name changes - relay as groupNameChanged so MainWindow can update
    // the tab label.  Both a direct group switch (groupChanged) and a rename
    // (groupRenamed) produce the same visible effect on the tab.
    connect(m_session, &ICBSession::groupChanged, this,
        [this](const QString& g) { emit groupNameChanged(g); });
    connect(m_session, &ICBSession::groupRenamed, this,
        [this](const QString& /*old*/, const QString& newName, const QString& /*by*/) {
            emit groupNameChanged(newName);
        });

    // Incoming messages
    connect(m_session, &ICBSession::messageReceived,
            this, &ConnectionWidget::onMessageReceived);
    connect(m_session, &ICBSession::systemMessageReceived,
            this, &ConnectionWidget::onSystemMessageReceived);
    connect(m_session, &ICBSession::personalMessageReceived,
            this, &ConnectionWidget::onPersonalMessageReceived);

    // Echo our own sent messages in dark green.
    connect(m_session, &ICBSession::selfMessageSent, this, [this](const QString& msg) {
        m_messageDisplay->appendUserMessage(m_session->nickname(), msg, COLOR_QT_DARKGREEN);
    });

    // Private chat plumbing: forward signals up to MainWindow so it can
    // create or focus the appropriate PrivateChat tab.
    connect(m_session, &ICBSession::selfPrivateMessageSent,
            this, &ConnectionWidget::privateMessageSent);
    connect(m_session, &ICBSession::openPrivateChatRequested,
            this, &ConnectionWidget::openPrivateChat);

    // Group flags and topic (from /who output or a live Topic packet).
    // ICBSession parses the raw "Group:" line and emits whoInfoReceived once
    // so neither UI has to duplicate the regex logic.  We relay it as
    // groupInfoChanged so MainWindow can update the window title bar.
    connect(m_session, &ICBSession::whoInfoReceived, this,
        [this](const QString& flags, const QString& topic) {
            emit groupInfoChanged(flags, topic);
        });
}

// ---------------------------------------------------------------------------
// Slot implementations
// ---------------------------------------------------------------------------

// Toggle between initiating a new TCP connection and dropping an existing one.
void ConnectionWidget::onConnectButtonClicked() {
    if (m_session->state() != ICBClient::Disconnected) {
        m_session->disconnectFromServer();
        m_connectButton->setText("Connect");
        m_userList->clear();
        return;
    }

    QString host     = m_serverEdit->text().trimmed();
    quint16 port     = m_portEdit->text().toUShort();
    QString nickname = m_nicknameEdit->text().trimmed();
    QString group    = m_groupEdit->text().trimmed();

    if (host.isEmpty() || nickname.isEmpty()) {
        QMessageBox::warning(this, "Input Error",
                             "Please enter server host and nickname.");
        return;
    }
    if (port == 0) port = 7326;  // fall back to the standard ICB port

    m_connectButton->setEnabled(false);
    m_connectButton->setText("Connecting...");
    m_messageInput->setEnabled(false);

    m_messageDisplay->appendSystemMessage(
        QString("Connecting to %1:%2...").arg(host).arg(port), COLOR_QT_BLUE);
    m_session->connectToServer(host, port, nickname, group);

    // Safety valve: if the connection hasn't completed within 10 seconds,
    // re-enable the button so the user can retry or change the address.
    QTimer::singleShot(10000, this, [this]() {
        if (m_session->state() == ICBClient::Connecting) {
            m_connectButton->setEnabled(true);
            m_connectButton->setText("Connect");
        }
    });
}

// Route text from the input line.  "/clear" is handled locally; everything
// else goes to ICBSession which dispatches commands or sends public messages.
void ConnectionWidget::processInput(const QString& text) {
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;

    if (trimmed == "/clear") {
        m_messageDisplay->clear();
        return;
    }

    m_session->processUserInput(trimmed);
}

// Update button labels and input-field availability as the connection moves
// through its state machine:
//   Disconnected --> Connecting --> Connected --> LoggingIn --> LoggedIn
// (and back to Disconnected on error or /quit).
void ConnectionWidget::onConnectionStateChanged(ICBClient::ConnectionState state) {
    switch (state) {
        case ICBClient::Disconnected:
            m_connectButton->setText("Connect");
            m_connectButton->setEnabled(true);
            m_messageInput->setEnabled(false);
            break;
        case ICBClient::Connecting:
            m_connectButton->setText("Connecting...");
            break;
        case ICBClient::Connected:
            // TCP connection established; waiting for the server's opening packet.
            m_connectButton->setText("Disconnect");
            m_connectButton->setEnabled(true);
            break;
        case ICBClient::LoggingIn:
            // Login packet sent; waiting for the server to acknowledge.
            break;
        case ICBClient::LoggedIn:
            m_connectButton->setText("Disconnect");
            m_connectButton->setEnabled(true);
            m_messageInput->setEnabled(true);
            m_messageInput->setFocus();
            break;
    }
}

// A public group message arrived from another user.
// Displayed in black, and messageActivity() is emitted so MainWindow can
// highlight this tab red if it is currently in the background.
void ConnectionWidget::onMessageReceived(const QString& sender, const QString& message) {
    m_messageDisplay->appendUserMessage(sender, message, COLOR_QT_BLACK);
    emit messageActivity();
}

// An incoming private (personal) message.  We only forward it up to
// MainWindow here; PrivateChatLogic is already connected to
// ICBSession::personalMessageReceived and will render the message itself.
void ConnectionWidget::onPersonalMessageReceived(const QString& from, const QString& message) {
    emit privateMessageReceived(from, message);
}

// A server or status message (join/leave notices, /who output, etc.).
// Prefixed with "* " to visually distinguish it from user messages.
void ConnectionWidget::onSystemMessageReceived(const QString& message) {
    m_messageDisplay->appendSystemMessage(QString("* %1").arg(message), COLOR_QT_GRAY);
}

// A socket or protocol error - shown in red so it stands out.
void ConnectionWidget::onErrorOccurred(const QString& error) {
    m_messageDisplay->appendSystemMessage(
        QString("Error: %1").arg(error), COLOR_QT_RED);
}

// Server acknowledged the login.  Emit groupNameChanged so MainWindow can
// replace the "Disconnected" placeholder text on the tab with the actual
// group name the user has joined.
void ConnectionWidget::onLoggedIn() {
    emit groupNameChanged(m_session->currentGroup());
}

// ---------------------------------------------------------------------------
// Event filter
// ---------------------------------------------------------------------------

// Handles two special keyboard/mouse interactions:
//   1. Mouse click on m_messageDisplay --> move focus to the input line.
//   2. PageUp/Down pressed on m_messageInput --> scroll the chat display.
bool ConnectionWidget::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_messageDisplay && event->type() == QEvent::MouseButtonPress) {
        m_messageInput->setFocus();
        return true;
    }

    if (obj == m_messageInput && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_PageUp) {
            QScrollBar* vbar = m_messageDisplay->verticalScrollBar();
            vbar->setValue(vbar->value() - vbar->pageStep());
            return true;  // consume event - do not pass to the line edit
        } else if (keyEvent->key() == Qt::Key_PageDown) {
            QScrollBar* vbar = m_messageDisplay->verticalScrollBar();
            vbar->setValue(vbar->value() + vbar->pageStep());
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
