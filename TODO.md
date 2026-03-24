# Geany Code - Feature Status

## Completed

- [x] **Plugin scaffold** — C plugin with WebKit2GTK chat view, GTK input area,
  sidebar/message window integration, menus, keybindings
- [x] **CLI session** — spawns `claude -p --output-format stream-json` with
  `--include-partial-messages`, `--permission-prompt-tool stdio`
- [x] **Streaming chat** — real-time message rendering with markdown, code
  blocks, syntax highlighting via highlight.js
- [x] **Permission prompts** — inline Allow/Deny UI with suggestion labels
  (setMode, addRules with scope), updatedPermissions passthrough
- [x] **Session ID tracking** — captured from system init, included in all
  user message envelopes for multi-turn conversations
- [x] **Initialize handshake** — sent on process start, parses commands,
  models, and account info from response
- [x] **Tool result display** — results bound to original tool calls by ID,
  status dot (green/red), result text appended
- [x] **Stop via interrupt** — sends interrupt `control_request` instead of
  killing the process
- [x] **New Session** — header button clears chat and restarts process
- [x] **Error display** — inline error messages with distinct styling
- [x] **Enter/Shift+Enter** — Enter sends, Shift+Enter inserts newline
- [x] **Edit diff rendering** — LCS-based unified diff with syntax
  highlighting, auto-expanded, "Jump to file" link
- [x] **Write tool rendering** — syntax-highlighted file content display
- [x] **Read tool rendering** — syntax-highlighted code with line numbers
- [x] **Bash tool rendering** — command in header slug, full command display,
  ANSI color rendering, exit code badge, scrollable output
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

## Remaining

### Easy
- [x] **Alt+V paste** — Geany intercepts Ctrl+V at the accelerator level;
  Alt+V is used instead for text and image paste in our input
- [x] **Model switching** — sends `control_request` with `subtype: "set_model"`
  when model dropdown changes, no restart needed
- [x] **Mode switching** — restarts process with `--permission-mode` and
  `--resume` when mode dropdown changes

### Medium
- [ ] **Session resume** — persist `session_id` to disk, pass `--resume <id>`
  on next start so conversations survive Geany restarts
- [ ] **Extended thinking** — handle `thinking_delta` events, render in a
  collapsible "Thinking..." block
- [x] **Slash command completion** — `/` prefix triggers popover with commands
  from initialize response, filtered as you type, click to insert
- [ ] **`@` file completion** — `@` prefix triggers popup with project files
- [ ] **Grep result rendering** — render grep results with file paths and
  matched line content

### Hard
- [ ] **User question UI** — handle `AskUserQuestion` MCP tool with
  multi-select, single-select, and free-text input fields
- [ ] **Settings page** — preferences dialog for claude binary path, default
  permission mode, keybindings
- [ ] **Session browser** — list and resume past sessions from a picker
- [ ] **Edit tracking** — record all file modifications, show edit summary
  panel with jump-to-file links
- [ ] **Transcript persistence** — save conversation transcripts to disk
