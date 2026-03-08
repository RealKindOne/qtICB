# qtICB

A Qt5 client for the [ICB (Internet Citizen's Band)](http://www.icb.net) chat protocol, available in two front-ends built from a single shared codebase:

- **GUI** — tabbed Qt Widgets interface (`qtICB`)
- **Console** — full-screen ncurses terminal interface (`qtICB-console`)

---

## Known Issues / Limits

- Cannot disable group or private message logging.
- Using the same nick and group name for logging causes issues.
- Timestamps format is not customizable.
- Does not use the complete login command.
- Only one connection is allowed for the console.
- No automatic reconnecting.

---

## Features

- Connect to any ICB server (default: `default.icb.net:7326`)
- Public group chat with real-time user list sidebar
- Private messaging with dedicated per-nick tabs/buffers
- Group moderation events (moderator pass, boot, idle-mod)
- Nick changes, group renames, topic changes
- `/who` output formatted as a column-aligned table
- Tab-completion for nicks
- Command history (Up/Down arrow)
- Per-day log files for group and private conversations
- Day-change banners inserted automatically at midnight
- Keep-alive pings to prevent server idle timeouts
- Unicode support via wide-character ncurses (`-lncursesw`)

---

## Requirements

Qt 5.14.x or newer
ncursesw for console

Note: C++11 is used due to currently using MinGW 7.3.0 on Windows.


---

## Building

The project uses a single `.pro` file with `CONFIG+=gui` (default) or `CONFIG+=console` to select the front-end.

### GUI build

```bash
qmake CONFIG+=gui qtICB.pro
make
```

Produces `qtICB`.

### Console build

```bash
qmake CONFIG+=console qtICB.pro
make
```

Produces `qtICB-console`. Requires `libncursesw-dev` (or equivalent) to be installed.

### Windows (GUI only)

```bash
qmake CONFIG+=gui qtICB.pro
make
windeployqt qtICB.exe
```

`windeployqt` copies the required Qt DLLs and platform plugins alongside the binary for distribution.

---

## Usage

### GUI

Launch `qtICB`. The window opens with a single tab containing a connection form. Fill in:

| Field | Default |
|---|---|
| Server | `default.icb.net` |
| Port | `7326` |
| Nickname | `example` |
| Group | `foobar` |

Click **Connect**. Additional server connections can be opened as new tabs using the **+** button. Private chat tabs open automatically when a `/query` or `/msg` command is used, or when an incoming private message arrives.

### Console

```
qtICB-console [options]

Options:
  -s, --server <host>   Server address  (default: default.icb.net)
  -p, --port   <port>   Port            (default: 7326)
  -n, --nick   <nick>   Nickname        (default: guest)
  -g, --group  <group>  Initial group   (default: icb)
  -h, --help            Show help
```

Example:

```bash
./qtICB-console -s default.icb.net -n mynick -g icb
```

---

## Commands

These slash commands are recognized by both front-ends.  Anything not listed is passed directly to the server as a raw ICB command, so native ICB subcommands (`/topic`, `/boot`, `/invite`, `/pass`, etc.) work without needing explicit support.

| Command | Description |
|---|---|
| `/join <group>` | Switch to a different group |
| `/nick <new>` | Change your nickname |
| `/query <nick> [msg]` | Open a private chat (and optionally send a first message) |
| `/msg <nick> [msg]` | Same as `/query` |
| `/list` | List all active groups |
| `/quit` or `/exit` | Disconnect from the server |
| `/who [group]` | List users in the current or specified group |
| `/topic <text>` | Change the group topic (passed through as a raw ICB command) |

### Console-only commands

| Command | Description |
|---|---|
| `/clear` | Clear the current buffer's message history |
| `/close` | Close the current private-chat buffer |
| `/nicklist` | Toggle the user-list sidebar (same as Ctrl+L) |

---

## Console key bindings

| Key | Action |
|---|---|
| Enter | Send message / command |
| Up / Down | Browse command history |
| Left / Right | Move cursor in input line |
| Backspace | Delete character before cursor |
| Tab | Cycle through nick completions |
| Page Up / Page Down | Scroll chat area |
| F2 or Ctrl+P | Switch to previous buffer |
| F3 or Ctrl+N | Switch to next buffer |
| Ctrl+A | Jump to next buffer with unread activity |
| Ctrl+L | Toggle user-list sidebar |
| ESC + 1–9 | Switch directly to buffer N |

---

## Log files

Both front-ends write plaintext logs to a `logs/` directory created next to the binary.

```
logs/
  icb_20260308.log          <-- group "icb", March 8 2026
  alice_20260308.log        <-- private chat with "alice"
```

Each line is prefixed with a `[hh:mm:ss]` timestamp. A new file is opened automatically at midnight, so each calendar day has its own file per channel. Characters that are not safe for filenames (e.g. `#`, `@`) are replaced with `_`.