# Geany Code - Feature Status

## Completed

- [x] **Plugin scaffold** — C plugin with GTK3 chat view, input area,
  sidebar/message window integration, menus, keybindings
- [x] **Pure GTK3 rendering** — replaced WebKit2GTK with native GTK3 widgets;
  GtkListBox for messages, GtkSourceView for code blocks, Pango markup for
  formatted text (bold, italic, inline code, headings, links, lists)
- [x] **CLI session** — spawns `claude -p --output-format stream-json` with
  `--include-partial-messages`, `--permission-prompt-tool stdio`
- [x] **Streaming chat** — real-time message rendering with cursor indicator,
  markdown formatting, syntax-highlighted code blocks via GtkSourceView
- [x] **Permission prompts** — inline Allow/Deny UI with suggestion labels
  (setMode, addRules with scope), updatedPermissions passthrough
- [x] **Session ID tracking** — captured from system init, included in all
  user message envelopes for multi-turn conversations
- [x] **Initialize handshake** — sent on process start, parses commands,
  models, and account info from response
- [x] **Tool result display** — results bound to original tool calls by ID,
  status dot (green/red/running), result text appended
- [x] **Stop via interrupt** — sends interrupt `control_request` instead of
  killing the process
- [x] **New Session** — header button clears chat and restarts process
- [x] **Error display** — inline error messages with distinct styling
- [x] **Enter/Shift+Enter** — Enter sends, Shift+Enter inserts newline
- [x] **Edit diff rendering** — side-by-side diffs using GtkSourceView with
  diff syntax highlighting, collapsible panel
- [x] **Write tool rendering** — syntax-highlighted file content display
- [x] **Read tool rendering** — syntax-highlighted code with line numbers,
  stripped line prefixes and offset numbering
- [x] **Bash tool rendering** — command in header slug, full command display,
  sh syntax highlighting, exit code badge, scrollable output
- [x] **Glob tool rendering** — pattern in header slug, file list with count
- [x] **Grep tool rendering** — pattern in header slug
- [x] **Todo/task tracking** — sticky bottom panel from TodoWrite tool calls,
  collapsible, status icons (○ ⟳ ✓)
- [x] **GTK theme injection** — reads `~/.config/gtk-3.0/colors.css`, maps
  Breeze colors to CSS variables, reads GTK font
- [x] **Image attachment** — Alt+V pastes images, thumbnail chips with ×,
  rendered inline in user messages, sent as base64 PNG
- [x] **Context chips** — Alt+A adds editor selection as context chip with
  filename:lines, prepended to prompt on send
- [x] **Collapsible parameters** — all rich tool blocks have a collapsed
  "Parameters" disclosure for raw JSON input
- [x] **Model dropdown** — populated from initialize response
- [x] **Mode dropdown** — static modes, active mode set from system init
- [x] **Alt+V paste** — Geany intercepts Ctrl+V at the accelerator level;
  Alt+V is used instead for text and image paste in our input
- [x] **Model switching** — sends `control_request` with `subtype: "set_model"`
  when model dropdown changes, no restart needed
- [x] **Mode switching** — restarts process with `--permission-mode` and
  `--resume` when mode dropdown changes
- [x] **Slash command completion** — `/` prefix triggers popover with commands
  from initialize response, filtered as you type, keyboard navigation
- [x] **`@` file completion** — `@` prefix triggers popup with project files,
  recursive scanning (8 levels), fuzzy matching, keyboard navigation
- [x] **Session resume** — session picker discovers `.jsonl` files from
  `~/.claude/projects/`, loads history, passes `--resume <id>` on start
- [x] **Session browser** — modal picker showing recent sessions (max 15)
  with slug, timestamp, and first message preview
- [x] **User question UI** — handles question prompts with radio buttons
  (single-select), checkboxes (multi-select), submit/cancel actions
- [x] **Settings** — `~/.config/geany/plugins/geany-code/settings.ini` with
  `claude_path` and `permission_mode` config via GKeyFile

## Remaining

### Medium
- [x] **Extended thinking** — collapsible "Thinking..." blocks, fragmented
  when interleaved with tool calls for holistic reasoning view
### Hard
- [x] **Edit tracking** — header indicator (N files, +added, -removed) with
  popover listing edited files, per-file stats, jump-to-file links

---

## Bug Fixes

Ranked roughly by impact ÷ effort.

### Trivial
- [x] **Fragile tool-name matching for edit tracking** — replaced `strstr`
  with exact-name allowlists (`EDIT_TOOLS` / `WRITE_TOOLS`) in
  `chat_widget.c`.
- [x] **NULL deref on empty chat export** — already guarded at
  `chat_widget.c:893` (`if (md && md[0] != '\0')`). No change needed.
- [x] **Unvalidated `suggestion_N` index** — replaced `atoi` with `strtol`
  + endptr/range validation in `cli_session.c`.
- [x] **Scan-done missing priv null-check in all branches** — verified both
  early-return paths in `scan_done` already handle `priv == NULL`.

### Small
- [x] **Silent stdin write failures** — prompt-send path was already wired to
  `error_cb`. Verified; no change.
- [x] **Streaming-finish double dispatch** — added `finished_dispatched`
  flag; EOF and read-error paths now also reset streaming / dispatch finish
  exactly once. Reset on new session start.
- [x] **D-Bus question timeout vs. response race** — on inspection the
  nested main loop is single-threaded and hash-table removal prevents UAF; no
  real race. No change.
- [x] **`@` file menu silently caps at 20 matches** — now appends an inert
  "… and N more" item when total matches exceed the cap.
- [x] **MCP status callback missing null-check on `mcp_servers`** — now
  validates the node is an array and checks the generated JSON is non-NULL
  before invoking the callback.

### Medium
- [x] **MCP D-Bus proxy never retries** — MCP server startup no longer fails
  hard if Geany D-Bus isn't ready; `call_dbus` lazy-connects on first use and
  reconnects on transport errors (`NO_REPLY`, `DISCONNECTED`,
  `SERVICE_UNKNOWN`, `NAME_HAS_NO_OWNER`).
- [x] **`reload_if_stale` only checks `st_size`** — now tracks an
  `(mtime, size)` fingerprint per file in a hash table; reloads when either
  changes, catching same-length disk edits.
- [x] **No key-repeat debounce in slash/file menu** — `on_buffer_changed`
  now coalesces into a 30 ms `g_timeout_add` that rebuilds the menu only
  once typing pauses.

---

## Features & UX Improvements

Ranked loosely by value ÷ effort.

### Quick wins (trivial / small)
- [ ] **Stop-reason badge** — Show `end_turn` / `max_tokens` / `stop_sequence`
  after each response. Helps users notice truncated outputs.
- [ ] **Recent-files quick panel** — `Ctrl+R` in chat pops Geany's
  recent-files list; click to jump or add as context.
- [x] **Usage tracker** — header indicator shows cumulative
  `↑input ↓output $cost` across the session, with a `~` prefix when we
  had to estimate cost from per-model rates. Tooltip breaks down input /
  output / cache-write / cache-read tokens and model. Resets on New
  Session.
- [x] **Right-click "Add to chat" / "Explain"** — appended a Claude section
  (Add to Context, Send Selection, Explain, Find Bugs, Suggest Improvements)
  to Geany's editor context menu. `update-editor-menu` handler greys them
  out when no selection.
- [x] **Input drafts + per-project prompt history** —
  `~/.config/geany/plugins/geany-code/history/<project-slug>.json` now
  persists: (a) the current unsent draft (debounced save while typing,
  restored on next open if the buffer is empty), and (b) the last 200
  sent prompts (deduplicated against the most-recent entry). Bare Up /
  Down in the input browse history when the cursor is on the first /
  last line — otherwise Up/Down move the cursor normally. Past the
  newest entry, Down restores whatever was typed before browsing began.
- [ ] **Context meter** — Show approx. token count for currently attached
  context chunks + selection + images before sending; warn near limit.
- [x] **Export-conversation variants** — export button now opens a menu
  with: Copy as Markdown (previous default), Save as Markdown…, Save as
  HTML… (styled, standalone, dark/light aware), Save as JSON… (structured
  entry list).

### Medium
- [ ] **Keybinding panel** — Let users rebind send / focus-input / new-session
  / add-context / paste-image via a settings dialog (`GKeyFile`).
- [x] **AI command palette** — `Ctrl+Shift+P` opens a modal search window
  listing chat actions (Focus Input, New/Resume Session, Stop), selection
  quick-actions (Add Context, Send, Explain, Find Bugs, Improve), export
  variants (Copy/Save MD/HTML/JSON), Manage Hooks, and all CLI-reported
  `/slash` commands. Fuzzy subsequence match with score sort;
  up/down/page-up/page-down nav, Enter to run, Esc to cancel.
- [ ] **Session metadata popover** — Click session name → creation time,
  message count, tokens, models used, files edited. Optional CSV export of all
  sessions.
- [x] **Inline accept/reject for diffs** — when a permission prompt
  arrives for an edit/write-family tool and a diff block has just been
  rendered for it, Accept / Reject / suggestion buttons are appended
  directly under the diff (green/red outline, small) instead of a
  separate permission card. Clicking collapses the row to a muted
  `✓ Accepted` / `✗ Rejected` / `✓ Accepted for session` status so the
  diff stays visible. Non-edit tools keep the existing card.
- [ ] **Sidebar project file tree** — Panel of open/recent files with hover
  preview; click opens in editor.
- [ ] **Re-run bash result** — Add a "Run again" action on `Bash` tool
  results; keeps the last N command variants for quick iteration.
- [x] **Copy single response as Markdown** — hover-reveal copy icon in
  the top-right of each assistant message copies that message's raw
  content to the clipboard. `Ctrl+Shift+Y` from anywhere copies the
  most-recent assistant response; status bar confirms the action.

### Large
- [ ] **Conversation branching** — Fork a session from any message; preserve
  branches in session files and in the session picker.
- [ ] **Resume-from-interrupt** — When the user hits Stop mid-stream, keep the
  partial response and offer a "Continue" button that picks up where it
  stopped.
- [ ] **Edit-preview approval flow** — Intercept the `edit` tool, render a
  side-by-side preview, and require explicit approval before applying
  (distinct from `acceptEdits` mode).
