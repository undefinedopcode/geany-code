#include "plugin.h"
#include "chat_widget.h"
#include "chat_input.h"
#include "editor_bridge.h"
#include "editor_dbus.h"

GeanyPlugin    *geany_plugin;
GeanyData      *geany_data;

GeanyCodePlugin *geany_code = NULL;

enum {
    KB_SEND_SELECTION = 0,
    KB_EXPLAIN_CODE,
    KB_FIND_BUGS,
    KB_FOCUS_INPUT,
    KB_ADD_CONTEXT,
    KB_COUNT
};

/* ── Keybinding callback ─────────────────────────────────────────── */

static gboolean on_keybinding(guint key_id)
{
    GeanyDocument *doc = document_get_current();

    switch (key_id) {
    case KB_FOCUS_INPUT:
        chat_widget_focus_input(geany_code->chat_widget);
        return TRUE;
    case KB_SEND_SELECTION:
        if (doc && doc->is_valid)
            chat_widget_send_selection(geany_code->chat_widget, doc);
        return TRUE;
    case KB_EXPLAIN_CODE:
        if (doc && doc->is_valid)
            chat_widget_quick_action(geany_code->chat_widget, doc, "Explain this code");
        return TRUE;
    case KB_FIND_BUGS:
        if (doc && doc->is_valid)
            chat_widget_quick_action(geany_code->chat_widget, doc, "Find bugs in this code");
        return TRUE;
    case KB_ADD_CONTEXT:
        chat_widget_add_context_from_editor(geany_code->chat_widget);
        return TRUE;
    }
    return FALSE;
}

/* ── MCP question handler ─────────────────────────────────────────── */

static void on_mcp_question(const gchar *request_id,
                            const gchar *questions_json,
                            gpointer user_data)
{
    (void)user_data;
    /* Forward to the chat web view for inline rendering */
    if (geany_code && geany_code->chat_widget)
        chat_widget_show_user_question(geany_code->chat_widget,
                                       request_id, questions_json);
}

/* ── Window-level Alt+V for image paste ───────────────────────────── */

static gboolean on_window_key_press(GtkWidget *window, GdkEventKey *event,
                                    gpointer data)
{
    (void)window; (void)data;

    /* Only intercept Alt+V */
    if (!((event->keyval == GDK_KEY_v || event->keyval == GDK_KEY_V) &&
          (event->state & GDK_MOD1_MASK)))
        return FALSE;

    if (!geany_code || !geany_code->chat_widget)
        return FALSE;

    GtkWidget *focus = gtk_window_get_focus(
        GTK_WINDOW(geany->main_widgets->window));
    if (!focus || !gtk_widget_is_ancestor(focus, geany_code->chat_widget))
        return FALSE;

    /* Our input has focus — check for image */
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (gtk_clipboard_wait_is_image_available(cb)) {
        chat_widget_paste_image(geany_code->chat_widget);
        return TRUE;
    }

    /* Text paste via Alt+V */
    if (GTK_IS_TEXT_VIEW(focus)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus));
        gtk_text_buffer_paste_clipboard(buf, cb, NULL, TRUE);
        return TRUE;
    }

    return FALSE;
}

/* ── Menu callbacks ──────────────────────────────────────────────── */

static void on_explain_code(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    GeanyDocument *doc = document_get_current();
    if (doc && doc->is_valid)
        chat_widget_quick_action(geany_code->chat_widget, doc, "Explain this code");
}

static void on_find_bugs(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    GeanyDocument *doc = document_get_current();
    if (doc && doc->is_valid)
        chat_widget_quick_action(geany_code->chat_widget, doc, "Find bugs in this code");
}

static void on_add_to_context(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    chat_widget_add_context_from_editor(geany_code->chat_widget);
}

static void on_suggest_improvements(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    GeanyDocument *doc = document_get_current();
    if (doc && doc->is_valid)
        chat_widget_quick_action(geany_code->chat_widget, doc,
                                 "Suggest improvements for this code");
}

/* ── Plugin lifecycle ────────────────────────────────────────────── */

static gboolean gc_init(GeanyPlugin *plugin, gpointer pdata)
{
    (void)pdata;

    /* Populate globals so other modules can use the `geany` macro */
    geany_plugin = plugin;
    geany_data = plugin->geany_data;

    geany_code = g_new0(GeanyCodePlugin, 1);
    geany_code->geany_plugin = plugin;

    /* Start DBus service for MCP server */
    editor_dbus_start();
    editor_dbus_set_question_callback(on_mcp_question, NULL);

    /* Create the chat widget */
    geany_code->chat_widget = chat_widget_new();
    gtk_widget_show_all(geany_code->chat_widget);

    /* Add to message window (bottom panel) */
    geany_code->sidebar_page = geany_code->chat_widget;
    GtkWidget *label = gtk_label_new("Claude Code");
    gtk_notebook_append_page(
        GTK_NOTEBOOK(geany->main_widgets->message_window_notebook),
        geany_code->sidebar_page,
        label);

    /* Tools menu > Claude Code submenu */
    geany_code->menu_item = gtk_menu_item_new_with_label("Claude Code");
    geany_code->submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(geany_code->menu_item),
                              geany_code->submenu);

    GtkWidget *item;

    item = gtk_menu_item_new_with_label("Explain Code");
    g_signal_connect(item, "activate", G_CALLBACK(on_explain_code), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu), item);

    item = gtk_menu_item_new_with_label("Find Bugs");
    g_signal_connect(item, "activate", G_CALLBACK(on_find_bugs), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu), item);

    item = gtk_menu_item_new_with_label("Suggest Improvements");
    g_signal_connect(item, "activate", G_CALLBACK(on_suggest_improvements), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu),
                          gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Add to Context");
    g_signal_connect(item, "activate", G_CALLBACK(on_add_to_context), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu), item);

    gtk_widget_show_all(geany_code->menu_item);
    gtk_menu_shell_append(
        GTK_MENU_SHELL(geany->main_widgets->tools_menu),
        geany_code->menu_item);

    /* Keybindings */
    GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_code", KB_COUNT,
                                             on_keybinding);
    keybindings_set_item(kg, KB_FOCUS_INPUT, NULL, 0, 0,
                         "focus_input", "Focus Claude Code Input", NULL);
    keybindings_set_item(kg, KB_SEND_SELECTION, NULL, 0, 0,
                         "send_selection", "Send Selection to Claude", NULL);
    keybindings_set_item(kg, KB_EXPLAIN_CODE, NULL, 0, 0,
                         "explain_code", "Explain Code", NULL);
    keybindings_set_item(kg, KB_FIND_BUGS, NULL, 0, 0,
                         "find_bugs", "Find Bugs", NULL);
    keybindings_set_item(kg, KB_ADD_CONTEXT, NULL,
                         GDK_KEY_a, GDK_MOD1_MASK,
                         "add_context", "Add Selection to Claude Context", NULL);

    /* Hook Alt+V for paste in our input */
    g_signal_connect(geany->main_widgets->window, "key-press-event",
                     G_CALLBACK(on_window_key_press), NULL);

    return TRUE;
}

static void gc_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
    (void)plugin; (void)pdata;

    if (!geany_code)
        return;

    /* Stop DBus service */
    editor_dbus_stop();

    /* Remove message window page */
    if (geany_code->sidebar_page) {
        gint page = gtk_notebook_page_num(
            GTK_NOTEBOOK(geany->main_widgets->message_window_notebook),
            geany_code->sidebar_page);
        if (page >= 0)
            gtk_notebook_remove_page(
                GTK_NOTEBOOK(geany->main_widgets->message_window_notebook), page);
    }

    /* Disconnect window key handler */
    g_signal_handlers_disconnect_by_func(geany->main_widgets->window,
                                         on_window_key_press, NULL);

    /* Remove menu item */
    if (geany_code->menu_item)
        gtk_widget_destroy(geany_code->menu_item);

    g_free(geany_code);
    geany_code = NULL;
}

static GtkWidget *gc_configure(GeanyPlugin *plugin, GtkDialog *dialog,
                               gpointer pdata)
{
    (void)plugin; (void)dialog; (void)pdata;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);

    GtkWidget *label = gtk_label_new(
        "Claude Code integration for Geany.\n\n"
        "Configuration options will be added here.");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    gtk_widget_show_all(vbox);
    return vbox;
}

/* ── Plugin registration ─────────────────────────────────────────── */

G_MODULE_EXPORT void geany_load_module(GeanyPlugin *plugin)
{
    plugin->info->name        = "Geany Code";
    plugin->info->description = "Claude Code AI assistant integration";
    plugin->info->version     = GEANY_CODE_VERSION;
    plugin->info->author      = "April";

    plugin->funcs->init      = gc_init;
    plugin->funcs->cleanup   = gc_cleanup;
    plugin->funcs->configure = gc_configure;

    GEANY_PLUGIN_REGISTER(plugin, 250);
}
