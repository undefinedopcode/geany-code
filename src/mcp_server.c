/*
 * geany-code-mcp-server — MCP server that bridges Claude to Geany's editor
 *
 * Speaks JSON-RPC 2.0 on stdin/stdout (MCP protocol).
 * Calls Geany's DBus interface for editor operations.
 */

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static GDBusProxy *editor_proxy = NULL;

/* ── DBus helpers ────────────────────────────────────────────────── */

static gboolean connect_dbus(void)
{
    GError *error = NULL;
    editor_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.geanycode.editor",
        "/GeanyCode/Editor",
        "org.geanycode.Editor",
        NULL, &error);

    if (error) {
        g_printerr("Failed to connect to Geany DBus: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }
    return TRUE;
}

/* ── JSON helpers ────────────────────────────────────────────────── */

static gchar *node_to_string(JsonNode *node)
{
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar *str = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    return str;
}

static void send_response(JsonBuilder *b)
{
    JsonNode *root = json_builder_get_root(b);
    gchar *json = node_to_string(root);
    fprintf(stdout, "%s\n", json);
    fflush(stdout);
    g_free(json);
    json_node_free(root);
}

static void send_result(gint64 id, JsonBuilder *result_builder)
{
    JsonNode *result_node = json_builder_get_root(result_builder);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, id);
    json_builder_set_member_name(b, "result");
    json_builder_add_value(b, json_node_copy(result_node));
    json_builder_end_object(b);

    send_response(b);
    g_object_unref(b);
    json_node_free(result_node);
}

static void send_text_result(gint64 id, const gchar *text)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "content");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "text");
    json_builder_set_member_name(b, "text");
    json_builder_add_string_value(b, text);
    json_builder_end_object(b);
    json_builder_end_array(b);
    json_builder_end_object(b);

    send_result(id, b);
    g_object_unref(b);
}

static void send_error(gint64 id, gint code, const gchar *message)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "jsonrpc");
    json_builder_add_string_value(b, "2.0");
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, id);
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code");
    json_builder_add_int_value(b, code);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
    json_builder_end_object(b);
    json_builder_end_object(b);

    send_response(b);
    g_object_unref(b);
}

/* ── DBus call helper ────────────────────────────────────────────── */

static GVariant *call_dbus(const gchar *method, GVariant *params,
                           gint64 mcp_id)
{
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        editor_proxy, method, params,
        G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);

    if (error) {
        send_error(mcp_id, -32000, error->message);
        g_error_free(error);
        return NULL;
    }
    return result;
}

/* ── Tool schema helper ──────────────────────────────────────────── */

static void add_tool(JsonBuilder *b, const gchar *name, const gchar *desc,
                     const gchar *props_json, const gchar *required_json)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, name);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, desc);
    json_builder_set_member_name(b, "inputSchema");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");

    /* Parse properties JSON */
    json_builder_set_member_name(b, "properties");
    if (props_json) {
        JsonParser *p = json_parser_new();
        json_parser_load_from_data(p, props_json, -1, NULL);
        json_builder_add_value(b, json_node_copy(json_parser_get_root(p)));
        g_object_unref(p);
    } else {
        json_builder_begin_object(b);
        json_builder_end_object(b);
    }

    if (required_json) {
        json_builder_set_member_name(b, "required");
        JsonParser *p = json_parser_new();
        json_parser_load_from_data(p, required_json, -1, NULL);
        json_builder_add_value(b, json_node_copy(json_parser_get_root(p)));
        g_object_unref(p);
    }

    json_builder_end_object(b);  /* inputSchema */
    json_builder_end_object(b);  /* tool */
}

/* ── MCP method handlers ─────────────────────────────────────────── */

static void handle_initialize(gint64 id)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "protocolVersion");
    json_builder_add_string_value(b, "2024-11-05");
    json_builder_set_member_name(b, "capabilities");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_object(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "serverInfo");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "geany-code-mcp");
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, "0.1.0");
    json_builder_end_object(b);
    json_builder_set_member_name(b, "instructions");
    json_builder_add_string_value(b,
        "You are connected to the Geany IDE via MCP. "
        "Prefer the geanycode_* tools over built-in equivalents when available:\n"
        "- geanycode_edit: Edit files (opens in editor, returns match line number for diff display)\n"
        "- geanycode_write: Create/overwrite files (opens in editor)\n"
        "- geanycode_read: Read files (opens in editor, stays in sync)\n"
        "- geanycode_goto: Navigate to a specific line\n"
        "- geanycode_build: Build the project (cmake/make/compile)\n"
        "- geanycode_documents: List open documents\n"
        "- geanycode_cursor: Get current cursor position and selection\n"
        "- geanycode_ask_user: Ask the user a question via Geany UI\n"
        "These tools integrate with the editor so the user can see changes in real time.");
    json_builder_end_object(b);

    send_result(id, b);
    g_object_unref(b);
}

static void handle_tools_list(gint64 id)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tools");
    json_builder_begin_array(b);

    add_tool(b, "geanycode_documents",
        "List all documents currently open in Geany editor",
        NULL, NULL);

    add_tool(b, "geanycode_read",
        "PREFERRED over the built-in Read tool. "
        "Read content of a file via Geany editor — opens the file in the IDE and reloads from disk to get latest content.",
        "{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"}}",
        "[\"file_path\"]");

    add_tool(b, "geanycode_edit",
        "PREFERRED over the built-in Edit tool. "
        "Edit a file via Geany editor — applies the edit in the IDE so the user sees changes in real time. "
        "Returns the matched line number for diff display. The old_text must be a unique exact match.",
        "{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"},"
        "\"old_text\":{\"type\":\"string\",\"description\":\"Exact text to find and replace (must be unique)\"},"
        "\"new_text\":{\"type\":\"string\",\"description\":\"Replacement text\"}}",
        "[\"file_path\",\"old_text\",\"new_text\"]");

    add_tool(b, "geanycode_write",
        "PREFERRED over the built-in Write tool. "
        "Write content to a file via Geany editor — creates or overwrites the file, opens it in the IDE, and saves.",
        "{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}}",
        "[\"file_path\",\"content\"]");

    add_tool(b, "geanycode_goto",
        "Navigate to a specific line in a file. Opens the file in Geany and jumps to the line.",
        "{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"},"
        "\"line\":{\"type\":\"integer\",\"description\":\"Line number (1-based)\"}}",
        "[\"file_path\",\"line\"]");

    add_tool(b, "geanycode_build",
        "PREFERRED for building. "
        "Trigger a build action in Geany. Supported commands: compile, build, make, run. "
        "Returns command output and exit code.",
        "{\"command\":{\"type\":\"string\",\"description\":\"Build command: compile, build, make, or run\"}}",
        "[\"command\"]");

    add_tool(b, "geanycode_project",
        "Get information about the current Geany project (name, base path, open files count).",
        NULL, NULL);

    add_tool(b, "geanycode_search",
        "Search for text across all open documents in Geany. Returns file:line: matches.",
        "{\"pattern\":{\"type\":\"string\",\"description\":\"Search pattern\"},"
        "\"regex\":{\"type\":\"boolean\",\"description\":\"Use regex matching (default false)\"}}",
        "[\"pattern\"]");

    add_tool(b, "geanycode_indicators",
        "Set or clear error/warning indicators on lines in a file. "
        "Set type 0 for error (red), 1 for warning. Use action 'clear' to remove all.",
        "{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute file path\"},"
        "\"action\":{\"type\":\"string\",\"description\":\"'set' or 'clear'\"},"
        "\"line\":{\"type\":\"integer\",\"description\":\"Line number (1-based, for set)\"},"
        "\"type\":{\"type\":\"integer\",\"description\":\"0=error, 1=warning (for set)\"}}",
        "[\"file_path\",\"action\"]");

    add_tool(b, "geanycode_selection",
        "Get the current cursor position, selection, and context from the active editor.",
        NULL, NULL);

    add_tool(b, "geanycode_ask_user",
        "Ask the user 1-4 structured questions with selectable options. "
        "Each question has a header (short key), question text, multiSelect flag, and 2-4 options. "
        "An 'Other' option is automatically added. Returns answers as JSON keyed by headers. "
        "Always prefer this over AskUserQuestion when available.",
        "{\"questions\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
        "\"properties\":{"
        "\"header\":{\"type\":\"string\",\"description\":\"Short label used as answer key\"},"
        "\"question\":{\"type\":\"string\",\"description\":\"The question text\"},"
        "\"multiSelect\":{\"type\":\"boolean\",\"description\":\"Allow multiple selections\"},"
        "\"options\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
        "\"properties\":{"
        "\"label\":{\"type\":\"string\",\"description\":\"Option label\"},"
        "\"description\":{\"type\":\"string\",\"description\":\"Option description\"}"
        "},\"required\":[\"label\",\"description\"]},"
        "\"minItems\":2,\"maxItems\":4}"
        "},\"required\":[\"header\",\"question\",\"multiSelect\",\"options\"]},"
        "\"minItems\":1,\"maxItems\":4}}",
        "[\"questions\"]");

    json_builder_end_array(b);
    json_builder_end_object(b);

    send_result(id, b);
    g_object_unref(b);
}

static void handle_tools_call(gint64 id, JsonObject *params)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : "";
    JsonObject *args = json_object_has_member(params, "arguments")
        ? json_object_get_object_member(params, "arguments") : NULL;

    if (g_strcmp0(name, "geanycode_documents") == 0) {
        GVariant *result = call_dbus("ListDocuments", NULL, id);
        if (!result) return;

        GVariantIter *iter = NULL;
        g_variant_get(result, "(as)", &iter);

        GString *text = g_string_new("Open documents:\n");
        const gchar *path;
        while (g_variant_iter_next(iter, "&s", &path))
            g_string_append_printf(text, "  %s\n", path);
        g_variant_iter_free(iter);
        g_variant_unref(result);

        send_text_result(id, text->str);
        g_string_free(text, TRUE);

    } else if (g_strcmp0(name, "geanycode_read") == 0) {
        const gchar *fp = args && json_object_has_member(args, "file_path")
            ? json_object_get_string_member(args, "file_path") : "";

        GVariant *result = call_dbus("ReadDocument",
            g_variant_new("(s)", fp), id);
        if (!result) return;

        const gchar *content = NULL;
        g_variant_get(result, "(&s)", &content);
        send_text_result(id, content ? content : "");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_edit") == 0) {
        const gchar *fp = args && json_object_has_member(args, "file_path")
            ? json_object_get_string_member(args, "file_path") : "";
        const gchar *old_text = args && json_object_has_member(args, "old_text")
            ? json_object_get_string_member(args, "old_text") : "";
        const gchar *new_text = args && json_object_has_member(args, "new_text")
            ? json_object_get_string_member(args, "new_text") : "";

        GVariant *result = call_dbus("EditDocument",
            g_variant_new("(sss)", fp, old_text, new_text), id);
        if (!result) return;

        const gchar *msg = NULL;
        g_variant_get(result, "(&s)", &msg);
        send_text_result(id, msg ? msg : "Done");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_write") == 0) {
        const gchar *fp = args && json_object_has_member(args, "file_path")
            ? json_object_get_string_member(args, "file_path") : "";
        const gchar *content = args && json_object_has_member(args, "content")
            ? json_object_get_string_member(args, "content") : "";

        GVariant *result = call_dbus("WriteDocument",
            g_variant_new("(ss)", fp, content), id);
        if (!result) return;

        const gchar *msg = NULL;
        g_variant_get(result, "(&s)", &msg);
        send_text_result(id, msg ? msg : "Done");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_goto") == 0) {
        const gchar *fp = args && json_object_has_member(args, "file_path")
            ? json_object_get_string_member(args, "file_path") : "";
        gint64 line = args && json_object_has_member(args, "line")
            ? json_object_get_int_member(args, "line") : 1;

        GVariant *result = call_dbus("GotoLine",
            g_variant_new("(si)", fp, (gint)line), id);
        if (!result) return;

        const gchar *msg = NULL;
        g_variant_get(result, "(&s)", &msg);
        send_text_result(id, msg ? msg : "Done");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_build") == 0) {
        const gchar *command = args && json_object_has_member(args, "command")
            ? json_object_get_string_member(args, "command") : "";

        GVariant *result = call_dbus("Build",
            g_variant_new("(s)", command), id);
        if (!result) return;

        const gchar *msg = NULL;
        g_variant_get(result, "(&s)", &msg);
        send_text_result(id, msg ? msg : "Done");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_project") == 0) {
        GVariant *result = call_dbus("GetProject", NULL, id);
        if (!result) return;

        const gchar *info = NULL;
        g_variant_get(result, "(&s)", &info);
        send_text_result(id, info ? info : "No project info");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_search") == 0) {
        const gchar *pattern = args && json_object_has_member(args, "pattern")
            ? json_object_get_string_member(args, "pattern") : "";
        gboolean use_regex = args && json_object_has_member(args, "regex")
            ? json_object_get_boolean_member(args, "regex") : FALSE;

        GVariant *result = call_dbus("Search",
            g_variant_new("(sb)", pattern, use_regex), id);
        if (!result) return;

        const gchar *text = NULL;
        g_variant_get(result, "(&s)", &text);
        send_text_result(id, text ? text : "");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_indicators") == 0) {
        const gchar *fp = args && json_object_has_member(args, "file_path")
            ? json_object_get_string_member(args, "file_path") : "";
        const gchar *action = args && json_object_has_member(args, "action")
            ? json_object_get_string_member(args, "action") : "set";

        if (g_strcmp0(action, "clear") == 0) {
            GVariant *result = call_dbus("ClearIndicators",
                g_variant_new("(s)", fp), id);
            if (!result) return;
            const gchar *msg = NULL;
            g_variant_get(result, "(&s)", &msg);
            send_text_result(id, msg ? msg : "Done");
            g_variant_unref(result);
        } else {
            gint64 line = args && json_object_has_member(args, "line")
                ? json_object_get_int_member(args, "line") : 1;
            gint64 itype = args && json_object_has_member(args, "type")
                ? json_object_get_int_member(args, "type") : 0;

            GVariant *result = call_dbus("SetIndicator",
                g_variant_new("(sii)", fp, (gint)line, (gint)itype), id);
            if (!result) return;
            const gchar *msg = NULL;
            g_variant_get(result, "(&s)", &msg);
            send_text_result(id, msg ? msg : "Done");
            g_variant_unref(result);
        }

    } else if (g_strcmp0(name, "geanycode_selection") == 0) {
        GVariant *result = call_dbus("GetSelection", NULL, id);
        if (!result) return;

        const gchar *info = NULL;
        g_variant_get(result, "(&s)", &info);
        send_text_result(id, info ? info : "");
        g_variant_unref(result);

    } else if (g_strcmp0(name, "geanycode_ask_user") == 0) {
        /* Serialize the questions array to a JSON string for DBus */
        if (!args || !json_object_has_member(args, "questions")) {
            send_error(id, -32602, "Missing 'questions' parameter");
            return;
        }

        JsonGenerator *qg = json_generator_new();
        json_generator_set_root(qg,
            json_object_get_member(args, "questions"));
        gchar *questions_json = json_generator_to_data(qg, NULL);
        g_object_unref(qg);

        /* Call DBus — this blocks until user responds (up to 5 min) */
        GError *error = NULL;
        GVariant *result = g_dbus_proxy_call_sync(
            editor_proxy, "AskUserQuestion",
            g_variant_new("(s)", questions_json),
            G_DBUS_CALL_FLAGS_NONE, 310000, /* 5 min + 10s buffer */
            NULL, &error);
        g_free(questions_json);

        if (error) {
            send_error(id, -32000, error->message);
            g_error_free(error);
            return;
        }

        const gchar *response = NULL;
        g_variant_get(result, "(&s)", &response);
        send_text_result(id, response ? response : "No response");
        g_variant_unref(result);

    } else {
        send_error(id, -32601, "Unknown tool");
    }
}

/* ── Message dispatch ────────────────────────────────────────────── */

static void process_message(const gchar *line)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, NULL)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
    gint64 id = json_object_has_member(msg, "id")
        ? json_object_get_int_member(msg, "id") : 0;
    const gchar *method = json_object_has_member(msg, "method")
        ? json_object_get_string_member(msg, "method") : "";

    if (g_strcmp0(method, "initialize") == 0) {
        handle_initialize(id);
    } else if (g_strcmp0(method, "notifications/initialized") == 0) {
        /* No response needed */
    } else if (g_strcmp0(method, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (g_strcmp0(method, "tools/call") == 0) {
        JsonObject *params = json_object_has_member(msg, "params")
            ? json_object_get_object_member(msg, "params") : NULL;
        if (params)
            handle_tools_call(id, params);
        else
            send_error(id, -32602, "Missing params");
    } else if (id > 0) {
        send_error(id, -32601, "Method not found");
    }

    g_object_unref(parser);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!connect_dbus()) {
        g_printerr("geany-code-mcp-server: cannot connect to Geany DBus\n");
        return 1;
    }

    char buf[1024 * 1024];
    while (fgets(buf, sizeof(buf), stdin)) {
        gsize len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
        if (strlen(buf) > 0)
            process_message(buf);
    }

    g_object_unref(editor_proxy);
    return 0;
}
