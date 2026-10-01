# lilJack native MVP

Run from any directory:

```bash
lilJack
```

The case-sensitive command is installed at `~/.local/bin/lilJack`, which is on
this machine’s PATH. It opens demo by default; `--project DIR` selects another
workspace. Reinstall after moving this checkout with
`bash liljack_app/install.sh`. The checkout-local `./liljack` also works.

The launcher builds the C executable into `~/.cache/liljack/build/` on the first run,
then opens the blue-and-yellow HUI workspace. `python3 ball.py liljack` is equivalent.
The app preserves your CUDA environment setting; it does not force GPUs hidden.

```bash
lilJack --tui                   # inside your terminal, including SSH
lilJack --window --fullscreen   # HUI window
lilJack --demo                  # isolated visual fixtures; no agents launched
```

Without a display, an interactive terminal automatically selects `--tui`.
The window uses SDL software presentation of HUI's framebuffer. Missing font
files fall back through the available faces; with none installed, HUI's built-in
ASCII bitmap font keeps the interface readable. Unsupported glyphs remain visible
as boxes. `--tui` uses your terminal's fonts and truecolor support.

## Sessions and layout

Click Claude, Codex, DeepSeek or Shell to open a new independent session. Repeat
as needed: two Claudes and four DeepSeeks are separate sessions, with separate
IDs, PTYs and control scopes. DeepSeek launches OpenCode with
`--model deepseek/deepseek-v4-pro`, using the existing OpenCode login.

At the operator's request, new app-managed agents start with permission prompts bypassed:
Codex uses `--dangerously-bypass-approvals-and-sandbox`, Claude uses
`--dangerously-skip-permissions`, and OpenCode gets
`OPENCODE_PERMISSION={"*":"allow"}`. These apply to new agent processes;
reattaching an existing session retains that process's original permissions.
Reopen the app to load the updated launch backend. Organization-enforced policies
may still restrict a provider. CUDA settings are inherited unchanged.

- Click a card or terminal to focus it. Clicking a disconnected managed session’s card retries its attachment. Drag a card or title to a tile edge to
  split and snap; drop an existing tile in the center to swap positions.
- Drag dividers to resize the actual terminals. Retile arranges the first three
  managed sessions plus the room; additional sessions remain available in the
  scrollable sidebar. Zoom expands the focused tile.
- Closing a tile hides it. Closing the app detaches its PTY clients; the dedicated
  `liljack-app` tmux sessions survive. Reopening restores the saved layout.
- Sessions launched outside this workspace appear as **observed**. Their original
  terminals cannot be taken over automatically; open app-managed instances to
  interact here.

The UI displays up to 64 session records. A terminal currently allocates about
5.4 MB for its grid and scrollback. Window resizing preserves history without
reflowing old lines.

## The terminal layout

⚠ The SDL/HUI window is not the product any more. `lilJack --tui` is, and its
layout is counted in whole ROWS because a terminal is a cell grid:

| row | what |
|---|---|
| 0 | title, project, mood |
| 1 | room tabs on the left, view menu on the right |
| 2 | agent chips for the open room, spawn buttons on the right |
| 3+ | tiles, across the **full width** |
| last two | toast and status ribbon |

⚠ **Rooms are TABS, not a side panel.** The old sidebar cost a 19-to-24 column
column down the whole height. Click a tab to open that room; drag an agent chip
onto a tab to move it there.

⚠ **A tile is a VIEWPORT, not the terminal's size.** The PTY never goes below
80x24; a shorter tile scrolls a window that follows the cursor. tmux sizes a
window to its SMALLEST attached client and every tile is a client, so without
this a column of tiles handed each harness an unusable terminal — measured, a
second attached view took live panes from 129x24 to 82x5.

⚠ **Hit areas are snapped to whole cells and the smallest target wins.** A click
arrives at a cell CENTRE, so a control narrower than a cell painted cells that
did not respond: every close button had one or two dead cells, and an 8px dock
divider had no clickable centre at all for 2 of every 10 pixel offsets. Each
separator now owns a reserved row or column that no tile occupies.

⚠ **Rules are HAIRLINES, and focus changes their colour, not their weight.**
A filled cell is the thickest mark a terminal has, so a divider painted as a
filled rect is a solid bar a whole column wide. `c_ansi` renders a thin rect as
a box-drawing glyph (`h<10` gives `─`, `w<5` gives `│`), so a 1px rect is a real
hairline in the terminal and in the window alike. Every element is framed by two
of them — a tile by its header bar and its last row, the composer by a rule
above and below — and the focused one is the one whose rules are yellow. A
divider's GRAB area stays a whole cell; only the line is thin.

⚠ **A hairline needs its own ROW.** There is no space between rows to put one
in, so a rule drawn across the header landed among the title and the ×. The last
row of a terminal tile therefore *is* the rule, with the status written into it
(`── attached · 80x24 ─────`).

⚠ **The cursor is reverse video**, never a rect. A thin underline drawn as a
rect became a line glyph that replaced the character under the cursor and spilled
into the next cell.

## Room and control

**+ ROOM** opens the NEW ROOM form: a name, a working **folder** every agent
starts in, a one-line **purpose**, up to three **first todos**, and the agents
with their **roles**. Click an agent to cycle it off, worker, reviewer, lead.
Exactly one lead is required, and promoting a second demotes the first, because
the lead owns every todo in the room. Creating sends one `room_start`: the room,
the agents, their roles, the seeded todos and each agent's intro. A start that
only partly succeeded says so rather than looking finished.

The intro tells an agent its purpose, folder, role, who else is in the room, and
a one-line digest of what the OTHER rooms are working on. Never their messages:
rooms stay isolated, they just know of each other.

**FILES** opens the room's working folder, read-only. Directories navigate, UP
goes back, and a failure prints its reason rather than an empty list, because a
blank listing reads as an empty folder and that is a different fact.

Room headers collapse or expand the member list; selecting a room opens its
agents and shared chat. Drag an agent onto a room tab to join, or onto
**Standalone** to leave. Each workspace keeps its own terminal arrangement.

Room chat preserves multilingual text. A named-room message is visible to the operator
and the agents who belonged to that room when it was sent. Joining does not
grant old history; leaving does not erase messages already addressed to you.
Replies cannot add recipients who could not see the original message.

A **DM** view contains only your conversation with the selected exact session.
Private messages never appear in a shared room feed. Conversation drafts remain
separate, so switching rooms or agents does not silently change a draft's
recipient. **Stage in agent** pastes a request into a live agent prompt; press
Enter in that terminal to submit. Chat posting itself never executes terminal
input.

The **public lobby** bridges the existing lilJack provider mailbox. Named rooms
and all DMs stay in the workspace; agents read them with their session-specific
room CLI. Posting does not mean an agent has read or acted on a message.
Public posts above the mailbox's 4,000-character limit remain local with an
explicit delivery issue. The mailbox refuses oversized sends rather than
silently truncating them.

To make agent coordination visible in a named room, post with
`lilJack room --text 'Update'` using the app session's identity. Posting defaults
to that session's current room, or the public lobby for a standalone session.
Use `--to ROOM_ID` for an explicit room or `--to room` for the public lobby.
Provider MCP mailbox messages do not appear in a named room. Staged room
requests include a command to reply into the same room; staged DMs reply privately.
Successful local room posts and DMs are not mailbox delivery failures.

New human and teammate posts in named rooms receive a durable delivery status.
The room's single lead receives one notice after its exact lifecycle reports
idle. Lead-authored posts and DMs do not trigger this mechanism. Codex uses
`codex queue`; Claude/OpenCode use a short generated notice and Enter in the
exact app-owned pane after lifecycle and terminal checks. Room bodies are never
automatically typed into a terminal. Missing, unknown or busy lifecycle waits.
`room --message MESSAGE_ID` reads the original message and acknowledges delivery
when called by the assigned lead. Pending reasons, queue acceptance and read
status are distinct; ambiguous submissions are not retried automatically.
Unsupported lead harnesses show an explicit delivery reason.

Delivery can run without any UI using the existing heartbeat runner:

```sh
python3 toolbox/liljack_heartbeat.py --rooms --once --cwd "$PWD"
python3 toolbox/liljack_heartbeat.py --rooms --loop --interval 5 --cwd "$PWD"
```

The first command is read-only inspection; `--once --live` makes one real tick.
The loop runs in the foreground and must be managed by the operator's service
manager for persistence. It does not install or start a service itself. Room mode
does not process the legacy task/mailbox queue. `--workspace-root` selects an
isolated workspace. The heartbeat respects `~/.cache/liljack/heartbeat.stop`
(or `--root`); `<workspace>/room-delivery.stop` pauses both heartbeat and UI
dispatch. Concurrent frontends and heartbeats share the same delivery lock.

Pending wake notices expire after 15 minutes measured from the original message
creation time. They become `superseded`, not `read`; the message stays in room
history. This housekeeping also runs while dispatch is paused. An explicit
`./liljack room --message <id>` read by the sole assigned room lead can acknowledge
a pending notice before any wake. Reading it as a peer does not clear delivery.

Each terminal's **Controls** button opens its page in **Team**. Focus, DM,
Submit (Enter), Interrupt (Ctrl+C), and a two-click Stop target that exact
session. Closing a tile only hides it; Stop ends its tmux session.

**Review** opens App review: board completion, TODO state/progress, git history
and recorded fit scores. It is a movable, resizable dock tile. Select Review
again to leave its zoomed view; scroll with the wheel, arrows, Page Up/Down or
Home/End. Escape returns focus to room chat. Review is read-only and does not
route tasks by score. Data refreshes from a cached collection every 30 seconds.
The paged panel keeps every session reachable on smaller displays.
**Make lead** atomically hands the member's room to that session, preserving
worker/reviewer roles and making previous leads reviewers. Unsent notifications
follow the new lead; already-submitted notifications retain their recipient.
Cross-room child dependencies must be reassigned before a handoff.

**Team** also cycles a session between worker, reviewer and lead, and selects its
parent lead. the operator assigns authority. Leads can spawn workers/reviewers under
themselves and control descendants; workers/reviewers cannot control peers.
Cycles and cross-project dependencies are rejected.

Agents launched by the app inherit their session identity and can use:

```bash
./liljack room --text 'Привіт команді!' --language uk
./liljack room --since 0
./liljack room --to ROOM_ID --text 'Room-only update'
./liljack room --to SESSION_ID --text 'Private message'
./liljack session list
./liljack session spawn --agent deepseek --role reviewer
./liljack session send --session SESSION_ID --text 'Please review this change'
./liljack session submit --session SESSION_ID
```

`send` stages one line; `submit` sends Enter separately. `interrupt` sends Ctrl+C
and `stop` ends the named managed tmux session. These commands enforce the
assigned session scope. An unidentified CLI caller is refused. For an explicit
operator CLI outside an agent session, use `LILJACK_AGENT=operator` with no
`LILJACK_WORKSPACE_SESSION` set. Environment identity is a cooperation check among
same-user processes, **not an OS security sandbox**.

Operator room controls also work from any directory (set the project explicitly
when using the CLI outside your project):

```bash
LILJACK_AGENT=operator lilJack room --project /path/to/project --create 'Review'
LILJACK_AGENT=operator lilJack room --project /path/to/project --move SESSION_ID --into ROOM_ID
LILJACK_AGENT=operator lilJack room --project /path/to/project --collapse ROOM_ID
LILJACK_AGENT=operator lilJack room --project /path/to/project --expand ROOM_ID
LILJACK_AGENT=operator lilJack room --project /path/to/project --move SESSION_ID
```


## Keyboard and media

| Action | Shortcut |
|---|---|
| Shared room | F6 |
| Team | F7 |
| Window fullscreen | F11 |
| Release terminal focus | Ctrl+] |
| Paste in window | Ctrl+Shift+V |
| Copy in window | Shift-drag terminal text |
| Host selection in text mode | F8 pauses redraw and releases mouse; F8 resumes |
| Quit/detach window | Ctrl+Shift+Q |
| Quit/detach text mode | Ctrl+Q |
| Room newline | Shift+Enter in window |

In text mode, press **F8**, select text with the host terminal, and use its Copy
command (often Ctrl+Shift+C). Press **F8** again to resume app input and redraws.
Agent sessions continue running while the display is paused. Shift-drag events
that reach the app request clipboard copying through OSC 52; the host terminal
may block that request, so F8 provides a host-selection fallback. Classic terminals
cannot distinguish every shifted key combination. Terminal programs receive
mouse events when they request them; otherwise the wheel browses scrollback.

Drop an image/video file into the **window**, or run:

```bash
./liljack --window --media /path/to/image.png
./liljack --window --media /path/to/video.mp4
./liljack --window --media 'https://www.youtube.com/watch?v=VIDEO_ID'
```

One media tile is supported. Video is muted, limited to 640×360 at 12 fps and
decoded on the CPU. Public YouTube resolution uses `yt-dlp`; access restrictions
and site changes may prevent playback. Local image/video decoding was tested;
live YouTube playback was not. ANSI mode does not implement Kitty/Sixel images.

## Dependencies and validation

Linux MVP: GCC, pkg-config, SDL2, FreeType, json-c, Python 3 and tmux. Fonts used
when installed: Noto Sans Mono, Noto Sans CJK and Noto Color Emoji. HUI source is
resolved from `../hui` or `LILJACK_HUI`. Optional media dependencies: ffmpeg and
yt-dlp. Provider CLIs must already be installed and authenticated.

This machine initially lacked tmux; the tested tmux 3.7c binary is in
`~/.cache/liljack/build/usr/bin/tmux`. Discovery prefers `LILJACK_TMUX`, then PATH,
then that cache. A fresh machine needs its own tmux installation.

```bash
./tests/run_liljack_native_tests.sh --sanitize
python3 tests/test_liljack_app.py
python3 tests/test_liljack_cli.py
python3 tests/test_liljack_workspace_bridge.py
python3 tests/test_liljack_backend.py
python3 tests/test_liljack_vt_c.py --sanitize
./liljack --screenshot /tmp/liljack-demo.png
```

The frontend is C: input, docking, PTYs, VT parsing, fonts, HUI drawing, ANSI
presentation and media. Python is confined to existing session/mail integration
and CLI commands; the earlier Python UI prototype was removed. The C terminal
adapter derives from `../the handheld/app/owkterm.c`; handheld sources are unchanged.

Not verified: live provider completion flows, physical desktop mouse interaction,
live YouTube, non-Linux platforms, complex-script shaping, or full xterm
compatibility. Unicode supports en/uk/ja and individual emoji; grapheme sequences
such as ZWJ families are not fully shaped. The lilJackV1 model and Telegram
coordinator remain on Claude's separate model track.

## owkTerm native app

owkTerm is its own brain project: `../owkTerm` (app source, build, installer,
desktop entry, README). It builds against this directory's VT parser
(`owkterm_vt.c`) and FreeType renderer (`c_render.c`), the same way lilJack
builds against `../hui`. `./liljack owkterm` and `python3 ball.py owkterm`
still work: `build.sh owkterm` delegates to `../owkTerm/build.sh`
(`OWKTERM_DIR` overrides the location). To run lilJack inside it:

```bash
owkterm -- lilJack --tui        # after bash ../owkTerm/install.sh
./liljack owkterm -- "$PWD/liljack" --tui --project "$PWD"
```

Theme colours and decorative glyphs load at launch from
`~/.config/liljack/theme.json` (or `$XDG_CONFIG_HOME/liljack/theme.json`).
Set `LILJACK_THEME=/path/to/theme.json` to select a different file. Copy
`liljack_app/theme.default.json` as a starting point, or provide only overrides:

```json
{"version":1,"tokens":{"bg":{"rgb":"#101820"},"cyan":{"rgb":"#80cfff"}}}
```

Run `./liljack --gallery` to inspect the colours. The ANSI composition lab uses
the same loader, with its original probe colours under `lab_*` tokens. Each
token exposes its RGB colour and nearest standard ANSI-256/ANSI-16 indices.
Glyph overrides must be one printable Unicode character with the same cell
width as the default; message text and terminal decoding are not theme settings.
Invalid files are rejected as a whole. A missing file uses compiled defaults;
editing the file takes effect on the next launch. No user theme file is written
automatically.

**SHELL** opens a shell session in the current grid. Launch a separate terminal
with the standalone commands above.
The older deck preview is `../owkTerm/build.sh host` (`build.sh owkterm-host` here).
