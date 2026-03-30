#include "chat_widget.h"
#include "chat_webview.h"
#include "chat_input.h"
#include "cli_session.h"
#include "editor_bridge.h"
#include "session_picker.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* Per-file edit statistics */
typedef struct {
    gchar *file_path;
    gint   lines_added;
    gint   lines_removed;
} FileEditStat;

static void free_file_edit_stat(gpointer p)
{
    FileEditStat *s = p;
    g_free(s->file_path);
    g_free(s);
}

typedef struct {
    GtkWidget   *container;  /* top-level vbox, for finding parent window */
    GtkWidget   *webview;
    GtkWidget   *input;
    CLISession  *session;
    guint        msg_counter;
    GHashTable  *known_msg_ids;  /* tracks which msg IDs have been added to webview */

    /* Edit tracking */
    GHashTable  *edit_stats;     /* file_path -> FileEditStat* */
    GtkWidget   *edit_indicator; /* clickable button in header */
    GtkWidget   *edit_popover;   /* GtkPopover with file list */
    GtkWidget   *edit_list_box;  /* GtkListBox inside popover */

    /* MCP server status */
    GtkWidget   *mcp_indicator;  /* label button in header */
    GtkWidget   *mcp_popover;    /* GtkPopover with server list */
    GtkWidget   *mcp_list_box;   /* GtkListBox inside popover */
    gchar       *mcp_servers_json; /* cached latest status JSON */
} ChatWidgetPrivate;

static const gchar *CHAT_PRIV_KEY = "geany-code-chat-private";

static ChatWidgetPrivate *get_priv(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), CHAT_PRIV_KEY);
}

/* ── Ensure session is running ───────────────────────────────────── */

static void ensure_session(ChatWidgetPrivate *priv)
{
    gchar *root = editor_bridge_get_project_root();

    /* Update file completion with current project root */
    chat_input_set_project_root(priv->input, root);

    if (cli_session_is_running(priv->session)) {
        /* Restart if the project root changed (e.g. project opened after
         * the eager start that fell back to $HOME) */
        const gchar *cur = cli_session_get_working_dir(priv->session);
        if (cur && root && g_strcmp0(cur, root) != 0) {
            cli_session_stop(priv->session);
            cli_session_start(priv->session, root);
        }
        g_free(root);
        return;
    }

    cli_session_start(priv->session, root);
    g_free(root);
}

/* ── Edit tracking ───────────────────────────────────────────────── */

static gint count_lines(const gchar *s)
{
    if (!s || *s == '\0') return 0;
    gint n = 1;
    for (const gchar *p = s; *p; p++)
        if (*p == '\n') n++;
    return n;
}

static void update_edit_indicator(ChatWidgetPrivate *priv);

static void track_edit(ChatWidgetPrivate *priv,
                       const gchar *tool_name,
                       const gchar *input_json)
{
    gboolean is_edit = tool_name &&
        (g_strcmp0(tool_name, "Edit") == 0 || strstr(tool_name, "edit") != NULL);
    gboolean is_write = tool_name &&
        (g_strcmp0(tool_name, "Write") == 0 ||
         (strstr(tool_name, "write") != NULL && !strstr(tool_name, "Todo")));
    if (!is_edit && !is_write) return;
    if (!input_json) return;

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, input_json, -1, NULL)) {
        g_object_unref(jp);
        return;
    }
    JsonNode *root = json_parser_get_root(jp);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(jp); return; }
    JsonObject *obj = json_node_get_object(root);

    const gchar *file_path = json_object_has_member(obj, "file_path")
        ? json_object_get_string_member(obj, "file_path") : NULL;
    if (!file_path) { g_object_unref(jp); return; }

    gint added = 0, removed = 0;

    if (is_edit) {
        const gchar *old_s = json_object_has_member(obj, "old_string")
            ? json_object_get_string_member(obj, "old_string")
            : (json_object_has_member(obj, "old_text")
               ? json_object_get_string_member(obj, "old_text") : NULL);
        const gchar *new_s = json_object_has_member(obj, "new_string")
            ? json_object_get_string_member(obj, "new_string")
            : (json_object_has_member(obj, "new_text")
               ? json_object_get_string_member(obj, "new_text") : NULL);
        removed = count_lines(old_s);
        added = count_lines(new_s);
    } else {
        const gchar *content = json_object_has_member(obj, "content")
            ? json_object_get_string_member(obj, "content") : NULL;
        added = count_lines(content);
    }

    FileEditStat *stat = g_hash_table_lookup(priv->edit_stats, file_path);
    if (!stat) {
        stat = g_new0(FileEditStat, 1);
        stat->file_path = g_strdup(file_path);
        g_hash_table_insert(priv->edit_stats, stat->file_path, stat);
    }
    stat->lines_added += added;
    stat->lines_removed += removed;

    g_object_unref(jp);
    update_edit_indicator(priv);
}

static void update_edit_indicator(ChatWidgetPrivate *priv)
{
    guint n_files = g_hash_table_size(priv->edit_stats);
    if (n_files == 0) {
        gtk_widget_hide(priv->edit_indicator);
        return;
    }

    gint total_added = 0, total_removed = 0;
    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init(&iter, priv->edit_stats);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        FileEditStat *s = val;
        total_added += s->lines_added;
        total_removed += s->lines_removed;
    }

    gchar *markup = g_markup_printf_escaped(
        "%u file%s  <span foreground=\"#4ec94e\">+%d</span>"
        " <span foreground=\"#e05050\">-%d</span>",
        n_files, n_files == 1 ? "" : "s",
        total_added, total_removed);

    GtkWidget *child = gtk_bin_get_child(GTK_BIN(priv->edit_indicator));
    gtk_label_set_markup(GTK_LABEL(child), markup);
    g_free(markup);

    gtk_widget_show(priv->edit_indicator);
}

static void on_edit_file_clicked(GtkButton *btn, gpointer user_data)
{
    (void)user_data;
    const gchar *file = g_object_get_data(G_OBJECT(btn), "file");
    if (file)
        editor_bridge_jump_to(file, 1, 1);
}

static void rebuild_edit_list(ChatWidgetPrivate *priv)
{
    GList *children = gtk_container_get_children(
        GTK_CONTAINER(priv->edit_list_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init(&iter, priv->edit_stats);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        FileEditStat *s = val;
        gchar *base = g_path_get_basename(s->file_path);

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row_box, 6);
        gtk_widget_set_margin_end(row_box, 6);
        gtk_widget_set_margin_top(row_box, 2);
        gtk_widget_set_margin_bottom(row_box, 2);

        GtkWidget *file_btn = gtk_button_new_with_label(base);
        gtk_button_set_relief(GTK_BUTTON(file_btn), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(file_btn, s->file_path);
        g_object_set_data_full(G_OBJECT(file_btn), "file",
            g_strdup(s->file_path), g_free);
        g_signal_connect(file_btn, "clicked",
            G_CALLBACK(on_edit_file_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(row_box), file_btn, TRUE, TRUE, 0);

        gchar *stat_markup = g_markup_printf_escaped(
            "<span foreground=\"#4ec94e\">+%d</span> "
            "<span foreground=\"#e05050\">-%d</span>",
            s->lines_added, s->lines_removed);
        GtkWidget *stat_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(stat_label), stat_markup);
        g_free(stat_markup);
        gtk_box_pack_end(GTK_BOX(row_box), stat_label, FALSE, FALSE, 0);

        g_free(base);
        gtk_list_box_insert(GTK_LIST_BOX(priv->edit_list_box), row_box, -1);
    }
}

static void on_edit_indicator_clicked(GtkButton *btn, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    if (!priv->edit_popover) {
        priv->edit_popover = gtk_popover_new(GTK_WIDGET(btn));
        gtk_popover_set_position(GTK_POPOVER(priv->edit_popover), GTK_POS_BOTTOM);

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, 300, -1);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(scroll), 250);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(scroll), TRUE);

        priv->edit_list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(priv->edit_list_box),
            GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), priv->edit_list_box);
        gtk_container_add(GTK_CONTAINER(priv->edit_popover), scroll);
    }

    rebuild_edit_list(priv);
    gtk_widget_show_all(priv->edit_popover);
    gtk_popover_popup(GTK_POPOVER(priv->edit_popover));
}

/* ── MCP server status ───────────────────────────────────────────── */

static void rebuild_mcp_list(ChatWidgetPrivate *priv);

static void update_mcp_indicator(ChatWidgetPrivate *priv)
{
    if (!priv->mcp_servers_json || priv->mcp_servers_json[0] == '\0') {
        gtk_widget_hide(priv->mcp_indicator);
        return;
    }

    /* Parse to count statuses */
    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, priv->mcp_servers_json, -1, NULL)) {
        g_object_unref(jp);
        gtk_widget_hide(priv->mcp_indicator);
        return;
    }
    JsonNode *root = json_parser_get_root(jp);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(jp);
        gtk_widget_hide(priv->mcp_indicator);
        return;
    }

    JsonArray *arr = json_node_get_array(root);
    guint n = json_array_get_length(arr);
    if (n == 0) {
        g_object_unref(jp);
        gtk_widget_hide(priv->mcp_indicator);
        return;
    }

    guint n_connected = 0, n_failed = 0, n_disabled = 0, n_other = 0;
    for (guint i = 0; i < n; i++) {
        JsonObject *srv = json_array_get_object_element(arr, i);
        const gchar *status = json_object_has_member(srv, "status")
            ? json_object_get_string_member(srv, "status") : "";
        if (g_strcmp0(status, "connected") == 0) n_connected++;
        else if (g_strcmp0(status, "failed") == 0) n_failed++;
        else if (g_strcmp0(status, "disabled") == 0) n_disabled++;
        else n_other++;
    }

    /* Choose color: green if all ok, red if any failed, yellow if mixed */
    const gchar *color;
    if (n_failed > 0)
        color = "#e05050";
    else if (n_other > 0)
        color = "#e0c050";
    else
        color = "#4ec94e";

    gchar *markup = g_markup_printf_escaped(
        "<span foreground=\"%s\">\u25CF</span> MCP", color);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(priv->mcp_indicator));
    gtk_label_set_markup(GTK_LABEL(child), markup);
    g_free(markup);

    /* Tooltip summary */
    GString *tip = g_string_new("");
    if (n_connected > 0) g_string_append_printf(tip, "%u connected", n_connected);
    if (n_failed > 0) {
        if (tip->len > 0) g_string_append(tip, ", ");
        g_string_append_printf(tip, "%u failed", n_failed);
    }
    if (n_disabled > 0) {
        if (tip->len > 0) g_string_append(tip, ", ");
        g_string_append_printf(tip, "%u disabled", n_disabled);
    }
    if (n_other > 0) {
        if (tip->len > 0) g_string_append(tip, ", ");
        g_string_append_printf(tip, "%u pending", n_other);
    }
    gtk_widget_set_tooltip_text(priv->mcp_indicator, tip->str);
    g_string_free(tip, TRUE);

    gtk_widget_show(priv->mcp_indicator);
    g_object_unref(jp);

    /* Rebuild popover if visible */
    if (priv->mcp_popover && gtk_widget_get_visible(priv->mcp_popover))
        rebuild_mcp_list(priv);
}

static void on_mcp_action_clicked(GtkButton *btn, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    const gchar *server = g_object_get_data(G_OBJECT(btn), "server");
    const gchar *action = g_object_get_data(G_OBJECT(btn), "action");
    if (!server || !action) return;

    if (g_strcmp0(action, "disable") == 0)
        cli_session_mcp_toggle(priv->session, server, FALSE);
    else if (g_strcmp0(action, "enable") == 0)
        cli_session_mcp_toggle(priv->session, server, TRUE);
    else if (g_strcmp0(action, "reconnect") == 0)
        cli_session_mcp_reconnect(priv->session, server);

    /* Refresh status after action (response will update popover) */
    cli_session_query_mcp_status(priv->session);
}

static void rebuild_mcp_list(ChatWidgetPrivate *priv)
{
    /* Clear existing rows */
    GList *children = gtk_container_get_children(
        GTK_CONTAINER(priv->mcp_list_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    if (!priv->mcp_servers_json)
        return;

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, priv->mcp_servers_json, -1, NULL)) {
        g_object_unref(jp);
        return;
    }
    JsonNode *root = json_parser_get_root(jp);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(jp);
        return;
    }

    JsonArray *arr = json_node_get_array(root);
    guint n = json_array_get_length(arr);

    for (guint i = 0; i < n; i++) {
        JsonObject *srv = json_array_get_object_element(arr, i);
        const gchar *name = json_object_has_member(srv, "name")
            ? json_object_get_string_member(srv, "name") : "unknown";
        const gchar *status = json_object_has_member(srv, "status")
            ? json_object_get_string_member(srv, "status") : "unknown";
        const gchar *error = json_object_has_member(srv, "error")
            ? json_object_get_string_member(srv, "error") : NULL;

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(row_box, 8);
        gtk_widget_set_margin_end(row_box, 8);
        gtk_widget_set_margin_top(row_box, 4);
        gtk_widget_set_margin_bottom(row_box, 4);

        GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        /* Status dot */
        const gchar *dot_color;
        const gchar *dot_char = "\u25CF";
        if (g_strcmp0(status, "connected") == 0) dot_color = "#4ec94e";
        else if (g_strcmp0(status, "failed") == 0) dot_color = "#e05050";
        else if (g_strcmp0(status, "disabled") == 0) { dot_color = "#888888"; dot_char = "\u25CB"; }
        else dot_color = "#e0c050";

        gchar *dot_markup = g_markup_printf_escaped(
            "<span foreground=\"%s\">%s</span>", dot_color, dot_char);
        GtkWidget *dot = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(dot), dot_markup);
        g_free(dot_markup);
        gtk_box_pack_start(GTK_BOX(top_row), dot, FALSE, FALSE, 0);

        /* Server name (bold) */
        GtkWidget *name_label = gtk_label_new(name);
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes(GTK_LABEL(name_label), attrs);
        pango_attr_list_unref(attrs);
        gtk_box_pack_start(GTK_BOX(top_row), name_label, FALSE, FALSE, 0);

        /* Spacer */
        gtk_box_pack_start(GTK_BOX(top_row), gtk_label_new(""), TRUE, TRUE, 0);

        /* Status text (dim) */
        gchar *status_cap = g_strdup(status);
        if (status_cap[0] >= 'a' && status_cap[0] <= 'z')
            status_cap[0] -= 32;
        GtkWidget *status_label = gtk_label_new(status_cap);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(status_label), "dim-label");
        g_free(status_cap);
        gtk_box_pack_start(GTK_BOX(top_row), status_label, FALSE, FALSE, 0);

        /* Action button */
        /* Action buttons — pack_end so they appear right-to-left */
        typedef struct { const gchar *label; const gchar *action; } BtnDef;
        BtnDef btns[3];
        gint n_btns = 0;

        if (g_strcmp0(status, "connected") == 0) {
            btns[n_btns++] = (BtnDef){"Disable", "disable"};
            btns[n_btns++] = (BtnDef){"\u27F3", "reconnect"};
        } else if (g_strcmp0(status, "failed") == 0 ||
                   g_strcmp0(status, "needs-auth") == 0) {
            btns[n_btns++] = (BtnDef){"\u27F3", "reconnect"};
        } else if (g_strcmp0(status, "disabled") == 0) {
            btns[n_btns++] = (BtnDef){"Enable", "enable"};
        }

        for (gint bi = 0; bi < n_btns; bi++) {
            GtkWidget *btn = gtk_button_new_with_label(btns[bi].label);
            gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
            if (g_strcmp0(btns[bi].action, "reconnect") == 0)
                gtk_widget_set_tooltip_text(btn, "Reconnect");
            g_object_set_data_full(G_OBJECT(btn), "server",
                g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(btn), "action",
                g_strdup(btns[bi].action), g_free);
            g_signal_connect(btn, "clicked",
                G_CALLBACK(on_mcp_action_clicked), priv);
            gtk_box_pack_end(GTK_BOX(top_row), btn, FALSE, FALSE, 0);
        }

        gtk_box_pack_start(GTK_BOX(row_box), top_row, FALSE, FALSE, 0);

        /* Error text for failed servers */
        if (error && g_strcmp0(status, "failed") == 0) {
            GtkWidget *err_label = gtk_label_new(error);
            gtk_label_set_xalign(GTK_LABEL(err_label), 0);
            gtk_label_set_line_wrap(GTK_LABEL(err_label), TRUE);
            gtk_widget_set_margin_start(err_label, 24);
            PangoAttrList *ea = pango_attr_list_new();
            pango_attr_list_insert(ea, pango_attr_scale_new(PANGO_SCALE_SMALL));
            pango_attr_list_insert(ea,
                pango_attr_foreground_new(0xAAAA, 0x4444, 0x4444));
            gtk_label_set_attributes(GTK_LABEL(err_label), ea);
            pango_attr_list_unref(ea);
            gtk_box_pack_start(GTK_BOX(row_box), err_label, FALSE, FALSE, 0);
        }

        gtk_list_box_insert(GTK_LIST_BOX(priv->mcp_list_box), row_box, -1);
    }

    gtk_widget_show_all(priv->mcp_list_box);
    g_object_unref(jp);
}

static void on_mcp_indicator_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    ChatWidgetPrivate *priv = user_data;

    if (!priv->mcp_popover) {
        priv->mcp_popover = gtk_popover_new(GTK_WIDGET(priv->mcp_indicator));
        gtk_popover_set_position(GTK_POPOVER(priv->mcp_popover), GTK_POS_BOTTOM);

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, 350, -1);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(scroll), 300);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(scroll), TRUE);

        priv->mcp_list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(priv->mcp_list_box),
            GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), priv->mcp_list_box);
        gtk_container_add(GTK_CONTAINER(priv->mcp_popover), scroll);
    }

    /* Refresh status from CLI and show cached data immediately */
    cli_session_query_mcp_status(priv->session);
    rebuild_mcp_list(priv);
    gtk_widget_show_all(priv->mcp_popover);
    gtk_popover_popup(GTK_POPOVER(priv->mcp_popover));
}

/* ── CLISession callbacks ────────────────────────────────────────── */

static void on_cli_mcp_status(const gchar *servers_json, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    g_free(priv->mcp_servers_json);
    priv->mcp_servers_json = g_strdup(servers_json);
    update_mcp_indicator(priv);
}

static void on_cli_message(const gchar *msg_id, const gchar *role,
                           const gchar *content, gboolean is_streaming,
                           gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    /* First time we see this msg_id: add the message to the web view */
    if (!g_hash_table_contains(priv->known_msg_ids, msg_id)) {
        g_hash_table_add(priv->known_msg_ids, g_strdup(msg_id));

        GDateTime *now = g_date_time_new_now_local();
        gchar *ts = g_date_time_format_iso8601(now);
        chat_webview_add_message(priv->webview, msg_id, role,
                                 content ? content : "", ts, is_streaming);
        g_free(ts);
        g_date_time_unref(now);
    } else {
        /* Subsequent updates */
        chat_webview_update_message(priv->webview, msg_id,
                                    content ? content : "", is_streaming);
    }

    if (!is_streaming)
        chat_input_set_busy(priv->input, FALSE);
}

static void on_cli_tool_call(const gchar *msg_id, const gchar *tool_id,
                             const gchar *tool_name, const gchar *input_json,
                             const gchar *result, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    track_edit(priv, tool_name, input_json);
    chat_webview_add_tool_call(priv->webview, msg_id, tool_id,
                               tool_name, input_json, result);
}

static void on_cli_permission(const gchar *request_id,
                              const gchar *tool_name,
                              const gchar *description,
                              const gchar *options_json,
                              gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_webview_show_permission(priv->webview, request_id, tool_name,
                                 description, options_json);
}

/* Permission response from web view → CLI session */
static void on_webview_permission(const gchar *request_id,
                                  const gchar *option_id,
                                  gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    cli_session_respond_permission(priv->session, request_id, option_id);
}

static void on_mode_changed(const gchar *mode_id, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    cli_session_set_mode(priv->session, mode_id);
}

static void on_model_changed(const gchar *model_value, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    cli_session_set_model(priv->session, model_value);
}

static void on_cli_init(const gchar *model, const gchar *permission_mode,
                        gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    if (permission_mode)
        chat_input_set_mode(priv->input, permission_mode);
}

static void on_cli_models(const gchar *models_json, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_input_set_models(priv->input, models_json);
}

static void on_cli_commands(const gchar *commands_json, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_input_set_commands(priv->input, commands_json);
}

static void on_cli_todos(const gchar *todos_json, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_input_update_todos(priv->input, todos_json);
}

static void on_cli_thinking(const gchar *msg_id, guint fragment_index,
                            const gchar *text, gboolean is_streaming,
                            gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_webview_add_thinking(priv->webview, msg_id, fragment_index,
                              text, is_streaming);
}

static void on_cli_error(const gchar *error_msg, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    gchar *id = g_strdup_printf("err_%u", priv->msg_counter++);
    chat_webview_add_message(priv->webview, id, "error", error_msg, "", FALSE);
    g_hash_table_add(priv->known_msg_ids, id); /* hash table takes ownership */

    chat_input_set_busy(priv->input, FALSE);
}

static void on_cli_finished(gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    chat_input_set_busy(priv->input, FALSE);
}

/* ── Input callbacks ─────────────────────────────────────────────── */

static void on_send(const gchar *text, gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    ensure_session(priv);

    /* Take attachments before adding message */
    GList *images = chat_input_take_images(priv->input);
    GList *contexts = chat_input_take_contexts(priv->input);

    /* Build the full prompt with context chunks prepended */
    GString *full_prompt = g_string_new("");
    for (GList *l = contexts; l; l = l->next) {
        ContextChunk *c = l->data;
        g_string_append_printf(full_prompt,
            "Context from %s (lines %d-%d):\n```\n%s\n```\n\n",
            c->file_path, c->start_line, c->end_line, c->content);
    }
    g_string_append(full_prompt, text);

    /* Add user message to the web view */
    gchar *id = g_strdup_printf("user_%u", priv->msg_counter++);
    GDateTime *now = g_date_time_new_now_local();
    gchar *ts = g_date_time_format_iso8601(now);
    chat_webview_add_message(priv->webview, id, "user",
                             full_prompt->str, ts, FALSE);
    g_free(ts);
    g_date_time_unref(now);

    /* Render attached images in the message */
    for (GList *l = images; l; l = l->next) {
        const gchar *b64 = (const gchar *)l->data;
        chat_webview_add_message_image(priv->webview, id, b64);
    }
    g_free(id);

    /* Send to claude (no implicit file/selection — user adds context explicitly) */
    cli_session_send_message(priv->session, full_prompt->str,
                             NULL, NULL, images);
    g_list_free_full(images, g_free);
    for (GList *l = contexts; l; l = l->next) {
        ContextChunk *c = l->data;
        g_free(c->file_path);
        g_free(c->content);
        g_free(c);
    }
    g_list_free(contexts);
    g_string_free(full_prompt, TRUE);

    chat_input_set_busy(priv->input, TRUE);
}

static void on_stop(gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;
    /* Try graceful interrupt first; only kill if not running */
    if (cli_session_is_running(priv->session))
        cli_session_interrupt(priv->session);
    else
        cli_session_stop(priv->session);
    chat_input_set_busy(priv->input, FALSE);
}

static void on_new_session(gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    /* Stop the current session */
    cli_session_stop(priv->session);

    /* Clear tracked message IDs and edit stats */
    g_hash_table_remove_all(priv->known_msg_ids);
    g_hash_table_remove_all(priv->edit_stats);
    update_edit_indicator(priv);
    g_free(priv->mcp_servers_json);
    priv->mcp_servers_json = NULL;
    gtk_widget_hide(priv->mcp_indicator);

    /* Clear the web view */
    chat_webview_clear(priv->webview);

    /* Reset state */
    chat_input_set_busy(priv->input, FALSE);
    chat_input_clear(priv->input);

    /* Recreate the session (it will start on next send) */
    cli_session_free(priv->session);
    priv->session = cli_session_new();
    cli_session_set_message_callback(priv->session, on_cli_message, priv);
    cli_session_set_tool_call_callback(priv->session, on_cli_tool_call, priv);
    cli_session_set_permission_callback(priv->session, on_cli_permission, priv);
    cli_session_set_init_callback(priv->session, on_cli_init, priv);
    cli_session_set_models_callback(priv->session, on_cli_models, priv);
    cli_session_set_commands_callback(priv->session, on_cli_commands, priv);
    cli_session_set_todos_callback(priv->session, on_cli_todos, priv);
    cli_session_set_thinking_callback(priv->session, on_cli_thinking, priv);
    cli_session_set_mcp_status_callback(priv->session, on_cli_mcp_status, priv);
    cli_session_set_error_callback(priv->session, on_cli_error, priv);
    cli_session_set_finished_callback(priv->session, on_cli_finished, priv);
}

static void on_new_session_btn(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    on_new_session(user_data);
}

/* ── Resume session ──────────────────────────────────────────────── */

static void on_resume_session_btn(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    ChatWidgetPrivate *priv = user_data;

    /* Use git root for session lookup — Claude stores sessions there */
    gchar *root = editor_bridge_get_git_root();
    if (!root) return;

    GtkWidget *toplevel = gtk_widget_get_toplevel(priv->container);
    GtkWindow *parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;

    gchar *session_id = session_picker_run(parent, root);
    if (!session_id) {  /* cancelled */
        g_free(root);
        return;
    }

    if (*session_id == '\0') {
        /* "Start new session" chosen */
        g_free(root);
        g_free(session_id);
        on_new_session(priv);
        return;
    }

    /* Reset state (same as on_new_session) then resume */
    on_new_session(priv);
    cli_session_set_session_id(priv->session, session_id);

    /* Load and render recent history from the JSONL file */
    GList *history = session_load_history(root, session_id, 20);
    for (GList *l = history; l; l = l->next) {
        HistoryMessage *hm = l->data;
        gchar *id = g_strdup_printf("hist_%s", hm->uuid);

        /* Render text content if present */
        if (hm->content)
            chat_webview_add_history_message(priv->webview, id, hm->role,
                                             hm->content, hm->timestamp);

        /* Render tool calls */
        for (GList *tc = hm->tool_calls; tc; tc = tc->next) {
            HistoryToolCall *htc = tc->data;
            chat_webview_add_tool_call(priv->webview, id, htc->tool_id,
                                       htc->tool_name, htc->input_json, NULL);
        }

        g_hash_table_add(priv->known_msg_ids, id); /* takes ownership */
    }
    if (history)
        chat_webview_add_history_separator(priv->webview);
    history_message_list_free(history);

    g_free(root);
    g_free(session_id);

    /* Start immediately with --resume */
    ensure_session(priv);
}

/* ── Jump-to-edit callback from web view ─────────────────────────── */

static void on_jump_to_edit(const gchar *file_path,
                            gint start_line, gint end_line,
                            gpointer user_data)
{
    ChatWidgetPrivate *priv = user_data;

    /* Grep results use paths relative to the project root — resolve them */
    if (file_path && file_path[0] != '/') {
        const gchar *wd = cli_session_get_working_dir(priv->session);
        if (wd) {
            gchar *abs = g_build_filename(wd, file_path, NULL);
            editor_bridge_jump_to(abs, start_line, end_line);
            g_free(abs);
            return;
        }
    }
    editor_bridge_jump_to(file_path, start_line, end_line);
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ChatWidgetPrivate *priv = data;
    if (priv) {
        cli_session_free(priv->session);
        g_hash_table_unref(priv->known_msg_ids);
        g_hash_table_unref(priv->edit_stats);
        g_free(priv->mcp_servers_json);
        g_free(priv);
    }
}

/* ── Construction ────────────────────────────────────────────────── */

GtkWidget *chat_widget_new(void)
{
    ChatWidgetPrivate *priv = g_new0(ChatWidgetPrivate, 1);
    priv->known_msg_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    priv->edit_stats = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, free_file_edit_stat);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    priv->container = vbox;
    g_object_set_data(G_OBJECT(vbox), CHAT_PRIV_KEY, priv);

    /* Header bar */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);
    gtk_widget_set_margin_top(header, 4);
    gtk_widget_set_margin_bottom(header, 4);

    GtkWidget *title = gtk_label_new("Claude Code");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);

    /* Spacer */
    gtk_box_pack_start(GTK_BOX(header),
                       gtk_label_new(""), TRUE, TRUE, 0);

    /* Edit tracking indicator (hidden until edits occur) */
    priv->edit_indicator = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(priv->edit_indicator), GTK_RELIEF_NONE);
    gtk_widget_set_no_show_all(priv->edit_indicator, TRUE);
    gtk_box_pack_start(GTK_BOX(header), priv->edit_indicator, FALSE, FALSE, 0);
    g_signal_connect(priv->edit_indicator, "clicked",
                     G_CALLBACK(on_edit_indicator_clicked), priv);

    /* MCP server status indicator (hidden until init provides data) */
    priv->mcp_indicator = gtk_button_new_with_label("");
    gtk_button_set_relief(GTK_BUTTON(priv->mcp_indicator), GTK_RELIEF_NONE);
    gtk_widget_set_no_show_all(priv->mcp_indicator, TRUE);
    gtk_box_pack_start(GTK_BOX(header), priv->mcp_indicator, FALSE, FALSE, 0);
    g_signal_connect(priv->mcp_indicator, "clicked",
                     G_CALLBACK(on_mcp_indicator_clicked), priv);

    /* Resume session button (icon) */
    GtkWidget *resume_btn = gtk_button_new_from_icon_name(
        "document-open-recent", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(resume_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(resume_btn, "Resume Session");
    gtk_box_pack_start(GTK_BOX(header), resume_btn, FALSE, FALSE, 0);

    /* New session button (icon) */
    GtkWidget *new_btn = gtk_button_new_from_icon_name(
        "document-new", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(new_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(new_btn, "New Session");
    gtk_box_pack_start(GTK_BOX(header), new_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Web view (takes most of the space) */
    priv->webview = chat_webview_new();
    gtk_box_pack_start(GTK_BOX(vbox), priv->webview, TRUE, TRUE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Input area */
    priv->input = chat_input_new();
    gtk_box_pack_start(GTK_BOX(vbox), priv->input, FALSE, FALSE, 4);

    /* Wire header buttons */
    g_signal_connect(resume_btn, "clicked",
                     G_CALLBACK(on_resume_session_btn), priv);
    g_signal_connect(new_btn, "clicked",
                     G_CALLBACK(on_new_session_btn), priv);

    /* CLI session */
    priv->session = cli_session_new();
    cli_session_set_message_callback(priv->session, on_cli_message, priv);
    cli_session_set_tool_call_callback(priv->session, on_cli_tool_call, priv);
    cli_session_set_permission_callback(priv->session, on_cli_permission, priv);
    cli_session_set_init_callback(priv->session, on_cli_init, priv);
    cli_session_set_models_callback(priv->session, on_cli_models, priv);
    cli_session_set_commands_callback(priv->session, on_cli_commands, priv);
    cli_session_set_todos_callback(priv->session, on_cli_todos, priv);
    cli_session_set_thinking_callback(priv->session, on_cli_thinking, priv);
    cli_session_set_mcp_status_callback(priv->session, on_cli_mcp_status, priv);
    cli_session_set_error_callback(priv->session, on_cli_error, priv);
    cli_session_set_finished_callback(priv->session, on_cli_finished, priv);

    /* Wire up input */
    chat_input_set_send_callback(priv->input, on_send, priv);
    chat_input_set_stop_callback(priv->input, on_stop, priv);
    chat_input_set_mode_changed_callback(priv->input, on_mode_changed, priv);
    chat_input_set_model_changed_callback(priv->input, on_model_changed, priv);

    /* Wire up web view callbacks */
    chat_webview_set_jump_callback(priv->webview, on_jump_to_edit, priv);
    chat_webview_set_permission_callback(priv->webview, on_webview_permission, priv);

    /* Cleanup on destroy */
    g_signal_connect(vbox, "destroy", G_CALLBACK(on_destroy), priv);

    /* Start the claude process eagerly so we get init data (models, commands)
     * before the user sends their first message */
    ensure_session(priv);

    return vbox;
}

void chat_widget_add_context_from_editor(GtkWidget *widget)
{
    ChatWidgetPrivate *priv = get_priv(widget);
    if (!priv) return;

    GeanyDocument *doc = document_get_current();
    if (!doc || !doc->is_valid || !doc->file_name) return;

    ScintillaObject *sci = doc->editor->sci;
    gchar *sel = sci_get_selection_contents(sci);
    if (!sel || strlen(sel) == 0) {
        g_free(sel);
        return;
    }

    /* Get line range of selection */
    gint start_pos = sci_get_selection_start(sci);
    gint end_pos = sci_get_selection_end(sci);
    gint start_line = sci_get_line_from_position(sci, start_pos) + 1;
    gint end_line = sci_get_line_from_position(sci, end_pos) + 1;

    chat_input_add_context(priv->input, doc->file_name, sel,
                            start_line, end_line);
    g_free(sel);

    msgwin_status_add("[geany-code] Added context: %s:%d-%d",
                      doc->file_name, start_line, end_line);
}

void chat_widget_show_user_question(GtkWidget *widget,
                                    const gchar *request_id,
                                    const gchar *questions_json)
{
    ChatWidgetPrivate *priv = get_priv(widget);
    if (!priv) return;
    chat_webview_show_user_question(priv->webview, request_id, questions_json);
}

void chat_widget_paste_image(GtkWidget *widget)
{
    ChatWidgetPrivate *priv = get_priv(widget);
    if (!priv) return;

    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(cb);
    msgwin_status_add("[geany-code] paste_image: pixbuf=%p", (void *)pixbuf);
    if (!pixbuf) return;

    msgwin_status_add("[geany-code] paste_image: %dx%d",
        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));

    gchar *png_data = NULL;
    gsize png_len = 0;
    if (gdk_pixbuf_save_to_buffer(pixbuf, &png_data, &png_len, "png", NULL, NULL)) {
        gchar *b64 = g_base64_encode((guchar *)png_data, png_len);
        g_free(png_data);
        msgwin_status_add("[geany-code] paste_image: b64 len=%lu, input=%p",
            (unsigned long)strlen(b64), (void *)priv->input);
        chat_input_add_image(priv->input, pixbuf, b64);
        msgwin_status_add("[geany-code] paste_image: chip added");
    }

    g_object_unref(pixbuf);
}

void chat_widget_focus_input(GtkWidget *widget)
{
    ChatWidgetPrivate *priv = get_priv(widget);
    if (priv)
        chat_input_grab_focus(priv->input);
}

void chat_widget_send_selection(GtkWidget *widget, GeanyDocument *doc)
{
    (void)doc;
    ChatWidgetPrivate *priv = get_priv(widget);
    if (!priv) return;

    gchar *selection = editor_bridge_get_selection();
    if (selection && strlen(selection) > 0)
        chat_input_set_text(priv->input, selection);

    chat_input_grab_focus(priv->input);
    g_free(selection);
}

void chat_widget_quick_action(GtkWidget *widget, GeanyDocument *doc,
                              const gchar *instruction)
{
    (void)doc;
    ChatWidgetPrivate *priv = get_priv(widget);
    if (!priv) return;

    ensure_session(priv);

    gchar *selection = editor_bridge_get_selection();
    const gchar *file = editor_bridge_get_current_file();

    /* Build the prompt */
    GString *prompt = g_string_new(instruction);
    if (selection && strlen(selection) > 0) {
        g_string_append(prompt, ":\n\n```\n");
        g_string_append(prompt, selection);
        g_string_append(prompt, "\n```");
    }

    /* Show in chat */
    gchar *id = g_strdup_printf("user_%u", priv->msg_counter++);
    GDateTime *now = g_date_time_new_now_local();
    gchar *ts = g_date_time_format_iso8601(now);
    chat_webview_add_message(priv->webview, id, "user", prompt->str, ts, FALSE);
    g_free(ts);
    g_date_time_unref(now);
    g_free(id);

    /* Send */
    cli_session_send_message(priv->session, prompt->str, file, NULL, NULL);
    chat_input_set_busy(priv->input, TRUE);

    g_string_free(prompt, TRUE);
    g_free(selection);
}
