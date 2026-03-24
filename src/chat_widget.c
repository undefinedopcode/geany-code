#include "chat_widget.h"
#include "chat_webview.h"
#include "chat_input.h"
#include "cli_session.h"
#include "editor_bridge.h"
#include <string.h>

typedef struct {
    GtkWidget   *webview;
    GtkWidget   *input;
    CLISession  *session;
    guint        msg_counter;
    GHashTable  *known_msg_ids;  /* tracks which msg IDs have been added to webview */
} ChatWidgetPrivate;

static const gchar *CHAT_PRIV_KEY = "geany-code-chat-private";

static ChatWidgetPrivate *get_priv(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), CHAT_PRIV_KEY);
}

/* ── Ensure session is running ───────────────────────────────────── */

static void ensure_session(ChatWidgetPrivate *priv)
{
    if (cli_session_is_running(priv->session))
        return;

    gchar *root = editor_bridge_get_project_root();
    cli_session_start(priv->session, root);
    g_free(root);
}

/* ── CLISession callbacks ────────────────────────────────────────── */

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
    chat_webview_update_todos(priv->webview, todos_json);
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

    /* Send to claude */
    const gchar *file = editor_bridge_get_current_file();
    gchar *selection = editor_bridge_get_selection();
    cli_session_send_message(priv->session, full_prompt->str,
                             file, selection, images);
    g_list_free_full(images, g_free);
    for (GList *l = contexts; l; l = l->next) {
        ContextChunk *c = l->data;
        g_free(c->file_path);
        g_free(c->content);
        g_free(c);
    }
    g_list_free(contexts);
    g_free(selection);
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

    /* Clear tracked message IDs */
    g_hash_table_remove_all(priv->known_msg_ids);

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
    cli_session_set_error_callback(priv->session, on_cli_error, priv);
    cli_session_set_finished_callback(priv->session, on_cli_finished, priv);
}

static void on_new_session_btn(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    on_new_session(user_data);
}

/* ── Jump-to-edit callback from web view ─────────────────────────── */

static void on_jump_to_edit(const gchar *file_path,
                            gint start_line, gint end_line,
                            gpointer user_data)
{
    (void)user_data;
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
        g_free(priv);
    }
}

/* ── Construction ────────────────────────────────────────────────── */

GtkWidget *chat_widget_new(void)
{
    ChatWidgetPrivate *priv = g_new0(ChatWidgetPrivate, 1);
    priv->known_msg_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
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

    /* Wire new session button */
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
