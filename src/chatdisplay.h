#ifndef CHATDISPLAY_H
#define CHATDISPLAY_H

#include <QColor>
#include <QTextEdit>

// ChatDisplay is a read-only QTextEdit specialized for displaying ICB chat.
//
// It provides two append methods that produce consistently formatted output:
//   - appendUserMessage  - formats a public or private message as
//                          "[HH:MM:SS] <nick> message text"
//   - appendSystemMessage - formats a server/status line as
//                          "[HH:MM:SS] message text"
//
// Formatting details:
//   - Each new message is inserted as a new QTextBlock.
//   - The "[HH:MM:SS] " timestamp is always rendered in gray regardless of the
//     message color, so it recedes visually while remaining readable.
//   - Message text uses a configurable QColor (black for public messages,
//     dark green for our own sends, purple for private messages, etc.).
//   - A hanging indent of 20 pixels is applied so wrapped lines align neatly
//     under the message text rather than under the timestamp.
//   - After each insertion the scroll bar is pinned to the bottom so the
//     most recent message is always visible.
//
// The clicked() signal is emitted on any mouse press so parent widgets can
// redirect keyboard focus to the input line without extra event filters.
class ChatDisplay : public QTextEdit {
    Q_OBJECT

  public:
    explicit ChatDisplay(QWidget* parent = nullptr);

    // Appends a user message formatted as "[HH:MM:SS] <sender> message".
    // 'color' controls the color of both the nick and the message text.
    void appendUserMessage(const QString& sender, const QString& message,
                           const QColor& color = Qt::black);

    // Appends a system/status line formatted as "[HH:MM:SS] message".
    // 'color' controls the color of the message text (timestamp is always gray).
    void appendSystemMessage(const QString& message,
                             const QColor& color = Qt::gray);

  signals:
    // Emitted on any mouse press so the parent widget can refocus the input line.
    void clicked();

  protected:
    // Calls the base class implementation (preserves text selection behaviour)
    // and then emits clicked().
    void mousePressEvent(QMouseEvent* event) override;
};

#endif  // CHATDISPLAY_H
