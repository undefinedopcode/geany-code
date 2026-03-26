#include "cli_session.h"
#include "plugin.h"
#include <gio/gio.h>
#include <string.h>

/* Debug helper - logs to Geany's Status tab */
#define DBG(fmt, ...) msgwin_status_add("[geany-code] " fmt, ##__VA_ARGS__)

struct _CLISession {
    GSubprocess   *process;
    GOutputStream *stdin_pipe;
    GInputStream  *stdout_pipe;
    GDataInputStream *stdout_reader;
    GDataInputStream *stderr_reader;

    gchar         *working_dir;
    gchar         *session_id;
    gchar         *permission_mode;
    gchar         *mcp_config_path;
    gboolean       running;
    GCancellable  *cancellable;

    /* Accumulated line buffer */
    GString       *line_buf;

    /* Current message being streamed */
    gchar         *current_msg_id;
    GString       *current_content;

    /* Initialize handshake */
    gchar         *init_request_id;

    /* Pending permission requests: request_id -> original input JSON */
    GHashTable    *pending_permissions;

    /* Callbacks */
    CLIMessageCb   message_cb;
    gpointer       message_data;
    CLIToolCallCb  tool_call_cb;
    gpointer       tool_call_data;
    CLIPermissionCb permission_cb;
    gpointer       permission_data;
    CLIInitCb      init_cb;
    gpointer       init_data;
    CLIModelsCb    models_cb;
    gpointer       models_data;
    CLICommandsCb  commands_cb;
    gpointer       commands_data;
    CLITodosCb     todos_cb;
    gpointer       todos_data;
    CLIThinkingCb  thinking_cb;
    gpointer       thinking_data;
    CLIMcpStatusCb mcp_status_cb;
    gpointer       mcp_status_data;
    gchar         *mcp_status_request_id;
    CLIErrorCb     error_cb;
    gpointer       error_data;
    CLIFinishedCb  finished_cb;
    gpointer       finished_data;
};

/* ── Forward declarations ────────────────────────────────────────── */

static void read_next_line(CLISession *session);
static void on_line_read(GObject *source, GAsyncResult *result, gpointer data);
static void read_next_stderr_line(CLISession *session);
static void on_stderr_line_read(GObject *source, GAsyncResult *result, gpointer data);
static void process_json_line(CLISession *session, const gchar *line);

/* ── Construction / destruction ──────────────────────────────────── */

CLISession *cli_session_new(void)
{
    CLISession *s = g_new0(CLISession, 1);
    s->line_buf = g_string_new("");
    s->current_content = g_string_new("");
    s->cancellable = g_cancellable_new();
    s->pending_permissions = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    return s;
}

void cli_session_free(CLISession *session)
{
    if (!session) return;

    cli_session_stop(session);

    g_object_unref(session->cancellable);
    g_string_free(session->line_buf, TRUE);
    g_string_free(session->current_content, TRUE);
    g_hash_table_unref(session->pending_permissions);
    g_free(session->init_request_id);
    g_free(session->mcp_status_request_id);
    g_free(session->current_msg_id);
    g_free(session->session_id);
    g_free(session->permission_mode);
    g_free(session->mcp_config_path);
    g_free(session->working_dir);
    g_free(session);
}

const gchar *cli_session_get_working_dir(CLISession *session)
{
    return session->working_dir;
}

/* ── Start the claude process ────────────────────────────────────── */

gboolean cli_session_start(CLISession *session, const gchar *working_dir)
{
    if (session->running)
        return FALSE;

    g_free(session->working_dir);
    session->working_dir = g_strdup(working_dir);

    GError *error = NULL;

    /*
     * Spawn `claude` in verbose streaming JSON mode.
     * The --verbose flag outputs JSON objects, one per line, for each
     * event (message start, content delta, tool use, result, etc.).
     *
     * Adjust this command as needed for your claude CLI installation.
     */
    /* Find claude binary - check PATH via g_find_program_in_path,
     * which searches the full PATH including ~/.local/bin */
    gchar *claude_bin = g_find_program_in_path("claude");
    if (!claude_bin) {
        /* Common locations as fallback */
        const gchar *fallbacks[] = {
            g_build_filename(g_get_home_dir(), ".local", "bin", "claude", NULL),
            g_build_filename(g_get_home_dir(), "bin", "claude", NULL),
            NULL
        };
        for (const gchar **p = fallbacks; *p; p++) {
            if (g_file_test(*p, G_FILE_TEST_IS_EXECUTABLE)) {
                claude_bin = g_strdup(*p);
                break;
            }
        }
    }
    if (!claude_bin) {
        if (session->error_cb)
            session->error_cb("Could not find 'claude' binary. "
                              "Is Claude Code CLI installed?",
                              session->error_data);
        return FALSE;
    }

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE);

    if (working_dir)
        g_subprocess_launcher_set_cwd(launcher, working_dir);

    /* Write MCP config file pointing to our MCP server binary */
    if (!session->mcp_config_path) {
        gchar *config_dir = g_build_filename(
            g_get_home_dir(), ".config", "geany", "plugins", "geany-code", NULL);
        g_mkdir_with_parents(config_dir, 0755);
        session->mcp_config_path = g_build_filename(
            config_dir, "mcp-config.json", NULL);
        g_free(config_dir);
    }

    {
        gchar *mcp_json = g_strdup_printf(
            "{\"mcpServers\":{\"geany\":{\"command\":\"%s\",\"args\":[]}}}\n",
            GEANY_CODE_MCP_SERVER);

        GError *werr = NULL;
        g_file_set_contents(session->mcp_config_path, mcp_json, -1, &werr);
        if (werr) {
            DBG("Failed to write MCP config: %s", werr->message);
            g_error_free(werr);
        } else {
            DBG("Wrote MCP config: %s", session->mcp_config_path);
        }
        g_free(mcp_json);
    }

    /* Build args dynamically for optional --permission-mode and --resume */
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, claude_bin);
    g_ptr_array_add(args, "-p");
    g_ptr_array_add(args, "--input-format"); g_ptr_array_add(args, "stream-json");
    g_ptr_array_add(args, "--output-format"); g_ptr_array_add(args, "stream-json");
    g_ptr_array_add(args, "--verbose");
    g_ptr_array_add(args, "--include-partial-messages");
    g_ptr_array_add(args, "--permission-prompt-tool"); g_ptr_array_add(args, "stdio");

    if (session->permission_mode) {
        g_ptr_array_add(args, "--permission-mode");
        g_ptr_array_add(args, session->permission_mode);
    }
    if (session->session_id) {
        g_ptr_array_add(args, "--resume");
        g_ptr_array_add(args, session->session_id);
    }
    if (session->mcp_config_path) {
        g_ptr_array_add(args, "--mcp-config");
        g_ptr_array_add(args, session->mcp_config_path);
    }
    g_ptr_array_add(args, NULL);

    DBG("Starting claude: %s (mode=%s, resume=%s)", claude_bin,
        session->permission_mode ? session->permission_mode : "default",
        session->session_id ? session->session_id : "none");
    DBG("Working dir: %s", working_dir ? working_dir : "(none)");

    session->process = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)args->pdata, &error);
    g_ptr_array_free(args, TRUE);

    g_free(claude_bin);
    g_object_unref(launcher);

    if (!session->process) {
        DBG("Failed to spawn: %s", error->message);
        if (session->error_cb)
            session->error_cb(error->message, session->error_data);
        g_error_free(error);
        return FALSE;
    }

    DBG("Claude process started");

    session->stdin_pipe = g_subprocess_get_stdin_pipe(session->process);
    session->stdout_pipe = g_subprocess_get_stdout_pipe(session->process);
    session->stdout_reader = g_data_input_stream_new(session->stdout_pipe);
    session->running = TRUE;

    /* Also read stderr for diagnostics */
    GInputStream *stderr_pipe = g_subprocess_get_stderr_pipe(session->process);
    if (stderr_pipe) {
        session->stderr_reader = g_data_input_stream_new(stderr_pipe);
        read_next_stderr_line(session);
    }

    /* Start reading output lines */
    read_next_line(session);

    /* Send initialize handshake immediately — must come before user messages */
    g_free(session->init_request_id);
    session->init_request_id = g_strdup_printf("init_%ld",
        g_get_monotonic_time());

    gchar *init_json = g_strdup_printf(
        "{\"type\":\"control_request\","
        "\"request_id\":\"%s\","
        "\"request\":{\"subtype\":\"initialize\"}}\n",
        session->init_request_id);

    DBG("Sending initialize handshake (request_id=%s)",
        session->init_request_id);

    g_output_stream_write_all(session->stdin_pipe,
                              init_json, strlen(init_json),
                              NULL, NULL, NULL);
    g_free(init_json);

    return TRUE;
}

/* ── Send a message to claude ────────────────────────────────────── */

void cli_session_send_message(CLISession *session, const gchar *text,
                              const gchar *file_path,
                              const gchar *selection,
                              GList *images)
{
    DBG("send_message: running=%d, stdin_pipe=%p", session->running,
        (void *)session->stdin_pipe);

    if (!session->running || !session->stdin_pipe) {
        DBG("send_message: not running or no stdin pipe, ignoring");
        return;
    }

    /*
     * With --input-format stream-json, we send a JSON envelope per message.
     * Stdin stays open for subsequent messages.
     *
     * Format: {"type":"user","message":{"role":"user","content":[{"type":"text","text":"..."}]},"session_id":null}
     */

    /* Build prompt text with optional context */
    GString *prompt_text = g_string_new("");
    if (file_path && strlen(file_path) > 0)
        g_string_append_printf(prompt_text, "[File: %s]\n", file_path);
    if (selection && strlen(selection) > 0)
        g_string_append_printf(prompt_text, "```\n%s\n```\n\n", selection);
    g_string_append(prompt_text, text);

    /* Build JSON envelope */
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "user");

    json_builder_set_member_name(builder, "message");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");
    json_builder_set_member_name(builder, "content");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "text");
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, prompt_text->str);
    json_builder_end_object(builder);

    /* Append image content blocks */
    for (GList *l = images; l; l = l->next) {
        const gchar *b64 = (const gchar *)l->data;
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "image");
        json_builder_set_member_name(builder, "source");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "base64");
        json_builder_set_member_name(builder, "media_type");
        json_builder_add_string_value(builder, "image/png");
        json_builder_set_member_name(builder, "data");
        json_builder_add_string_value(builder, b64);
        json_builder_end_object(builder);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "session_id");
    if (session->session_id)
        json_builder_add_string_value(builder, session->session_id);
    else
        json_builder_add_null_value(builder);

    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(builder));
    gchar *json_str = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    g_object_unref(builder);

    /* Append newline delimiter */
    GString *payload = g_string_new(json_str);
    g_string_append_c(payload, '\n');
    g_free(json_str);

    DBG("Sending %lu bytes to claude stdin", (unsigned long)payload->len);

    GError *error = NULL;
    g_output_stream_write_all(session->stdin_pipe,
                              payload->str, payload->len,
                              NULL, NULL, &error);
    if (error) {
        DBG("stdin write error: %s", error->message);
        if (session->error_cb)
            session->error_cb(error->message, session->error_data);
        g_error_free(error);
    } else {
        DBG("Prompt sent (stdin kept open for future messages)");
    }

    g_string_free(payload, TRUE);
    g_string_free(prompt_text, TRUE);
}

/* ── Session ID setter ───────────────────────────────────────────── */

void cli_session_set_session_id(CLISession *session, const gchar *session_id)
{
    g_free(session->session_id);
    session->session_id = g_strdup(session_id);
}

/* ── Mode switching ───────────────────────────────────────────────── */

void cli_session_set_mode(CLISession *session, const gchar *mode_id)
{
    if (!mode_id) return;

    /* No change? */
    if (session->permission_mode &&
        g_strcmp0(session->permission_mode, mode_id) == 0)
        return;

    DBG("Switching permission mode: %s -> %s",
        session->permission_mode ? session->permission_mode : "default",
        mode_id);

    g_free(session->permission_mode);
    session->permission_mode = g_strdup(mode_id);

    /* Restart the process with the new mode and --resume */
    if (!session->running)
        return;

    gchar *working_dir = g_strdup(session->working_dir);

    cli_session_stop(session);
    cli_session_start(session, working_dir);

    g_free(working_dir);
}

/* ── Model switching ──────────────────────────────────────────────── */

void cli_session_set_model(CLISession *session, const gchar *model_value)
{
    if (!model_value || !session->running || !session->stdin_pipe)
        return;

    DBG("Switching model to: %s", model_value);

    gchar *json = g_strdup_printf(
        "{\"type\":\"control_request\","
        "\"request_id\":\"model_%ld\","
        "\"request\":{\"subtype\":\"set_model\",\"model\":\"%s\"}}\n",
        g_get_monotonic_time(), model_value);

    g_output_stream_write_all(session->stdin_pipe,
                              json, strlen(json),
                              NULL, NULL, NULL);
    g_free(json);
}

/* ── Interrupt (graceful) ─────────────────────────────────────────── */

void cli_session_interrupt(CLISession *session)
{
    if (!session->running || !session->stdin_pipe) {
        DBG("interrupt: not running or no stdin, ignoring");
        return;
    }

    DBG("Sending interrupt control_request");

    const gchar *json =
        "{\"type\":\"control_request\","
        "\"request\":{\"subtype\":\"interrupt\"}}\n";

    GError *error = NULL;
    g_output_stream_write_all(session->stdin_pipe,
                              json, strlen(json),
                              NULL, NULL, &error);
    if (error) {
        DBG("interrupt write error: %s", error->message);
        g_error_free(error);
    }
}

/* ── Stop the process ────────────────────────────────────────────── */

void cli_session_stop(CLISession *session)
{
    if (!session->running)
        return;

    g_cancellable_cancel(session->cancellable);

    if (session->process) {
        g_subprocess_force_exit(session->process);
        g_clear_object(&session->process);
    }

    session->stdout_reader = NULL;  /* owned by process pipes */
    session->stdin_pipe = NULL;
    session->stdout_pipe = NULL;
    session->running = FALSE;

    /* Reset cancellable for next start */
    g_object_unref(session->cancellable);
    session->cancellable = g_cancellable_new();
}

gboolean cli_session_is_running(CLISession *session)
{
    return session->running;
}

/* ── Permission response ─────────────────────────────────────────── */

void cli_session_respond_permission(CLISession *session,
                                    const gchar *request_id,
                                    const gchar *option_id)
{
    if (!session->running || !session->stdin_pipe) {
        DBG("respond_permission: not running, ignoring");
        return;
    }

    DBG("Permission response: %s -> %s", request_id, option_id);

    /* Look up stored request JSON (contains input + permission_suggestions) */
    const gchar *stored_request = g_hash_table_lookup(
        session->pending_permissions, request_id);

    /* Parse stored request to extract input and suggestions */
    JsonParser *rp = json_parser_new();
    JsonObject *req_obj = NULL;
    if (stored_request && json_parser_load_from_data(rp, stored_request, -1, NULL))
        req_obj = json_node_get_object(json_parser_get_root(rp));

    /* Determine suggestion index if this is a suggestion response */
    gint suggestion_idx = -1;
    if (g_str_has_prefix(option_id, "suggestion_"))
        suggestion_idx = atoi(option_id + strlen("suggestion_"));

    /* Build the control_response envelope */
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "control_response");

    json_builder_set_member_name(b, "response");
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "subtype");
    json_builder_add_string_value(b, "success");

    json_builder_set_member_name(b, "request_id");
    json_builder_add_string_value(b, request_id);

    json_builder_set_member_name(b, "response");
    json_builder_begin_object(b);

    if (g_strcmp0(option_id, "deny") == 0) {
        json_builder_set_member_name(b, "behavior");
        json_builder_add_string_value(b, "deny");
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, "User denied this action");
    } else {
        json_builder_set_member_name(b, "behavior");
        json_builder_add_string_value(b, "allow");

        /* Include the original input as updatedInput */
        if (req_obj && json_object_has_member(req_obj, "input")) {
            json_builder_set_member_name(b, "updatedInput");
            json_builder_add_value(b,
                json_node_copy(json_object_get_member(req_obj, "input")));
        }

        /* For suggestion_ options, include updatedPermissions */
        if (suggestion_idx >= 0 && req_obj &&
            json_object_has_member(req_obj, "permission_suggestions")) {
            JsonArray *suggestions = json_object_get_array_member(
                req_obj, "permission_suggestions");
            if ((guint)suggestion_idx < json_array_get_length(suggestions)) {
                JsonNode *suggestion = json_array_get_element(
                    suggestions, suggestion_idx);
                json_builder_set_member_name(b, "updatedPermissions");
                json_builder_begin_array(b);
                json_builder_add_value(b, json_node_copy(suggestion));
                json_builder_end_array(b);
            }
        }
    }

    json_builder_end_object(b);  /* response (inner) */
    json_builder_end_object(b);  /* response (outer) */
    json_builder_end_object(b);  /* envelope */

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *json_str = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    g_object_unref(b);

    GString *payload = g_string_new(json_str);
    g_string_append_c(payload, '\n');
    g_free(json_str);

    DBG("Sending control_response: %.200s", payload->str);

    GError *error = NULL;
    g_output_stream_write_all(session->stdin_pipe,
                              payload->str, payload->len,
                              NULL, NULL, &error);
    if (error) {
        DBG("control_response write error: %s", error->message);
        g_error_free(error);
    }

    g_string_free(payload, TRUE);

    /* For setMode suggestions, send a follow-up control_request to apply */
    if (suggestion_idx >= 0 && req_obj &&
        json_object_has_member(req_obj, "permission_suggestions")) {
        JsonArray *suggestions = json_object_get_array_member(
            req_obj, "permission_suggestions");
        if ((guint)suggestion_idx < json_array_get_length(suggestions)) {
            JsonObject *sug = json_array_get_object_element(
                suggestions, suggestion_idx);
            const gchar *stype = json_object_has_member(sug, "type")
                ? json_object_get_string_member(sug, "type") : "";
            if (g_strcmp0(stype, "setMode") == 0) {
                const gchar *mode = json_object_has_member(sug, "mode")
                    ? json_object_get_string_member(sug, "mode") : NULL;
                if (mode) {
                    gchar *mode_json = g_strdup_printf(
                        "{\"type\":\"control_request\","
                        "\"request\":{\"subtype\":\"setMode\",\"mode\":\"%s\"}}\n",
                        mode);
                    DBG("Sending setMode control_request: %s", mode);
                    g_output_stream_write_all(session->stdin_pipe,
                                              mode_json, strlen(mode_json),
                                              NULL, NULL, NULL);
                    g_free(mode_json);
                }
            }
        }
    }

    g_object_unref(rp);
    g_hash_table_remove(session->pending_permissions, request_id);
}

/* ── Async stdout reading ────────────────────────────────────────── */

static void read_next_line(CLISession *session)
{
    if (!session->running || !session->stdout_reader)
        return;

    g_data_input_stream_read_line_async(
        session->stdout_reader,
        G_PRIORITY_DEFAULT,
        session->cancellable,
        on_line_read,
        session);
}

static void on_line_read(GObject *source, GAsyncResult *result, gpointer data)
{
    CLISession *session = data;
    GError *error = NULL;
    gsize length = 0;

    gchar *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, &length, &error);

    if (error) {
        DBG("stdout read error: %s", error->message);
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (session->error_cb)
                session->error_cb(error->message, session->error_data);
        }
        g_error_free(error);
        return;
    }

    if (!line) {
        DBG("stdout EOF - claude process ended");
        session->running = FALSE;
        if (session->finished_cb)
            session->finished_cb(session->finished_data);
        return;
    }

    DBG("stdout [%lu bytes]: %.120s%s", (unsigned long)length, line,
        length > 120 ? "..." : "");

    if (length > 0)
        process_json_line(session, line);

    g_free(line);

    /* Continue reading */
    read_next_line(session);
}

/* ── Async stderr reading ────────────────────────────────────────── */

static void read_next_stderr_line(CLISession *session)
{
    if (!session->running || !session->stderr_reader)
        return;

    g_data_input_stream_read_line_async(
        session->stderr_reader,
        G_PRIORITY_DEFAULT,
        session->cancellable,
        on_stderr_line_read,
        session);
}

static void on_stderr_line_read(GObject *source, GAsyncResult *result,
                                gpointer data)
{
    CLISession *session = data;
    GError *error = NULL;
    gsize length = 0;

    gchar *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, &length, &error);

    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            DBG("stderr read error: %s", error->message);
        g_error_free(error);
        return;
    }

    if (!line) {
        DBG("stderr EOF");
        return;
    }

    if (length > 0)
        DBG("stderr: %s", line);

    g_free(line);
    read_next_stderr_line(session);
}

/* ── JSON line parsing ───────────────────────────────────────────── */

static void process_json_line(CLISession *session, const gchar *line)
{
    JsonParser *parser = json_parser_new();

    if (!json_parser_load_from_data(parser, line, -1, NULL)) {
        DBG("JSON parse failed for line: %.80s", line);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        DBG("JSON root is not an object");
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);
    const gchar *type = NULL;

    if (json_object_has_member(obj, "type"))
        type = json_object_get_string_member(obj, "type");

    DBG("Event type: %s", type ? type : "(null)");

    if (!type) {
        g_object_unref(parser);
        return;
    }

    /*
     * Event types from `claude -p --output-format stream-json`:
     *   system     - init message with session_id, tools, etc.
     *   assistant  - assistant message (partial or complete), contains message.content[]
     *   stream_event - raw API streaming events (content_block_delta, etc.)
     *   result     - prompt complete (success or error)
     *   rate_limit_event - rate limit info (ignored)
     */

    if (g_strcmp0(type, "system") == 0) {
        /* Init message - capture session_id and other metadata */
        if (json_object_has_member(obj, "session_id")) {
            const gchar *sid = json_object_get_string_member(obj, "session_id");
            g_free(session->session_id);
            session->session_id = g_strdup(sid);
            DBG("Session ID: %s", sid ? sid : "(null)");
        }

        const gchar *model = json_object_has_member(obj, "model")
            ? json_object_get_string_member(obj, "model") : NULL;
        const gchar *perm_mode = json_object_has_member(obj, "permissionMode")
            ? json_object_get_string_member(obj, "permissionMode") : NULL;

        DBG("Model: %s", model ? model : "(null)");
        DBG("Permission mode: %s", perm_mode ? perm_mode : "(null)");

        if (perm_mode && !session->permission_mode) {
            session->permission_mode = g_strdup(perm_mode);
        }

        if (session->init_cb)
            session->init_cb(model, perm_mode, session->init_data);

        if (json_object_has_member(obj, "tools")) {
            JsonArray *tools = json_object_get_array_member(obj, "tools");
            DBG("Available tools: %u", json_array_get_length(tools));
        }

        if (json_object_has_member(obj, "slash_commands")) {
            JsonArray *cmds = json_object_get_array_member(obj, "slash_commands");
            guint clen = json_array_get_length(cmds);
            DBG("Slash commands: %u", clen);
            for (guint ci = 0; ci < clen && ci < 8; ci++) {
                JsonNode *cnode = json_array_get_element(cmds, ci);
                if (JSON_NODE_HOLDS_VALUE(cnode))
                    DBG("  /%s", json_node_get_string(cnode));
                else if (JSON_NODE_HOLDS_OBJECT(cnode)) {
                    JsonObject *cobj = json_node_get_object(cnode);
                    const gchar *cname = json_object_has_member(cobj, "name")
                        ? json_object_get_string_member(cobj, "name") : "?";
                    DBG("  /%s", cname);
                }
            }
            if (clen > 8) DBG("  ... and %u more", clen - 8);
        }

        /* Extract MCP server status */
        if (json_object_has_member(obj, "mcp_servers") && session->mcp_status_cb) {
            JsonArray *mcp = json_object_get_array_member(obj, "mcp_servers");
            DBG("MCP servers: %u", json_array_get_length(mcp));
            JsonGenerator *mg = json_generator_new();
            JsonNode *mnode = json_node_new(JSON_NODE_ARRAY);
            json_node_set_array(mnode, mcp);
            json_generator_set_root(mg, mnode);
            gchar *mjson = json_generator_to_data(mg, NULL);
            session->mcp_status_cb(mjson, session->mcp_status_data);
            g_free(mjson);
            json_node_free(mnode);
            g_object_unref(mg);
        }

        /* Create a placeholder message for streaming into */
        g_free(session->current_msg_id);
        session->current_msg_id = g_strdup_printf("msg_%ld",
            g_get_monotonic_time());
        g_string_truncate(session->current_content, 0);

    } else if (g_strcmp0(type, "assistant") == 0) {
        /* Assistant message - may be partial (streaming) or complete.
         * Extract text from message.content[] array */
        JsonObject *message = json_object_has_member(obj, "message")
            ? json_object_get_object_member(obj, "message") : NULL;
        if (!message) goto done;

        gboolean partial = FALSE;
        if (json_object_has_member(obj, "partial"))
            partial = json_object_get_boolean_member(obj, "partial");

        /* Use the API message ID to detect new vs continuing messages */
        const gchar *api_id = json_object_has_member(message, "id")
            ? json_object_get_string_member(message, "id") : NULL;

        if (api_id && (!session->current_msg_id ||
                       g_strcmp0(session->current_msg_id, api_id) != 0)) {
            /* New message — use API ID directly */
            g_free(session->current_msg_id);
            session->current_msg_id = g_strdup(api_id);
            g_string_truncate(session->current_content, 0);
        } else if (!session->current_msg_id) {
            session->current_msg_id = g_strdup_printf("msg_%ld",
                g_get_monotonic_time());
            g_string_truncate(session->current_content, 0);
        }

        /* Extract text and thinking from content blocks */
        if (json_object_has_member(message, "content")) {
            JsonArray *content = json_object_get_array_member(message, "content");
            guint len = json_array_get_length(content);

            /* Rebuild full text from all text blocks, emit thinking fragments */
            GString *full_text = g_string_new("");
            guint thinking_fragment = 0;
            gboolean prev_was_thinking = FALSE;
            for (guint i = 0; i < len; i++) {
                JsonObject *block = json_array_get_object_element(content, i);
                const gchar *block_type = json_object_get_string_member(block, "type");

                if (g_strcmp0(block_type, "thinking") == 0) {
                    const gchar *thinking = json_object_has_member(block, "thinking")
                        ? json_object_get_string_member(block, "thinking") : NULL;
                    if (thinking && strlen(thinking) > 0) {
                        if (!prev_was_thinking && i > 0)
                            thinking_fragment++;
                        if (session->thinking_cb)
                            session->thinking_cb(session->current_msg_id,
                                                 thinking_fragment, thinking,
                                                 partial, session->thinking_data);
                        prev_was_thinking = TRUE;
                    }
                } else if (g_strcmp0(block_type, "text") == 0) {
                    prev_was_thinking = FALSE;
                    const gchar *text = json_object_get_string_member(block, "text");
                    if (text)
                        g_string_append(full_text, text);
                } else if (g_strcmp0(block_type, "tool_use") == 0 && !partial) {
                    prev_was_thinking = FALSE;
                    /* Complete tool call */
                    const gchar *tool_id = json_object_has_member(block, "id")
                        ? json_object_get_string_member(block, "id") : "";
                    const gchar *tool_name = json_object_has_member(block, "name")
                        ? json_object_get_string_member(block, "name") : "unknown";

                    gchar *input_str = NULL;
                    if (json_object_has_member(block, "input")) {
                        JsonGenerator *g = json_generator_new();
                        json_generator_set_root(g,
                            json_object_get_member(block, "input"));
                        input_str = json_generator_to_data(g, NULL);
                        g_object_unref(g);
                    }

                    DBG("Tool call: %s (%s)", tool_name, tool_id);
                    if (session->tool_call_cb)
                        session->tool_call_cb(
                            session->current_msg_id, tool_id,
                            tool_name, input_str ? input_str : "",
                            NULL, session->tool_call_data);

                    /* TodoWrite: extract todos and emit callback */
                    if (g_strcmp0(tool_name, "TodoWrite") == 0 &&
                        session->todos_cb &&
                        json_object_has_member(block, "input")) {
                        JsonObject *input = json_object_get_object_member(
                            block, "input");
                        if (json_object_has_member(input, "todos")) {
                            JsonGenerator *tg = json_generator_new();
                            json_generator_set_root(tg,
                                json_object_get_member(input, "todos"));
                            gchar *todos_json = json_generator_to_data(tg, NULL);
                            session->todos_cb(todos_json, session->todos_data);
                            g_free(todos_json);
                            g_object_unref(tg);
                        }
                    }

                    g_free(input_str);
                }
            }

            /* Update accumulated content if we got new text */
            if (full_text->len > 0) {
                g_string_assign(session->current_content, full_text->str);

                if (session->message_cb)
                    session->message_cb(session->current_msg_id, "assistant",
                                       session->current_content->str, partial,
                                       session->message_data);
            }
            g_string_free(full_text, TRUE);
        }

    } else if (g_strcmp0(type, "user") == 0) {
        /* User message echoed back — contains tool_result content blocks */
        JsonObject *message = json_object_has_member(obj, "message")
            ? json_object_get_object_member(obj, "message") : NULL;
        if (!message || !json_object_has_member(message, "content")) goto done;

        JsonArray *content = json_object_get_array_member(message, "content");
        guint len = json_array_get_length(content);

        for (guint i = 0; i < len; i++) {
            JsonObject *block = json_array_get_object_element(content, i);
            const gchar *block_type = json_object_has_member(block, "type")
                ? json_object_get_string_member(block, "type") : "";

            if (g_strcmp0(block_type, "tool_result") != 0)
                continue;

            const gchar *tool_use_id = json_object_has_member(block, "tool_use_id")
                ? json_object_get_string_member(block, "tool_use_id") : "";
            gboolean is_error = json_object_has_member(block, "is_error")
                ? json_object_get_boolean_member(block, "is_error") : FALSE;

            /* Extract result text — can be a string or array of content blocks */
            GString *result_text = g_string_new("");
            if (json_object_has_member(block, "content")) {
                JsonNode *cnode = json_object_get_member(block, "content");
                if (JSON_NODE_HOLDS_VALUE(cnode)) {
                    g_string_append(result_text,
                        json_node_get_string(cnode));
                } else if (JSON_NODE_HOLDS_ARRAY(cnode)) {
                    JsonArray *carr = json_node_get_array(cnode);
                    guint clen = json_array_get_length(carr);
                    for (guint ci = 0; ci < clen; ci++) {
                        JsonObject *citem = json_array_get_object_element(carr, ci);
                        const gchar *ctype = json_object_has_member(citem, "type")
                            ? json_object_get_string_member(citem, "type") : "";
                        if (g_strcmp0(ctype, "text") == 0 &&
                            json_object_has_member(citem, "text")) {
                            g_string_append(result_text,
                                json_object_get_string_member(citem, "text"));
                        }
                    }
                }
            }

            DBG("Tool result: %s (error=%d, %lu bytes)",
                tool_use_id, is_error, (unsigned long)result_text->len);

            if (session->tool_call_cb && tool_use_id[0] != '\0') {
                session->tool_call_cb(
                    session->current_msg_id ? session->current_msg_id : "",
                    tool_use_id, is_error ? "(error)" : "(result)",
                    "", result_text->str,
                    session->tool_call_data);
            }

            g_string_free(result_text, TRUE);
        }

    /* stream_event — ignored since --include-partial-messages gives us
     * the full accumulated text in assistant events already */

    } else if (g_strcmp0(type, "control_request") == 0) {
        /* Permission prompt from claude */
        const gchar *request_id = json_object_has_member(obj, "request_id")
            ? json_object_get_string_member(obj, "request_id") : NULL;
        if (!request_id) goto done;

        JsonObject *request = json_object_has_member(obj, "request")
            ? json_object_get_object_member(obj, "request") : NULL;
        if (!request) goto done;

        const gchar *subtype = json_object_has_member(request, "subtype")
            ? json_object_get_string_member(request, "subtype") : "";
        if (g_strcmp0(subtype, "can_use_tool") != 0) {
            DBG("Unknown control_request subtype: %s", subtype);
            goto done;
        }

        const gchar *tool_name = json_object_has_member(request, "tool_name")
            ? json_object_get_string_member(request, "tool_name") : "unknown";
        const gchar *description = json_object_has_member(request, "description")
            ? json_object_get_string_member(request, "description") : "";

        /* Store the full request object for the response (we need both
         * input and permission_suggestions when responding) */
        {
            JsonGenerator *ig = json_generator_new();
            JsonNode *req_node = json_node_new(JSON_NODE_OBJECT);
            json_node_set_object(req_node, request);
            json_generator_set_root(ig, req_node);
            gchar *request_json = json_generator_to_data(ig, NULL);
            g_hash_table_insert(session->pending_permissions,
                                g_strdup(request_id), request_json);
            json_node_free(req_node);
            g_object_unref(ig);
        }

        /* Build options JSON: [{"id":"allow","label":"Allow"},{"id":"deny","label":"Deny"}] */
        JsonBuilder *ob = json_builder_new();
        json_builder_begin_array(ob);

        json_builder_begin_object(ob);
        json_builder_set_member_name(ob, "id");
        json_builder_add_string_value(ob, "allow");
        json_builder_set_member_name(ob, "label");
        json_builder_add_string_value(ob, "Allow");
        json_builder_end_object(ob);

        /* Add permission suggestions with human-friendly labels */
        if (json_object_has_member(request, "permission_suggestions")) {
            JsonArray *suggestions = json_object_get_array_member(
                request, "permission_suggestions");
            guint slen = json_array_get_length(suggestions);
            for (guint si = 0; si < slen; si++) {
                JsonObject *s = json_array_get_object_element(suggestions, si);
                const gchar *slabel = json_object_has_member(s, "label")
                    ? json_object_get_string_member(s, "label") : NULL;
                const gchar *stype = json_object_has_member(s, "type")
                    ? json_object_get_string_member(s, "type") : "";

                gchar *opt_id = g_strdup_printf("suggestion_%u", si);
                gchar *label = NULL;

                if (slabel && strlen(slabel) > 0) {
                    label = g_strdup(slabel);
                } else if (g_strcmp0(stype, "setMode") == 0) {
                    const gchar *mode = json_object_has_member(s, "mode")
                        ? json_object_get_string_member(s, "mode") : "";
                    if (g_strcmp0(mode, "acceptEdits") == 0)
                        label = g_strdup("Allow + Accept Edits");
                    else if (g_strcmp0(mode, "plan") == 0)
                        label = g_strdup("Allow + Plan Mode");
                    else if (g_strcmp0(mode, "bypassPermissions") == 0)
                        label = g_strdup("Allow + Bypass Permissions");
                    else
                        label = g_strdup_printf("Allow + Set Mode: %s", mode);
                } else if (g_strcmp0(stype, "addRules") == 0) {
                    const gchar *behavior = json_object_has_member(s, "behavior")
                        ? json_object_get_string_member(s, "behavior") : "allow";
                    const gchar *dest = json_object_has_member(s, "destination")
                        ? json_object_get_string_member(s, "destination") : "";

                    /* Extract tool names from rules array */
                    GString *tools = g_string_new("");
                    if (json_object_has_member(s, "rules")) {
                        JsonArray *rules = json_object_get_array_member(s, "rules");
                        guint rlen = json_array_get_length(rules);
                        for (guint ri = 0; ri < rlen; ri++) {
                            JsonObject *r = json_array_get_object_element(rules, ri);
                            const gchar *tn = json_object_has_member(r, "toolName")
                                ? json_object_get_string_member(r, "toolName") : NULL;
                            if (tn) {
                                /* Clean MCP names: mcp__x__y -> x y */
                                if (g_str_has_prefix(tn, "mcp__")) {
                                    gchar *clean = g_strdup(tn + 5);
                                    gchar *p;
                                    while ((p = strstr(clean, "__")) != NULL) {
                                        *p = ' ';
                                        memmove(p + 1, p + 2, strlen(p + 2) + 1);
                                    }
                                    if (tools->len > 0) g_string_append(tools, ", ");
                                    g_string_append(tools, clean);
                                    g_free(clean);
                                } else {
                                    if (tools->len > 0) g_string_append(tools, ", ");
                                    g_string_append(tools, tn);
                                }
                            }
                        }
                    }

                    const gchar *scope = "";
                    if (g_strcmp0(dest, "localSettings") == 0)
                        scope = "in project";
                    else if (g_strcmp0(dest, "session") == 0)
                        scope = "for session";
                    else if (dest && strlen(dest) > 0)
                        scope = dest;

                    if (g_strcmp0(behavior, "allow") == 0 && tools->len > 0)
                        label = g_strdup_printf("Allow %s %s", tools->str, scope);
                    else if (g_strcmp0(behavior, "allow") == 0)
                        label = g_strdup_printf("Allow %s", scope);
                    else
                        label = g_strdup_printf("%s %s %s", behavior,
                                                tools->str, scope);

                    /* Trim trailing space */
                    g_strstrip(label);
                    g_string_free(tools, TRUE);
                } else {
                    label = g_strdup(stype);
                }

                json_builder_begin_object(ob);
                json_builder_set_member_name(ob, "id");
                json_builder_add_string_value(ob, opt_id);
                json_builder_set_member_name(ob, "label");
                json_builder_add_string_value(ob, label);
                json_builder_end_object(ob);

                g_free(opt_id);
                g_free(label);
            }
        }

        json_builder_begin_object(ob);
        json_builder_set_member_name(ob, "id");
        json_builder_add_string_value(ob, "deny");
        json_builder_set_member_name(ob, "label");
        json_builder_add_string_value(ob, "Deny");
        json_builder_end_object(ob);

        json_builder_end_array(ob);

        JsonGenerator *og = json_generator_new();
        json_generator_set_root(og, json_builder_get_root(ob));
        gchar *options_json = json_generator_to_data(og, NULL);
        g_object_unref(og);
        g_object_unref(ob);

        DBG("Permission request: %s for tool %s", request_id, tool_name);

        if (session->permission_cb)
            session->permission_cb(request_id, tool_name, description,
                                   options_json, session->permission_data);

        g_free(options_json);

    } else if (g_strcmp0(type, "control_response") == 0) {
        /* Response to a control_request (e.g., initialize handshake) */
        JsonObject *outer = json_object_has_member(obj, "response")
            ? json_object_get_object_member(obj, "response") : NULL;
        if (!outer) goto done;

        const gchar *resp_id = json_object_has_member(outer, "request_id")
            ? json_object_get_string_member(outer, "request_id") : "";
        JsonObject *inner = json_object_has_member(outer, "response")
            ? json_object_get_object_member(outer, "response") : NULL;

        /* Check if this is our initialize handshake response */
        if (session->init_request_id &&
            g_strcmp0(resp_id, session->init_request_id) == 0 && inner) {

            g_free(session->init_request_id);
            session->init_request_id = NULL;

            /* Extract slash commands */
            if (json_object_has_member(inner, "commands")) {
                JsonArray *cmds = json_object_get_array_member(inner, "commands");
                guint clen = json_array_get_length(cmds);
                DBG("Initialize: %u slash commands", clen);
                for (guint ci = 0; ci < clen; ci++) {
                    JsonObject *cmd = json_array_get_object_element(cmds, ci);
                    const gchar *name = json_object_has_member(cmd, "name")
                        ? json_object_get_string_member(cmd, "name") : "";
                    const gchar *desc = json_object_has_member(cmd, "description")
                        ? json_object_get_string_member(cmd, "description") : "";
                    DBG("  /%s - %s", name, desc);
                }

                /* Pass commands JSON to callback */
                if (session->commands_cb) {
                    JsonGenerator *cg = json_generator_new();
                    JsonNode *cnode = json_node_new(JSON_NODE_ARRAY);
                    json_node_set_array(cnode, cmds);
                    json_generator_set_root(cg, cnode);
                    gchar *cjson = json_generator_to_data(cg, NULL);
                    session->commands_cb(cjson, session->commands_data);
                    g_free(cjson);
                    json_node_free(cnode);
                    g_object_unref(cg);
                }
            }

            /* Extract available models
             * Format: {"value":"...", "displayName":"...", "description":"..."} */
            if (json_object_has_member(inner, "models")) {
                JsonArray *models = json_object_get_array_member(inner, "models");
                guint mlen = json_array_get_length(models);
                DBG("Initialize: %u models available", mlen);
                for (guint mi = 0; mi < mlen; mi++) {
                    JsonObject *m = json_array_get_object_element(models, mi);
                    const gchar *val = json_object_has_member(m, "value")
                        ? json_object_get_string_member(m, "value") : "";
                    const gchar *display = json_object_has_member(m, "displayName")
                        ? json_object_get_string_member(m, "displayName") : val;
                    DBG("  model: %s (%s)", display, val);
                }

                /* Pass models JSON to callback */
                if (session->models_cb) {
                    JsonGenerator *mg = json_generator_new();
                    JsonNode *mnode = json_node_new(JSON_NODE_ARRAY);
                    json_node_set_array(mnode, models);
                    json_generator_set_root(mg, mnode);
                    gchar *mjson = json_generator_to_data(mg, NULL);
                    session->models_cb(mjson, session->models_data);
                    g_free(mjson);
                    json_node_free(mnode);
                    g_object_unref(mg);
                }
            }

            /* Extract account info */
            if (json_object_has_member(inner, "account")) {
                JsonObject *acct = json_object_get_object_member(inner, "account");
                const gchar *email = json_object_has_member(acct, "email")
                    ? json_object_get_string_member(acct, "email") : "";
                const gchar *sub = json_object_has_member(acct, "subscriptionType")
                    ? json_object_get_string_member(acct, "subscriptionType") : "";
                DBG("Initialize: account %s (%s)", email, sub);
            }
        } else if (session->mcp_status_request_id &&
                   g_strcmp0(resp_id, session->mcp_status_request_id) == 0) {
            g_free(session->mcp_status_request_id);
            session->mcp_status_request_id = NULL;

            /* Response may be a direct array or {mcpServers: [...]} */
            if (session->mcp_status_cb &&
                json_object_has_member(outer, "response")) {
                JsonNode *resp_node = json_object_get_member(outer, "response");
                JsonNode *arr_node = NULL;

                if (JSON_NODE_HOLDS_ARRAY(resp_node)) {
                    arr_node = resp_node;
                } else if (JSON_NODE_HOLDS_OBJECT(resp_node)) {
                    JsonObject *robj = json_node_get_object(resp_node);
                    if (json_object_has_member(robj, "mcpServers")) {
                        JsonNode *ms = json_object_get_member(robj, "mcpServers");
                        if (JSON_NODE_HOLDS_ARRAY(ms))
                            arr_node = ms;
                    }
                }

                if (arr_node) {
                    JsonGenerator *rg = json_generator_new();
                    json_generator_set_root(rg, arr_node);
                    gchar *rjson = json_generator_to_data(rg, NULL);
                    session->mcp_status_cb(rjson, session->mcp_status_data);
                    g_free(rjson);
                    g_object_unref(rg);
                } else {
                    DBG("mcp_status response: no server array found");
                }
            }
        } else {
            DBG("control_response for request: %s", resp_id);
        }

    } else if (g_strcmp0(type, "result") == 0) {
        /* Prompt complete */
        const gchar *subtype = json_object_has_member(obj, "subtype")
            ? json_object_get_string_member(obj, "subtype") : "";

        if (g_strcmp0(subtype, "error") == 0) {
            const gchar *err = json_object_has_member(obj, "error")
                ? json_object_get_string_member(obj, "error")
                : "Unknown error";
            if (session->error_cb)
                session->error_cb(err, session->error_data);
        }

        /* Finalize the streaming message */
        if (session->message_cb && session->current_msg_id)
            session->message_cb(session->current_msg_id, "assistant",
                               session->current_content->str, FALSE,
                               session->message_data);

        /* Clear for next turn */
        g_free(session->current_msg_id);
        session->current_msg_id = NULL;
        g_string_truncate(session->current_content, 0);

        DBG("Prompt complete (subtype=%s)", subtype);
    }
    /* rate_limit_event - ignored */

done:

    g_object_unref(parser);
}

/* ── Callback setters ────────────────────────────────────────────── */

void cli_session_set_message_callback(CLISession *session, CLIMessageCb cb,
                                      gpointer data)
{
    session->message_cb = cb;
    session->message_data = data;
}

void cli_session_set_tool_call_callback(CLISession *session, CLIToolCallCb cb,
                                        gpointer data)
{
    session->tool_call_cb = cb;
    session->tool_call_data = data;
}

void cli_session_set_permission_callback(CLISession *session, CLIPermissionCb cb,
                                         gpointer data)
{
    session->permission_cb = cb;
    session->permission_data = data;
}

void cli_session_set_init_callback(CLISession *session, CLIInitCb cb,
                                   gpointer data)
{
    session->init_cb = cb;
    session->init_data = data;
}

void cli_session_set_models_callback(CLISession *session, CLIModelsCb cb,
                                     gpointer data)
{
    session->models_cb = cb;
    session->models_data = data;
}

void cli_session_set_commands_callback(CLISession *session, CLICommandsCb cb,
                                       gpointer data)
{
    session->commands_cb = cb;
    session->commands_data = data;
}

void cli_session_set_todos_callback(CLISession *session, CLITodosCb cb,
                                    gpointer data)
{
    session->todos_cb = cb;
    session->todos_data = data;
}

void cli_session_set_thinking_callback(CLISession *session, CLIThinkingCb cb,
                                       gpointer data)
{
    session->thinking_cb = cb;
    session->thinking_data = data;
}

void cli_session_set_mcp_status_callback(CLISession *session, CLIMcpStatusCb cb,
                                          gpointer data)
{
    session->mcp_status_cb = cb;
    session->mcp_status_data = data;
}

void cli_session_query_mcp_status(CLISession *session)
{
    if (!session->running || !session->stdin_pipe)
        return;

    g_free(session->mcp_status_request_id);
    session->mcp_status_request_id = g_strdup_printf("mcp_status_%ld",
        g_get_monotonic_time());

    gchar *json = g_strdup_printf(
        "{\"type\":\"control_request\","
        "\"request_id\":\"%s\","
        "\"request\":{\"subtype\":\"mcp_status\"}}\n",
        session->mcp_status_request_id);

    g_output_stream_write_all(session->stdin_pipe,
                              json, strlen(json), NULL, NULL, NULL);
    g_free(json);
}

void cli_session_mcp_toggle(CLISession *session, const gchar *server_name,
                             gboolean enabled)
{
    if (!session->running || !session->stdin_pipe || !server_name)
        return;

    DBG("MCP toggle: %s -> %s", server_name, enabled ? "enabled" : "disabled");

    gchar *json = g_strdup_printf(
        "{\"type\":\"control_request\","
        "\"request_id\":\"mcp_toggle_%ld\","
        "\"request\":{\"subtype\":\"mcp_toggle\","
        "\"serverName\":\"%s\",\"enabled\":%s}}\n",
        g_get_monotonic_time(), server_name, enabled ? "true" : "false");

    g_output_stream_write_all(session->stdin_pipe,
                              json, strlen(json), NULL, NULL, NULL);
    g_free(json);
}

void cli_session_mcp_reconnect(CLISession *session, const gchar *server_name)
{
    if (!session->running || !session->stdin_pipe || !server_name)
        return;

    DBG("MCP reconnect: %s", server_name);

    gchar *json = g_strdup_printf(
        "{\"type\":\"control_request\","
        "\"request_id\":\"mcp_reconnect_%ld\","
        "\"request\":{\"subtype\":\"mcp_reconnect\","
        "\"serverName\":\"%s\"}}\n",
        g_get_monotonic_time(), server_name);

    g_output_stream_write_all(session->stdin_pipe,
                              json, strlen(json), NULL, NULL, NULL);
    g_free(json);
}

void cli_session_set_error_callback(CLISession *session, CLIErrorCb cb,
                                    gpointer data)
{
    session->error_cb = cb;
    session->error_data = data;
}

void cli_session_set_finished_callback(CLISession *session, CLIFinishedCb cb,
                                       gpointer data)
{
    session->finished_cb = cb;
    session->finished_data = data;
}
