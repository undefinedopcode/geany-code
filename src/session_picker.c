#include "session_picker.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <glib/gstdio.h>

#define MAX_SESSIONS      15
#define MAX_PARSE_LINES   50
#define MAX_PREVIEW_CHARS 2000

/* ── Helpers ─────────────────────────────────────────────────────── */

void session_info_free(SessionInfo *info)
{
    if (!info) return;
    g_free(info->session_id);
    g_free(info->slug);
    g_free(info->timestamp);
    g_free(info->first_message);
    g_free(info);
}

void session_info_list_free(GList *list)
{
    g_list_free_full(list, (GDestroyNotify)session_info_free);
}

/* Build project key: /home/user/proj -> -home-user-proj */
static gchar *project_key_from_path(const gchar *path)
{
    gchar *key = g_strdup(path);
    for (gchar *p = key; *p; p++) {
        if (*p == '/')
            *p = '-';
    }
    return key;
}

/* ── JSONL Parsing ───────────────────────────────────────────────── */

static void parse_session_jsonl(const gchar *filepath, SessionInfo *info)
{
    GError *error = NULL;
    GFile *file = g_file_new_for_path(filepath);
    GFileInputStream *fis = g_file_read(file, NULL, &error);
    g_object_unref(file);

    if (!fis) {
        if (error) g_error_free(error);
        return;
    }

    GDataInputStream *dis = g_data_input_stream_new(G_INPUT_STREAM(fis));
    JsonParser *parser = json_parser_new();

    gint line_count = 0;
    while (line_count < MAX_PARSE_LINES) {
        gsize length;
        gchar *line = g_data_input_stream_read_line(dis, &length, NULL, &error);
        if (!line) break;
        line_count++;

        if (length == 0 || line[0] != '{') {
            g_free(line);
            continue;
        }

        if (!json_parser_load_from_data(parser, line, length, NULL)) {
            g_free(line);
            continue;
        }

        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
            g_free(line);
            continue;
        }

        JsonObject *obj = json_node_get_object(root);

        /* Extract slug */
        if (!info->slug && json_object_has_member(obj, "slug")) {
            const gchar *s = json_object_get_string_member(obj, "slug");
            if (s && *s)
                info->slug = g_strdup(s);
        }

        /* Extract timestamp */
        if (!info->timestamp && json_object_has_member(obj, "timestamp")) {
            const gchar *ts = json_object_get_string_member(obj, "timestamp");
            if (ts && *ts)
                info->timestamp = g_strdup(ts);
        }

        /* Extract first external user message */
        if (!info->first_message &&
            json_object_has_member(obj, "type") &&
            json_object_has_member(obj, "userType")) {

            const gchar *type = json_object_get_string_member(obj, "type");
            const gchar *utype = json_object_get_string_member(obj, "userType");

            if (g_strcmp0(type, "user") == 0 &&
                g_strcmp0(utype, "external") == 0 &&
                json_object_has_member(obj, "message")) {

                JsonObject *msg = json_object_get_object_member(obj, "message");
                if (msg && json_object_has_member(msg, "content")) {
                    JsonNode *content_node = json_object_get_member(msg, "content");

                    if (JSON_NODE_HOLDS_VALUE(content_node)) {
                        const gchar *text = json_node_get_string(content_node);
                        if (text) {
                            glong len = g_utf8_strlen(text, -1);
                            if (len > MAX_PREVIEW_CHARS) {
                                gchar *end = g_utf8_offset_to_pointer(text, MAX_PREVIEW_CHARS);
                                info->first_message = g_strndup(text, end - text);
                            } else {
                                info->first_message = g_strdup(text);
                            }
                        }
                    } else if (JSON_NODE_HOLDS_ARRAY(content_node)) {
                        JsonArray *arr = json_node_get_array(content_node);
                        guint n = json_array_get_length(arr);
                        for (guint i = 0; i < n; i++) {
                            JsonNode *el = json_array_get_element(arr, i);
                            if (!JSON_NODE_HOLDS_OBJECT(el)) continue;
                            JsonObject *block = json_node_get_object(el);
                            if (!json_object_has_member(block, "type")) continue;
                            const gchar *btype = json_object_get_string_member(block, "type");
                            if (g_strcmp0(btype, "text") == 0 &&
                                json_object_has_member(block, "text")) {
                                const gchar *text = json_object_get_string_member(block, "text");
                                if (text) {
                                    glong len = g_utf8_strlen(text, -1);
                                    if (len > MAX_PREVIEW_CHARS) {
                                        gchar *end = g_utf8_offset_to_pointer(text, MAX_PREVIEW_CHARS);
                                        info->first_message = g_strndup(text, end - text);
                                    } else {
                                        info->first_message = g_strdup(text);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        g_free(line);

        /* Stop early if we have everything */
        if (info->timestamp && info->first_message)
            break;
    }

    g_object_unref(parser);
    g_object_unref(dis);
    g_object_unref(fis);
}

/* ── Sorting helper for filenames by mtime ───────────────────────── */

typedef struct {
    gchar  *filepath;
    gchar  *basename;
    gint64  mtime;
} FileEntry;

static gint cmp_file_mtime_desc(gconstpointer a, gconstpointer b)
{
    const FileEntry *fa = *(const FileEntry *const *)a;
    const FileEntry *fb = *(const FileEntry *const *)b;
    if (fb->mtime > fa->mtime) return 1;
    if (fb->mtime < fa->mtime) return -1;
    return 0;
}

/* ── Discovery ───────────────────────────────────────────────────── */

GList *session_discover(const gchar *working_dir)
{
    if (!working_dir) return NULL;

    gchar *key = project_key_from_path(working_dir);
    gchar *dir_path = g_build_filename(g_get_home_dir(), ".claude", "projects", key, NULL);
    g_free(key);

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        g_free(dir_path);
        return NULL;
    }

    /* Collect .jsonl files with their mtime */
    GPtrArray *entries = g_ptr_array_new();
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".jsonl"))
            continue;

        gchar *filepath = g_build_filename(dir_path, name, NULL);
        GStatBuf st;
        if (g_stat(filepath, &st) != 0) {
            g_free(filepath);
            continue;
        }

        FileEntry *fe = g_new0(FileEntry, 1);
        fe->filepath = filepath;
        fe->basename = g_strdup(name);
        fe->mtime = st.st_mtime;
        g_ptr_array_add(entries, fe);
    }
    g_dir_close(dir);
    g_free(dir_path);

    /* Sort newest first */
    g_ptr_array_sort(entries, cmp_file_mtime_desc);

    /* Parse top N entries */
    GList *sessions = NULL;
    guint limit = MIN(entries->len, MAX_SESSIONS);
    for (guint i = 0; i < limit; i++) {
        FileEntry *fe = g_ptr_array_index(entries, i);

        SessionInfo *info = g_new0(SessionInfo, 1);

        /* Session ID = filename without .jsonl */
        gchar *dot = g_strrstr(fe->basename, ".jsonl");
        if (dot)
            info->session_id = g_strndup(fe->basename, dot - fe->basename);
        else
            info->session_id = g_strdup(fe->basename);

        info->mtime = fe->mtime;
        parse_session_jsonl(fe->filepath, info);

        /* Skip entries without a timestamp */
        if (info->timestamp) {
            sessions = g_list_append(sessions, info);
        } else {
            session_info_free(info);
        }
    }

    /* Free file entries */
    for (guint i = 0; i < entries->len; i++) {
        FileEntry *fe = g_ptr_array_index(entries, i);
        g_free(fe->filepath);
        g_free(fe->basename);
        g_free(fe);
    }
    g_ptr_array_free(entries, TRUE);

    return sessions;
}

/* ── Session history loading ─────────────────────────────────────── */

static void history_tool_call_free(gpointer p)
{
    HistoryToolCall *tc = p;
    if (!tc) return;
    g_free(tc->tool_id);
    g_free(tc->tool_name);
    g_free(tc->input_json);
    g_free(tc);
}

void history_message_free(HistoryMessage *msg)
{
    if (!msg) return;
    g_free(msg->uuid);
    g_free(msg->role);
    g_free(msg->content);
    g_free(msg->timestamp);
    g_list_free_full(msg->tool_calls, history_tool_call_free);
    g_free(msg);
}

void history_message_list_free(GList *list)
{
    g_list_free_full(list, (GDestroyNotify)history_message_free);
}

/* Extract concatenated text from a message object's content field.
 * Handles both string and array-of-blocks formats.
 * Caller must g_free() the result. Returns NULL if no text found. */
static gchar *extract_text_content(JsonObject *msg)
{
    if (!msg || !json_object_has_member(msg, "content"))
        return NULL;

    JsonNode *content_node = json_object_get_member(msg, "content");

    /* Simple string content */
    if (JSON_NODE_HOLDS_VALUE(content_node)) {
        const gchar *text = json_node_get_string(content_node);
        return (text && *text) ? g_strdup(text) : NULL;
    }

    /* Array of content blocks — concatenate text blocks */
    if (JSON_NODE_HOLDS_ARRAY(content_node)) {
        JsonArray *arr = json_node_get_array(content_node);
        guint n = json_array_get_length(arr);
        GString *result = g_string_new("");

        for (guint i = 0; i < n; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (!JSON_NODE_HOLDS_OBJECT(el)) continue;
            JsonObject *block = json_node_get_object(el);
            if (!json_object_has_member(block, "type")) continue;

            const gchar *btype = json_object_get_string_member(block, "type");
            if (g_strcmp0(btype, "text") != 0) continue;
            if (!json_object_has_member(block, "text")) continue;

            const gchar *text = json_object_get_string_member(block, "text");
            if (text && *text) {
                if (result->len > 0)
                    g_string_append_c(result, '\n');
                g_string_append(result, text);
            }
        }

        if (result->len > 0)
            return g_string_free(result, FALSE);
        g_string_free(result, TRUE);
        return NULL;
    }

    return NULL;
}

/* Extract tool_use blocks from a message's content array.
 * Returns a GList of HistoryToolCall* (caller frees). */
static GList *extract_tool_calls(JsonObject *msg)
{
    if (!msg || !json_object_has_member(msg, "content"))
        return NULL;

    JsonNode *content_node = json_object_get_member(msg, "content");
    if (!JSON_NODE_HOLDS_ARRAY(content_node))
        return NULL;

    JsonArray *arr = json_node_get_array(content_node);
    guint n = json_array_get_length(arr);
    GList *result = NULL;

    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(arr, i);
        if (!JSON_NODE_HOLDS_OBJECT(el)) continue;
        JsonObject *block = json_node_get_object(el);
        if (!json_object_has_member(block, "type")) continue;

        const gchar *btype = json_object_get_string_member(block, "type");
        if (g_strcmp0(btype, "tool_use") != 0) continue;

        const gchar *tid = json_object_has_member(block, "id")
            ? json_object_get_string_member(block, "id") : "";
        const gchar *tname = json_object_has_member(block, "name")
            ? json_object_get_string_member(block, "name") : "unknown";

        gchar *input_str = NULL;
        if (json_object_has_member(block, "input")) {
            JsonGenerator *gen = json_generator_new();
            json_generator_set_root(gen,
                json_object_get_member(block, "input"));
            input_str = json_generator_to_data(gen, NULL);
            g_object_unref(gen);
        }

        HistoryToolCall *tc = g_new0(HistoryToolCall, 1);
        tc->tool_id = g_strdup(tid);
        tc->tool_name = g_strdup(tname);
        tc->input_json = input_str ? input_str : g_strdup("");
        result = g_list_append(result, tc);
    }

    return result;
}

GList *session_load_history(const gchar *working_dir,
                            const gchar *session_id,
                            guint max_messages)
{
    if (!working_dir || !session_id || max_messages == 0)
        return NULL;

    /* Build path: ~/.claude/projects/{key}/{session_id}.jsonl */
    gchar *key = project_key_from_path(working_dir);
    gchar *filename = g_strdup_printf("%s.jsonl", session_id);
    gchar *filepath = g_build_filename(g_get_home_dir(), ".claude", "projects",
                                       key, filename, NULL);
    g_free(key);
    g_free(filename);

    /* Read entire file */
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(filepath, &contents, &length, NULL)) {
        g_free(filepath);
        return NULL;
    }
    g_free(filepath);

    /* Split into lines and parse each */
    gchar **lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    GPtrArray *msgs = g_ptr_array_new();
    JsonParser *parser = json_parser_new();
    guint line_idx = 0;

    for (gchar **lp = lines; *lp; lp++, line_idx++) {
        const gchar *line = *lp;
        if (!line[0] || line[0] != '{')
            continue;

        if (!json_parser_load_from_data(parser, line, -1, NULL))
            continue;

        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root))
            continue;

        JsonObject *obj = json_node_get_object(root);
        if (!json_object_has_member(obj, "type"))
            continue;

        const gchar *type = json_object_get_string_member(obj, "type");
        const gchar *role = NULL;

        if (g_strcmp0(type, "user") == 0) {
            /* Only external user messages (not tool results) */
            if (!json_object_has_member(obj, "userType"))
                continue;
            const gchar *utype = json_object_get_string_member(obj, "userType");
            if (g_strcmp0(utype, "external") != 0)
                continue;
            role = "user";
        } else if (g_strcmp0(type, "assistant") == 0) {
            role = "assistant";
        } else {
            continue;
        }

        if (!json_object_has_member(obj, "message"))
            continue;

        JsonObject *msg = json_object_get_object_member(obj, "message");
        gchar *text = extract_text_content(msg);
        GList *tools = (g_strcmp0(role, "assistant") == 0)
            ? extract_tool_calls(msg) : NULL;

        /* Skip if no text and no tool calls */
        if (!text && !tools)
            continue;

        /* Extract UUID */
        gchar *uuid = NULL;
        if (json_object_has_member(obj, "uuid")) {
            const gchar *u = json_object_get_string_member(obj, "uuid");
            if (u && *u)
                uuid = g_strdup(u);
        }
        if (!uuid)
            uuid = g_strdup_printf("line_%u", line_idx);

        /* Extract timestamp */
        gchar *timestamp = NULL;
        if (json_object_has_member(obj, "timestamp")) {
            const gchar *ts = json_object_get_string_member(obj, "timestamp");
            if (ts && *ts)
                timestamp = g_strdup(ts);
        }

        HistoryMessage *hm = g_new0(HistoryMessage, 1);
        hm->uuid = uuid;
        hm->role = g_strdup(role);
        hm->content = text;
        hm->timestamp = timestamp;
        hm->tool_calls = tools;
        g_ptr_array_add(msgs, hm);
    }

    g_strfreev(lines);
    g_object_unref(parser);

    /* Take only the last max_messages entries */
    GList *result = NULL;
    guint start = (msgs->len > max_messages) ? msgs->len - max_messages : 0;
    for (guint i = start; i < msgs->len; i++)
        result = g_list_append(result, g_ptr_array_index(msgs, i));

    /* Free skipped entries */
    for (guint i = 0; i < start; i++)
        history_message_free(g_ptr_array_index(msgs, i));

    g_ptr_array_free(msgs, TRUE);
    return result;
}

/* ── Dialog ──────────────────────────────────────────────────────── */

/* Helper: build the combo display string for a session */
static gchar *build_combo_label(SessionInfo *info)
{
    GString *label = g_string_new("");

    /* Label: prefer slug, then first_message preview, then session ID */
    if (info->slug && *info->slug) {
        glong len = g_utf8_strlen(info->slug, -1);
        if (len > 50) {
            gchar *end = g_utf8_offset_to_pointer(info->slug, 50);
            g_string_append_len(label, info->slug, end - info->slug);
            g_string_append(label, "...");
        } else {
            g_string_append(label, info->slug);
        }
    } else if (info->first_message && *info->first_message) {
        /* First line only, truncated to 50 chars */
        const gchar *nl = strchr(info->first_message, '\n');
        glong avail;
        if (nl)
            avail = nl - info->first_message;
        else
            avail = strlen(info->first_message);
        if (avail > 50) avail = 50;
        g_string_append_len(label, info->first_message, avail);
        if ((glong)strlen(info->first_message) > avail)
            g_string_append(label, "...");
    } else {
        /* Fallback: truncated session ID */
        g_string_append_len(label, info->session_id,
                            MIN(12, (glong)strlen(info->session_id)));
        g_string_append(label, "...");
    }

    /* Append formatted date from mtime */
    GDateTime *dt = g_date_time_new_from_unix_local(info->mtime);
    if (dt) {
        gchar *fmt = g_date_time_format(dt, "%Y-%m-%d %H:%M");
        g_string_append_printf(label, "  |  %s", fmt);
        g_free(fmt);
        g_date_time_unref(dt);
    }

    return g_string_free(label, FALSE);
}

/* Callback: update the first-message text view when combo selection changes */
static void on_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(user_data);
    gint idx = gtk_combo_box_get_active(combo);
    GList *sessions = g_object_get_data(G_OBJECT(combo), "sessions");

    if (idx < 0 || !sessions) {
        gtk_text_buffer_set_text(buf, "", -1);
        return;
    }

    SessionInfo *info = g_list_nth_data(sessions, idx);
    if (!info) {
        gtk_text_buffer_set_text(buf, "", -1);
        return;
    }

    /* Build the preview: session ID, date, then the full first message */
    GString *preview = g_string_new("");

    g_string_append_printf(preview, "ID: %s\n", info->session_id);

    if (info->timestamp) {
        GDateTime *dt = g_date_time_new_from_iso8601(info->timestamp, NULL);
        if (dt) {
            GDateTime *local = g_date_time_to_local(dt);
            gchar *fmt = g_date_time_format(local, "%Y-%m-%d %H:%M:%S");
            g_string_append_printf(preview, "Date: %s\n", fmt);
            g_free(fmt);
            g_date_time_unref(local);
            g_date_time_unref(dt);
        }
    }

    if (info->first_message && *info->first_message) {
        g_string_append_c(preview, '\n');
        g_string_append(preview, info->first_message);
    }

    gtk_text_buffer_set_text(buf, preview->str, -1);
    g_string_free(preview, TRUE);
}

/* Callback: toggle sensitivity of combo when radio buttons change */
static void on_radio_toggled(GtkToggleButton *toggle, gpointer user_data)
{
    GtkWidget *combo = GTK_WIDGET(user_data);
    gtk_widget_set_sensitive(combo, gtk_toggle_button_get_active(toggle));
}

gchar *session_picker_run(GtkWindow *parent, const gchar *working_dir)
{
    GList *sessions = session_discover(working_dir);
    if (!sessions) {
        GtkWidget *msg = gtk_message_dialog_new(
            parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "No previous sessions found.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        return NULL;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Resume Session", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Continue", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 420);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 6);

    /* Description label */
    GtkWidget *desc = gtk_label_new("Previous Claude sessions found for this project.");
    gtk_widget_set_halign(desc, GTK_ALIGN_START);
    gtk_widget_set_margin_start(desc, 12);
    gtk_widget_set_margin_top(desc, 8);
    gtk_box_pack_start(GTK_BOX(content), desc, FALSE, FALSE, 0);

    /* ── Radio: Resume session ── */
    GtkWidget *radio_resume = gtk_radio_button_new_with_label(NULL, "Resume session:");
    gtk_widget_set_margin_start(radio_resume, 12);
    gtk_box_pack_start(GTK_BOX(content), radio_resume, FALSE, FALSE, 0);

    /* Session combo box */
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_widget_set_margin_start(combo, 32);
    gtk_widget_set_margin_end(combo, 12);

    for (GList *l = sessions; l; l = l->next) {
        SessionInfo *info = l->data;
        gchar *label = build_combo_label(info);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), label);
        g_free(label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(content), combo, FALSE, FALSE, 0);

    /* First Message label */
    GtkWidget *msg_label = gtk_label_new("First Message");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(msg_label), attrs);
    pango_attr_list_unref(attrs);
    gtk_widget_set_margin_start(msg_label, 12);
    gtk_widget_set_margin_top(msg_label, 4);
    gtk_box_pack_start(GTK_BOX(content), msg_label, FALSE, FALSE, 0);

    /* First Message text view in a scrolled window */
    GtkWidget *textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textview), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textview), 6);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), textview);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    /* ── Radio: Start new session ── */
    GtkWidget *radio_new = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(radio_resume), "Start new session");
    gtk_widget_set_margin_start(radio_new, 12);
    gtk_widget_set_margin_bottom(radio_new, 8);
    gtk_box_pack_start(GTK_BOX(content), radio_new, FALSE, FALSE, 0);

    /* Wire combo ↔ text view updates */
    g_object_set_data(G_OBJECT(combo), "sessions", sessions);
    g_signal_connect(combo, "changed", G_CALLBACK(on_combo_changed), buf);

    /* Wire radio ↔ combo sensitivity */
    g_signal_connect(radio_resume, "toggled",
                     G_CALLBACK(on_radio_toggled), combo);

    /* Populate initial first-message preview */
    on_combo_changed(GTK_COMBO_BOX(combo), buf);

    gtk_widget_show_all(dialog);

    /* Run dialog */
    gchar *result = NULL;
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_ACCEPT) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_resume))) {
            gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            if (idx >= 0) {
                SessionInfo *info = g_list_nth_data(sessions, idx);
                if (info)
                    result = g_strdup(info->session_id);
            }
        } else {
            /* "Start new session" selected */
            result = g_strdup("");
        }
    }

    gtk_widget_destroy(dialog);
    session_info_list_free(sessions);
    return result;
}
