/*
 * hooks_dialog.c — Manage Claude Code project hooks from Geany
 *
 * Reads/writes hooks in .claude/settings.local.json, preserving all
 * other keys (permissions, enabledMcpjsonServers, etc.).
 */

#include "hooks_dialog.h"
#include "editor_bridge.h"
#include "plugin.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ── Data model ─────────────────────────────────────────────────── */

typedef enum { HOOK_TYPE_COMMAND, HOOK_TYPE_PROMPT, HOOK_TYPE_AGENT } HookType;

typedef struct {
    gchar    *matcher;
    HookType  type;
    gchar    *command;   /* shell command  (type == COMMAND) */
    gchar    *prompt;    /* prompt text    (type == PROMPT or AGENT) */
    gchar    *if_cond;   /* optional permission-rule filter, e.g. "Bash(git *)" */
} HookEntry;

static HookEntry *hook_entry_new_command(const gchar *matcher,
                                         const gchar *command)
{
    HookEntry *e = g_new0(HookEntry, 1);
    e->matcher = g_strdup(matcher);
    e->type = HOOK_TYPE_COMMAND;
    e->command = g_strdup(command);
    return e;
}

static HookEntry *hook_entry_new_prompt(const gchar *matcher,
                                        const gchar *prompt)
{
    HookEntry *e = g_new0(HookEntry, 1);
    e->matcher = g_strdup(matcher);
    e->type = HOOK_TYPE_PROMPT;
    e->prompt = g_strdup(prompt);
    return e;
}

static HookEntry *hook_entry_new_agent(const gchar *matcher,
                                       const gchar *prompt)
{
    HookEntry *e = g_new0(HookEntry, 1);
    e->matcher = g_strdup(matcher);
    e->type = HOOK_TYPE_AGENT;
    e->prompt = g_strdup(prompt);
    return e;
}

static void hook_entry_free(gpointer p)
{
    HookEntry *e = p;
    if (!e) return;
    g_free(e->matcher);
    g_free(e->command);
    g_free(e->prompt);
    g_free(e->if_cond);
    g_free(e);
}

/* ── Event types and presets ────────────────────────────────────── */

typedef struct {
    const gchar *label;
    const gchar *matcher;
    HookType     type;
    const gchar *command;  /* for HOOK_TYPE_COMMAND */
    const gchar *prompt;   /* for HOOK_TYPE_PROMPT  */
} HookPreset;

/* PreToolUse presets */
static const HookPreset pre_tool_presets[] = {
    {
        "Block rm -rf", "Bash", HOOK_TYPE_COMMAND,
        "INPUT=$(cat)\n"
        "CMD=$(echo \"$INPUT\" | jq -r '.tool_input.command // empty')\n"
        "if echo \"$CMD\" | grep -q 'rm -rf'; then\n"
        "  echo 'Blocked: rm -rf is not allowed' >&2\n"
        "  exit 2\n"
        "fi",
        NULL
    },
    {
        "Block force push", "Bash", HOOK_TYPE_COMMAND,
        "INPUT=$(cat)\n"
        "CMD=$(echo \"$INPUT\" | jq -r '.tool_input.command // empty')\n"
        "if echo \"$CMD\" | grep -qE 'push.*(--force|-f)'; then\n"
        "  echo 'Blocked: force push is not allowed' >&2\n"
        "  exit 2\n"
        "fi",
        NULL
    },
    {
        "Block destructive git", "Bash", HOOK_TYPE_COMMAND,
        "INPUT=$(cat)\n"
        "CMD=$(echo \"$INPUT\" | jq -r '.tool_input.command // empty')\n"
        "if echo \"$CMD\" | grep -qE 'git (reset --hard|clean -f|checkout -- \\.)'; then\n"
        "  echo 'Blocked: destructive git operation' >&2\n"
        "  exit 2\n"
        "fi",
        NULL
    },
    {
        "Protect .env files", "Edit|Write", HOOK_TYPE_COMMAND,
        "INPUT=$(cat)\n"
        "FP=$(echo \"$INPUT\" | jq -r '.tool_input.file_path // empty')\n"
        "if echo \"$FP\" | grep -qE '\\.env($|\\.)'; then\n"
        "  echo 'Blocked: .env files are protected' >&2\n"
        "  exit 2\n"
        "fi",
        NULL
    },
    {
        "Review dangerous commands (prompt)", "Bash", HOOK_TYPE_PROMPT,
        NULL,
        "Review this Bash command for safety. The command is:\n"
        "$ARGUMENTS\n\n"
        "Block the command if it could delete files, modify system config,\n"
        "or cause data loss. Allow routine operations like ls, cat, grep,\n"
        "git status, build commands, and test runners."
    },
    {
        "Review file edits (prompt)", "Edit|Write", HOOK_TYPE_PROMPT,
        NULL,
        "Review this file edit for safety. The edit details are:\n"
        "$ARGUMENTS\n\n"
        "Block the edit if it modifies credentials, secrets, CI/CD config,\n"
        "or deployment files. Allow edits to source code and documentation."
    },
    {
        "Verify command safety (agent)", "Bash", HOOK_TYPE_AGENT,
        NULL,
        "A Bash command is about to execute:\n"
        "$ARGUMENTS\n\n"
        "Verify it is safe by:\n"
        "1. Check for destructive operations (rm -rf, DROP TABLE, etc.)\n"
        "2. If the command modifies files, read them first to confirm context\n"
        "3. Verify no secrets or credentials are being leaked\n\n"
        "Allow routine operations like builds, tests, git status, and searches."
    },
    {
        "Verify edit in context (agent)", "Edit|Write", HOOK_TYPE_AGENT,
        NULL,
        "A file edit is about to be applied:\n"
        "$ARGUMENTS\n\n"
        "Verify the edit is appropriate:\n"
        "1. Read the target file to understand the surrounding code\n"
        "2. Check that the edit doesn't break imports or function signatures\n"
        "3. Verify no security-sensitive values are being hardcoded\n\n"
        "Allow edits that look correct in context."
    },
    { NULL, NULL, 0, NULL, NULL }
};

/* PostToolUse presets */
static const HookPreset post_tool_presets[] = {
    {
        "Auto-format after edit", "Edit|Write", HOOK_TYPE_COMMAND,
        "# Auto-format modified file\n"
        "INPUT=$(cat)\n"
        "FP=$(echo \"$INPUT\" | jq -r '.tool_input.file_path // empty')\n"
        "[ -z \"$FP\" ] && exit 0\n"
        "case \"$FP\" in\n"
        "  *.js|*.ts|*.jsx|*.tsx|*.json|*.css)\n"
        "    npx prettier --write \"$FP\" 2>/dev/null ;;\n"
        "  *.py)\n"
        "    black \"$FP\" 2>/dev/null ;;\n"
        "  *.go)\n"
        "    gofmt -w \"$FP\" 2>/dev/null ;;\n"
        "esac",
        NULL
    },
    {
        "Log tool usage", ".*", HOOK_TYPE_COMMAND,
        "# Log all tool calls to .claude/tool-log.txt\n"
        "INPUT=$(cat)\n"
        "TOOL=$(echo \"$INPUT\" | jq -r '.tool_name // \"unknown\"')\n"
        "echo \"$(date '+%Y-%m-%d %H:%M:%S') $TOOL\" >> .claude/tool-log.txt",
        NULL
    },
    { NULL, NULL, 0, NULL, NULL }
};

/* Notification presets */
static const HookPreset notification_presets[] = {
    {
        "Desktop notification", ".*", HOOK_TYPE_COMMAND,
        "# Send desktop notification when Claude needs attention\n"
        "INPUT=$(cat)\n"
        "MSG=$(echo \"$INPUT\" | jq -r '.message // \"Claude needs your attention\"')\n"
        "notify-send 'Claude Code' \"$MSG\" 2>/dev/null",
        NULL
    },
    {
        "Play sound on prompt", ".*", HOOK_TYPE_COMMAND,
        "# Play a sound when Claude needs input\n"
        "paplay /usr/share/sounds/freedesktop/stereo/message.oga 2>/dev/null &",
        NULL
    },
    { NULL, NULL, 0, NULL, NULL }
};

typedef struct {
    const gchar       *event_name;    /* JSON key: "PreToolUse" etc. */
    const gchar       *display_name;  /* Combo box label */
    const gchar       *matcher_hint;  /* Placeholder for matcher entry */
    const gchar       *help_text;     /* Help in edit sub-dialog */
    const HookPreset  *presets;       /* NULL-terminated preset array */
    gboolean           has_bash_wizard; /* Show "Block Bash Command..." */
} EventTypeDef;

static const EventTypeDef event_types[] = {
    {
        "PreToolUse",
        "PreToolUse \u2014 before tool execution",
        "e.g. Bash, Edit|Write, .*",
        "Runs before a tool executes. Matcher is a regex on tool_name.\n"
        "Exit 0 = allow.  Exit 2 + stderr message = block.",
        pre_tool_presets,
        TRUE
    },
    {
        "PostToolUse",
        "PostToolUse \u2014 after tool execution",
        "e.g. Edit|Write, Bash, .*",
        "Runs after a tool completes successfully. Matcher is a regex on tool_name.\n"
        "Exit 0 = ok.  Non-zero = logged but does not undo.",
        post_tool_presets,
        FALSE
    },
    {
        "Notification",
        "Notification \u2014 when Claude needs attention",
        "e.g. .*, permission_prompt, idle_prompt",
        "Runs when Claude sends a notification (e.g. permission prompt, idle).\n"
        "Matcher is a regex on notification type.  Exit code is ignored.",
        notification_presets,
        FALSE
    },
};

#define N_EVENT_TYPES G_N_ELEMENTS(event_types)

/* ── Settings file path ─────────────────────────────────────────── */

static gchar *get_settings_path(void)
{
    gchar *root = editor_bridge_get_project_root();
    if (!root) root = g_strdup(g_get_home_dir());
    gchar *path = g_build_filename(root, ".claude", "settings.local.json", NULL);
    g_free(root);
    return path;
}

/* ── JSON load / save ───────────────────────────────────────────── */

static GList *load_hooks(const gchar *path, const gchar *event_name)
{
    GList *list = NULL;
    gchar *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, contents, -1, NULL)) {
        g_free(contents);
        g_object_unref(jp);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(jp);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        goto out;

    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "hooks"))
        goto out;

    JsonObject *hooks_obj = json_object_get_object_member(root_obj, "hooks");
    if (!hooks_obj || !json_object_has_member(hooks_obj, event_name))
        goto out;

    JsonArray *arr = json_object_get_array_member(hooks_obj, event_name);
    guint n = json_array_get_length(arr);

    for (guint i = 0; i < n; i++) {
        JsonObject *entry = json_array_get_object_element(arr, i);
        if (!entry) continue;

        const gchar *matcher = json_object_has_member(entry, "matcher")
            ? json_object_get_string_member(entry, "matcher") : "";

        /* Extract hook type and content from hooks[0] */
        if (json_object_has_member(entry, "hooks")) {
            JsonArray *inner = json_object_get_array_member(entry, "hooks");
            if (json_array_get_length(inner) > 0) {
                JsonObject *h = json_array_get_object_element(inner, 0);
                if (!h) continue;
                const gchar *type_str = json_object_has_member(h, "type")
                    ? json_object_get_string_member(h, "type") : "command";

                const gchar *if_cond = json_object_has_member(h, "if")
                    ? json_object_get_string_member(h, "if") : NULL;
                HookEntry *he;

                if (g_strcmp0(type_str, "prompt") == 0) {
                    const gchar *prompt = json_object_has_member(h, "prompt")
                        ? json_object_get_string_member(h, "prompt") : "";
                    he = hook_entry_new_prompt(matcher, prompt);
                } else if (g_strcmp0(type_str, "agent") == 0) {
                    const gchar *prompt = json_object_has_member(h, "prompt")
                        ? json_object_get_string_member(h, "prompt") : "";
                    he = hook_entry_new_agent(matcher, prompt);
                } else {
                    const gchar *command = json_object_has_member(h, "command")
                        ? json_object_get_string_member(h, "command") : "";
                    he = hook_entry_new_command(matcher, command);
                }
                he->if_cond = g_strdup(if_cond);
                list = g_list_append(list, he);
            }
        }
    }

out:
    g_free(contents);
    g_object_unref(jp);
    return list;
}

static gboolean save_hooks(const gchar *path, const gchar *event_name,
                           GList *hooks)
{
    /* Parse existing file or start fresh */
    JsonNode *root = NULL;
    gchar *contents = NULL;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, contents, -1, NULL)) {
            root = json_node_copy(json_parser_get_root(jp));
        }
        g_object_unref(jp);
        g_free(contents);
    }

    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        if (root) json_node_unref(root);
        root = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(root, json_object_new());
    }

    JsonObject *root_obj = json_node_get_object(root);

    /* Ensure "hooks" object exists */
    if (!json_object_has_member(root_obj, "hooks")) {
        json_object_set_object_member(root_obj, "hooks", json_object_new());
    }
    JsonObject *hooks_obj = json_object_get_object_member(root_obj, "hooks");

    if (!hooks) {
        /* No hooks — remove the event key */
        if (json_object_has_member(hooks_obj, event_name))
            json_object_remove_member(hooks_obj, event_name);
    } else {
        /* Build the array */
        JsonArray *arr = json_array_new();

        for (GList *l = hooks; l; l = l->next) {
            HookEntry *e = l->data;

            JsonObject *inner_hook = json_object_new();
            if (e->type == HOOK_TYPE_PROMPT) {
                json_object_set_string_member(inner_hook, "type", "prompt");
                json_object_set_string_member(inner_hook, "prompt",
                                              e->prompt ? e->prompt : "");
            } else if (e->type == HOOK_TYPE_AGENT) {
                json_object_set_string_member(inner_hook, "type", "agent");
                json_object_set_string_member(inner_hook, "prompt",
                                              e->prompt ? e->prompt : "");
            } else {
                json_object_set_string_member(inner_hook, "type", "command");
                json_object_set_string_member(inner_hook, "command",
                                              e->command ? e->command : "");
            }

            if (e->if_cond && e->if_cond[0] != '\0')
                json_object_set_string_member(inner_hook, "if", e->if_cond);

            JsonArray *inner_arr = json_array_new();
            json_array_add_object_element(inner_arr, inner_hook);

            JsonObject *entry = json_object_new();
            json_object_set_string_member(entry, "matcher", e->matcher);
            json_object_set_array_member(entry, "hooks", inner_arr);

            json_array_add_object_element(arr, entry);
        }

        json_object_set_array_member(hooks_obj, event_name, arr);
    }

    /* Ensure directory exists */
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    /* Serialize */
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_indent(gen, 2);
    json_generator_set_root(gen, root);

    gboolean ok = json_generator_to_file(gen, path, NULL);

    g_object_unref(gen);
    json_node_unref(root);
    return ok;
}

/* ── Edit sub-dialog ────────────────────────────────────────────── */

/* Switch visible stack page when type combo changes */
static void on_type_combo_changed(GtkComboBox *combo, gpointer data)
{
    GtkStack *stack = GTK_STACK(data);
    gint active = gtk_combo_box_get_active(combo);
    const gchar *pages[] = { "command", "prompt", "agent" };
    if (active >= 0 && active <= 2)
        gtk_stack_set_visible_child_name(stack, pages[active]);
}

static gboolean edit_hook_dialog(GtkWindow *parent, HookEntry *entry,
                                 gboolean is_new,
                                 const EventTypeDef *evtype)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        is_new ? "Add Hook" : "Edit Hook",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 500, 400);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    /* Matcher */
    GtkWidget *mlabel = gtk_label_new("Matcher (regex):");
    gtk_label_set_xalign(GTK_LABEL(mlabel), 0);
    gtk_box_pack_start(GTK_BOX(content), mlabel, FALSE, FALSE, 0);

    GtkWidget *matcher_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(matcher_entry), evtype->matcher_hint);
    if (entry->matcher)
        gtk_entry_set_text(GTK_ENTRY(matcher_entry), entry->matcher);
    gtk_box_pack_start(GTK_BOX(content), matcher_entry, FALSE, FALSE, 0);

    /* Condition (optional argument-level filter) */
    GtkWidget *iflabel = gtk_label_new("Condition (optional, permission-rule syntax):");
    gtk_label_set_xalign(GTK_LABEL(iflabel), 0);
    gtk_box_pack_start(GTK_BOX(content), iflabel, FALSE, FALSE, 0);

    GtkWidget *if_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(if_entry),
                                   "e.g. Bash(git *), Edit(*.ts), Write(src/*)");
    if (entry->if_cond)
        gtk_entry_set_text(GTK_ENTRY(if_entry), entry->if_cond);
    gtk_box_pack_start(GTK_BOX(content), if_entry, FALSE, FALSE, 0);

    /* Hook type selector */
    GtkWidget *type_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tlabel = gtk_label_new("Type:");
    gtk_box_pack_start(GTK_BOX(type_box), tlabel, FALSE, FALSE, 0);

    GtkWidget *type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(type_combo),
                                   "Command (shell script)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(type_combo),
                                   "Prompt (single-turn LLM)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(type_combo),
                                   "Agent (multi-turn with tools)");
    /* Don't set active yet — wait until the stack is built */
    gtk_box_pack_start(GTK_BOX(type_box), type_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), type_box, FALSE, FALSE, 0);

    /* GtkStack for command / prompt panels */
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack),
        GTK_STACK_TRANSITION_TYPE_NONE);

    /* ── Command page ── */
    GtkWidget *cmd_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *clabel = gtk_label_new("Command (shell):");
    gtk_label_set_xalign(GTK_LABEL(clabel), 0);
    gtk_box_pack_start(GTK_BOX(cmd_box), clabel, FALSE, FALSE, 0);

    GtkWidget *cmd_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cmd_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(cmd_scroll),
        GTK_SHADOW_IN);

    GtkWidget *cmd_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(cmd_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(cmd_view), GTK_WRAP_WORD_CHAR);
    if (entry->command) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cmd_view));
        gtk_text_buffer_set_text(buf, entry->command, -1);
    }
    gtk_container_add(GTK_CONTAINER(cmd_scroll), cmd_view);
    gtk_box_pack_start(GTK_BOX(cmd_box), cmd_scroll, TRUE, TRUE, 0);

    GtkWidget *cmd_help = gtk_label_new(evtype->help_text);
    gtk_label_set_xalign(GTK_LABEL(cmd_help), 0);
    gtk_label_set_line_wrap(GTK_LABEL(cmd_help), TRUE);
    gtk_widget_set_opacity(cmd_help, 0.6);
    gtk_box_pack_start(GTK_BOX(cmd_box), cmd_help, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), cmd_box, "command");

    /* ── Prompt page ── */
    GtkWidget *prompt_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *plabel = gtk_label_new("Prompt (sent to a fast model for evaluation):");
    gtk_label_set_xalign(GTK_LABEL(plabel), 0);
    gtk_box_pack_start(GTK_BOX(prompt_box), plabel, FALSE, FALSE, 0);

    GtkWidget *prompt_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prompt_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(prompt_scroll),
        GTK_SHADOW_IN);

    GtkWidget *prompt_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(prompt_view), GTK_WRAP_WORD_CHAR);
    if (entry->prompt) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(prompt_view));
        gtk_text_buffer_set_text(buf, entry->prompt, -1);
    }
    gtk_container_add(GTK_CONTAINER(prompt_scroll), prompt_view);
    gtk_box_pack_start(GTK_BOX(prompt_box), prompt_scroll, TRUE, TRUE, 0);

    GtkWidget *prompt_help = gtk_label_new(
        "Use $ARGUMENTS where the hook's JSON input should be inserted.\n"
        "The model returns a decision (allow/deny) based on your prompt.");
    gtk_label_set_xalign(GTK_LABEL(prompt_help), 0);
    gtk_label_set_line_wrap(GTK_LABEL(prompt_help), TRUE);
    gtk_widget_set_opacity(prompt_help, 0.6);
    gtk_box_pack_start(GTK_BOX(prompt_box), prompt_help, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), prompt_box, "prompt");

    /* ── Agent page ── */
    GtkWidget *agent_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *alabel = gtk_label_new("Agent prompt (multi-turn, has tool access):");
    gtk_label_set_xalign(GTK_LABEL(alabel), 0);
    gtk_box_pack_start(GTK_BOX(agent_box), alabel, FALSE, FALSE, 0);

    GtkWidget *agent_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(agent_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(agent_scroll),
        GTK_SHADOW_IN);

    GtkWidget *agent_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(agent_view), GTK_WRAP_WORD_CHAR);
    if (entry->type == HOOK_TYPE_AGENT && entry->prompt) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(agent_view));
        gtk_text_buffer_set_text(buf, entry->prompt, -1);
    }
    gtk_container_add(GTK_CONTAINER(agent_scroll), agent_view);
    gtk_box_pack_start(GTK_BOX(agent_box), agent_scroll, TRUE, TRUE, 0);

    GtkWidget *agent_help = gtk_label_new(
        "The agent can use Read, Grep, Glob, Edit, Write, and Bash tools\n"
        "to inspect the codebase before making a decision.\n"
        "Use $ARGUMENTS for the hook's JSON input. Default timeout: 60s.");
    gtk_label_set_xalign(GTK_LABEL(agent_help), 0);
    gtk_label_set_line_wrap(GTK_LABEL(agent_help), TRUE);
    gtk_widget_set_opacity(agent_help, 0.6);
    gtk_box_pack_start(GTK_BOX(agent_box), agent_help, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), agent_box, "agent");

    gtk_box_pack_start(GTK_BOX(content), stack, TRUE, TRUE, 0);

    /* Wire combo to stack */
    g_signal_connect(type_combo, "changed",
                     G_CALLBACK(on_type_combo_changed), stack);

    /* Show everything first, then set the initial state.
     * GtkStack shows the first child by default after show_all,
     * so we must set the combo (and thus the stack page) AFTER. */
    gtk_widget_show_all(dlg);

    gint init_type = (entry->type == HOOK_TYPE_AGENT) ? 2
                   : (entry->type == HOOK_TYPE_PROMPT) ? 1 : 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(type_combo), init_type);

    gboolean result = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const gchar *m = gtk_entry_get_text(GTK_ENTRY(matcher_entry));
        const gchar *active_page = gtk_stack_get_visible_child_name(
            GTK_STACK(stack));

        /* Validate matcher is valid regex */
        GError *err = NULL;
        GRegex *re = g_regex_new(m, 0, 0, &err);
        if (!re) {
            GtkWidget *msg = gtk_message_dialog_new(
                GTK_WINDOW(dlg), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Invalid regex: %s", err->message);
            gtk_dialog_run(GTK_DIALOG(msg));
            gtk_widget_destroy(msg);
            g_error_free(err);
        } else {
            g_regex_unref(re);

            /* Get text from the active page's text view */
            GtkTextView *active_view;
            if (g_strcmp0(active_page, "agent") == 0)
                active_view = GTK_TEXT_VIEW(agent_view);
            else if (g_strcmp0(active_page, "prompt") == 0)
                active_view = GTK_TEXT_VIEW(prompt_view);
            else
                active_view = GTK_TEXT_VIEW(cmd_view);

            GtkTextBuffer *buf = gtk_text_view_get_buffer(active_view);
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buf, &start, &end);
            gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

            if (m[0] != '\0' && text[0] != '\0') {
                const gchar *if_val = gtk_entry_get_text(GTK_ENTRY(if_entry));
                g_free(entry->matcher);
                g_free(entry->command);
                g_free(entry->prompt);
                g_free(entry->if_cond);
                entry->matcher = g_strdup(m);
                entry->if_cond = (if_val && if_val[0] != '\0')
                    ? g_strdup(if_val) : NULL;
                entry->command = NULL;
                entry->prompt = NULL;
                if (g_strcmp0(active_page, "agent") == 0) {
                    entry->type = HOOK_TYPE_AGENT;
                    entry->prompt = text;
                } else if (g_strcmp0(active_page, "prompt") == 0) {
                    entry->type = HOOK_TYPE_PROMPT;
                    entry->prompt = text;
                } else {
                    entry->type = HOOK_TYPE_COMMAND;
                    entry->command = text;
                }
                result = TRUE;
            } else {
                g_free(text);
            }
        }
    }

    gtk_widget_destroy(dlg);
    return result;
}

/* ── Main dialog ────────────────────────────────────────────────── */

typedef struct {
    GtkWidget          *list_box;
    GtkWidget          *preset_btn;
    GtkWindow          *parent;
    gchar              *path;

    /* Per-event-type state */
    guint               cur_event;           /* index into event_types[] */
    GList              *hooks[N_EVENT_TYPES]; /* hook lists per event type */
    gboolean            dirty[N_EVENT_TYPES]; /* modified flags */
    gboolean            loaded[N_EVENT_TYPES];/* loaded from disk? */
} HooksDialogCtx;

static void rebuild_list(HooksDialogCtx *ctx);
static void rebuild_preset_menu(HooksDialogCtx *ctx);

/* Ensure the current event type's hooks are loaded from disk */
static void ensure_loaded(HooksDialogCtx *ctx, guint idx)
{
    if (ctx->loaded[idx]) return;
    ctx->hooks[idx] = load_hooks(ctx->path, event_types[idx].event_name);
    ctx->loaded[idx] = TRUE;
}

static const EventTypeDef *cur_evtype(HooksDialogCtx *ctx)
{
    return &event_types[ctx->cur_event];
}

static void on_edit_clicked(GtkButton *btn, gpointer data)
{
    HooksDialogCtx *ctx = data;
    gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx"));
    HookEntry *entry = g_list_nth_data(ctx->hooks[ctx->cur_event], idx);
    if (entry && edit_hook_dialog(ctx->parent, entry, FALSE, cur_evtype(ctx))) {
        ctx->dirty[ctx->cur_event] = TRUE;
        rebuild_list(ctx);
    }
}

static void on_delete_clicked(GtkButton *btn, gpointer data)
{
    HooksDialogCtx *ctx = data;
    gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx"));
    GList *link = g_list_nth(ctx->hooks[ctx->cur_event], idx);
    if (link) {
        hook_entry_free(link->data);
        ctx->hooks[ctx->cur_event] = g_list_delete_link(
            ctx->hooks[ctx->cur_event], link);
        ctx->dirty[ctx->cur_event] = TRUE;
        rebuild_list(ctx);
    }
}

static void rebuild_list(HooksDialogCtx *ctx)
{
    GList *hooks = ctx->hooks[ctx->cur_event];

    /* Clear */
    GList *children = gtk_container_get_children(GTK_CONTAINER(ctx->list_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    if (!hooks) {
        GtkWidget *empty = gtk_label_new(
            "No hooks defined. Click \"Add\" or use a preset.");
        gtk_widget_set_opacity(empty, 0.5);
        gtk_widget_set_margin_top(empty, 20);
        gtk_widget_set_margin_bottom(empty, 20);
        gtk_container_add(GTK_CONTAINER(ctx->list_box), empty);
        gtk_widget_show_all(ctx->list_box);
        return;
    }

    gint idx = 0;
    for (GList *l = hooks; l; l = l->next, idx++) {
        HookEntry *e = l->data;

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(row_box, 6);
        gtk_widget_set_margin_end(row_box, 6);
        gtk_widget_set_margin_top(row_box, 4);
        gtk_widget_set_margin_bottom(row_box, 4);

        /* Matcher (bold) + type tag + optional if condition */
        const gchar *type_tag = (e->type == HOOK_TYPE_AGENT) ? "agent"
            : (e->type == HOOK_TYPE_PROMPT) ? "prompt" : "command";
        gchar *markup;
        if (e->if_cond && e->if_cond[0] != '\0')
            markup = g_markup_printf_escaped(
                "<b>%s</b>  <small>(%s, if %s)</small>",
                e->matcher, type_tag, e->if_cond);
        else
            markup = g_markup_printf_escaped(
                "<b>%s</b>  <small>(%s)</small>", e->matcher, type_tag);
        GtkWidget *match_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(match_label), markup);
        gtk_label_set_xalign(GTK_LABEL(match_label), 0);
        g_free(markup);
        gtk_box_pack_start(GTK_BOX(row_box), match_label, FALSE, FALSE, 0);

        /* Content preview — show first line only */
        const gchar *content_text = (e->type == HOOK_TYPE_COMMAND)
            ? e->command : e->prompt;
        gchar *first_line = g_strdup(content_text ? content_text : "");
        gchar *nl = strchr(first_line, '\n');
        if (nl) *nl = '\0';

        GtkWidget *cmd_label = gtk_label_new(first_line);
        gtk_label_set_xalign(GTK_LABEL(cmd_label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(cmd_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(cmd_label), 60);
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_family_new("monospace"));
        pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(cmd_label), attrs);
        pango_attr_list_unref(attrs);
        gtk_widget_set_opacity(cmd_label, 0.7);
        g_free(first_line);
        gtk_box_pack_start(GTK_BOX(row_box), cmd_label, FALSE, FALSE, 0);

        /* Buttons row */
        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(btn_box, GTK_ALIGN_END);

        GtkWidget *edit_btn = gtk_button_new_with_label("Edit");
        gtk_button_set_relief(GTK_BUTTON(edit_btn), GTK_RELIEF_NONE);
        g_object_set_data(G_OBJECT(edit_btn), "idx", GINT_TO_POINTER(idx));
        g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_edit_clicked), ctx);

        GtkWidget *del_btn = gtk_button_new_with_label("Delete");
        gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
        g_object_set_data(G_OBJECT(del_btn), "idx", GINT_TO_POINTER(idx));
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_clicked), ctx);

        gtk_box_pack_start(GTK_BOX(btn_box), edit_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(btn_box), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), btn_box, FALSE, FALSE, 0);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), row_box);
        gtk_list_box_insert(GTK_LIST_BOX(ctx->list_box), row, -1);
    }

    gtk_widget_show_all(ctx->list_box);
}

static void on_add_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    HooksDialogCtx *ctx = data;
    HookEntry *entry = hook_entry_new_command("", "");
    if (edit_hook_dialog(ctx->parent, entry, TRUE, cur_evtype(ctx))) {
        ctx->hooks[ctx->cur_event] = g_list_append(
            ctx->hooks[ctx->cur_event], entry);
        ctx->dirty[ctx->cur_event] = TRUE;
        rebuild_list(ctx);
    } else {
        hook_entry_free(entry);
    }
}

static void on_preset_activate(GtkMenuItem *item, gpointer data)
{
    HooksDialogCtx *ctx = data;
    const HookPreset *p = g_object_get_data(G_OBJECT(item), "preset");
    if (!p) return;

    HookEntry *entry = (p->type == HOOK_TYPE_AGENT)
        ? hook_entry_new_agent(p->matcher, p->prompt)
        : (p->type == HOOK_TYPE_PROMPT)
        ? hook_entry_new_prompt(p->matcher, p->prompt)
        : hook_entry_new_command(p->matcher, p->command);
    if (edit_hook_dialog(ctx->parent, entry, TRUE, cur_evtype(ctx))) {
        ctx->hooks[ctx->cur_event] = g_list_append(
            ctx->hooks[ctx->cur_event], entry);
        ctx->dirty[ctx->cur_event] = TRUE;
        rebuild_list(ctx);
    } else {
        hook_entry_free(entry);
    }
}

/* ── "Block Bash Command" wizard ─────────────────────────────────── */

static HookEntry *bash_block_wizard(GtkWindow *parent)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Block a Bash Command",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Create", GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 450, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    /* Description */
    GtkWidget *desc = gtk_label_new(
        "Enter a pattern to block. The hook will prevent Claude from\n"
        "running any Bash command matching this pattern.");
    gtk_label_set_xalign(GTK_LABEL(desc), 0);
    gtk_box_pack_start(GTK_BOX(content), desc, FALSE, FALSE, 0);

    /* Pattern */
    GtkWidget *plabel = gtk_label_new("Pattern (grep extended regex):");
    gtk_label_set_xalign(GTK_LABEL(plabel), 0);
    gtk_box_pack_start(GTK_BOX(content), plabel, FALSE, FALSE, 0);

    GtkWidget *pattern_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pattern_entry),
                                   "e.g. rm -rf, docker rm, DROP TABLE");
    gtk_box_pack_start(GTK_BOX(content), pattern_entry, FALSE, FALSE, 0);

    /* Reason */
    GtkWidget *rlabel = gtk_label_new("Block message (shown to Claude):");
    gtk_label_set_xalign(GTK_LABEL(rlabel), 0);
    gtk_box_pack_start(GTK_BOX(content), rlabel, FALSE, FALSE, 0);

    GtkWidget *reason_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(reason_entry),
                                   "e.g. This command is not allowed");
    gtk_box_pack_start(GTK_BOX(content), reason_entry, FALSE, FALSE, 0);

    /* Match mode */
    GtkWidget *case_check = gtk_check_button_new_with_label(
        "Case insensitive");
    gtk_box_pack_start(GTK_BOX(content), case_check, FALSE, FALSE, 0);

    GtkWidget *help = gtk_label_new(
        "The pattern is matched against the full command string.\n"
        "Uses grep -E (extended regex).");
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    gtk_widget_set_opacity(help, 0.5);
    gtk_box_pack_start(GTK_BOX(content), help, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    HookEntry *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const gchar *pattern = gtk_entry_get_text(GTK_ENTRY(pattern_entry));
        const gchar *reason = gtk_entry_get_text(GTK_ENTRY(reason_entry));
        gboolean icase = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(case_check));

        if (pattern[0] != '\0') {
            const gchar *reason_text = (reason[0] != '\0')
                ? reason : "Blocked by project hook";
            const gchar *grep_flag = icase ? " -i" : "";

            gchar *command = g_strdup_printf(
                "# %s\n"
                "INPUT=$(cat)\n"
                "CMD=$(echo \"$INPUT\" | jq -r '.tool_input.command // empty')\n"
                "if echo \"$CMD\" | grep -qE%s '%s'; then\n"
                "  echo '%s' >&2\n"
                "  exit 2\n"
                "fi",
                reason_text, grep_flag, pattern, reason_text);

            result = hook_entry_new_command("Bash", command);
            g_free(command);
        }
    }

    gtk_widget_destroy(dlg);
    return result;
}

static void on_bash_block_wizard(GtkMenuItem *item, gpointer data)
{
    (void)item;
    HooksDialogCtx *ctx = data;
    HookEntry *entry = bash_block_wizard(ctx->parent);
    if (entry) {
        ctx->hooks[ctx->cur_event] = g_list_append(
            ctx->hooks[ctx->cur_event], entry);
        ctx->dirty[ctx->cur_event] = TRUE;
        rebuild_list(ctx);
    }
}

/* ── Preset menu rebuild ────────────────────────────────────────── */

static void rebuild_preset_menu(HooksDialogCtx *ctx)
{
    GtkMenu *old_menu = gtk_menu_button_get_popup(
        GTK_MENU_BUTTON(ctx->preset_btn));
    if (old_menu)
        gtk_widget_destroy(GTK_WIDGET(old_menu));

    const EventTypeDef *ev = cur_evtype(ctx);
    GtkWidget *menu = gtk_menu_new();

    for (const HookPreset *p = ev->presets; p && p->label; p++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(p->label);
        g_object_set_data(G_OBJECT(mi), "preset", (gpointer)p);
        g_signal_connect(mi, "activate", G_CALLBACK(on_preset_activate), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    if (ev->has_bash_wizard) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        GtkWidget *wiz = gtk_menu_item_new_with_label("Block Bash Command...");
        g_signal_connect(wiz, "activate",
                         G_CALLBACK(on_bash_block_wizard), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), wiz);
    }

    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(ctx->preset_btn), menu);
}

/* ── Event type combo changed ───────────────────────────────────── */

static void on_event_type_changed(GtkComboBox *combo, gpointer data)
{
    HooksDialogCtx *ctx = data;
    gint active = gtk_combo_box_get_active(combo);
    if (active < 0 || (guint)active == ctx->cur_event) return;

    ctx->cur_event = (guint)active;
    ensure_loaded(ctx, ctx->cur_event);
    rebuild_list(ctx);
    rebuild_preset_menu(ctx);
}

/* ── Open raw JSON ──────────────────────────────────────────────── */

static void on_open_json(GtkButton *btn, gpointer data)
{
    (void)btn;
    HooksDialogCtx *ctx = data;
    /* Ensure the file exists before opening */
    if (!g_file_test(ctx->path, G_FILE_TEST_EXISTS)) {
        gchar *dir = g_path_get_dirname(ctx->path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        g_file_set_contents(ctx->path, "{}\n", -1, NULL);
    }
    document_open_file(ctx->path, FALSE, NULL, NULL);
}

/* ── Main dialog ────────────────────────────────────────────────── */

void hooks_dialog_run(GtkWindow *parent)
{
    HooksDialogCtx ctx = { 0 };
    ctx.parent = parent;
    ctx.cur_event = 0;
    ctx.path = get_settings_path();

    /* Load the initial event type */
    ensure_loaded(&ctx, 0);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Manage Hooks",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        "_Save", GTK_RESPONSE_APPLY,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 550, 450);

    ctx.parent = GTK_WINDOW(dlg);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_set_spacing(GTK_BOX(content), 6);

    /* Event type selector */
    GtkWidget *event_combo = gtk_combo_box_text_new();
    for (guint i = 0; i < N_EVENT_TYPES; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(event_combo),
                                       event_types[i].display_name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(event_combo), 0);
    g_signal_connect(event_combo, "changed",
                     G_CALLBACK(on_event_type_changed), &ctx);
    gtk_box_pack_start(GTK_BOX(content), event_combo, FALSE, FALSE, 0);

    /* Toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *add_btn = gtk_button_new_with_label("+ Add");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), &ctx);
    gtk_box_pack_start(GTK_BOX(toolbar), add_btn, FALSE, FALSE, 0);

    /* Preset menu button */
    ctx.preset_btn = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(ctx.preset_btn), "+ From Preset");
    gtk_box_pack_start(GTK_BOX(toolbar), ctx.preset_btn, FALSE, FALSE, 0);

    /* Spacer */
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    /* Open raw JSON */
    GtkWidget *json_btn = gtk_button_new_with_label("Open JSON");
    gtk_widget_set_tooltip_text(json_btn, "Open settings.local.json in editor");
    g_signal_connect(json_btn, "clicked", G_CALLBACK(on_open_json), &ctx);
    gtk_box_pack_end(GTK_BOX(toolbar), json_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), toolbar, FALSE, FALSE, 0);

    /* Hook list */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
        GTK_SHADOW_IN);

    ctx.list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ctx.list_box),
        GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroll), ctx.list_box);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    /* Build initial state */
    rebuild_preset_menu(&ctx);
    rebuild_list(&ctx);
    gtk_widget_show_all(dlg);

    /* Run dialog loop */
    gboolean done = FALSE;
    while (!done) {
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        switch (resp) {
        case GTK_RESPONSE_APPLY: {
            gboolean ok = TRUE;
            for (guint i = 0; i < N_EVENT_TYPES; i++) {
                if (ctx.dirty[i]) {
                    if (save_hooks(ctx.path, event_types[i].event_name,
                                   ctx.hooks[i]))
                        ctx.dirty[i] = FALSE;
                    else
                        ok = FALSE;
                }
            }
            if (!ok)
                msgwin_status_add("[geany-code] Failed to save some hooks");
            done = TRUE;
            break;
        }
        default: {
            gboolean any_dirty = FALSE;
            for (guint i = 0; i < N_EVENT_TYPES; i++)
                if (ctx.dirty[i]) { any_dirty = TRUE; break; }

            if (any_dirty) {
                GtkWidget *confirm = gtk_message_dialog_new(
                    GTK_WINDOW(dlg), GTK_DIALOG_MODAL,
                    GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                    "Discard unsaved changes?");
                gint r = gtk_dialog_run(GTK_DIALOG(confirm));
                gtk_widget_destroy(confirm);
                if (r == GTK_RESPONSE_YES)
                    done = TRUE;
            } else {
                done = TRUE;
            }
            break;
        }
        }
    }

    gtk_widget_destroy(dlg);
    for (guint i = 0; i < N_EVENT_TYPES; i++)
        g_list_free_full(ctx.hooks[i], hook_entry_free);
    g_free(ctx.path);
}
