/*
 * chat_webview.c — Native GTK3 chat renderer (replaces WebKit2GTK)
 *
 * Uses GtkListBox + GtkLabel (Pango markup) + Scintilla for
 * a cross-platform chat view with no web engine dependency.
 */

#include "chat_webview.h"
#include "editor_dbus.h"
#include "plugin.h"
#include <Scintilla.h>
#include <ScintillaWidget.h>
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
    gchar       *key;           /* "msg_id:fragment_index" */
    GtkWidget   *row;
    GtkWidget   *arrow_label;
    GtkWidget   *revealer;
    GtkWidget   *body_box;
    GtkWidget   *content_label;
    gboolean     expanded;
    gchar       *last_text;     /* for diff-based skip */
} ThinkingEntry;

typedef struct {
    GtkWidget   *scroll;
    GtkWidget   *outer_box;
    GtkWidget   *list_box;
    GtkWidget   *welcome;
    GtkWidget   *todos_box;
    GHashTable  *msg_widgets;    /* id → MessageEntry* */
    GHashTable  *tool_widgets;   /* tool_id → ToolEntry* */
    GHashTable  *thinking_widgets; /* "msg_id:fragment" → ThinkingEntry* */
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

static void free_thinking_entry(gpointer p)
{
    ThinkingEntry *e = p;
    g_free(e->key);
    g_free(e->last_text);
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
    /* Use idle + short timeout to ensure layout is complete before scrolling */
    g_idle_add(do_scroll_to_bottom, adj);
    g_timeout_add(50, do_scroll_to_bottom, adj);
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

/* Forward declaration */
static void get_editor_font(const gchar **out_family, gint *out_size);

/* Process inline markdown formatting within a line (already escaped) */
static void process_inline(GString *out, const gchar *line)
{
    const gchar *p = line;
    while (*p) {
        /* Strikethrough: ~~text~~ */
        if (p[0] == '~' && p[1] == '~') {
            const gchar *end = strstr(p + 2, "~~");
            if (end) {
                g_string_append(out, "<s>");
                process_inline(out, g_strndup(p + 2, end - (p + 2)));
                g_string_append(out, "</s>");
                p = end + 2;
                continue;
            }
        }
        /* Bold: **text** */
        if (p[0] == '*' && p[1] == '*') {
            const gchar *end = strstr(p + 2, "**");
            if (end) {
                g_string_append(out, "<b>");
                /* Recurse for nested italic inside bold */
                gchar *inner = g_strndup(p + 2, end - (p + 2));
                process_inline(out, inner);
                g_free(inner);
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
        /* Inline code: `text` — with dimmed background, editor font, padding */
        if (p[0] == '`' && p[1] != '`') {
            const gchar *end = strchr(p + 1, '`');
            if (end) {
                const gchar *ff = NULL;
                gint fs = 0;
                get_editor_font(&ff, &fs);
                gint code_size = fs > 2 ? fs - 2 : fs;
                g_string_append_printf(out,
                    "<span background=\"#00000040\" font_family=\"%s\" "
                    "font_size=\"%dpt\">"
                    "\u2009",  /* thin space padding left */
                    ff, code_size);
                g_string_append_len(out, p + 1, end - (p + 1));
                g_string_append(out, "\u2009</span>");  /* thin space padding right */
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

/* Get the visual length of text with markdown formatting stripped */
static gint visual_len(const gchar *text)
{
    gint len = 0;
    const gchar *p = text;
    while (*p) {
        if (p[0] == '~' && p[1] == '~') {
            const gchar *end = strstr(p + 2, "~~");
            if (end) { len += visual_len(g_strndup(p + 2, end - (p + 2))); p = end + 2; continue; }
        }
        if (p[0] == '*' && p[1] == '*') {
            const gchar *end = strstr(p + 2, "**");
            if (end) { len += visual_len(g_strndup(p + 2, end - (p + 2))); p = end + 2; continue; }
        }
        if (p[0] == '*' && p[1] != '*') {
            const gchar *end = strchr(p + 1, '*');
            if (end && end != p + 1) { len += (gint)(end - (p + 1)); p = end + 1; continue; }
        }
        if (p[0] == '`' && p[1] != '`') {
            const gchar *end = strchr(p + 1, '`');
            if (end) { len += (gint)(end - (p + 1)); p = end + 1; continue; }
        }
        len++;
        p = g_utf8_next_char(p);
    }
    return len;
}

/* Split a table row "| a | b | c |" into cells (trimmed). Returns GPtrArray of gchar*. */
static GPtrArray *split_table_row(const gchar *line)
{
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    const gchar *p = line;

    /* Skip leading | */
    if (*p == '|') p++;

    while (*p) {
        const gchar *pipe = strchr(p, '|');
        if (!pipe) {
            gchar *cell = g_strstrip(g_strdup(p));
            if (strlen(cell) > 0)
                g_ptr_array_add(cells, cell);
            else
                g_free(cell);
            break;
        }
        gchar *cell = g_strstrip(g_strndup(p, pipe - p));
        g_ptr_array_add(cells, cell);
        p = pipe + 1;
    }

    return cells;
}

/* Check if a line is a table separator (|---|---|) */
static gboolean is_table_separator(const gchar *line)
{
    if (line[0] != '|') return FALSE;
    for (const gchar *c = line + 1; *c; c++) {
        if (*c != '-' && *c != '|' && *c != ':' && *c != ' ')
            return FALSE;
    }
    return TRUE;
}

/* Convert a subset of markdown to Pango markup */
static gchar *md_to_pango(const gchar *text)
{
    /* First escape for Pango safety */
    gchar *escaped = g_markup_escape_text(text, -1);
    GString *out = g_string_new("");

    gchar **lines = g_strsplit(escaped, "\n", -1);
    gboolean in_blockquote = FALSE;

    for (gchar **lp = lines; *lp; lp++) {
        const gchar *line = *lp;

        if (out->len > 0)
            g_string_append_c(out, '\n');

        /* Horizontal rule: --- or *** or ___ (3+ chars, possibly with spaces) */
        {
            const gchar *s = line;
            while (*s == ' ') s++;
            if ((g_str_has_prefix(s, "---") || g_str_has_prefix(s, "***") ||
                 g_str_has_prefix(s, "___"))) {
                gboolean is_rule = TRUE;
                gchar ch = s[0];
                for (const gchar *c = s; *c; c++) {
                    if (*c != ch && *c != ' ') { is_rule = FALSE; break; }
                }
                if (is_rule && strlen(s) >= 3) {
                    g_string_append(out,
                        "<span foreground=\"#666666\">\u2500\u2500\u2500\u2500"
                        "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                        "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                        "</span>");
                    continue;
                }
            }
        }

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

        /* Blockquotes: > text (can be multi-line) */
        if (g_str_has_prefix(line, "&gt; ") || g_strcmp0(line, "&gt;") == 0) {
            /* Note: > was escaped to &gt; by g_markup_escape_text */
            const gchar *content = strlen(line) > 5 ? line + 5 : "";
            g_string_append(out,
                "  <span foreground=\"#888888\">\u2502 </span>"
                "<span foreground=\"#aaaaaa\"><i>");
            process_inline(out, content);
            g_string_append(out, "</i></span>");
            in_blockquote = TRUE;
            continue;
        }
        in_blockquote = FALSE;

        /* Nested bullet lists — count leading spaces for indentation */
        {
            gint indent = 0;
            const gchar *s = line;
            while (*s == ' ') { indent++; s++; }

            if (g_str_has_prefix(s, "- ") || g_str_has_prefix(s, "* ")) {
                /* Each 2 spaces of indent = one nesting level */
                gint level = indent / 2;
                for (gint i = 0; i <= level; i++)
                    g_string_append(out, "  ");
                g_string_append(out, "\u2022 ");
                process_inline(out, s + 2);
                continue;
            }

            /* Numbered lists — handle nesting */
            if (*s >= '0' && *s <= '9') {
                const gchar *d = s;
                while (*d >= '0' && *d <= '9') d++;
                if (d[0] == '.' && d[1] == ' ') {
                    gint level = indent / 2;
                    for (gint i = 0; i <= level; i++)
                        g_string_append(out, "  ");
                    g_string_append_len(out, s, d - s + 1);
                    g_string_append_c(out, ' ');
                    process_inline(out, d + 2);
                    continue;
                }
            }
        }

        /* Table: collect all consecutive | rows, compute column widths, render */
        if (line[0] == '|') {
            /* Collect all table rows starting from current line */
            GPtrArray *table_rows = g_ptr_array_new();  /* GPtrArray of GPtrArray of gchar* */
            GArray *is_sep_row = g_array_new(FALSE, FALSE, sizeof(gboolean));
            gint num_cols = 0;

            for (gchar **tp = lp; *tp && (*tp)[0] == '|'; tp++) {
                gboolean sep = is_table_separator(*tp);
                g_array_append_val(is_sep_row, sep);
                if (!sep) {
                    GPtrArray *cells = split_table_row(*tp);
                    if ((gint)cells->len > num_cols)
                        num_cols = cells->len;
                    g_ptr_array_add(table_rows, cells);
                } else {
                    g_ptr_array_add(table_rows, NULL);  /* placeholder for separator */
                }
            }

            /* Calculate max visual width per column */
            gint *col_widths = g_new0(gint, num_cols);
            for (guint r = 0; r < table_rows->len; r++) {
                GPtrArray *cells = g_ptr_array_index(table_rows, r);
                if (!cells) continue;  /* separator row */
                for (guint c = 0; c < cells->len && (gint)c < num_cols; c++) {
                    gint vlen = visual_len(g_ptr_array_index(cells, c));
                    if (vlen > col_widths[c])
                        col_widths[c] = vlen;
                }
            }

            /* Render each row */
            for (guint r = 0; r < table_rows->len; r++) {
                if (r > 0) g_string_append_c(out, '\n');

                gboolean sep = g_array_index(is_sep_row, gboolean, r);
                if (sep) {
                    /* Separator: draw ─ characters */
                    g_string_append(out,
                        "<span foreground=\"#666666\" font_family=\"monospace\">");
                    for (gint c = 0; c < num_cols; c++) {
                        if (c > 0) g_string_append(out, "\u2500\u253C\u2500");
                        for (gint w = 0; w < col_widths[c] + 2; w++)
                            g_string_append(out, "\u2500");
                    }
                    g_string_append(out, "</span>");
                } else {
                    GPtrArray *cells = g_ptr_array_index(table_rows, r);
                    g_string_append(out, "<tt>");
                    for (gint c = 0; c < num_cols; c++) {
                        if (c > 0)
                            g_string_append(out,
                                " <span foreground=\"#666666\">\u2502</span> ");
                        const gchar *cell = (cells && (guint)c < cells->len)
                            ? g_ptr_array_index(cells, c) : "";
                        gint vlen = visual_len(cell);
                        gint pad = col_widths[c] - vlen;

                        process_inline(out, cell);
                        for (gint p = 0; p < pad; p++)
                            g_string_append_c(out, ' ');
                    }
                    g_string_append(out, "</tt>");
                }
            }

            /* Advance lp past all table rows */
            guint table_len = table_rows->len;
            for (guint r = 0; r < table_rows->len; r++) {
                GPtrArray *cells = g_ptr_array_index(table_rows, r);
                if (cells) g_ptr_array_free(cells, TRUE);
            }
            g_ptr_array_free(table_rows, TRUE);
            g_array_free(is_sep_row, TRUE);
            g_free(col_widths);

            /* Skip past the rows we consumed (lp will be incremented by the for loop) */
            lp += table_len - 1;
            continue;
        }

        /* Regular line — process inline formatting */
        process_inline(out, line);
    }

    g_strfreev(lines);
    g_free(escaped);
    return g_string_free(out, FALSE);
}

/* ── Scintilla code block helper (uses Geany's highlighting theme) ── */

/* Map common language hints to Geany filetype names */
static GeanyFiletype *detect_filetype(const gchar *lang_hint,
                                       const gchar *file_path)
{
    /* Try filename first */
    if (file_path) {
        GeanyFiletype *ft = filetypes_detect_from_file(file_path);
        if (ft && ft->id != GEANY_FILETYPES_NONE)
            return ft;
    }

    if (!lang_hint || strlen(lang_hint) == 0)
        return NULL;

    /* Try direct lookup */
    GeanyFiletype *ft = filetypes_lookup_by_name(lang_hint);
    if (ft) return ft;

    /* Map common markdown fence hints to Geany filetype names */
    static const struct { const gchar *hint; const gchar *ft_name; } map[] = {
        {"c", "C"}, {"cpp", "C++"}, {"c++", "C++"}, {"cxx", "C++"},
        {"h", "C"}, {"hpp", "C++"},
        {"python", "Python"}, {"py", "Python"},
        {"javascript", "Javascript"}, {"js", "Javascript"},
        {"typescript", "Javascript"}, {"ts", "Javascript"},
        {"go", "Go"}, {"golang", "Go"},
        {"rust", "Rust"}, {"rs", "Rust"},
        {"java", "Java"},
        {"ruby", "Ruby"}, {"rb", "Ruby"},
        {"lua", "Lua"},
        {"sh", "Sh"}, {"bash", "Sh"}, {"zsh", "Sh"}, {"shell", "Sh"},
        {"html", "HTML"}, {"xml", "XML"}, {"svg", "XML"},
        {"css", "CSS"}, {"scss", "CSS"},
        {"json", "JSON"},
        {"yaml", "YAML"}, {"yml", "YAML"},
        {"toml", "Conf"},
        {"sql", "SQL"},
        {"markdown", "Markdown"}, {"md", "Markdown"},
        {"makefile", "Make"}, {"cmake", "CMake"},
        {"dockerfile", "Dockerfile"},
        {"diff", "Diff"}, {"patch", "Diff"},
        {"php", "PHP"},
        {"perl", "Perl"}, {"pl", "Perl"},
        {"r", "R"},
        {"swift", "Swift"},
        {"kotlin", "Kotlin"}, {"kt", "Kotlin"},
        {"scala", "Scala"},
        {"haskell", "Haskell"}, {"hs", "Haskell"},
        {NULL, NULL}
    };

    gchar *lower = g_ascii_strdown(lang_hint, -1);
    for (gint i = 0; map[i].hint; i++) {
        if (g_strcmp0(lower, map[i].hint) == 0) {
            g_free(lower);
            return filetypes_lookup_by_name(map[i].ft_name);
        }
    }
    g_free(lower);
    return NULL;
}

/* Read editor font from geany.conf (cached after first call) */
static void get_editor_font(const gchar **out_family, gint *out_size)
{
    static gchar *cached_family = NULL;
    static gint cached_size = 0;
    static gboolean loaded = FALSE;

    if (!loaded) {
        loaded = TRUE;
        gchar *conf = g_build_filename(
            g_get_user_config_dir(), "geany", "geany.conf", NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, conf, G_KEY_FILE_NONE, NULL)) {
            gchar *font = g_key_file_get_string(kf, "geany", "editor_font", NULL);
            if (font) {
                /* Parse "Font Name Size" — last space-separated token is size */
                gchar *last_space = g_strrstr(font, " ");
                if (last_space) {
                    cached_size = atoi(last_space + 1);
                    cached_family = g_strndup(font, last_space - font);
                } else {
                    cached_family = g_strdup(font);
                    cached_size = 11;
                }
                g_free(font);
            }
        }
        g_key_file_free(kf);
        g_free(conf);

        if (!cached_family)
            cached_family = g_strdup("Monospace");
        if (cached_size <= 0)
            cached_size = 11;
    }

    *out_family = cached_family;
    *out_size = cached_size;
}

/* Copy code block content to clipboard */
static void on_copy_code_clicked(GtkButton *btn, gpointer data)
{
    (void)data;
    const gchar *code = g_object_get_data(G_OBJECT(btn), "code");
    if (code) {
        GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clip, code, -1);
    }
}

/* Propagate scroll events from embedded Scintilla widgets to the parent
 * scrolled window so mouse-wheel scrolling works over code blocks. */
static gboolean on_sci_scroll(GtkWidget *widget, GdkEventScroll *event,
                               gpointer data)
{
    (void)data;
    GtkWidget *parent = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
    if (parent)
        gtk_propagate_event(parent, (GdkEvent *)event);
    return TRUE;  /* consumed — don't let Scintilla handle it */
}

static GtkWidget *create_source_view(const gchar *code, const gchar *lang_hint,
                                      gboolean show_line_numbers)
{
    ScintillaObject *sci = SCINTILLA(scintilla_object_new());

    /* Apply Geany's active color scheme.
     * First apply a "base" language (C) to get the editor's default
     * background/foreground, then apply the actual language on top. */
    GeanyFiletype *base_ft = filetypes_lookup_by_name("C");
    if (base_ft)
        highlighting_set_styles(sci, base_ft);

    /* Capture the default bg/fg from the base scheme */
    gint default_bg = scintilla_send_message(sci, SCI_STYLEGETBACK,
        STYLE_DEFAULT, 0);
    gint default_fg = scintilla_send_message(sci, SCI_STYLEGETFORE,
        STYLE_DEFAULT, 0);

    GeanyFiletype *ft = detect_filetype(lang_hint, NULL);
    if (ft && ft != base_ft)
        highlighting_set_styles(sci, ft);

    if (!ft || (lang_hint == NULL)) {
        /* No language / plain text: reset all styles to default colors
         * so no syntax highlighting leaks through from the base "C" scheme */
        for (gint s = 0; s <= STYLE_MAX; s++) {
            scintilla_send_message(sci, SCI_STYLESETFORE, s, default_fg);
            scintilla_send_message(sci, SCI_STYLESETBACK, s, default_bg);
            scintilla_send_message(sci, SCI_STYLESETBOLD, s, 0);
            scintilla_send_message(sci, SCI_STYLESETITALIC, s, 0);
        }
    } else if (g_strcmp0(lang_hint, "diff") == 0) {
        /* For diff: restore the default background so it matches the editor,
         * but keep the syntax-colored foregrounds */
        scintilla_send_message(sci, SCI_STYLESETBACK, STYLE_DEFAULT, default_bg);
        for (gint s = 0; s <= 6; s++) {
            scintilla_send_message(sci, SCI_STYLESETBACK, s, default_bg);
        }
    }

    /* Set content */
    scintilla_send_message(sci, SCI_SETTEXT, 0, (sptr_t)(code ? code : ""));

    /* Read-only */
    scintilla_send_message(sci, SCI_SETREADONLY, 1, 0);

    /* Line numbers */
    if (show_line_numbers) {
        gint line_count = scintilla_send_message(sci, SCI_GETLINECOUNT, 0, 0);
        gint digits = 1;
        gint n = line_count;
        while (n >= 10) { digits++; n /= 10; }
        gint margin_width = scintilla_send_message(sci, SCI_TEXTWIDTH,
            STYLE_LINENUMBER, (sptr_t)"9") * (digits + 1) + 4;
        scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 0, margin_width);
    } else {
        scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 0, 0);
    }

    /* Hide other margins (markers, fold) */
    scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 1, 0);
    scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 2, 0);

    /* Word wrap */
    scintilla_send_message(sci, SCI_SETWRAPMODE, SC_WRAP_WORD, 0);

    /* No scrollbars — we want it to size to content */
    scintilla_send_message(sci, SCI_SETHSCROLLBAR, 0, 0);
    scintilla_send_message(sci, SCI_SETVSCROLLBAR, 0, 0);

    /* No caret */
    scintilla_send_message(sci, SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE, 0);

    /* Apply editor font to ALL styles (must be done last, after
     * highlighting_set_styles which sets per-style fonts) */
    const gchar *font_family = NULL;
    gint font_size = 0;
    get_editor_font(&font_family, &font_size);
    gint chat_font_size = font_size > 2 ? font_size - 2 : font_size;
    for (gint s = 0; s <= STYLE_MAX; s++) {
        scintilla_send_message(sci, SCI_STYLESETFONT, s, (sptr_t)font_family);
        scintilla_send_message(sci, SCI_STYLESETSIZE, s, chat_font_size);
    }

    /* Horizontal text padding inside the editor area */
    scintilla_send_message(sci, SCI_SETMARGINLEFT, 0, 8);
    scintilla_send_message(sci, SCI_SETMARGINRIGHT, 0, 8);

    /* Size to exact content height — wrapper provides vertical padding */
    gint line_count = scintilla_send_message(sci, SCI_GETLINECOUNT, 0, 0);
    gint line_height = scintilla_send_message(sci, SCI_TEXTHEIGHT, 0, 0);
    if (line_height < 14) line_height = 14;
    gint height = MIN(line_count * line_height + 2, 400);
    gtk_widget_set_size_request(GTK_WIDGET(sci), -1, height);

    /* Forward scroll events to parent so chat view scrolls freely */
    g_signal_connect(sci, "scroll-event", G_CALLBACK(on_sci_scroll), NULL);

    /* Wrap in a container with rounded corners and matching background */
    GtkWidget *bg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(bg_box),
                                "code-block");

    /* Store the editor bg color as CSS */
    gint bg = default_bg;
    gchar *css_str = g_strdup_printf(
        ".code-block { background: #%02x%02x%02x; border-radius: 6px; "
        "padding: 4px 0; }",
        bg & 0xFF, (bg >> 8) & 0xFF, (bg >> 16) & 0xFF);
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css_str, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(bg_box),
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
    g_free(css_str);

    /* GtkOverlay: Scintilla as main child, copy button floated top-right */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(sci));

    /* Copy button — monochrome symbolic icon */
    GtkWidget *copy_btn = gtk_button_new();
    GtkWidget *copy_icon = gtk_image_new_from_icon_name(
        "edit-copy-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_container_add(GTK_CONTAINER(copy_btn), copy_icon);
    gtk_button_set_relief(GTK_BUTTON(copy_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(copy_btn, GTK_ALIGN_END);
    gtk_widget_set_valign(copy_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_end(copy_btn, 4);
    gtk_widget_set_margin_top(copy_btn, 2);
    gtk_widget_set_tooltip_text(copy_btn, "Copy");
    gtk_widget_set_opacity(copy_btn, 0.4);
    g_object_set_data_full(G_OBJECT(copy_btn), "code",
        g_strdup(code ? code : ""), g_free);
    g_signal_connect(copy_btn, "clicked",
        G_CALLBACK(on_copy_code_clicked), NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), copy_btn);

    gtk_box_pack_start(GTK_BOX(bg_box), overlay, TRUE, TRUE, 0);

    /* Store the ScintillaObject so callers can access it for markers etc. */
    g_object_set_data(G_OBJECT(bg_box), "scintilla", sci);

    return bg_box;
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

/* Create a source view for read results — no line numbers, just highlighted code */
static GtkWidget *create_source_view_with_offset(const gchar *code,
                                                   const gchar *lang_id,
                                                   gint start_line)
{
    (void)start_line;
    return create_source_view(code, lang_id, FALSE);
}

/* Create a source view using filename-based language detection.
 * Delegates to create_source_view so font/style is consistent. */
static GtkWidget *create_source_view_for_file(const gchar *code,
                                               const gchar *file_path,
                                               gboolean show_line_numbers)
{
    /* Detect filetype from filename, use its name as the language hint */
    GeanyFiletype *ft = file_path ? filetypes_detect_from_file(file_path) : NULL;
    const gchar *lang = (ft && ft->name) ? ft->name : NULL;
    return create_source_view(code, lang, show_line_numbers);
}

/* ── Line-level LCS diff ─────────────────────────────────────────── */

typedef enum { DIFF_UNCHANGED, DIFF_REMOVED, DIFF_ADDED } DiffLineKind;
typedef struct { DiffLineKind kind; const gchar *text; } DiffLine;

/* Compute a line-level diff using LCS (Longest Common Subsequence).
 * Returns a GArray of DiffLine in unified order. Caller frees with
 * g_array_free(result, TRUE). text pointers borrow from old/new_lines. */
static GArray *compute_diff_lines(gchar **old_lines, guint n_old,
                                  gchar **new_lines, guint n_new)
{
    GArray *result = g_array_new(FALSE, FALSE, sizeof(DiffLine));

    /* Build LCS table: L[(n_old+1) * (n_new+1)] flattened */
    guint cols = n_new + 1;
    guint *L = g_malloc0(sizeof(guint) * (n_old + 1) * cols);

    for (guint i = 1; i <= n_old; i++) {
        for (guint j = 1; j <= n_new; j++) {
            if (g_strcmp0(old_lines[i - 1], new_lines[j - 1]) == 0)
                L[i * cols + j] = L[(i - 1) * cols + (j - 1)] + 1;
            else
                L[i * cols + j] = MAX(L[(i - 1) * cols + j],
                                      L[i * cols + (j - 1)]);
        }
    }

    /* Backtrack to produce edit script (in reverse) */
    guint i = n_old, j = n_new;
    while (i > 0 || j > 0) {
        DiffLine dl;
        if (i > 0 && j > 0 &&
            g_strcmp0(old_lines[i - 1], new_lines[j - 1]) == 0) {
            dl.kind = DIFF_UNCHANGED;
            dl.text = old_lines[i - 1];
            i--; j--;
        } else if (j > 0 &&
                   (i == 0 || L[i * cols + (j - 1)] >= L[(i - 1) * cols + j])) {
            dl.kind = DIFF_ADDED;
            dl.text = new_lines[j - 1];
            j--;
        } else {
            dl.kind = DIFF_REMOVED;
            dl.text = old_lines[i - 1];
            i--;
        }
        g_array_append_val(result, dl);
    }

    g_free(L);

    /* Reverse (backtracking produced reverse order) */
    for (guint a = 0, b = result->len - 1; a < b; a++, b--) {
        DiffLine tmp = g_array_index(result, DiffLine, a);
        g_array_index(result, DiffLine, a) = g_array_index(result, DiffLine, b);
        g_array_index(result, DiffLine, b) = tmp;
    }

    return result;
}

/* Create a unified diff view with LCS-based line matching.
 * Unchanged lines get no marker, removed = red bg, added = green bg. */
static GtkWidget *create_diff_source_view(const gchar *old_s,
                                           const gchar *new_s,
                                           const gchar *file_path)
{
    gchar **old_lines = old_s ? g_strsplit(old_s, "\n", -1) : NULL;
    gchar **new_lines = new_s ? g_strsplit(new_s, "\n", -1) : NULL;
    guint n_old = old_lines ? g_strv_length(old_lines) : 0;
    guint n_new = new_lines ? g_strv_length(new_lines) : 0;

    /* Compute diff */
    GArray *diff = compute_diff_lines(
        old_lines ? old_lines : (gchar *[]){NULL}, n_old,
        new_lines ? new_lines : (gchar *[]){NULL}, n_new);

    /* Build output text and per-line marker assignments */
    GString *buf = g_string_new("");
    GArray *markers = g_array_new(FALSE, FALSE, sizeof(gint));

    for (guint i = 0; i < diff->len; i++) {
        DiffLine *dl = &g_array_index(diff, DiffLine, i);
        if (buf->len > 0) g_string_append_c(buf, '\n');
        g_string_append(buf, dl->text);
        gint m = (dl->kind == DIFF_REMOVED) ? 0
               : (dl->kind == DIFF_ADDED)   ? 1
               : -1;
        g_array_append_val(markers, m);
    }

    /* Create language-highlighted widget */
    GtkWidget *sv = create_source_view_for_file(buf->str, file_path, FALSE);
    ScintillaObject *sci = g_object_get_data(G_OBJECT(sv), "scintilla");

    scintilla_send_message(sci, SCI_SETREADONLY, 0, 0);

    /* Marker 0 = removed (red), Marker 1 = added (green) */
    scintilla_send_message(sci, SCI_MARKERDEFINE, 0, SC_MARK_BACKGROUND);
    scintilla_send_message(sci, SCI_MARKERSETBACK, 0, 0x5555CC); /* red (BGR) */
    scintilla_send_message(sci, SCI_MARKERSETALPHA, 0, 40);

    scintilla_send_message(sci, SCI_MARKERDEFINE, 1, SC_MARK_BACKGROUND);
    scintilla_send_message(sci, SCI_MARKERSETBACK, 1, 0x55CC55); /* green (BGR) */
    scintilla_send_message(sci, SCI_MARKERSETALPHA, 1, 40);

    /* Apply markers per line */
    for (guint i = 0; i < markers->len; i++) {
        gint m = g_array_index(markers, gint, i);
        if (m >= 0)
            scintilla_send_message(sci, SCI_MARKERADD, i, m);
    }

    scintilla_send_message(sci, SCI_SETREADONLY, 1, 0);

    g_array_free(diff, TRUE);
    g_array_free(markers, TRUE);
    g_strfreev(old_lines);
    g_strfreev(new_lines);
    g_string_free(buf, TRUE);

    return sv;
}

/* Forward declaration — defined further down after CSS setup */
static void on_jump_btn_clicked(GtkButton *btn, gpointer data);

/* ── Grep result rendering ────────────────────────────────────────── */

/* A group of grep matches within a single file */
typedef struct {
    gchar  *file_path;
    GArray *line_nums;   /* gint */
    GString *content;    /* concatenated matched lines */
} GrepFileGroup;

/* Try to parse a line as "file:linenum:content".
 * Returns TRUE if parsed, sets *out_file, *out_line, *out_content.
 * The caller must NOT free out_file/out_content — they point into the line. */
static gboolean parse_grep_line(const gchar *line,
                                const gchar **out_file, gint *out_line,
                                const gchar **out_content)
{
    /* Find first colon — file path portion */
    const gchar *c1 = strchr(line, ':');
    if (!c1 || c1 == line) return FALSE;

    /* Second colon after a number */
    const gchar *p = c1 + 1;
    if (*p < '0' || *p > '9') return FALSE;
    gint num = 0;
    while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
    if (*p != ':') return FALSE;

    *out_file = line;      /* file path ends at c1 */
    *out_line = num;
    *out_content = p + 1;  /* content after second colon */
    return TRUE;
}

/* Build a widget tree from grep output: groups matches by file, shows
 * file path headers and syntax-highlighted code for each group. */
static GtkWidget *render_grep_result(const gchar *result,
                                     ChatViewPrivate *priv)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Parse lines and group by file */
    gchar **lines = g_strsplit(result, "\n", -1);
    GPtrArray *groups = g_ptr_array_new();
    GrepFileGroup *cur = NULL;

    for (gchar **lp = lines; *lp; lp++) {
        const gchar *line = *lp;

        /* Skip separators ("--") between context groups */
        if (g_strcmp0(line, "--") == 0) continue;
        /* Skip empty lines */
        if (line[0] == '\0') continue;

        const gchar *fpath, *content;
        gint linenum;

        if (parse_grep_line(line, &fpath, &linenum, &content)) {
            /* Check if same file as current group (compare up to first colon) */
            gsize flen = (gsize)(strchr(line, ':') - line);
            if (!cur || strncmp(cur->file_path, fpath, flen) != 0
                     || cur->file_path[flen] != '\0') {
                /* Start new group */
                cur = g_new0(GrepFileGroup, 1);
                cur->file_path = g_strndup(fpath, flen);
                cur->line_nums = g_array_new(FALSE, FALSE, sizeof(gint));
                cur->content = g_string_new("");
                g_ptr_array_add(groups, cur);
            }
            g_array_append_val(cur->line_nums, linenum);
            if (cur->content->len > 0)
                g_string_append_c(cur->content, '\n');
            g_string_append(cur->content, content);
        } else {
            /* Not file:line:content — might be a plain file path (files_with_matches mode)
             * or other unstructured output. Show as-is. */
            GtkWidget *lbl = gtk_label_new(line);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0);
            gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
            PangoFontDescription *mono = pango_font_description_from_string("monospace 10");
            gtk_widget_override_font(lbl, mono);
            pango_font_description_free(mono);
            gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
        }
    }
    g_strfreev(lines);

    /* Render each file group */
    for (guint i = 0; i < groups->len; i++) {
        GrepFileGroup *grp = g_ptr_array_index(groups, i);

        /* File header — clickable "Jump to file" style */
        GtkWidget *file_btn = gtk_button_new_with_label(grp->file_path);
        gtk_button_set_relief(GTK_BUTTON(file_btn), GTK_RELIEF_NONE);
        gtk_widget_set_halign(file_btn, GTK_ALIGN_START);

        /* Style the label inside the button */
        GtkWidget *btn_label = gtk_bin_get_child(GTK_BIN(file_btn));
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_attr_list_insert(attrs,
            pango_attr_foreground_new(0x5555, 0x9999, 0xdddd)); /* blueish */
        gtk_label_set_attributes(GTK_LABEL(btn_label), attrs);
        pango_attr_list_unref(attrs);

        if (priv && priv->jump_cb) {
            g_object_set_data_full(G_OBJECT(file_btn), "file",
                g_strdup(grp->file_path), g_free);
            g_object_set_data(G_OBJECT(file_btn), "priv", priv);
            g_signal_connect(file_btn, "clicked",
                G_CALLBACK(on_jump_btn_clicked), NULL);
        }
        gtk_box_pack_start(GTK_BOX(box), file_btn, FALSE, FALSE, 0);

        /* Build content with line number prefixes */
        GString *numbered = g_string_new("");
        gchar **clines = g_strsplit(grp->content->str, "\n", -1);
        for (guint j = 0; clines[j]; j++) {
            gint lnum = (j < grp->line_nums->len)
                ? g_array_index(grp->line_nums, gint, j) : 0;
            if (numbered->len > 0)
                g_string_append_c(numbered, '\n');
            g_string_append_printf(numbered, "%5d %s", lnum, clines[j]);
        }
        g_strfreev(clines);

        /* Syntax-highlighted code block */
        GtkWidget *sv = create_source_view_for_file(
            numbered->str, grp->file_path, FALSE);
        g_string_free(numbered, TRUE);
        gtk_box_pack_start(GTK_BOX(box), sv, FALSE, FALSE, 0);

        /* Clean up */
        g_free(grp->file_path);
        g_array_free(grp->line_nums, TRUE);
        g_string_free(grp->content, TRUE);
        g_free(grp);
    }
    g_ptr_array_free(groups, TRUE);

    return box;
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
            /* Slightly increase line spacing for readability */
            PangoAttrList *line_attrs = pango_attr_list_new();
            pango_attr_list_insert(line_attrs, pango_attr_line_height_new(1.4));
            gtk_label_set_attributes(GTK_LABEL(label), line_attrs);
            pango_attr_list_unref(line_attrs);
            gtk_widget_set_halign(label, GTK_ALIGN_FILL);
            gtk_widget_set_hexpand(label, FALSE);
            /* Force label to wrap within allocated width */
            gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
            gtk_box_pack_start(GTK_BOX(content_box), label, FALSE, FALSE, 2);
            g_free(pango);
        } else if (seg->type == SEG_CODE && strlen(seg->text) > 0) {
            GtkWidget *sv = create_source_view(seg->text, seg->lang, FALSE);
            gtk_widget_set_margin_top(sv, 4);
            gtk_widget_set_margin_bottom(sv, 4);
            gtk_box_pack_start(GTK_BOX(content_box), sv, FALSE, FALSE, 0);
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
static gboolean is_grep_tool(const gchar *n)
{ return n && (g_strcmp0(n,"Grep")==0 || strstr(n,"grep")!=NULL); }
static gboolean is_mcp_tool(const gchar *n)
{ return n && strstr(n, "mcp__") != NULL; }

/* Get display name for tool */
static const gchar *tool_display_name(const gchar *name)
{
    if (g_strcmp0(name, "TodoWrite") == 0) return "Todo";
    if (is_edit_tool(name)) return "Edit";
    if (is_write_tool(name)) return "Write";
    if (is_read_tool(name)) return "Read";
    if (is_bash_tool(name)) return "Bash";
    if (is_glob_tool(name)) return "Glob";
    if (is_grep_tool(name)) return "Grep";
    if (is_mcp_tool(name)) {
        /* Use last segment after "__" as display name */
        const gchar *last = strrchr(name, '_');
        if (last && last > name && *(last - 1) == '_')
            return last + 1;
        return name;
    }
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
    ".thinking-border { border: 1px solid alpha(@insensitive_fg_color, 0.3); "
    "  border-radius: 6px; margin-top: 4px; margin-bottom: 4px; }\n"
    ".thinking-header { padding: 4px 10px; }\n"
    ".thinking-header:hover { background: alpha(@theme_fg_color, 0.06); }\n"
    ".thinking-body { padding: 6px 10px; }\n"
    ".thinking-label { color: @insensitive_fg_color; font-style: italic; }\n"
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
    ".attachment-chip { border: 1px solid @borders; border-radius: 6px; "
    "  padding: 2px 4px; }\n"
    ;

/* ── Widget construction ─────────────────────────────────────────── */

/* Constrain content width to viewport — prevents horizontal overflow */
static void on_scroll_size_allocate(GtkWidget *widget, GdkRectangle *alloc,
                                    gpointer data)
{
    GtkWidget *outer_box = GTK_WIDGET(data);
    /* Leave room for scrollbar (~16px) */
    gint content_width = alloc->width - 16;
    if (content_width < 100) content_width = 100;
    gtk_widget_set_size_request(outer_box, content_width, -1);
}

GtkWidget *chat_webview_new(void)
{
    ChatViewPrivate *priv = g_new0(ChatViewPrivate, 1);
    priv->msg_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, free_message_entry);
    priv->tool_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, free_tool_entry);
    priv->thinking_widgets = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, free_thinking_entry);

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

    /* Track container width to constrain content */
    g_signal_connect(priv->scroll, "size-allocate",
                     G_CALLBACK(on_scroll_size_allocate), priv->outer_box);

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
        /* Update status — red dot for errors, green for success */
        gboolean is_error = (tool_name && g_strcmp0(tool_name, "(error)") == 0);
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(existing->status_label), "tool-status-running");
        gtk_style_context_add_class(
            gtk_widget_get_style_context(existing->status_label),
            is_error ? "tool-status-error" : "tool-status-success");
        gtk_label_set_text(GTK_LABEL(existing->status_label), "\u25CF");

        /* Append result — render based on tool type */
        if (result && strlen(result) > 0) {
            GtkWidget *result_widget = NULL;

            if (is_read_tool(existing->tool_name)) {
                /* Strip line number prefixes and get starting line */
                gint start_line = 1;
                gchar *stripped = strip_line_numbers(result, &start_line);

                /* Detect language from file path via Geany filetypes */
                GeanyFiletype *ft = existing->file_path
                    ? filetypes_detect_from_file(existing->file_path) : NULL;
                const gchar *lang_id = ft ? ft->name : NULL;

                result_widget = create_source_view_with_offset(
                    stripped, lang_id, start_line);
                g_free(stripped);
            } else if (is_bash_tool(existing->tool_name)) {
                /* Bash output as plain monospace text */
                result_widget = create_source_view(result, NULL, FALSE);
            } else if (is_grep_tool(existing->tool_name)) {
                result_widget = render_grep_result(result, priv);
            } else if (is_mcp_tool(existing->tool_name)) {
                /* MCP tool output as plain monospace text */
                result_widget = create_source_view(result, NULL, FALSE);
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
    te->expanded = is_edit_tool(tool_name) || is_write_tool(tool_name);

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
            /* Unified diff block: removed lines (red bg) then added (green bg),
             * all language-highlighted from the file extension */
            GtkWidget *sv = create_diff_source_view(old_s, new_s, file_path);
            gtk_box_pack_start(GTK_BOX(te->body_box), sv, FALSE, FALSE, 0);

            /* Jump to file link */
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
        /* Detect language from filename via Geany filetypes */
        GtkWidget *sv = create_source_view_for_file(wcontent, file_path, TRUE);
        gtk_box_pack_start(GTK_BOX(te->body_box), sv, FALSE, FALSE, 0);
    } else if (is_bash_tool(tool_name) && input_obj &&
               json_object_has_member(input_obj, "command")) {
        const gchar *cmd = json_object_get_string_member(input_obj, "command");
        /* Show command as plain monospace text */
        GtkWidget *cmd_sv = create_source_view(cmd, NULL, FALSE);
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
        GtkWidget *result_widget = NULL;
        if (is_grep_tool(tool_name)) {
            result_widget = render_grep_result(result, priv);
        } else {
            result_widget = gtk_label_new(result);
            gtk_label_set_line_wrap(GTK_LABEL(result_widget), TRUE);
            gtk_label_set_xalign(GTK_LABEL(result_widget), 0);
            gtk_label_set_selectable(GTK_LABEL(result_widget), TRUE);
        }
        gtk_box_pack_start(GTK_BOX(te->body_box), result_widget, FALSE, FALSE, 0);
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

/* ── Thinking blocks ─────────────────────────────────────────────── */

static gboolean on_thinking_header_click(GtkWidget *w, GdkEventButton *ev,
                                          gpointer data)
{
    (void)w; (void)ev;
    ThinkingEntry *te = data;
    te->expanded = !te->expanded;
    gtk_revealer_set_reveal_child(GTK_REVEALER(te->revealer), te->expanded);
    gtk_label_set_text(GTK_LABEL(te->arrow_label),
                       te->expanded ? "\u25BC" : "\u25B6");
    return TRUE;
}

void chat_webview_add_thinking(GtkWidget *webview,
                               const gchar *msg_id, guint fragment_index,
                               const gchar *text, gboolean is_streaming)
{
    ChatViewPrivate *priv = get_priv(webview);
    if (!priv) return;

    gchar *key = g_strdup_printf("%s:%u", msg_id, fragment_index);

    /* Update existing thinking widget */
    ThinkingEntry *existing = g_hash_table_lookup(priv->thinking_widgets, key);
    if (existing) {
        /* Skip if unchanged */
        if (existing->last_text && g_strcmp0(existing->last_text, text) == 0) {
            g_free(key);
            return;
        }
        g_free(existing->last_text);
        existing->last_text = g_strdup(text);
        gtk_label_set_text(GTK_LABEL(existing->content_label), text);
        scroll_to_bottom(priv);
        g_free(key);
        return;
    }

    /* Create new thinking block */
    ThinkingEntry *te = g_new0(ThinkingEntry, 1);
    te->key = g_strdup(key);
    te->expanded = FALSE;  /* collapsed by default */
    te->last_text = g_strdup(text);

    /* Outer box */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(outer),
                                "thinking-border");

    /* Header — clickable to expand/collapse */
    GtkWidget *header_event = gtk_event_box_new();
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(header_box),
                                "thinking-header");

    te->arrow_label = gtk_label_new("\u25B6");  /* collapsed */
    GtkWidget *title = gtk_label_new(is_streaming ? "Thinking\u2026" : "Thinking");
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "thinking-label");

    gtk_box_pack_start(GTK_BOX(header_box), te->arrow_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_box), title, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(header_event), header_box);
    g_signal_connect(header_event, "button-press-event",
                     G_CALLBACK(on_thinking_header_click), te);
    gtk_box_pack_start(GTK_BOX(outer), header_event, FALSE, FALSE, 0);

    /* Revealer + body */
    te->revealer = gtk_revealer_new();
    gtk_revealer_set_reveal_child(GTK_REVEALER(te->revealer), te->expanded);
    gtk_revealer_set_transition_type(GTK_REVEALER(te->revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);

    te->body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(te->body_box),
                                "thinking-body");

    te->content_label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(te->content_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(te->content_label), 0);
    gtk_label_set_selectable(GTK_LABEL(te->content_label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(te->content_label),
                                "thinking-label");
    gtk_box_pack_start(GTK_BOX(te->body_box), te->content_label, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(te->revealer), te->body_box);
    gtk_box_pack_start(GTK_BOX(outer), te->revealer, FALSE, FALSE, 0);

    /* Add row to list */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_container_add(GTK_CONTAINER(row), outer);
    te->row = row;

    /* Hide welcome if still visible */
    if (priv->welcome && gtk_widget_get_visible(priv->welcome))
        gtk_widget_hide(priv->welcome);

    gtk_list_box_insert(GTK_LIST_BOX(priv->list_box), row, -1);
    gtk_widget_show_all(row);

    g_hash_table_insert(priv->thinking_widgets, key, te);
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
    g_hash_table_remove_all(priv->thinking_widgets);

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
