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
