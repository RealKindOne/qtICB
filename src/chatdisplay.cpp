#include "chatdisplay.h"
#include <QDateTime>
#include <QScrollBar>
#include <QTextBlock>
#include "formatting.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ChatDisplay::ChatDisplay(QWidget* parent) : QTextEdit(parent) {
    setReadOnly(true);
    setFont(QFont("FixedSys", 9));
    document()->setDocumentMargin(0);  // remove default QTextDocument padding
}

// ---------------------------------------------------------------------------
// Message appending helpers
// ---------------------------------------------------------------------------

// Appends a formatted user message:  "[HH:MM:SS] <sender> message text"
//
// Implementation notes:
//   - We use QTextCursor to insert directly rather than the inherited
//     append() because append() can introduce unwanted paragraph spacing.
//   - The first message has no preceding block (cursor.atStart()), so we
//     skip the leading insertBlock() call to avoid a blank first line.
//   - A hanging indent of 20 pixels makes wrapped text align under the
//     message body rather than at column 0.
//   - The timestamp is always gray; the nick+message uses 'color'.
//   - After insertion we pin the scroll bar to the bottom.
void ChatDisplay::appendUserMessage(const QString& sender, const QString& message,
                                    const QColor& color) {
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);

    if (!cursor.atStart()) {
        // Not the first message: start a new block with zero vertical margins
        // and a hanging indent so wrapped lines line up nicely.
        cursor.insertBlock();
        QTextBlockFormat blockFormat;
        blockFormat.setTopMargin(0);
        blockFormat.setBottomMargin(0);
        const int hangingIndent = 20;  // pixels; matches the ncurses HANGING_INDENT in character terms
        blockFormat.setLeftMargin(hangingIndent);
        blockFormat.setTextIndent(-hangingIndent);  // first line pulled back to column 0
        cursor.mergeBlockFormat(blockFormat);
    }

    // Gray timestamp prefix.
    QTextCharFormat grayFormat;
    grayFormat.setForeground(Qt::gray);
    cursor.insertText("[" + currentTimestamp() + "] ", grayFormat);

    // Colored message body.
    QTextCharFormat msgFormat;
    msgFormat.setForeground(color);
    cursor.insertText(QString("<%1> %2").arg(sender, message), msgFormat);

    setTextCursor(cursor);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());  // scroll to bottom
}

// Appends a formatted system/status message:  "[HH:MM:SS] message text"
//
// Identical layout to appendUserMessage except the body is the raw 'message'
// string rather than a "<nick> text" composite.  The timestamp is always gray;
// 'color' applies only to the message text (typically Qt::gray for status lines
// and Qt::red for errors).
void ChatDisplay::appendSystemMessage(const QString& message, const QColor& color) {
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);

    if (!cursor.atStart()) {
        cursor.insertBlock();
        QTextBlockFormat blockFormat;
        blockFormat.setTopMargin(0);
        blockFormat.setBottomMargin(0);
        const int hangingIndent = 20;
        blockFormat.setLeftMargin(hangingIndent);
        blockFormat.setTextIndent(-hangingIndent);
        cursor.mergeBlockFormat(blockFormat);
    }

    QTextCharFormat grayFormat;
    grayFormat.setForeground(Qt::gray);
    cursor.insertText("[" + currentTimestamp() + "] ", grayFormat);

    QTextCharFormat msgFormat;
    msgFormat.setForeground(color);
    cursor.insertText(message, msgFormat);

    setTextCursor(cursor);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

// ---------------------------------------------------------------------------
// Mouse event
// ---------------------------------------------------------------------------

// Let the base class handle the event first (so text selection still works),
// then emit clicked() so parent widgets can redirect focus to the input line.
void ChatDisplay::mousePressEvent(QMouseEvent* event) {
    QTextEdit::mousePressEvent(event);
    emit clicked();
}
