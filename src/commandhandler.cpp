#include "commandhandler.h"
#include <QStringList>

// Parse one line of user input and dispatch to the appropriate callback(s).
//
// Parsing steps:
//   1. Trim whitespace. If empty or no leading '/', return false (plain text).
//   2. Strip the leading '/' and split on spaces (skip empty parts).
//   3. Lowercase the first token as the command name.
//   4. Rejoin the remaining tokens as the argument string.
//   5. Match against built-in commands; fall through to raw send if unknown.
//
// Built-in command --> ICB mapping:
//   /quit, /exit        --> cb.disconnect()              (close connection)
//   /query, /msg <n>    --> cb.openPrivateChat(n)        (open PM window)
//   /query <n> <text>   --> cb.openPrivateChat + cb.sendPrivate
//   /join <group>       --> cb.sendRaw("g", group)       (ICB 'h g' command)
//   /nick <new>         --> cb.sendRaw("name", new)      (ICB 'h name' command)
//   /list               --> cb.sendRaw("w", "-g")        (list all groups)
//   /anything <arg>     --> cb.sendRaw(anything, arg)    (pass-through)
bool CommandHandler::handle(const QString& input, const Callbacks& cb) {
    QString trimmed = input.trimmed();

    // Not a command - caller should treat this as a plain chat message.
    if (trimmed.isEmpty() || !trimmed.startsWith('/'))
        return false;

    // Strip the leading '/' and tokenize.
    QString command = trimmed.mid(1);
    QStringList parts = command.split(' ', Qt::SkipEmptyParts);

    // Bare "/" with nothing after it: consumed as a command (suppress send)
    // but there is nothing to execute.
    if (parts.isEmpty())
        return true;

    QString cmd = parts[0].toLower();
    QString arg = parts.mid(1).join(' ');  // everything after the command token

    // /quit or /exit - disconnect from the server.
    if (cmd == "quit" || cmd == "exit") {
        if (cb.disconnect) cb.disconnect();
        return true;
    }

    // /query <nick> [message]  or  /msg <nick> [message]
    // Opens a private-chat window for 'nick'.  If a message body follows,
    // sends it immediately.
    // TODO: /msg should perhaps just echo in the active window rather than
    //       always opening a dedicated query tab.
    if ((cmd == "query" || cmd == "msg") && parts.size() >= 2) {
        if (cb.openPrivateChat) cb.openPrivateChat(parts[1]);
        if (parts.size() >= 3) {
            QString msg = parts.mid(2).join(' ');
            if (cb.sendPrivate) cb.sendPrivate(parts[1], msg);
        }
        return true;
    }

    // /join <group> - switch to a different ICB group.
    // Maps to the ICB 'h g <group>' command.
    if (cmd == "join" && parts.size() >= 2) {
        if (cb.sendRaw) cb.sendRaw("g", parts[1]);
        return true;
    }

    // /nick <newNick> - change our nickname.
    // Maps to the ICB 'h name <newNick>' command.
    if (cmd == "nick" && parts.size() >= 2) {
        if (cb.sendRaw) cb.sendRaw("name", parts[1]);
        return true;
    }

    // /list - request a listing of all active ICB groups.
    // Uses the special ICB wildcard argument "-g" to the 'w' (who) command.
    if (cmd == "list") {
        if (cb.sendRaw) cb.sendRaw("w", "-g");
        return true;
    }

    // Pass-through: any unrecognized "/foo [args]" is forwarded directly to
    // the server as ICB command subcommand 'foo' with argument 'args'.
    if (cb.sendRaw) cb.sendRaw(cmd, arg);
    return true;
}
