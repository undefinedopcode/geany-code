/*
 * chat_webview.c — Native GTK3 chat renderer (replaces WebKit2GTK)
 *
 * Uses GtkListBox + GtkLabel (Pango markup) + GtkSourceView for
 * a cross-platform chat view with no web engine dependency.
 */

#include "chat_webview.h"
#include "editor_dbus.h"
#include <gtksourceview/gtksource.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ── Private data ────────────────────────────────────────────────── */

typedef struct {
    gchar       *id;
    gchar       *role;
    GtkWidget   *row;
    GtkWidget   *content_box;   /* GtkBox(V) rebuilt on streaming update */
    gchar       *last_content;  /* for diff-based streaming optimization */
} MessageEntry;

typedef struct {
    gchar       *tool_id;
    gchar       *tool_name;
    gchar       *file_path;
    GtkWidget   *row;
    GtkWidget   *arrow_label;
    GtkWidget   *status_label;
    GtkWidget   *revealer;
    GtkWidget   *body_box;
    gboolean     expanded;
} ToolEntry;

typedef struct {
    GtkWidget   *scroll;
    GtkWidget   *outer_box;
    GtkWidget   *list_box;
    GtkWidget   *welcome;
    GtkWidget   *todos_box;
    GHashTable  *msg_widgets;    /* id → MessageEntry* */
    GHashTable  *tool_widgets;   /* tool_id → ToolEntry* */
    ChatWebViewPermissionCb permission_cb;
    gpointer    permission_data;
    ChatWebViewJumpToEditCb jump_cb;
    gpointer    jump_data;
    GtkCssProvider *css;
    gdouble     last_scroll_max; /* for auto-scroll tracking */
} ChatViewPrivate;

static const gchar *PRIV_KEY = "geany-code-chatview-priv";

static ChatViewPrivate *get_priv(GtkWidget *w)
{
    return g_object_get_data(G_OBJECT(w), PRIV_KEY);
}

/* ── Cleanup helpers ─────────────────────────────────────────────── */

static void free_message_entry(gpointer p)
{
    MessageEntry *e = p;
    g_free(e->id);
    g_free(e->role);
    g_free(e->last_content);
    g_free(e);
}

static void free_tool_entry(gpointer p)
{
    ToolEntry *e = p;
    g_free(e->tool_id);
    g_free(e->tool_name);
    g_free(e->file_path);
    g_free(e);
}

/* ── Auto-scroll ─────────────────────────────────────────────────── */

static gboolean do_scroll_to_bottom(gpointer data)
{
    GtkAdjustment *adj = GTK_ADJUSTMENT(data);
    gtk_adjustment_set_value(adj,
        gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    return G_SOURCE_REMOVE;
}

static void scroll_to_bottom(ChatViewPrivate *priv)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(priv->scroll));
    g_idle_add(do_scroll_to_bottom, adj);
}

/* ── Markdown → Pango conversion ─────────────────────────────────── */

/* Segment types for markdown splitting */
typedef enum { SEG_TEXT, SEG_CODE } SegType;
typedef struct {
    SegType  type;
    gchar   *text;
    gchar   *lang;  /* for SEG_CODE */
} Segment;

static void free_segment(gpointer p)
{
    Segment *s = p;
    g_free(s->text);
    g_free(s->lang);
    g_free(s);
}

/* Split markdown into text and fenced code block segments */
static GList *split_segments(const gchar *content)
{
    GList *segs = NULL;
    GString *buf = g_string_new("");
    gchar **lines = g_strsplit(content, "\n", -1);
    gboolean in_code = FALSE;
    gchar *code_lang = NULL;

    for (gchar **lp = lines; *lp; lp++) {
        const gchar *line = *lp;

        if (g_str_has_prefix(line, "```")) {
            if (!in_code) {
                /* Start of code block — flush text */
                if (buf->len > 0) {
                    Segment *s = g_new0(Segment, 1);
                    s->type = SEG_TEXT;
                    s->text = g_string_free(buf, FALSE);
                    segs = g_list_append(segs, s);
                    buf = g_string_new("");
                }
                code_lang = g_strdup(line + 3);
                g_strstrip(code_lang);
                if (strlen(code_lang) == 0) {
                    g_free(code_lang);
                    code_lang = NULL;
                }
                in_code = TRUE;
            } else {
                /* End of code block */
                Segment *s = g_new0(Segment, 1);
                s->type = SEG_CODE;
                s->text = g_string_free(buf, FALSE);
                s->lang = code_lang;
                code_lang = NULL;
                segs = g_list_append(segs, s);
                buf = g_string_new("");
                in_code = FALSE;
            }
        } else {
            if (buf->len > 0)
                g_string_append_c(buf, '\n');
            g_string_append(buf, line);
        }
    }

    /* Flush remaining */
    if (buf->len > 0) {
        Segment *s = g_new0(Segment, 1);
        s->type = in_code ? SEG_CODE : SEG_TEXT;
        s->text = g_string_free(buf, FALSE);
        s->lang = code_lang;
        segs = g_list_append(segs, s);
    } else {
        g_string_free(buf, TRUE);
        g_free(code_lang);
    }

    g_strfreev(lines);
    return segs;
}

/* Convert a subset of markdown to Pango markup */
static gchar *md_to_pango(const gchar *text)
{
    /* First escape for Pango safety */
    gchar *escaped = g_markup_escape_text(text, -1);
    GString *out = g_string_new("");

    gchar **lines = g_strsplit(escaped, "\n", -1);

    for (gchar **lp = lines; *lp; lp++) {
        const gchar *line = *lp;

        if (out->len > 0)
            g_string_append_c(out, '\n');

        /* Headings */
        if (g_str_has_prefix(line, "### ")) {
            g_string_append_printf(out,
                "<span weight=\"bold\">%s</span>", line + 4);
            continue;
        }
        if (g_str_has_prefix(line, "## ")) {
            g_string_append_printf(out,
                "<span size=\"large\" weight=\"bold\">%s</span>", line + 3);
            continue;
        }
        if (g_str_has_prefix(line, "# ")) {
            g_string_append_printf(out,
                "<span size=\"x-large\" weight=\"bold\">%s</span>", line + 2);
            continue;
        }

        /* Bullet lists */
        if (g_str_has_prefix(line, "- ") || g_str_has_prefix(line, "* ")) {
            g_string_append(out, "  \u2022 ");
            line += 2;
        }

        /* Process inline formatting within the line */
        const gchar *p = line;
        while (*p) {
            /* Bold: **text** */
            if (p[0] == '*' && p[1] == '*') {
                const gchar *end = strstr(p + 2, "**");
                if (end) {
                    g_string_append(out, "<b>");
                    g_string_append_len(out, p + 2, end - (p + 2));
                    g_string_append(out, "</b>");
                    p = end + 2;
                    continue;
                }
            }
            /* Italic: *text* (but not **) */
            if (p[0] == '*' && p[1] != '*') {
                const gchar *end = strchr(p + 1, '*');
                if (end && end != p + 1) {
                    g_string_append(out, "<i>");
                    g_string_append_len(out, p + 1, end - (p + 1));
                    g_string_append(out, "</i>");
                    p = end + 1;
                    continue;
                }
            }
            /* Inline code: `text` */
            if (p[0] == '`' && p[1] != '`') {
                const gchar *end = strchr(p + 1, '`');
                if (end) {
                    g_string_append(out, "<tt>");
                    g_string_append_len(out, p + 1, end - (p + 1));
                    g_string_append(out, "</tt>");
                    p = end + 1;
                    continue;
                }
            }
            /* Link: [text](url) */
            if (p[0] == '[') {
                const gchar *close = strchr(p + 1, ']');
                if (close && close[1] == '(') {
                    const gchar *url_end = strchr(close + 2, ')');
                    if (url_end) {
                        gchar *text_part = g_strndup(p + 1, close - (p + 1));
                        gchar *url_part = g_strndup(close + 2, url_end - (close + 2));
                        g_string_append_printf(out,
                            "<a href=\"%s\">%s</a>", url_part, text_part);
                        g_free(text_part);
                        g_free(url_part);
                        p = url_end + 1;
                        continue;
                    }
                }
            }

            g_string_append_c(out, *p);
            p++;
        }
    }

    g_strfreev(lines);
    g_free(escaped);
    return g_string_free(out, FALSE);
}

/* ── GtkSourceView helper ────────────────────────────────────────── */

static GtkWidget *create_source_view(const gchar *code, const gchar *lang_hint,
                                      gboolean show_line_numbers)
{
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    GtkSourceLanguage *lang = NULL;

    if (lang_hint && strlen(lang_hint) > 0)
        lang = gtk_source_language_manager_get_language(lm, lang_hint);

    GtkSourceBuffer *buf = gtk_source_buffer_new(NULL);
    if (lang)
        gtk_source_buffer_set_language(buf, lang);
    gtk_source_buffer_set_highlight_syntax(buf, TRUE);
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buf), code, -1);

    /* Try to use a dark scheme */
    GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(sm, "oblivion");
    if (!scheme)
        scheme = gtk_source_style_scheme_manager_get_scheme(sm, "classic-dark");
    if (scheme)
        gtk_source_buffer_set_style_scheme(buf, scheme);

    GtkWidget *view = gtk_source_view_new_with_buffer(buf);
    g_object_unref(buf);

    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(view), show_line_numbers);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 4);

    /* Size: count lines for reasonable height, but let it expand naturally
     * if wrapping causes more visual lines */
    gint line_count = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(buf));
    gint height = MIN(line_count * 18 + 12, 400);
    gtk_widget_set_size_request(view, -1, height);

    return view;
}

/* ── Offset line number gutter renderer ───────────────────────────── */

static void on_gutter_query_data(GtkSourceGutterRenderer *renderer,
                                 GtkTextIter *start, GtkTextIter *end,
                                 GtkSourceGutterRendererState state,
                                 gpointer user_data)
{
    (void)end; (void)state;
    gint offset = GPOINTER_TO_INT(user_data);
    gint line = gtk_text_iter_get_line(start) + offset;
    gchar *text = g_strdup_printf("%d", line);
    gtk_source_gutter_renderer_text_set_text(
        GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), text, -1);
    g_free(text);
}

/* Parse numbered line content: strip "  N→" or "  N\t" prefixes.
 * Returns stripped text (caller frees) and sets *start_line. */
static gchar *strip_line_numbers(const gchar *content, gint *start_line)
{
    *start_line = 1;
    GString *out = g_string_new("");
    gchar **lines = g_strsplit(content, "\n", -1);
    gboolean first = TRUE;

    for (gchar **lp = lines; *lp; lp++) {
        const gchar *line = *lp;

        /* Try to match "  N→" or "  N\t" prefix */
        const gchar *p = line;
        while (*p == ' ') p++;

        if (*p >= '0' && *p <= '9') {
            gint num = 0;
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            /* Check for → (UTF-8: 0xE2 0x86 0x92) or \t separator */
            if ((p[0] == '\xE2' && p[1] == '\x86' && p[2] == '\x92')) {
                p += 3;
                if (first) { *start_line = num; first = FALSE; }
                if (out->len > 0) g_string_append_c(out, '\n');
                g_string_append(out, p);
                continue;
            } else if (*p == '\t') {
                p++;
                if (first) { *start_line = num; first = FALSE; }
                if (out->len > 0) g_string_append_c(out, '\n');
                g_string_append(out, p);
                continue;
            }
        }

        /* No prefix found — pass through (skip trailing empty lines) */
        if (strlen(line) > 0 || *(lp + 1) != NULL) {
            if (out->len > 0) g_string_append_c(out, '\n');
            g_string_append(out, line);
        }
    }

    g_strfreev(lines);
    return g_string_free(out, FALSE);
}

/* Create a source view with offset line numbers in the gutter.
 * If start_line == 1, just use built-in line numbers. */
static GtkWidget *create_source_view_with_offset(const gchar *code,
                                                   const gchar *lang_id,
                                                   gint start_line)
{
    if (start_line <= 1) {
        /* No offset needed — use built-in line numbers */
        return create_source_view(code, lang_id, TRUE);
    }

    /* Build the source view with custom offset gutter */
    GtkWidget *view = create_source_view(code, lang_id, FALSE);

    GtkSourceGutter *gutter = gtk_source_view_get_gutter(
        GTK_SOURCE_VIEW(view), GTK_TEXT_WINDOW_LEFT);

    GtkSourceGutterRenderer *renderer = gtk_source_gutter_renderer_text_new();
    gtk_source_gutter_renderer_set_alignment_mode(renderer,
        GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST);
    gtk_source_gutter_renderer_set_visible(renderer, TRUE);

    /* Calculate width based on max line number digits */
    gint line_count = gtk_text_buffer_get_line_count(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)));
    gint max_line = start_line + line_count;
    gint digits = 1;
    gint n = max_line;
    while (n >= 10) { digits++; n /= 10; }
    gtk_source_gutter_renderer_set_size(renderer, digits * 10 + 12);
    gtk_source_gutter_renderer_set_padding(renderer, 4, 0);

    g_signal_connect(renderer, "query-data",
                     G_CALLBACK(on_gutter_query_data),
                     GINT_TO_POINTER(start_line));

    gtk_source_gutter_insert(gutter, renderer, 0);
    g_object_unref(renderer);

    return view;
}

/* Create a source view using filename-based language detection */
static GtkWidget *create_source_view_for_file(const gchar *code,
                                               const gchar *file_path,
                                               gboolean show_line_numbers)
{
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    GtkSourceLanguage *lang = NULL;

    if (file_path) {
        gchar *content_type = g_content_type_guess(file_path, NULL, 0, NULL);
        lang = gtk_source_language_manager_guess_language(lm, file_path, content_type);
        g_free(content_type);
    }

    /* Fall back to create_source_view with the language ID */
    const gchar *lang_id = lang ? gtk_source_language_get_id(lang) : NULL;
    return create_source_view(code, lang_id, show_line_numbers);
}

/* ── Render message content (markdown → widgets) ─────────────────── */

static void render_content(GtkWidget *content_box, const gchar *content,
                           const gchar *role, gboolean is_streaming)
{
    /* Clear existing children */
    GList *children = gtk_container_get_children(GTK_CONTAINER(content_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    if (!content || strlen(content) == 0) {
        if (is_streaming) {
            GtkWidget *cursor = gtk_label_new("\u2588");
            gtk_widget_set_name(cursor, "streaming-cursor");
            gtk_label_set_xalign(GTK_LABEL(cursor), 0);
            gtk_box_pack_start(GTK_BOX(content_box), cursor, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(content_box);
        return;
    }

    /* Split into text and code segments */
    GList *segments = split_segments(content);

    for (GList *l = segments; l; l = l->next) {
        Segment *seg = l->data;

        if (seg->type == SEG_TEXT && strlen(seg->text) > 0) {
            gchar *pango = md_to_pango(seg->text);
            GtkWidget *label = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(label), pango);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_widget_set_halign(label, GTK_ALIGN_FILL);
            gtk_widget_set_hexpand(label, FALSE);
            /* Force label to wrap within allocated width */
            gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
            gtk_box_pack_start(GTK_BOX(content_box), label, FALSE, FALSE, 2);
            g_free(pango);
        } else if (seg->type == SEG_CODE && strlen(seg->text) > 0) {
            GtkWidget *sv = create_source_view(seg->text, seg->lang, FALSE);
            GtkWidget *frame = gtk_frame_new(NULL);
            gtk_container_add(GTK_CONTAINER(frame), sv);
            gtk_widget_set_margin_top(frame, 4);
            gtk_widget_set_margin_bottom(frame, 4);
            gtk_box_pack_start(GTK_BOX(content_box), frame, FALSE, FALSE, 0);
        }
    }

    /* Streaming cursor at end */
    if (is_streaming) {
        GtkWidget *cursor = gtk_label_new("\u2588");
        gtk_widget_set_name(cursor, "streaming-cursor");
        gtk_label_set_xalign(GTK_LABEL(cursor), 0);
        gtk_box_pack_start(GTK_BOX(content_box), cursor, FALSE, FALSE, 0);
    }

    g_list_free_full(segments, free_segment);
    gtk_widget_show_all(content_box);
}

/* ── Tool type detection ─────────────────────────────────────────── */

static gboolean is_edit_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Edit")==0 || strstr(n,"edit")!=NULL); }
static gboolean is_write_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Write")==0 || strstr(n,"write")!=NULL) && !strstr(n,"Todo"); }
static gboolean is_read_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Read")==0 || strstr(n,"read")!=NULL); }
static gboolean is_bash_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Bash")==0 || strstr(n,"bash")!=NULL || strstr(n,"Bash")!=NULL); }
static gboolean is_glob_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Glob")==0 || strstr(n,"glob")!=NULL); }

/* Get display name for tool */
static const gchar *tool_display_name(const gchar *name)
{
    if (g_strcmp0(name, "TodoWrite") == 0) return "Todo";
    if (is_edit_tool(name)) return "Edit";
    if (is_write_tool(name)) return "Write";
    if (is_read_tool(name)) return "Read";
    if (is_bash_tool(name)) return "Bash";
    if (is_glob_tool(name)) return "Glob";
    return name;
}

/* ── Jump to file button click ────────────────────────────────────── */

static void on_jump_btn_clicked(GtkButton *btn, gpointer data)
{
    (void)data;
    const gchar *file = g_object_get_data(G_OBJECT(btn), "file");
    ChatViewPrivate *priv = g_object_get_data(G_OBJECT(btn), "priv");
    if (file && priv && priv->jump_cb)
        priv->jump_cb(file, 1, 1, priv->jump_data);
}

/* ── Tool panel header click ─────────────────────────────────────── */

static gboolean on_tool_header_click(GtkWidget *w, GdkEventButton *ev,
                                     gpointer data)
{
    (void)w; (void)ev;
    ToolEntry *te = data;
    te->expanded = !te->expanded;
    gtk_revealer_set_reveal_child(GTK_REVEALER(te->revealer), te->expanded);
    gtk_label_set_text(GTK_LABEL(te->arrow_label), te->expanded ? "\u25BC" : "\u25B6");
    return TRUE;
}

/* ── CSS ─────────────────────────────────────────────────────────── */

static const gchar *base_css =
    ".msg-user { padding: 8px 12px; border-left: 3px solid @theme_selected_bg_color; "
    "  background: alpha(@theme_selected_bg_color, 0.12); border-radius: 4px; }\n"
    ".msg-assistant { padding: 8px 12px; }\n"
    ".msg-error { padding: 8px 12px; border: 1px solid #e74c3c; "
    "  background: alpha(#e74c3c, 0.08); border-radius: 4px; color: #e74c3c; }\n"
    ".msg-system { padding: 6px 12px; font-size: small; color: @insensitive_fg_color; }\n"
    ".msg-history { opacity: 0.5; }\n"
    ".tool-header { padding: 6px 10px; border-radius: 4px 4px 0 0; }\n"
    ".tool-header:hover { background: alpha(@theme_fg_color, 0.06); }\n"
    ".tool-border { border: 2px solid @borders; border-radius: 6px; "
    "  margin-top: 4px; margin-bottom: 4px; }\n"
    ".tool-body { padding: 8px 10px; }\n"
    ".tool-file-pill { background: alpha(black, 0.2); border-radius: 9px; "
    "  padding: 1px 8px; font-size: small; }\n"
    ".tool-status-running { color: @theme_selected_bg_color; }\n"
    ".tool-status-success { color: #2ecc71; }\n"
    ".tool-status-error { color: #e74c3c; }\n"
    ".perm-btn { padding: 4px 12px; }\n"
    ".perm-card { padding: 10px; border: 2px solid @theme_selected_bg_color; "
    "  border-radius: 8px; background: alpha(@theme_selected_bg_color, 0.06); }\n"
    ".uq-card { padding: 12px; border: 2px solid @theme_selected_bg_color; "
    "  border-radius: 8px; background: alpha(@theme_selected_bg_color, 0.06); }\n"
    ".history-sep { font-size: small; color: @insensitive_fg_color; "
    "  padding: 8px 0; border-bottom: 1px solid @borders; margin-bottom: 8px; }\n"
    ".todos-header { padding: 6px 12px; font-size: small; font-weight: bold; }\n"
    ".todo-item-active { border-left: 3px solid @theme_selected_bg_color; "
    "  background: alpha(@theme_selected_bg_color, 0.1); }\n"
    "#streaming-cursor { animation: blink 1s step-end infinite; "
    "  color: @theme_selected_bg_color; }\n"
    "@keyframes blink { 50% { opacity: 0; } }\n"
    ".welcome-title { font-size: x-large; font-weight: bold; }\n"
    ".welcome-sub { font-size: small; color: @insensitive_fg_color; }\n"
    ;

/* ── Widget construction ─────────────────────────────────────────── */

GtkWidget *chat_webview_new(void)
{
    ChatViewPrivate *priv = g_new0(ChatViewPrivate, 1);
    priv->msg_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, free_message_entry);
    priv->tool_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, free_tool_entry);

    /* Load CSS */
    priv->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(priv->css, base_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(priv->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Outer scrolled window */
    priv->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(priv->scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    priv->outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Welcome */
    GtkWidget *welcome_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(welcome_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(welcome_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(welcome_box, 40);
    gtk_widget_set_margin_bottom(welcome_box, 40);

    GtkWidget *title = gtk_label_new("Geany Code");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "welcome-title");
    GtkWidget *subtitle = gtk_label_new("Type a message to get started");
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "welcome-sub");
    gtk_box_pack_start(GTK_BOX(welcome_box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(welcome_box), subtitle, FALSE, FALSE, 0);
    priv->welcome = welcome_box;
    gtk_box_pack_start(GTK_BOX(priv->outer_box), welcome_box, TRUE, TRUE, 0);

    /* List box */
    priv->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(priv->list_box),
        GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(priv->outer_box), priv->list_box, TRUE, TRUE, 0);

    /* Todos (initially hidden) */
    priv->todos_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(priv->todos_box, TRUE);
    gtk_box_pack_end(GTK_BOX(priv->outer_box), priv->todos_box, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(priv->scroll), priv->outer_box);

    g_object_set_data_full(G_OBJECT(priv->scroll), PRIV_KEY, priv, g_free);

    return priv->scroll;
}

/* ── Message API ─────────────────────────────────────────────────── */

void chat_webview_add_message(GtkWidget *webview,
                              const gchar *id, const gchar *role,
                              const gchar *content, const gchar *timestamp,
                              gboolean is_streaming)
{
    (void)timestamp;
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    /* Hide welcome on first message */
    if (priv->welcome && gtk_widget_get_visible(priv->welcome))
        gtk_widget_hide(priv->welcome);

    /* Create message entry */
    MessageEntry *me = g_new0(MessageEntry, 1);
    me->id = g_strdup(id);
    me->role = g_strdup(role);

    /* Build the row */
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(content_box, 8);
    gtk_widget_set_margin_end(content_box, 8);
    gtk_widget_set_margin_top(content_box, 4);
    gtk_widget_set_margin_bottom(content_box, 4);
    me->content_box = content_box;

    /* Apply role-specific CSS class */
    gchar *css_class = g_strdup_printf("msg-%s", role);
    gtk_style_context_add_class(gtk_widget_get_style_context(content_box), css_class);
    g_free(css_class);

    /* Render content */
    render_content(content_box, content, role, is_streaming);
    me->last_content = g_strdup(content ? content : "");

    /* Add to list */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), content_box);
    me->row = row;

    gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
    gtk_widget_show_all(row);

    g_hash_table_insert(priv->msg_widgets, g_strdup(id), me);
    scroll_to_bottom(priv);
}

void chat_webview_update_message(GtkWidget *webview,
                                 const gchar *id, const gchar *content,
                                 gboolean is_streaming)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    MessageEntry *me = g_hash_table_lookup(priv->msg_widgets, id);
    if (!me) return;

    /* Skip if content hasn't changed */
    if (me->last_content && content &&
        g_strcmp0(me->last_content, content) == 0 &&
        !is_streaming)
        return;

    render_content(me->content_box, content, me->role, is_streaming);
    g_free(me->last_content);
    me->last_content = g_strdup(content ? content : "");

    scroll_to_bottom(priv);
}

void chat_webview_add_message_image(GtkWidget *webview,
                                    const gchar *msg_id,
                                    const gchar *b64_png)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    MessageEntry *me = g_hash_table_lookup(priv->msg_widgets, msg_id);
    if (!me) return;

    gsize len = 0;
    guchar *data = g_base64_decode(b64_png, &len);
    if (!data) return;

    GInputStream *stream = g_memory_input_stream_new_from_data(data, len, g_free);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
    g_object_unref(stream);
    if (!pixbuf) return;

    /* Scale to max 300px */
    gint w = gdk_pixbuf_get_width(pixbuf);
    gint h = gdk_pixbuf_get_height(pixbuf);
    if (w > 400 || h > 300) {
        gdouble scale = MIN(400.0 / w, 300.0 / h);
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf,
            (gint)(w * scale), (gint)(h * scale), GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
        pixbuf = scaled;
    }

    GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_widget_set_halign(image, GTK_ALIGN_START);
    gtk_widget_set_margin_top(image, 4);
    gtk_box_pack_start(GTK_BOX(me->content_box), image, FALSE, FALSE, 0);
    gtk_widget_show(image);

    scroll_to_bottom(priv);
}

/* ── Tool calls ──────────────────────────────────────────────────── */

void chat_webview_add_tool_call(GtkWidget *webview,
                                const gchar *msg_id, const gchar *tool_id,
                                const gchar *tool_name, const gchar *input_json,
                                const gchar *result)
{
    (void)msg_id;
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    /* Result update for existing tool */
    ToolEntry *existing = g_hash_table_lookup(priv->tool_widgets, tool_id);
    if (existing) {
        /* Update status */
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(existing->status_label), "tool-status-running");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(existing->status_label), "tool-status-success");
        gtk_label_set_text(GTK_LABEL(existing->status_label), "\u25CF");

        /* Append result — render based on tool type */
        if (result && strlen(result) > 0) {
            GtkWidget *result_widget = NULL;

            if (is_read_tool(existing->tool_name)) {
                /* Strip line number prefixes and get starting line */
                gint start_line = 1;
                gchar *stripped = strip_line_numbers(result, &start_line);

                /* Detect language from file path */
                GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
                GtkSourceLanguage *lang = NULL;
                if (existing->file_path) {
                    gchar *ct = g_content_type_guess(existing->file_path, NULL, 0, NULL);
                    lang = gtk_source_language_manager_guess_language(
                        lm, existing->file_path, ct);
                    g_free(ct);
                }
                const gchar *lang_id = lang ? gtk_source_language_get_id(lang) : NULL;

                result_widget = create_source_view_with_offset(
                    stripped, lang_id, start_line);
                g_free(stripped);
            } else if (is_bash_tool(existing->tool_name)) {
                /* Bash output in monospace source view (sh language) */
                result_widget = create_source_view(result, "sh", FALSE);
            } else {
                /* Default: plain label */
                GtkWidget *res_label = gtk_label_new(result);
                gtk_label_set_line_wrap(GTK_LABEL(res_label), TRUE);
                gtk_label_set_xalign(GTK_LABEL(res_label), 0);
                gtk_label_set_selectable(GTK_LABEL(res_label), TRUE);
                result_widget = res_label;
            }

            gtk_widget_set_margin_top(result_widget, 4);
            gtk_box_pack_start(GTK_BOX(existing->body_box), result_widget, FALSE, FALSE, 0);
            gtk_widget_show_all(result_widget);
        }
        scroll_to_bottom(priv);
        return;
    }

    /* Parse input for file path */
    gchar *file_path = NULL;
    JsonParser *jp = json_parser_new();
    JsonObject *input_obj = NULL;
    if (input_json && json_parser_load_from_data(jp, input_json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(jp);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            input_obj = json_node_get_object(root);
        if (input_obj && json_object_has_member(input_obj, "file_path"))
            file_path = g_strdup(json_object_get_string_member(input_obj, "file_path"));
    }

    /* Create tool entry */
    ToolEntry *te = g_new0(ToolEntry, 1);
    te->tool_id = g_strdup(tool_id);
    te->tool_name = g_strdup(tool_name);
    te->file_path = file_path;
    te->expanded = is_edit_tool(tool_name) || is_write_tool(tool_name) ||
                   is_read_tool(tool_name) || is_bash_tool(tool_name);

    const gchar *display = tool_display_name(tool_name);

    /* Outer box with border */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer), "tool-border");

    /* Header */
    GtkWidget *header_event = gtk_event_box_new();
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(header_box), "tool-header");

    te->arrow_label = gtk_label_new(te->expanded ? "\u25BC" : "\u25B6");
    GtkWidget *name_label = gtk_label_new(display);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0);

    gtk_box_pack_start(GTK_BOX(header_box), te->arrow_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_box), name_label, FALSE, FALSE, 0);

    /* File path pill or slug */
    gchar *slug = NULL;
    if (file_path) {
        gchar *base = g_path_get_basename(file_path);
        slug = base;
    } else if (input_obj) {
        if (json_object_has_member(input_obj, "pattern"))
            slug = g_strdup(json_object_get_string_member(input_obj, "pattern"));
        else if (json_object_has_member(input_obj, "command")) {
            const gchar *cmd = json_object_get_string_member(input_obj, "command");
            slug = g_strndup(cmd, MIN((gsize)60, strlen(cmd)));
        }
    }
    if (slug) {
        GtkWidget *pill = gtk_label_new(slug);
        gtk_style_context_add_class(gtk_widget_get_style_context(pill), "tool-file-pill");
        gtk_box_pack_start(GTK_BOX(header_box), pill, FALSE, FALSE, 4);
        g_free(slug);
    }

    /* Spacer + status dot */
    gtk_box_pack_start(GTK_BOX(header_box), gtk_label_new(""), TRUE, TRUE, 0);
    te->status_label = gtk_label_new("\u25CF");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(te->status_label), "tool-status-running");
    gtk_box_pack_end(GTK_BOX(header_box), te->status_label, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(header_event), header_box);
    g_signal_connect(header_event, "button-press-event",
                     G_CALLBACK(on_tool_header_click), te);
    gtk_box_pack_start(GTK_BOX(outer), header_event, FALSE, FALSE, 0);

    /* Revealer + body */
    te->revealer = gtk_revealer_new();
    gtk_revealer_set_reveal_child(GTK_REVEALER(te->revealer), te->expanded);
    gtk_revealer_set_transition_type(GTK_REVEALER(te->revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);

    te->body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(te->body_box), "tool-body");

    /* Render tool-specific content */
    if (is_edit_tool(tool_name) && input_obj) {
        const gchar *old_s = json_object_has_member(input_obj, "old_string")
            ? json_object_get_string_member(input_obj, "old_string")
            : (json_object_has_member(input_obj, "old_text")
               ? json_object_get_string_member(input_obj, "old_text") : NULL);
        const gchar *new_s = json_object_has_member(input_obj, "new_string")
            ? json_object_get_string_member(input_obj, "new_string")
            : (json_object_has_member(input_obj, "new_text")
               ? json_object_get_string_member(input_obj, "new_text") : NULL);

        if (old_s && new_s) {
            /* Simple diff display using GtkSourceView with diff language */
            GString *diff = g_string_new("");
            if (file_path) {
                g_string_append_printf(diff, "--- %s\n+++ %s\n", file_path, file_path);
            }
            /* Split into lines and show removed/added */
            gchar **old_lines = g_strsplit(old_s, "\n", -1);
            gchar **new_lines = g_strsplit(new_s, "\n", -1);
            for (gchar **l = old_lines; *l; l++)
                g_string_append_printf(diff, "-%s\n", *l);
            for (gchar **l = new_lines; *l; l++)
                g_string_append_printf(diff, "+%s\n", *l);
            g_strfreev(old_lines);
            g_strfreev(new_lines);

            GtkWidget *sv = create_source_view(diff->str, "diff", FALSE);
            gtk_box_pack_start(GTK_BOX(te->body_box), sv, FALSE, FALSE, 0);
            g_string_free(diff, TRUE);

            /* Jump to file link — use data on the button + generic handler */
            if (file_path && priv->jump_cb) {
                GtkWidget *jump_btn = gtk_button_new_with_label("Jump to file");
                gtk_button_set_relief(GTK_BUTTON(jump_btn), GTK_RELIEF_NONE);
                g_object_set_data_full(G_OBJECT(jump_btn), "file",
                    g_strdup(file_path), g_free);
                g_object_set_data(G_OBJECT(jump_btn), "priv", priv);
                g_signal_connect(jump_btn, "clicked",
                    G_CALLBACK(on_jump_btn_clicked), NULL);
                gtk_widget_set_halign(jump_btn, GTK_ALIGN_START);
                gtk_box_pack_start(GTK_BOX(te->body_box), jump_btn, FALSE, FALSE, 0);
            }
        }
    } else if (is_write_tool(tool_name) && input_obj &&
               json_object_has_member(input_obj, "content")) {
        const gchar *wcontent = json_object_get_string_member(input_obj, "content");
        /* Use GtkSourceLanguageManager to guess from filename */
        GtkWidget *sv = create_source_view_for_file(wcontent, file_path, TRUE);
        gtk_box_pack_start(GTK_BOX(te->body_box), sv, FALSE, FALSE, 0);
    } else if (is_bash_tool(tool_name) && input_obj &&
               json_object_has_member(input_obj, "command")) {
        const gchar *cmd = json_object_get_string_member(input_obj, "command");
        /* Show command in a small source view with sh highlighting */
        GtkWidget *cmd_sv = create_source_view(cmd, "sh", FALSE);
        gtk_box_pack_start(GTK_BOX(te->body_box), cmd_sv, FALSE, FALSE, 0);
    } else if (input_json && strlen(input_json) > 2) {
        /* Generic: show formatted JSON */
        GtkWidget *json_label = gtk_label_new(input_json);
        gtk_label_set_line_wrap(GTK_LABEL(json_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(json_label), 0);
        gtk_label_set_selectable(GTK_LABEL(json_label), TRUE);
        PangoFontDescription *mono = pango_font_description_from_string("monospace 10");
        gtk_widget_override_font(json_label, mono);
        pango_font_description_free(mono);
        gtk_box_pack_start(GTK_BOX(te->body_box), json_label, FALSE, FALSE, 0);
    }

    /* Initial result if provided */
    if (result && strlen(result) > 0) {
        GtkWidget *res_label = gtk_label_new(result);
        gtk_label_set_line_wrap(GTK_LABEL(res_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(res_label), 0);
        gtk_label_set_selectable(GTK_LABEL(res_label), TRUE);
        gtk_box_pack_start(GTK_BOX(te->body_box), res_label, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(te->revealer), te->body_box);
    gtk_box_pack_start(GTK_BOX(outer), te->revealer, FALSE, FALSE, 0);

    /* Add row to list */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), outer);
    te->row = row;

    gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
    gtk_widget_show_all(row);

    g_hash_table_insert(priv->tool_widgets, g_strdup(tool_id), te);
    g_object_unref(jp);

    scroll_to_bottom(priv);
}

/* ── Permissions ─────────────────────────────────────────────────── */

typedef struct {
    ChatViewPrivate *priv;
    gchar *request_id;
    GtkWidget *row;
} PermClickData;

static void on_perm_btn_clicked(GtkButton *btn, gpointer data)
{
    PermClickData *pd = data;
    const gchar *option_id = g_object_get_data(G_OBJECT(btn), "option-id");

    if (pd->priv->permission_cb)
        pd->priv->permission_cb(pd->request_id, option_id, pd->priv->permission_data);

    /* Remove the permission row */
    gtk_widget_destroy(pd->row);
    g_free(pd->request_id);
    g_free(pd);
}

void chat_webview_show_permission(GtkWidget *webview,
                                  const gchar *request_id,
                                  const gchar *tool_name,
                                  const gchar *description,
                                  const gchar *options_json)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "perm-card");
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    gchar *title = g_strdup_printf("<b>Permission: %s</b>", tool_name);
    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label), title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    g_free(title);
    gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);

    if (description && strlen(description) > 0) {
        GtkWidget *desc_label = gtk_label_new(description);
        gtk_label_set_line_wrap(GTK_LABEL(desc_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(desc_label), 0);
        gtk_box_pack_start(GTK_BOX(box), desc_label, FALSE, FALSE, 0);
    }

    /* Parse options and create buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    JsonParser *jp = json_parser_new();
    if (options_json && json_parser_load_from_data(jp, options_json, -1, NULL)) {
        JsonArray *arr = json_node_get_array(json_parser_get_root(jp));
        guint n = json_array_get_length(arr);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

        for (guint i = 0; i < n; i++) {
            JsonObject *opt = json_array_get_object_element(arr, i);
            const gchar *opt_id = json_object_get_string_member(opt, "id");
            const gchar *opt_label = json_object_get_string_member(opt, "label");

            GtkWidget *btn = gtk_button_new_with_label(opt_label);
            gtk_style_context_add_class(gtk_widget_get_style_context(btn), "perm-btn");
            g_object_set_data_full(G_OBJECT(btn), "option-id",
                g_strdup(opt_id), g_free);

            PermClickData *pd = g_new0(PermClickData, 1);
            pd->priv = priv;
            pd->request_id = g_strdup(request_id);
            pd->row = row;

            g_signal_connect(btn, "clicked", G_CALLBACK(on_perm_btn_clicked), pd);
            gtk_box_pack_start(GTK_BOX(btn_box), btn, FALSE, FALSE, 0);
        }

        gtk_box_pack_start(GTK_BOX(box), btn_box, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(row), box);
        gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
        gtk_widget_show_all(row);
    }
    g_object_unref(jp);
    scroll_to_bottom(priv);
}

/* ── User questions ──────────────────────────────────────────────── */

typedef struct {
    gchar *request_id;
    GtkWidget *row;
    GList *question_groups; /* GList of GList* of GtkToggleButton* */
    GList *headers;         /* GList of gchar* */
} QuestionData;

static void on_question_submit(GtkButton *btn, gpointer data)
{
    (void)btn;
    QuestionData *qd = data;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    GList *hdr = qd->headers;
    GList *grp = qd->question_groups;
    while (hdr && grp) {
        json_builder_set_member_name(b, (gchar *)hdr->data);
        json_builder_begin_array(b);
        GList *buttons = grp->data;
        for (GList *bl = buttons; bl; bl = bl->next) {
            GtkToggleButton *tb = GTK_TOGGLE_BUTTON(bl->data);
            if (gtk_toggle_button_get_active(tb)) {
                const gchar *lbl = g_object_get_data(G_OBJECT(tb), "opt-label");
                if (lbl) json_builder_add_string_value(b, lbl);
            }
        }
        json_builder_end_array(b);
        hdr = hdr->next;
        grp = grp->next;
    }
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *json = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    g_object_unref(b);

    editor_dbus_provide_response(qd->request_id, json);
    g_free(json);

    /* Replace with summary */
    GList *children = gtk_container_get_children(GTK_CONTAINER(
        gtk_bin_get_child(GTK_BIN(qd->row))));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    GtkWidget *summary = gtk_label_new("Answered");
    gtk_label_set_xalign(GTK_LABEL(summary), 0);
    gtk_container_add(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(qd->row))), summary);
    gtk_widget_show(summary);

    g_free(qd->request_id);
    g_list_free_full(qd->question_groups, (GDestroyNotify)g_list_free);
    g_list_free(qd->headers);
    g_free(qd);
}

static void on_question_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;
    QuestionData *qd = data;
    editor_dbus_provide_response(qd->request_id, "ERROR: User cancelled");
    gtk_widget_destroy(qd->row);
    g_free(qd->request_id);
    g_list_free_full(qd->question_groups, (GDestroyNotify)g_list_free);
    g_list_free(qd->headers);
    g_free(qd);
}

void chat_webview_show_user_question(GtkWidget *webview,
                                     const gchar *request_id,
                                     const gchar *questions_json)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, questions_json, -1, NULL)) {
        g_object_unref(jp);
        return;
    }

    JsonArray *questions = json_node_get_array(json_parser_get_root(jp));
    guint n = json_array_get_length(questions);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "uq-card");
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Claude is asking:</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

    QuestionData *qd = g_new0(QuestionData, 1);
    qd->request_id = g_strdup(request_id);
    qd->row = row;

    for (guint qi = 0; qi < n; qi++) {
        JsonObject *q = json_array_get_object_element(questions, qi);
        const gchar *header = json_object_get_string_member(q, "header");
        const gchar *question = json_object_get_string_member(q, "question");
        gboolean multi = json_object_get_boolean_member(q, "multiSelect");
        JsonArray *options = json_object_get_array_member(q, "options");

        qd->headers = g_list_append(qd->headers, (gpointer)g_strdup(header));

        GtkWidget *q_label = gtk_label_new(NULL);
        gchar *q_markup = g_markup_printf_escaped("<b>%s</b>", question);
        gtk_label_set_markup(GTK_LABEL(q_label), q_markup);
        g_free(q_markup);
        gtk_label_set_line_wrap(GTK_LABEL(q_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(q_label), 0);
        gtk_box_pack_start(GTK_BOX(box), q_label, FALSE, FALSE, 0);

        GList *btn_list = NULL;
        GtkWidget *first_radio = NULL;
        guint opt_n = json_array_get_length(options);
        for (guint oi = 0; oi < opt_n; oi++) {
            JsonObject *opt = json_array_get_object_element(options, oi);
            const gchar *lbl = json_object_get_string_member(opt, "label");
            const gchar *desc = json_object_get_string_member(opt, "description");

            gchar *btn_text = g_strdup_printf("%s — %s", lbl, desc);
            GtkWidget *btn;
            if (multi) {
                btn = gtk_check_button_new_with_label(btn_text);
            } else {
                btn = gtk_radio_button_new_with_label_from_widget(
                    first_radio ? GTK_RADIO_BUTTON(first_radio) : NULL, btn_text);
                if (!first_radio) first_radio = btn;
            }
            g_free(btn_text);
            g_object_set_data_full(G_OBJECT(btn), "opt-label",
                g_strdup(lbl), g_free);
            gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
            btn_list = g_list_append(btn_list, btn);
        }
        qd->question_groups = g_list_append(qd->question_groups, btn_list);
    }

    /* Submit / Cancel buttons */
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *submit = gtk_button_new_with_label("Submit");
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(submit, "clicked", G_CALLBACK(on_question_submit), qd);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_question_cancel), qd);
    gtk_box_pack_start(GTK_BOX(action_box), submit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_box), cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), action_box, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
    gtk_widget_show_all(row);

    g_object_unref(jp);
    scroll_to_bottom(priv);
}

/* ── Todos ───────────────────────────────────────────────────────── */

void chat_webview_update_todos(GtkWidget *webview, const gchar *todos_json)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    /* Clear existing todos */
    GList *children = gtk_container_get_children(GTK_CONTAINER(priv->todos_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, todos_json, -1, NULL)) {
        gtk_widget_hide(priv->todos_box);
        g_object_unref(jp);
        return;
    }

    JsonArray *arr = json_node_get_array(json_parser_get_root(jp));
    guint n = json_array_get_length(arr);
    if (n == 0) {
        gtk_widget_hide(priv->todos_box);
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

    /* Header */
    gchar *header_text = g_strdup_printf("TASKS (%u/%u)", completed, n);
    GtkWidget *header = gtk_label_new(header_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(header), "todos-header");
    gtk_label_set_xalign(GTK_LABEL(header), 0);
    g_free(header_text);
    gtk_box_pack_start(GTK_BOX(priv->todos_box), header, FALSE, FALSE, 0);

    /* Items */
    for (guint i = 0; i < n; i++) {
        JsonObject *todo = json_array_get_object_element(arr, i);
        const gchar *content = json_object_get_string_member(todo, "content");
        const gchar *status = json_object_get_string_member(todo, "status");

        const gchar *icon = "\u25CB"; /* pending */
        if (g_strcmp0(status, "completed") == 0) icon = "\u2713";
        else if (g_strcmp0(status, "in_progress") == 0) icon = "\u27F3";

        GtkWidget *item_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(item_box, 12);
        gtk_widget_set_margin_top(item_box, 2);

        if (g_strcmp0(status, "in_progress") == 0)
            gtk_style_context_add_class(gtk_widget_get_style_context(item_box), "todo-item-active");

        GtkWidget *icon_label = gtk_label_new(icon);
        if (g_strcmp0(status, "completed") == 0) {
            PangoAttrList *attrs = pango_attr_list_new();
            pango_attr_list_insert(attrs, pango_attr_foreground_new(11796, 52428, 11796));
            gtk_label_set_attributes(GTK_LABEL(icon_label), attrs);
            pango_attr_list_unref(attrs);
        }

        GtkWidget *text_label = gtk_label_new(content);
        gtk_label_set_line_wrap(GTK_LABEL(text_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(text_label), 0);
        if (g_strcmp0(status, "completed") == 0) {
            PangoAttrList *attrs = pango_attr_list_new();
            pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
            pango_attr_list_insert(attrs, pango_attr_foreground_alpha_new(32768));
            gtk_label_set_attributes(GTK_LABEL(text_label), attrs);
            pango_attr_list_unref(attrs);
        }

        gtk_box_pack_start(GTK_BOX(item_box), icon_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(item_box), text_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(priv->todos_box), item_box, FALSE, FALSE, 0);
    }

    gtk_widget_set_no_show_all(priv->todos_box, FALSE);
    gtk_widget_show_all(priv->todos_box);
    g_object_unref(jp);
}

/* ── History ─────────────────────────────────────────────────────── */

void chat_webview_add_history_message(GtkWidget *webview,
                                      const gchar *id, const gchar *role,
                                      const gchar *content,
                                      const gchar *timestamp)
{
    /* Add as a regular message, then apply history class */
    chat_webview_add_message(webview, id, role, content, timestamp, FALSE);

    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;
    MessageEntry *me = g_hash_table_lookup(priv->msg_widgets, id);
    if (me)
        gtk_style_context_add_class(
            gtk_widget_get_style_context(me->content_box), "msg-history");
}

void chat_webview_add_history_separator(GtkWidget *webview)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    GtkWidget *label = gtk_label_new("Previous conversation");
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "history-sep");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
    gtk_widget_show_all(row);
}

/* ── Clear / Theme ───────────────────────────────────────────────── */

void chat_webview_clear(GtkWidget *webview)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    /* Remove all rows from list box */
    GList *children = gtk_container_get_children(GTK_CONTAINER(priv->list_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    g_hash_table_remove_all(priv->msg_widgets);
    g_hash_table_remove_all(priv->tool_widgets);

    /* Show welcome again */
    if (priv->welcome)
        gtk_widget_show(priv->welcome);

    /* Clear todos */
    children = gtk_container_get_children(GTK_CONTAINER(priv->todos_box));
    for (GList *c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);
    gtk_widget_hide(priv->todos_box);
}

void chat_webview_apply_theme(GtkWidget *webview)
{
    /* Theme is handled automatically via GTK CSS @-references in base_css.
     * The CSS uses @theme_selected_bg_color, @borders, @insensitive_fg_color
     * etc. which GTK resolves from the active theme. No manual color parsing needed. */
    (void)webview;
}

/* ── Callback setters ────────────────────────────────────────────── */

void chat_webview_set_permission_callback(GtkWidget *webview,
                                          ChatWebViewPermissionCb cb,
                                          gpointer user_data)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (priv) {
        priv->permission_cb = cb;
        priv->permission_data = user_data;
    }
}

void chat_webview_set_jump_callback(GtkWidget *webview,
                                    ChatWebViewJumpToEditCb cb,
                                    gpointer user_data)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (priv) {
        priv->jump_cb = cb;
        priv->jump_data = user_data;
    }
}
