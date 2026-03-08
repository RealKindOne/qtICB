#ifndef COMMANDHANDLER_H
#define COMMANDHANDLER_H

#include <QString>
#include <functional>

// CommandHandler parses user-typed slash commands and dispatches them through
// a set of caller-supplied callbacks.
//
// Return value of handle():
//   true  - the input was recognized as a slash command (even if the command
//            itself is a no-op like a bare "/").  The caller should NOT send
//            the raw text to the server.
//   false - the input does not start with '/' (plain chat message).  The
//            caller should send it as a public group message.
//
// Unrecognized commands:
//   Any "/foo bar" that doesn't match a built-in is forwarded raw to the
//   server via cb.sendRaw("foo", "bar"). This lets users send native ICB
//   commands without the client needing to enumerate every possible ICB
//   subcommand.
class CommandHandler {
  public:
    // Caller-supplied action callbacks. Any callback left as nullptr (the
    // default for std::function) is silently skipped - callers only need to
    // populate the actions they support.
    struct Callbacks {
        // Send a private message to 'to' with body 'msg'.
        std::function<void(const QString& to, const QString& msg)> sendPrivate;

        // Send a raw ICB command: sendRaw("g", "icb") --> /join icb.
        std::function<void(const QString& cmd, const QString& arg)> sendRaw;

        // Open (or focus) the private-chat window/session for 'nick'.
        std::function<void(const QString& nick)> openPrivateChat;

        // Gracefully close the current server connection.
        std::function<void()> disconnect;

        // Terminate the application (currently unused / reserved).
        std::function<void()> quitApplication;
    };

    // Parse 'input' and execute the matching callback(s).
    // Returns true if the input was a slash command, false if it was plain text.
    static bool handle(const QString& input, const Callbacks& cb);
};

#endif  // COMMANDHANDLER_H
