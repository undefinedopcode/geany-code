#include "chat_input.h"
#include "plugin.h"
#include <json-glib/json-glib.h>
#include <string.h>

#define DBG(fmt, ...) msgwin_status_add("[geany-code] " fmt, ##__VA_ARGS__)

typedef struct {
    GtkWidget      *text_view;
    GtkTextBuffer  *buffer;
    GtkWidget      *send_btn;
    GtkWidget      *stop_btn;
    GtkWidget      *mode_combo;
    GtkWidget      *model_combo;
    GtkWidget      *todos_box;      /* todo panel (above chips, shown when active) */
    GtkWidget      *todos_revealer; /* collapsible body */
    GtkWidget      *todos_arrow;    /* expand/collapse indicator */
    GtkWidget      *chips_box;      /* container for image + context chips */
    GtkWidget      *cmd_menu;       /* slash command completion popup menu */
    GtkWidget      *file_menu;      /* @ file completion popup menu */
    GList          *commands;       /* GList of CommandInfo structs */
    GList          *project_files;  /* GList of gchar* (relative paths) */
    gchar          *project_root;   /* absolute path to project root */
    GList          *images;         /* GList of gchar* (base64 PNG data) */
    GList          *contexts;       /* GList of ContextChunk* */
    ChatInputSendCb       send_cb;
    gpointer              send_data;
    ChatInputStopCb       stop_cb;
    gpointer              stop_data;
    ChatInputModeChangedCb mode_changed_cb;
    gpointer              mode_changed_data;
    ChatInputModelChangedCb model_changed_cb;
    gpointer              model_changed_data;
    gboolean              mode_update_internal; /* suppress callback during programmatic set */
} ChatInputPrivate;

static const gchar *INPUT_PRIV_KEY = "geany-code-input-private";

static ChatInputPrivate *get_priv(GtkWidget *input)
{
    return g_object_get_data(G_OBJECT(input), INPUT_PRIV_KEY);
}

/* ── Signal handlers ─────────────────────────────────────────────── */

static void on_send_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *input = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(input);
    if (!priv || !priv->send_cb)
        return;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(priv->buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(priv->buffer, &start, &end, FALSE);

    if (text && strlen(text) > 0) {
        priv->send_cb(text, priv->send_data);
        gtk_text_buffer_set_text(priv->buffer, "", -1);
    }

    g_free(text);
}

static void on_stop_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *input = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(input);
    if (priv && priv->stop_cb)
        priv->stop_cb(priv->stop_data);
}

static void on_mode_combo_changed(GtkComboBox *combo, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv || !priv->mode_changed_cb) return;
    if (priv->mode_update_internal) return;

    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (id)
        priv->mode_changed_cb(id, priv->mode_changed_data);
}

static void on_model_combo_changed(GtkComboBox *combo, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv || !priv->model_changed_cb) return;

    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (id)
        priv->model_changed_cb(id, priv->model_changed_data);
}

/* ── Image paste handling ─────────────────────────────────────────── */

static void update_chips_visibility(ChatInputPrivate *priv)
{
    if (priv->images || priv->contexts) {
        gtk_widget_set_no_show_all(priv->chips_box, FALSE);
        gtk_widget_show_all(priv->chips_box);
    } else {
        gtk_widget_hide(priv->chips_box);
        gtk_widget_set_no_show_all(priv->chips_box, TRUE);
    }
}

static void on_remove_image(GtkButton *btn, gpointer data)
{
    (void)btn;
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return;

    /* Find which image this button corresponds to — stored as widget data */
    gchar *b64 = g_object_get_data(G_OBJECT(btn), "image-b64");
    if (b64) {
        GList *link = g_list_find_custom(priv->images, b64,
                                          (GCompareFunc)g_strcmp0);
        if (link) {
            g_free(link->data);
            priv->images = g_list_delete_link(priv->images, link);
        }
    }

    /* Remove the chip widget */
    GtkWidget *chip = gtk_widget_get_parent(GTK_WIDGET(btn));
    if (chip)
        gtk_widget_destroy(chip);

    update_chips_visibility(priv);
}

static void add_image_chip(GtkWidget *vbox, ChatInputPrivate *priv,
                           GdkPixbuf *pixbuf, gchar *b64_data)
{
    /* Store the base64 data */
    priv->images = g_list_append(priv->images, b64_data);

    /* Create chip: [thumbnail] [x] */
    GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(chip), "attachment-chip");
    gtk_widget_set_margin_start(chip, 2);
    gtk_widget_set_margin_end(chip, 2);

    /* Thumbnail */
    GdkPixbuf *thumb = gdk_pixbuf_scale_simple(
        pixbuf, 32, 32, GDK_INTERP_BILINEAR);
    GtkWidget *img = gtk_image_new_from_pixbuf(thumb);
    g_object_unref(thumb);
    gtk_box_pack_start(GTK_BOX(chip), img, FALSE, FALSE, 0);

    /* Remove button */
    GtkWidget *remove_btn = gtk_button_new_with_label("\xC3\x97"); /* × */
    gtk_button_set_relief(GTK_BUTTON(remove_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(remove_btn, 20, 20);
    g_object_set_data_full(G_OBJECT(remove_btn), "image-b64",
                           g_strdup(b64_data), g_free);
    g_signal_connect(remove_btn, "clicked",
                     G_CALLBACK(on_remove_image), vbox);
    gtk_box_pack_start(GTK_BOX(chip), remove_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(priv->chips_box), chip, FALSE, FALSE, 0);
    update_chips_visibility(priv);
}

static void free_context_chunk(gpointer data)
{
    ContextChunk *c = data;
    g_free(c->file_path);
    g_free(c->content);
    g_free(c);
}

static void on_remove_context(GtkButton *btn, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return;

    ContextChunk *chunk = g_object_get_data(G_OBJECT(btn), "context-chunk");
    if (chunk) {
        priv->contexts = g_list_remove(priv->contexts, chunk);
        free_context_chunk(chunk);
    }

    GtkWidget *chip = gtk_widget_get_parent(GTK_WIDGET(btn));
    if (chip) gtk_widget_destroy(chip);

    update_chips_visibility(priv);
}

static void add_context_chip(GtkWidget *vbox, ChatInputPrivate *priv,
                              ContextChunk *chunk)
{
    priv->contexts = g_list_append(priv->contexts, chunk);

    /* Create chip: [label] [x] */
    GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(chip), "context-chip");
    gtk_style_context_add_class(gtk_widget_get_style_context(chip), "attachment-chip");
    gtk_widget_set_margin_start(chip, 2);
    gtk_widget_set_margin_end(chip, 2);

    /* Label: "filename.ext:10-25" */
    gchar *basename = g_path_get_basename(chunk->file_path);
    gchar *label_text;
    if (chunk->start_line > 0 && chunk->end_line > 0)
        label_text = g_strdup_printf("%s:%d-%d", basename, chunk->start_line, chunk->end_line);
    else
        label_text = g_strdup(basename);
    g_free(basename);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_tooltip_text(label, chunk->file_path);
    g_free(label_text);
    gtk_box_pack_start(GTK_BOX(chip), label, FALSE, FALSE, 4);

    /* Remove button */
    GtkWidget *remove_btn = gtk_button_new_with_label("\xC3\x97");
    gtk_button_set_relief(GTK_BUTTON(remove_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(remove_btn, 20, 20);
    g_object_set_data(G_OBJECT(remove_btn), "context-chunk", chunk);
    g_signal_connect(remove_btn, "clicked",
                     G_CALLBACK(on_remove_context), vbox);
    gtk_box_pack_start(GTK_BOX(chip), remove_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(priv->chips_box), chip, FALSE, FALSE, 0);
    update_chips_visibility(priv);
}

static void on_paste_clipboard(GtkTextView *text_view, gpointer data)
{
    DBG("paste-clipboard signal fired");

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

    gboolean has_image = gtk_clipboard_wait_is_image_available(clipboard);
    DBG("Clipboard has image: %d", has_image);

    if (!has_image) {
        /* Also check targets for debugging */
        GdkAtom *targets = NULL;
        gint n_targets = 0;
        if (gtk_clipboard_wait_for_targets(clipboard, &targets, &n_targets)) {
            GString *tnames = g_string_new("Clipboard targets:");
            for (gint i = 0; i < n_targets && i < 15; i++) {
                gchar *name = gdk_atom_name(targets[i]);
                g_string_append_printf(tnames, " %s", name);
                g_free(name);
            }
            DBG("%s", tnames->str);
            g_string_free(tnames, TRUE);
            g_free(targets);
        }
        return;  /* No image — let default text paste happen */
    }

    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(clipboard);
    if (!pixbuf) {
        DBG("Image available but pixbuf is NULL");
        return;
    }

    DBG("Image: %dx%d", gdk_pixbuf_get_width(pixbuf),
        gdk_pixbuf_get_height(pixbuf));

    /* Encode as PNG base64 */
    gchar *png_data = NULL;
    gsize png_len = 0;
    GError *error = NULL;
    gdk_pixbuf_save_to_buffer(pixbuf, &png_data, &png_len, "png", &error, NULL);
    if (error) {
        DBG("PNG encode error: %s", error->message);
        g_error_free(error);
        g_object_unref(pixbuf);
        return;
    }

    DBG("PNG encoded: %lu bytes", (unsigned long)png_len);
    gchar *b64 = g_base64_encode((guchar *)png_data, png_len);
    g_free(png_data);

    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (priv)
        add_image_chip(vbox, priv, pixbuf, b64);
    else
        g_free(b64);

    g_object_unref(pixbuf);

    /* Stop the default paste handler from inserting the image as text */
    g_signal_stop_emission_by_name(text_view, "paste-clipboard");
    DBG("Image chip added, paste suppressed");
}

/* ── Slash command completion ─────────────────────────────────────── */

typedef struct {
    gchar *name;
    gchar *description;
} CommandInfo;

static void free_command_info(gpointer data)
{
    CommandInfo *c = data;
    g_free(c->name);
    g_free(c->description);
    g_free(c);
}

static void on_cmd_item_activate(GtkMenuItem *item, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return;

    const gchar *cmd = g_object_get_data(G_OBJECT(item), "cmd-name");
    if (cmd) {
        gchar *text = g_strdup_printf("/%s ", cmd);
        gtk_text_buffer_set_text(priv->buffer, text, -1);
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(priv->buffer, &end);
        gtk_text_buffer_place_cursor(priv->buffer, &end);
        g_free(text);
    }

    gtk_widget_grab_focus(priv->text_view);
}

/* Forward key presses from the command menu to the text buffer so the user
 * can keep typing to narrow the filter while the popup is visible. */
static gboolean on_cmd_menu_key_press(GtkWidget *widget, GdkEventKey *event,
                                       gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return FALSE;

    guint kv = event->keyval;

    /* Escape — close menu, refocus text view */
    if (kv == GDK_KEY_Escape) {
        gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        gtk_widget_grab_focus(priv->text_view);
        return TRUE;
    }

    /* Enter — activate the selected item */
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
        GtkWidget *selected = gtk_menu_shell_get_selected_item(
            GTK_MENU_SHELL(widget));
        if (selected) {
            gtk_menu_item_activate(GTK_MENU_ITEM(selected));
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        }
        return TRUE;
    }

    /* Up/Down — let the menu handle navigation */
    if (kv == GDK_KEY_Up || kv == GDK_KEY_Down ||
        kv == GDK_KEY_KP_Up || kv == GDK_KEY_KP_Down)
        return FALSE;

    /* Tab — accept selected item (same as Enter) */
    if (kv == GDK_KEY_Tab) {
        GtkWidget *selected = gtk_menu_shell_get_selected_item(
            GTK_MENU_SHELL(widget));
        if (selected) {
            gtk_menu_item_activate(GTK_MENU_ITEM(selected));
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        }
        return TRUE;
    }

    /* Backspace — delete a character from the buffer */
    if (kv == GDK_KEY_BackSpace) {
        GtkTextMark *mark = gtk_text_buffer_get_insert(priv->buffer);
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(priv->buffer, &cursor, mark);
        if (!gtk_text_iter_is_start(&cursor)) {
            GtkTextIter prev = cursor;
            gtk_text_iter_backward_char(&prev);
            gtk_text_buffer_delete(priv->buffer, &prev, &cursor);
        }
        return TRUE;
    }

    /* Printable characters — insert into text buffer */
    gunichar ch = gdk_keyval_to_unicode(kv);
    if (ch && g_unichar_isprint(ch)) {
        gchar utf8[7];
        gint len = g_unichar_to_utf8(ch, utf8);
        utf8[len] = '\0';

        GtkTextMark *mark = gtk_text_buffer_get_insert(priv->buffer);
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(priv->buffer, &cursor, mark);
        gtk_text_buffer_insert(priv->buffer, &cursor, utf8, len);
        return TRUE;
    }

    return FALSE;
}

static void show_cmd_menu(ChatInputPrivate *priv, GtkWidget *vbox,
                           const gchar *prefix)
{
    if (!priv->commands) return;

    if (priv->cmd_menu)
        gtk_widget_destroy(priv->cmd_menu);

    priv->cmd_menu = gtk_menu_new();
    gint count = 0;

    for (GList *l = priv->commands; l; l = l->next) {
        CommandInfo *cmd = l->data;
        if (prefix && strlen(prefix) > 0 &&
            !g_str_has_prefix(cmd->name, prefix))
            continue;

        gchar *label;
        if (cmd->description && strlen(cmd->description) > 0) {
            gchar *desc = g_strndup(cmd->description, 40);
            if (strlen(cmd->description) > 40) {
                gchar *trunc = g_strdup_printf("%s...", desc);
                g_free(desc);
                desc = trunc;
            }
            label = g_strdup_printf("/%s — %s", cmd->name, desc);
            g_free(desc);
        } else {
            label = g_strdup_printf("/%s", cmd->name);
        }

        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_free(label);
        g_object_set_data_full(G_OBJECT(item), "cmd-name",
                               g_strdup(cmd->name), g_free);
        g_signal_connect(item, "activate",
                         G_CALLBACK(on_cmd_item_activate), vbox);
        gtk_menu_shell_append(GTK_MENU_SHELL(priv->cmd_menu), item);
        count++;
    }

    if (count > 0) {
        g_signal_connect(priv->cmd_menu, "key-press-event",
                         G_CALLBACK(on_cmd_menu_key_press), vbox);
        gtk_widget_show_all(priv->cmd_menu);
        gtk_menu_popup_at_widget(GTK_MENU(priv->cmd_menu),
                                 priv->text_view,
                                 GDK_GRAVITY_NORTH_WEST,
                                 GDK_GRAVITY_SOUTH_WEST,
                                 NULL);
    }
}

/* ── @ file completion ────────────────────────────────────────────── */

static const gchar *SKIP_DIRS[] = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", ".cache",
    "build", "_build", ".build", "dist", "target", ".next", ".nuxt",
    "vendor", ".tox", ".eggs", "venv", ".venv", ".mypy_cache",
    NULL
};

static gboolean should_skip_dir(const gchar *name)
{
    for (const gchar **d = SKIP_DIRS; *d; d++) {
        if (g_strcmp0(name, *d) == 0)
            return TRUE;
    }
    return name[0] == '.';  /* skip hidden dirs */
}

static void scan_dir_recursive(const gchar *base, const gchar *rel_prefix,
                                GList **out, gint depth)
{
    if (depth > 8)
        return;

    gchar *full = rel_prefix[0]
        ? g_build_filename(base, rel_prefix, NULL)
        : g_strdup(base);

    GDir *dir = g_dir_open(full, 0, NULL);
    g_free(full);
    if (!dir)
        return;

    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *rel = rel_prefix[0]
            ? g_build_filename(rel_prefix, name, NULL)
            : g_strdup(name);
        gchar *abs = g_build_filename(base, rel, NULL);

        if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
            if (!should_skip_dir(name))
                scan_dir_recursive(base, rel, out, depth + 1);
        } else {
            *out = g_list_prepend(*out, rel);
            rel = NULL;  /* ownership transferred */
        }

        g_free(abs);
        g_free(rel);
    }
    g_dir_close(dir);
}

static void scan_project_files(ChatInputPrivate *priv)
{
    g_list_free_full(priv->project_files, g_free);
    priv->project_files = NULL;

    if (!priv->project_root)
        return;

    scan_dir_recursive(priv->project_root, "", &priv->project_files, 0);
    priv->project_files = g_list_sort(priv->project_files,
                                       (GCompareFunc)g_strcmp0);
    DBG("Scanned %u project files", g_list_length(priv->project_files));
}

/* Find the @token at or before the cursor. Returns the text after '@',
 * and sets *at_offset to the byte offset of the '@' in the buffer text.
 * Returns NULL if no active @token found. Caller must g_free result. */
static gchar *find_at_token(GtkTextBuffer *buffer, gint *at_offset)
{
    GtkTextIter cursor;
    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, mark);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &cursor, FALSE);
    if (!text)
        return NULL;

    /* Scan backwards from end to find the last '@' that isn't preceded
     * by an alphanumeric char (i.e. it's at word boundary or start) */
    gint len = strlen(text);
    gint at_pos = -1;
    for (gint i = len - 1; i >= 0; i--) {
        if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')
            break;  /* hit whitespace before finding @, no token */
        if (text[i] == '@') {
            /* @ must be at start of text or preceded by whitespace */
            if (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\n' ||
                text[i - 1] == '\t') {
                at_pos = i;
            }
            break;
        }
    }

    if (at_pos < 0) {
        g_free(text);
        return NULL;
    }

    *at_offset = at_pos;
    gchar *prefix = g_strdup(text + at_pos + 1);
    g_free(text);
    return prefix;
}

static void on_file_item_activate(GtkMenuItem *item, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return;

    const gchar *rel_path = g_object_get_data(G_OBJECT(item), "file-path");
    if (!rel_path) return;

    /* Find the @token position and replace it */
    gint at_offset = 0;
    gchar *old_prefix = find_at_token(priv->buffer, &at_offset);
    if (!old_prefix) return;

    /* Get full buffer text */
    GtkTextIter buf_start, buf_end;
    gtk_text_buffer_get_bounds(priv->buffer, &buf_start, &buf_end);
    gchar *full_text = gtk_text_buffer_get_text(priv->buffer,
                                                 &buf_start, &buf_end, FALSE);

    /* Build new text: [before @] + @path + [after @prefix] */
    gint prefix_len = strlen(old_prefix);
    gint replace_end = at_offset + 1 + prefix_len;  /* @ + prefix */
    gchar *replacement = g_strdup_printf("@%s ", rel_path);

    GString *new_text = g_string_new("");
    g_string_append_len(new_text, full_text, at_offset);
    g_string_append(new_text, replacement);
    g_string_append(new_text, full_text + replace_end);

    gint cursor_pos = at_offset + strlen(replacement);

    gtk_text_buffer_set_text(priv->buffer, new_text->str, -1);

    /* Place cursor after the inserted path */
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_offset(priv->buffer, &cursor, cursor_pos);
    gtk_text_buffer_place_cursor(priv->buffer, &cursor);

    g_free(old_prefix);
    g_free(full_text);
    g_free(replacement);
    g_string_free(new_text, TRUE);

    gtk_widget_grab_focus(priv->text_view);
}

static gboolean file_matches_prefix(const gchar *rel_path, const gchar *prefix)
{
    if (!prefix || prefix[0] == '\0')
        return TRUE;

    /* Match against relative path (case-insensitive) */
    gchar *path_lower = g_utf8_strdown(rel_path, -1);
    gchar *prefix_lower = g_utf8_strdown(prefix, -1);
    gboolean match = strstr(path_lower, prefix_lower) != NULL;
    g_free(path_lower);
    g_free(prefix_lower);
    return match;
}

/* Forward key presses from the file menu to the text buffer so the user
 * can keep typing to narrow the filter while the popup is visible. */
static gboolean on_file_menu_key_press(GtkWidget *widget, GdkEventKey *event,
                                        gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return FALSE;

    guint kv = event->keyval;

    /* Escape — close menu, refocus text view */
    if (kv == GDK_KEY_Escape) {
        gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        gtk_widget_grab_focus(priv->text_view);
        return TRUE;
    }

    /* Enter — activate the selected item */
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
        GtkWidget *selected = gtk_menu_shell_get_selected_item(
            GTK_MENU_SHELL(widget));
        if (selected) {
            gtk_menu_item_activate(GTK_MENU_ITEM(selected));
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        }
        return TRUE;
    }

    /* Up/Down — let the menu handle navigation */
    if (kv == GDK_KEY_Up || kv == GDK_KEY_Down ||
        kv == GDK_KEY_KP_Up || kv == GDK_KEY_KP_Down)
        return FALSE;

    /* Tab — accept selected item (same as Enter) */
    if (kv == GDK_KEY_Tab) {
        GtkWidget *selected = gtk_menu_shell_get_selected_item(
            GTK_MENU_SHELL(widget));
        if (selected) {
            gtk_menu_item_activate(GTK_MENU_ITEM(selected));
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(widget));
        }
        return TRUE;
    }

    /* Backspace — delete a character from the buffer */
    if (kv == GDK_KEY_BackSpace) {
        GtkTextMark *mark = gtk_text_buffer_get_insert(priv->buffer);
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(priv->buffer, &cursor, mark);
        if (!gtk_text_iter_is_start(&cursor)) {
            GtkTextIter prev = cursor;
            gtk_text_iter_backward_char(&prev);
            gtk_text_buffer_delete(priv->buffer, &prev, &cursor);
        }
        return TRUE;
    }

    /* Printable characters — insert into text buffer */
    gunichar ch = gdk_keyval_to_unicode(kv);
    if (ch && g_unichar_isprint(ch)) {
        gchar utf8[7];
        gint len = g_unichar_to_utf8(ch, utf8);
        utf8[len] = '\0';

        GtkTextMark *mark = gtk_text_buffer_get_insert(priv->buffer);
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(priv->buffer, &cursor, mark);
        gtk_text_buffer_insert(priv->buffer, &cursor, utf8, len);
        return TRUE;
    }

    return FALSE;
}

static void show_file_menu(ChatInputPrivate *priv, GtkWidget *vbox,
                            const gchar *prefix)
{
    if (!priv->project_files) return;

    if (priv->file_menu)
        gtk_widget_destroy(priv->file_menu);

    priv->file_menu = gtk_menu_new();
    gint count = 0;
    const gint MAX_ITEMS = 20;

    for (GList *l = priv->project_files; l && count < MAX_ITEMS; l = l->next) {
        const gchar *rel_path = l->data;
        if (!file_matches_prefix(rel_path, prefix))
            continue;

        GtkWidget *item = gtk_menu_item_new_with_label(rel_path);
        g_object_set_data_full(G_OBJECT(item), "file-path",
                               g_strdup(rel_path), g_free);
        g_signal_connect(item, "activate",
                         G_CALLBACK(on_file_item_activate), vbox);
        gtk_menu_shell_append(GTK_MENU_SHELL(priv->file_menu), item);
        count++;
    }

    if (count > 0) {
        g_signal_connect(priv->file_menu, "key-press-event",
                         G_CALLBACK(on_file_menu_key_press), vbox);
        gtk_widget_show_all(priv->file_menu);
        gtk_menu_popup_at_widget(GTK_MENU(priv->file_menu),
                                 priv->text_view,
                                 GDK_GRAVITY_NORTH_WEST,
                                 GDK_GRAVITY_SOUTH_WEST,
                                 NULL);
    }
}

/* ── Buffer change handler (slash + file completion) ─────────────── */

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer data)
{
    GtkWidget *vbox = GTK_WIDGET(data);
    ChatInputPrivate *priv = get_priv(vbox);
    if (!priv) return;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    /* Slash command completion */
    if (priv->commands && text && text[0] == '/' &&
        !strchr(text, ' ') && !strchr(text, '\n')) {
        show_cmd_menu(priv, vbox, text + 1);
    } else {
        if (priv->cmd_menu) {
            gtk_widget_destroy(priv->cmd_menu);
            priv->cmd_menu = NULL;
        }
    }

    /* @ file completion */
    gint at_offset = 0;
    gchar *at_prefix = find_at_token(buffer, &at_offset);
    if (at_prefix) {
        show_file_menu(priv, vbox, at_prefix);
        g_free(at_prefix);
    } else {
        if (priv->file_menu) {
            gtk_widget_destroy(priv->file_menu);
            priv->file_menu = NULL;
        }
    }

    g_free(text);
}

/* Handle key presses — Enter sends, Shift+Enter newline */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                              gpointer data)
{
    (void)widget;
    if ((event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) &&
        !(event->state & GDK_SHIFT_MASK)) {
        on_send_clicked(NULL, data);
        return TRUE;
    }
    return FALSE;
}

/* ── Construction ────────────────────────────────────────────────── */

GtkWidget *chat_input_new(void)
{
    ChatInputPrivate *priv = g_new0(ChatInputPrivate, 1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_object_set_data_full(G_OBJECT(vbox), INPUT_PRIV_KEY, priv, g_free);

    /* Todos panel (hidden until active) */
    priv->todos_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(priv->todos_box, 4);
    gtk_widget_set_margin_end(priv->todos_box, 4);
    gtk_widget_set_margin_bottom(priv->todos_box, 4);
    gtk_widget_set_no_show_all(priv->todos_box, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), priv->todos_box, FALSE, FALSE, 0);

    /* Image chips container (hidden until images are pasted) */
    priv->chips_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(priv->chips_box, 4);
    gtk_widget_set_margin_bottom(priv->chips_box, 2);
    gtk_widget_set_no_show_all(priv->chips_box, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), priv->chips_box, FALSE, FALSE, 0);

    /* Mode selector (above text entry) */
    priv->mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->mode_combo),
                              "default", "Default");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->mode_combo),
                              "plan", "Plan");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->mode_combo),
                              "acceptEdits", "Accept Edits");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->mode_combo),
                              "dontAsk", "Don't Ask");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->mode_combo),
                              "auto", "Auto");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(priv->mode_combo), "default");
    g_signal_connect(priv->mode_combo, "changed",
                     G_CALLBACK(on_mode_combo_changed), vbox);

    /* Model selector (populated from initialize response) */
    priv->model_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->model_combo),
                              "default", "Default");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(priv->model_combo), "default");
    g_signal_connect(priv->model_combo, "changed",
                     G_CALLBACK(on_model_combo_changed), vbox);

    GtkWidget *mode_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(mode_row), priv->mode_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_row), priv->model_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_row), gtk_label_new(""), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), mode_row, FALSE, FALSE, 0);

    /* Text input row: [text entry] [stop] [send] */
    GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    priv->text_view = gtk_text_view_new();
    priv->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(priv->text_view));
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(priv->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(priv->text_view), FALSE);
    gtk_widget_set_tooltip_text(priv->text_view,
                                "Enter to send, Shift+Enter for newline");

    gtk_widget_set_size_request(priv->text_view, -1, 60);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 120);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), priv->text_view);
    gtk_box_pack_start(GTK_BOX(input_row), scroll, TRUE, TRUE, 0);

    /* Stop button (always visible, disabled when idle) */
    priv->stop_btn = gtk_button_new_from_icon_name(
        "process-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(priv->stop_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(priv->stop_btn, "Stop generation");
    gtk_widget_set_sensitive(priv->stop_btn, FALSE);
    g_signal_connect(priv->stop_btn, "clicked",
                     G_CALLBACK(on_stop_clicked), vbox);
    gtk_box_pack_start(GTK_BOX(input_row), priv->stop_btn, FALSE, FALSE, 0);

    /* Send button (icon) */
    priv->send_btn = gtk_button_new_from_icon_name(
        "mail-send", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(priv->send_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(priv->send_btn, "Send (Enter)");
    g_signal_connect(priv->send_btn, "clicked",
                     G_CALLBACK(on_send_clicked), vbox);
    gtk_box_pack_start(GTK_BOX(input_row), priv->send_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), input_row, FALSE, FALSE, 0);

    /* Key handler */
    g_signal_connect(priv->text_view, "key-press-event",
                     G_CALLBACK(on_key_press), vbox);

    /* Intercept paste to detect images */
    g_signal_connect(priv->text_view, "paste-clipboard",
                     G_CALLBACK(on_paste_clipboard), vbox);

    /* Monitor text changes for '/' prefix (slash command menu) */
    g_signal_connect(priv->buffer, "changed",
                     G_CALLBACK(on_buffer_changed), vbox);

    return vbox;
}

/* ── Public API ──────────────────────────────────────────────────── */

const gchar *chat_input_get_text(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return "";

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(priv->buffer, &start, &end);
    return gtk_text_buffer_get_text(priv->buffer, &start, &end, FALSE);
}

void chat_input_set_text(GtkWidget *input, const gchar *text)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv)
        gtk_text_buffer_set_text(priv->buffer, text ? text : "", -1);
}

void chat_input_clear(GtkWidget *input)
{
    chat_input_set_text(input, "");
}

void chat_input_grab_focus(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv)
        gtk_widget_grab_focus(priv->text_view);
}

void chat_input_set_mode(GtkWidget *input, const gchar *mode_id)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv && mode_id) {
        priv->mode_update_internal = TRUE;
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(priv->mode_combo), mode_id);
        priv->mode_update_internal = FALSE;
    }
}

gchar *chat_input_get_mode(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return g_strdup("default");
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(priv->mode_combo));
    return g_strdup(id ? id : "default");
}

void chat_input_add_image(GtkWidget *input, GdkPixbuf *pixbuf, gchar *b64_data)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) { g_free(b64_data); return; }
    add_image_chip(input, priv, pixbuf, b64_data);
}

GList *chat_input_take_images(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return NULL;

    GList *images = priv->images;
    priv->images = NULL;

    /* Clear chips display */
    GList *children = gtk_container_get_children(GTK_CONTAINER(priv->chips_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);
    update_chips_visibility(priv);

    return images;
}

void chat_input_add_context(GtkWidget *input, const gchar *file_path,
                             const gchar *content,
                             gint start_line, gint end_line)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return;

    ContextChunk *chunk = g_new0(ContextChunk, 1);
    chunk->file_path = g_strdup(file_path);
    chunk->content = g_strdup(content);
    chunk->start_line = start_line;
    chunk->end_line = end_line;

    add_context_chip(input, priv, chunk);
}

GList *chat_input_take_contexts(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return NULL;

    GList *contexts = priv->contexts;
    priv->contexts = NULL;

    /* Remove context chips from display (keep image chips) */
    GList *children = gtk_container_get_children(GTK_CONTAINER(priv->chips_box));
    for (GList *c = children; c; c = c->next) {
        GtkWidget *child = GTK_WIDGET(c->data);
        if (gtk_style_context_has_class(
                gtk_widget_get_style_context(child), "context-chip"))
            gtk_widget_destroy(child);
    }
    g_list_free(children);
    update_chips_visibility(priv);

    return contexts;
}

static gboolean on_todos_header_click(GtkWidget *w, GdkEventButton *ev,
                                       gpointer data)
{
    (void)w; (void)ev;
    ChatInputPrivate *priv = data;
    gboolean revealed = gtk_revealer_get_reveal_child(
        GTK_REVEALER(priv->todos_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(priv->todos_revealer), !revealed);
    gtk_label_set_text(GTK_LABEL(priv->todos_arrow), revealed ? "\u25B6" : "\u25BC");
    return TRUE;
}

void chat_input_update_todos(GtkWidget *input, const gchar *todos_json)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return;

    /* Clear existing todos */
    GList *children = gtk_container_get_children(GTK_CONTAINER(priv->todos_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);
    priv->todos_revealer = NULL;
    priv->todos_arrow = NULL;

    JsonParser *jp = json_parser_new();
    if (!todos_json || !json_parser_load_from_data(jp, todos_json, -1, NULL)) {
        gtk_widget_hide(priv->todos_box);
        gtk_widget_set_no_show_all(priv->todos_box, TRUE);
        g_object_unref(jp);
        return;
    }

    JsonArray *arr = json_node_get_array(json_parser_get_root(jp));
    guint n = json_array_get_length(arr);
    if (n == 0) {
        gtk_widget_hide(priv->todos_box);
        gtk_widget_set_no_show_all(priv->todos_box, TRUE);
        g_object_unref(jp);
        return;
    }

    /* Count completed */
    guint completed = 0;
    for (guint i = 0; i < n; i++) {
        JsonObject *todo = json_array_get_object_element(arr, i);
        const gchar *status = json_object_get_string_member(todo, "status");
        if (g_strcmp0(status, "completed") == 0) completed++;
    }

    /* Clickable header */
    GtkWidget *header_event = gtk_event_box_new();
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    priv->todos_arrow = gtk_label_new("\u25BC");
    gtk_box_pack_start(GTK_BOX(header_box), priv->todos_arrow, FALSE, FALSE, 0);

    gchar *header_text = g_strdup_printf("Tasks (%u/%u)", completed, n);
    GtkWidget *header_label = gtk_label_new(header_text);
    gtk_label_set_xalign(GTK_LABEL(header_label), 0);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(0.85));
    gtk_label_set_attributes(GTK_LABEL(header_label), attrs);
    pango_attr_list_unref(attrs);
    g_free(header_text);
    gtk_box_pack_start(GTK_BOX(header_box), header_label, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(header_event), header_box);
    g_signal_connect(header_event, "button-press-event",
                     G_CALLBACK(on_todos_header_click), priv);
    gtk_box_pack_start(GTK_BOX(priv->todos_box), header_event, FALSE, FALSE, 0);

    /* Revealer with items */
    priv->todos_revealer = gtk_revealer_new();
    gtk_revealer_set_reveal_child(GTK_REVEALER(priv->todos_revealer), TRUE);
    gtk_revealer_set_transition_type(GTK_REVEALER(priv->todos_revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);

    GtkWidget *items_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    for (guint i = 0; i < n; i++) {
        JsonObject *todo = json_array_get_object_element(arr, i);
        const gchar *content = json_object_get_string_member(todo, "content");
        const gchar *status = json_object_get_string_member(todo, "status");

        const gchar *icon = "\u25CB";
        if (g_strcmp0(status, "completed") == 0) icon = "\u2713";
        else if (g_strcmp0(status, "in_progress") == 0) icon = "\u27F3";

        GtkWidget *item_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start(item_box, 8);

        GtkWidget *icon_label = gtk_label_new(icon);
        if (g_strcmp0(status, "completed") == 0) {
            PangoAttrList *a = pango_attr_list_new();
            pango_attr_list_insert(a, pango_attr_foreground_new(11796, 52428, 11796));
            gtk_label_set_attributes(GTK_LABEL(icon_label), a);
            pango_attr_list_unref(a);
        } else if (g_strcmp0(status, "in_progress") == 0) {
            PangoAttrList *a = pango_attr_list_new();
            pango_attr_list_insert(a, pango_attr_foreground_new(19532, 35980, 61166));
            gtk_label_set_attributes(GTK_LABEL(icon_label), a);
            pango_attr_list_unref(a);
        }

        GtkWidget *text_label = gtk_label_new(content);
        gtk_label_set_xalign(GTK_LABEL(text_label), 0);
        gtk_label_set_line_wrap(GTK_LABEL(text_label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(text_label), 1);
        gtk_widget_set_hexpand(text_label, TRUE);

        PangoAttrList *ta = pango_attr_list_new();
        pango_attr_list_insert(ta, pango_attr_scale_new(0.85));
        if (g_strcmp0(status, "completed") == 0) {
            pango_attr_list_insert(ta, pango_attr_strikethrough_new(TRUE));
            pango_attr_list_insert(ta, pango_attr_foreground_alpha_new(32768));
        }
        gtk_label_set_attributes(GTK_LABEL(text_label), ta);
        pango_attr_list_unref(ta);

        gtk_box_pack_start(GTK_BOX(item_box), icon_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(item_box), text_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(items_box), item_box, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(priv->todos_revealer), items_box);
    gtk_box_pack_start(GTK_BOX(priv->todos_box), priv->todos_revealer, FALSE, FALSE, 0);

    gtk_widget_set_no_show_all(priv->todos_box, FALSE);
    gtk_widget_show_all(priv->todos_box);

    g_object_unref(jp);
}

void chat_input_set_commands(GtkWidget *input, const gchar *commands_json)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv || !commands_json) return;

    /* Free old commands */
    g_list_free_full(priv->commands, free_command_info);
    priv->commands = NULL;

    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, commands_json, -1, NULL)) {
        g_object_unref(p);
        return;
    }

    JsonArray *arr = json_node_get_array(json_parser_get_root(p));
    if (!arr) { g_object_unref(p); return; }

    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(arr, i);
        CommandInfo *cmd = g_new0(CommandInfo, 1);
        cmd->name = g_strdup(json_object_has_member(obj, "name")
            ? json_object_get_string_member(obj, "name") : "");
        cmd->description = g_strdup(json_object_has_member(obj, "description")
            ? json_object_get_string_member(obj, "description") : "");
        priv->commands = g_list_append(priv->commands, cmd);
    }

    g_object_unref(p);
    DBG("Loaded %u slash commands", len);
}

void chat_input_set_project_root(GtkWidget *input, const gchar *root)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return;

    /* Skip if unchanged */
    if (g_strcmp0(priv->project_root, root) == 0)
        return;

    g_free(priv->project_root);
    priv->project_root = g_strdup(root);
    scan_project_files(priv);
}

void chat_input_set_models(GtkWidget *input, const gchar *models_json)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv || !models_json) return;

    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, models_json, -1, NULL)) {
        g_object_unref(p);
        return;
    }

    JsonArray *arr = json_node_get_array(json_parser_get_root(p));
    if (!arr) { g_object_unref(p); return; }

    /* Remember current selection */
    const gchar *current = gtk_combo_box_get_active_id(
        GTK_COMBO_BOX(priv->model_combo));
    gchar *saved = g_strdup(current ? current : "default");

    /* Clear and repopulate */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(priv->model_combo));

    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *m = json_array_get_object_element(arr, i);
        const gchar *val = json_object_has_member(m, "value")
            ? json_object_get_string_member(m, "value") : "";
        const gchar *display = json_object_has_member(m, "displayName")
            ? json_object_get_string_member(m, "displayName") : val;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(priv->model_combo),
                                  val, display);
    }

    /* Restore selection */
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(priv->model_combo), saved))
        gtk_combo_box_set_active(GTK_COMBO_BOX(priv->model_combo), 0);

    g_free(saved);
    g_object_unref(p);
}

void chat_input_set_model(GtkWidget *input, const gchar *model_value)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv && model_value)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(priv->model_combo), model_value);
}

gchar *chat_input_get_model(GtkWidget *input)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return g_strdup("default");
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(priv->model_combo));
    return g_strdup(id ? id : "default");
}

void chat_input_set_busy(GtkWidget *input, gboolean busy)
{
    ChatInputPrivate *priv = get_priv(input);
    if (!priv) return;

    gtk_widget_set_sensitive(priv->send_btn, !busy);
    gtk_widget_set_sensitive(priv->stop_btn, busy);
}

void chat_input_set_send_callback(GtkWidget *input, ChatInputSendCb cb,
                                  gpointer user_data)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv) {
        priv->send_cb = cb;
        priv->send_data = user_data;
    }
}

void chat_input_set_stop_callback(GtkWidget *input, ChatInputStopCb cb,
                                  gpointer user_data)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv) {
        priv->stop_cb = cb;
        priv->stop_data = user_data;
    }
}

void chat_input_set_mode_changed_callback(GtkWidget *input,
                                           ChatInputModeChangedCb cb,
                                           gpointer user_data)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv) {
        priv->mode_changed_cb = cb;
        priv->mode_changed_data = user_data;
    }
}

void chat_input_set_model_changed_callback(GtkWidget *input,
                                            ChatInputModelChangedCb cb,
                                            gpointer user_data)
{
    ChatInputPrivate *priv = get_priv(input);
    if (priv) {
        priv->model_changed_cb = cb;
        priv->model_changed_data = user_data;
    }
}
