#include "plugin.h"
#include "chat_widget.h"
#include "chat_input.h"
#include "editor_bridge.h"
#include "editor_dbus.h"
#include "hooks_dialog.h"
#include <string.h>

GeanyPlugin    *geany_plugin;
GeanyData      *geany_data;

GeanyCodePlugin *geany_code = NULL;

enum {
    KB_SEND_SELECTION = 0,
    KB_EXPLAIN_CODE,
    KB_FIND_BUGS,
    KB_FOCUS_INPUT,
    KB_ADD_CONTEXT,
    KB_COMMAND_PALETTE,
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
    case KB_COMMAND_PALETTE:
        chat_widget_show_command_palette(geany_code->chat_widget);
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

/* ── Intercept Ctrl+V / Alt+V for paste in chat input ────────────── */

/* Connected via plugin_signal_connect to geany_data->object "key-press".
 * This fires BEFORE Geany's keybinding loop, so we can claim Ctrl+V
 * when our chat input has focus.  Returning TRUE tells Geany "handled"
 * and prevents keybinding processing. */
static gboolean on_geany_key_press(GObject *obj, GdkEventKey *event,
                                   gpointer data)
{
    (void)obj; (void)data;

    if (!geany_code || !geany_code->chat_widget)
        return FALSE;

    guint mod = event->state & gtk_accelerator_get_default_mod_mask();

    /* Ctrl+Shift+P opens the command palette regardless of focus — runs
     * before Geany's keybinding loop so Scintilla can't eat it. */
    if ((event->keyval == GDK_KEY_p || event->keyval == GDK_KEY_P) &&
        mod == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    {
        chat_widget_show_command_palette(geany_code->chat_widget);
        return TRUE;
    }

    /* Ctrl+Shift+Y copies the most-recent assistant response to the
     * clipboard — a keyboard accelerator for the hover-reveal copy
     * button on each assistant message. */
    if ((event->keyval == GDK_KEY_y || event->keyval == GDK_KEY_Y) &&
        mod == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    {
        if (chat_widget_copy_last_response(geany_code->chat_widget))
            msgwin_status_add(
                "[geany-code] Copied last response to clipboard");
        return TRUE;
    }

    gboolean is_paste = (event->keyval == GDK_KEY_v || event->keyval == GDK_KEY_V) &&
                        (mod == GDK_CONTROL_MASK || mod == GDK_MOD1_MASK);
    if (!is_paste)
        return FALSE;

    GtkWidget *focus = gtk_window_get_focus(
        GTK_WINDOW(geany->main_widgets->window));
    if (!focus || !gtk_widget_is_ancestor(focus, geany_code->chat_widget))
        return FALSE;

    /* Our input has focus — check for image first */
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (gtk_clipboard_wait_is_image_available(cb)) {
        chat_widget_paste_image(geany_code->chat_widget);
        return TRUE;
    }

    /* Text paste */
    if (GTK_IS_TEXT_VIEW(focus)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus));
        gtk_text_buffer_paste_clipboard(buf, cb, NULL, TRUE);
        return TRUE;
    }

    return FALSE;
}

/* ── Editor context menu ─────────────────────────────────────────── */

/* Items we append to Geany's editor context menu. Sensitivity is updated
 * in on_update_editor_menu based on whether the active document has a
 * selection. */
typedef struct {
    GtkWidget *separator;
    GtkWidget *add_context;
    GtkWidget *explain;
    GtkWidget *find_bugs;
    GtkWidget *improve;
    GtkWidget *send_selection;
} EditorMenuItems;

static EditorMenuItems editor_menu_items = {0};

static void on_update_editor_menu(GObject *obj,
                                   const gchar *word,
                                   gint pos,
                                   GeanyDocument *doc,
                                   gpointer user_data)
{
    (void)obj; (void)word; (void)pos; (void)user_data;

    gboolean has_selection = FALSE;
    if (doc && doc->is_valid) {
        ScintillaObject *sci = doc->editor->sci;
        has_selection = sci_has_selection(sci);
    }

    GtkWidget *items[] = {
        editor_menu_items.add_context,
        editor_menu_items.explain,
        editor_menu_items.find_bugs,
        editor_menu_items.improve,
        editor_menu_items.send_selection,
        NULL
    };
    for (GtkWidget **w = items; *w; w++)
        gtk_widget_set_sensitive(*w, has_selection);
}

/* ── Menu callbacks ──────────────────────────────────────────────── */

static void on_send_selection(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    GeanyDocument *doc = document_get_current();
    if (doc && doc->is_valid)
        chat_widget_send_selection(geany_code->chat_widget, doc);
}

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

static void on_manage_hooks(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    hooks_dialog_run(GTK_WINDOW(geany->main_widgets->window));
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

    /* Load settings */
    geany_code->settings = settings_new();
    settings_load(geany_code->settings);

    /* Start DBus service for MCP server */
    editor_dbus_start();
    editor_dbus_set_question_callback(on_mcp_question, NULL);

    /* Create the chat widget */
    geany_code->chat_widget = chat_widget_new();
    gtk_widget_show_all(geany_code->chat_widget);

    /* Add to message window (bottom panel) */
    geany_code->sidebar_page = geany_code->chat_widget;
    GtkWidget *label = gtk_label_new("Geany Code");
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

    gtk_menu_shell_append(GTK_MENU_SHELL(geany_code->submenu),
                          gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Manage Hooks...");
    g_signal_connect(item, "activate", G_CALLBACK(on_manage_hooks), NULL);
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
    keybindings_set_item(kg, KB_COMMAND_PALETTE, NULL,
                         GDK_KEY_P,
                         GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                         "command_palette", "Claude Command Palette", NULL);

    /* Hook Ctrl+V / Alt+V for paste in our input — fires before Geany's
     * keybinding loop so we can intercept Ctrl+V.  Auto-disconnects on
     * plugin unload. */
    plugin_signal_connect(plugin, geany_data->object, "key-press", FALSE,
                          G_CALLBACK(on_geany_key_press), NULL);

    /* Editor context menu: add a "Claude Code" section with selection
     * quick-actions. Sensitivity is driven by on_update_editor_menu. */
    GtkWidget *emenu = geany->main_widgets->editor_menu;
    editor_menu_items.separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.separator);

    editor_menu_items.add_context =
        gtk_menu_item_new_with_label("Add Selection to Claude Context");
    g_signal_connect(editor_menu_items.add_context, "activate",
                     G_CALLBACK(on_add_to_context), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.add_context);

    editor_menu_items.send_selection =
        gtk_menu_item_new_with_label("Send Selection to Claude");
    g_signal_connect(editor_menu_items.send_selection, "activate",
                     G_CALLBACK(on_send_selection), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.send_selection);

    editor_menu_items.explain =
        gtk_menu_item_new_with_label("Explain with Claude");
    g_signal_connect(editor_menu_items.explain, "activate",
                     G_CALLBACK(on_explain_code), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.explain);

    editor_menu_items.find_bugs =
        gtk_menu_item_new_with_label("Find Bugs with Claude");
    g_signal_connect(editor_menu_items.find_bugs, "activate",
                     G_CALLBACK(on_find_bugs), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.find_bugs);

    editor_menu_items.improve =
        gtk_menu_item_new_with_label("Suggest Improvements with Claude");
    g_signal_connect(editor_menu_items.improve, "activate",
                     G_CALLBACK(on_suggest_improvements), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(emenu),
                          editor_menu_items.improve);

    gtk_widget_show(editor_menu_items.separator);
    gtk_widget_show(editor_menu_items.add_context);
    gtk_widget_show(editor_menu_items.send_selection);
    gtk_widget_show(editor_menu_items.explain);
    gtk_widget_show(editor_menu_items.find_bugs);
    gtk_widget_show(editor_menu_items.improve);

    plugin_signal_connect(plugin, geany_data->object, "update-editor-menu",
                          FALSE, G_CALLBACK(on_update_editor_menu), NULL);

    return TRUE;
}

static void gc_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
    (void)plugin; (void)pdata;

    if (!geany_code)
        return;

    /* Save and free settings */
    if (geany_code->settings) {
        settings_save(geany_code->settings);
        settings_free(geany_code->settings);
    }

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

    /* plugin_signal_connect auto-disconnects on unload — no manual cleanup */

    /* Remove menu item */
    if (geany_code->menu_item)
        gtk_widget_destroy(geany_code->menu_item);

    /* Remove editor context menu items */
    GtkWidget *emenu_items[] = {
        editor_menu_items.separator,
        editor_menu_items.add_context,
        editor_menu_items.send_selection,
        editor_menu_items.explain,
        editor_menu_items.find_bugs,
        editor_menu_items.improve,
        NULL
    };
    for (GtkWidget **w = emenu_items; *w; w++)
        gtk_widget_destroy(*w);
    memset(&editor_menu_items, 0, sizeof(editor_menu_items));

    g_free(geany_code);
    geany_code = NULL;
}

static void on_diff_colors_changed(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    gint active = gtk_combo_box_get_active(combo);
    const gchar *schemes[] = { "green-red", "blue-red", "purple-orange" };
    if (active >= 0 && active < 3) {
        settings_set_diff_colors(geany_code->settings, schemes[active]);
        settings_save(geany_code->settings);
    }
}

static GtkWidget *gc_configure(GeanyPlugin *plugin, GtkDialog *dialog,
                               gpointer pdata)
{
    (void)plugin; (void)dialog; (void)pdata;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);

    /* Diff color scheme */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Diff colors:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Green / Red (default)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Blue / Red");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Purple / Orange");

    const gchar *current = settings_get_diff_colors(geany_code->settings);
    if (g_strcmp0(current, "blue-red") == 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 1);
    else if (g_strcmp0(current, "purple-orange") == 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 2);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    g_signal_connect(combo, "changed",
                     G_CALLBACK(on_diff_colors_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

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
