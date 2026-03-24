#include "chat_webview.h"
#include "editor_dbus.h"
#include <webkit2/webkit2.h>
#include <string.h>

/* ── Private data attached to the WebKitWebView widget ───────────── */

typedef struct {
    ChatWebViewPermissionCb permission_cb;
    gpointer                permission_data;
    ChatWebViewJumpToEditCb jump_cb;
    gpointer                jump_data;
} WebViewPrivate;

static const gchar *PRIV_KEY = "geany-code-webview-private";

static WebViewPrivate *get_priv(GtkWidget *webview)
{
    return g_object_get_data(G_OBJECT(webview), PRIV_KEY);
}

/* ── Custom URI scheme handler (JS → C bridge) ──────────────────── */

static void on_uri_scheme_request(WebKitURISchemeRequest *request,
                                  gpointer user_data)
{
    (void)user_data;

    const gchar *uri = webkit_uri_scheme_request_get_uri(request);
    /* URI format: geanycode://action?param1=val1&param2=val2 */

    /* Find the WebView this request came from */
    WebKitWebView *wv = webkit_uri_scheme_request_get_web_view(request);
    WebViewPrivate *priv = g_object_get_data(G_OBJECT(wv), PRIV_KEY);

    /* Parse action from host portion */
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed) goto done;

    const gchar *host = g_uri_get_host(parsed);
    const gchar *query = g_uri_get_query(parsed);

    if (g_strcmp0(host, "permission-response") == 0 && priv && priv->permission_cb) {
        /* Parse request_id and option_id from query */
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *req_id = g_hash_table_lookup(params, "request_id");
            const gchar *opt_id = g_hash_table_lookup(params, "option_id");
            if (req_id && opt_id)
                priv->permission_cb(req_id, opt_id, priv->permission_data);
            g_hash_table_unref(params);
        }
    } else if (g_strcmp0(host, "jump-to-edit") == 0 && priv && priv->jump_cb) {
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *path = g_hash_table_lookup(params, "file");
            const gchar *s = g_hash_table_lookup(params, "start");
            const gchar *e = g_hash_table_lookup(params, "end");
            if (path)
                priv->jump_cb(path,
                              s ? atoi(s) : 0,
                              e ? atoi(e) : 0,
                              priv->jump_data);
            g_hash_table_unref(params);
        }
    } else if (g_strcmp0(host, "question-response") == 0) {
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *req_id = g_hash_table_lookup(params, "request_id");
            const gchar *response = g_hash_table_lookup(params, "response");
            if (req_id && response)
                editor_dbus_provide_response(req_id, response);
            g_hash_table_unref(params);
        }
    } else if (g_strcmp0(host, "log") == 0) {
        /* Debug logging from JS */
        if (query)
            g_message("geany-code JS: %s", query);
    }

done:
    if (parsed)
        g_uri_unref(parsed);

    /* Send an empty response so WebKit doesn't show an error */
    GInputStream *stream = g_memory_input_stream_new_from_data(
        g_strdup(""), 0, g_free);
    webkit_uri_scheme_request_finish(request, stream, 0, "text/plain");
    g_object_unref(stream);
}

/* ── Helper: run JavaScript in the web view ──────────────────────── */

static void run_js(GtkWidget *webview, const gchar *script)
{
    webkit_web_view_evaluate_javascript(
        WEBKIT_WEB_VIEW(webview), script, -1,
        NULL, NULL, NULL, NULL, NULL);
}

/* Escape a string for safe embedding in a JavaScript string literal */
static gchar *js_escape(const gchar *str)
{
    if (!str) return g_strdup("");

    GString *out = g_string_sized_new(strlen(str) + 16);
    for (const gchar *p = str; *p; p++) {
        switch (*p) {
        case '\\': g_string_append(out, "\\\\"); break;
        case '\'': g_string_append(out, "\\'");  break;
        case '"':  g_string_append(out, "\\\"");  break;
        case '\n': g_string_append(out, "\\n");   break;
        case '\r': g_string_append(out, "\\r");   break;
        case '\t': g_string_append(out, "\\t");   break;
        default:
            if ((guchar)*p < 0x20)
                g_string_append_printf(out, "\\x%02x", (guchar)*p);
            else
                g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

/* ── Load callback ───────────────────────────────────────────────── */

/* Forward declaration */
void chat_webview_apply_theme(GtkWidget *webview);

static void on_load_changed(WebKitWebView *wv, WebKitLoadEvent event,
                            gpointer data)
{
    (void)data;
    if (event == WEBKIT_LOAD_FINISHED)
        chat_webview_apply_theme(GTK_WIDGET(wv));
}

/* ── Widget construction ─────────────────────────────────────────── */

GtkWidget *chat_webview_new(void)
{
    /* Register our custom URI scheme (once) */
    static gboolean scheme_registered = FALSE;
    if (!scheme_registered) {
        WebKitWebContext *ctx = webkit_web_context_get_default();
        webkit_web_context_register_uri_scheme(
            ctx, "geanycode", on_uri_scheme_request, NULL, NULL);

        /* Allow geanycode:// scheme to access file:// resources */
        WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
        webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "geanycode");
        scheme_registered = TRUE;
    }

    /* Create web view with permissive local settings */
    WebKitSettings *settings = webkit_settings_new_with_settings(
        "enable-javascript", TRUE,
        "allow-file-access-from-file-urls", TRUE,
        "allow-universal-access-from-file-urls", TRUE,
        "enable-developer-extras", TRUE,  /* for debugging with inspector */
        NULL);

    GtkWidget *webview = webkit_web_view_new_with_settings(settings);
    g_object_unref(settings);

    /* Attach private data */
    WebViewPrivate *priv = g_new0(WebViewPrivate, 1);
    g_object_set_data_full(G_OBJECT(webview), PRIV_KEY, priv, g_free);

    /* Apply theme once the page finishes loading */
    g_signal_connect(webview, "load-changed",
        G_CALLBACK(on_load_changed), NULL);

    /* Load the chat HTML */
    gchar *html_path = g_build_filename(
        GEANY_CODE_DATA_DIR, "web", "chat.html", NULL);
    gchar *html_uri = g_filename_to_uri(html_path, NULL, NULL);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webview), html_uri);
    g_free(html_uri);
    g_free(html_path);

    return webview;
}

/* ── C → JS API ──────────────────────────────────────────────────── */

void chat_webview_add_message(GtkWidget *webview,
                              const gchar *id, const gchar *role,
                              const gchar *content, const gchar *timestamp,
                              gboolean is_streaming)
{
    gchar *esc_id = js_escape(id);
    gchar *esc_role = js_escape(role);
    gchar *esc_content = js_escape(content);
    gchar *esc_ts = js_escape(timestamp);

    gchar *script = g_strdup_printf(
        "addMessage('%s', '%s', '%s', '%s', %s, []);",
        esc_id, esc_role, esc_content, esc_ts,
        is_streaming ? "true" : "false");

    run_js(webview, script);

    g_free(script);
    g_free(esc_id);
    g_free(esc_role);
    g_free(esc_content);
    g_free(esc_ts);
}

void chat_webview_update_message(GtkWidget *webview,
                                 const gchar *id, const gchar *content,
                                 gboolean is_streaming)
{
    gchar *esc_id = js_escape(id);
    gchar *esc_content = js_escape(content);

    gchar *script = g_strdup_printf(
        "updateMessage('%s', '%s', %s);",
        esc_id, esc_content,
        is_streaming ? "true" : "false");

    run_js(webview, script);

    g_free(script);
    g_free(esc_id);
    g_free(esc_content);
}

void chat_webview_add_tool_call(GtkWidget *webview,
                                const gchar *msg_id, const gchar *tool_id,
                                const gchar *tool_name, const gchar *input_json,
                                const gchar *result)
{
    gchar *esc_msg = js_escape(msg_id);
    gchar *esc_tid = js_escape(tool_id);
    gchar *esc_name = js_escape(tool_name);
    gchar *esc_input = js_escape(input_json);
    gchar *esc_result = js_escape(result ? result : "");

    gchar *script = g_strdup_printf(
        "addToolCall('%s', '%s', '%s', '%s', '%s');",
        esc_msg, esc_tid, esc_name, esc_input, esc_result);

    run_js(webview, script);

    g_free(script);
    g_free(esc_msg);
    g_free(esc_tid);
    g_free(esc_name);
    g_free(esc_input);
    g_free(esc_result);
}

void chat_webview_show_permission(GtkWidget *webview,
                                  const gchar *request_id,
                                  const gchar *tool_name,
                                  const gchar *description,
                                  const gchar *options_json)
{
    gchar *esc_rid = js_escape(request_id);
    gchar *esc_tool = js_escape(tool_name);
    gchar *esc_desc = js_escape(description);
    gchar *esc_opts = js_escape(options_json);

    gchar *script = g_strdup_printf(
        "showPermissionRequest('%s', '%s', '%s', '%s');",
        esc_rid, esc_tool, esc_desc, esc_opts);

    run_js(webview, script);

    g_free(script);
    g_free(esc_rid);
    g_free(esc_tool);
    g_free(esc_desc);
    g_free(esc_opts);
}

void chat_webview_add_message_image(GtkWidget *webview,
                                    const gchar *msg_id,
                                    const gchar *b64_png)
{
    gchar *esc_id = js_escape(msg_id);
    /* Call JS to append an <img> to the message element */
    gchar *script = g_strdup_printf(
        "addMessageImage('%s', '%s');", esc_id, b64_png);
    run_js(webview, script);
    g_free(script);
    g_free(esc_id);
}

void chat_webview_show_user_question(GtkWidget *webview,
                                     const gchar *request_id,
                                     const gchar *questions_json)
{
    gchar *esc_id = js_escape(request_id);
    gchar *esc_json = js_escape(questions_json);
    gchar *script = g_strdup_printf(
        "showUserQuestion('%s', '%s');", esc_id, esc_json);
    run_js(webview, script);
    g_free(script);
    g_free(esc_id);
    g_free(esc_json);
}

void chat_webview_update_todos(GtkWidget *webview, const gchar *todos_json)
{
    gchar *esc = js_escape(todos_json);
    gchar *script = g_strdup_printf("updateTodos('%s');", esc);
    run_js(webview, script);
    g_free(script);
    g_free(esc);
}

void chat_webview_clear(GtkWidget *webview)
{
    run_js(webview, "document.getElementById('messages').innerHTML = '';"
                    "document.getElementById('welcome-screen').style.display = 'flex';");
}

/* Parse a @define-color line: "@define-color name #hex;" -> hash table */
static void parse_gtk_color_line(const gchar *line, GHashTable *colors)
{
    if (!g_str_has_prefix(line, "@define-color "))
        return;
    const gchar *p = line + 14; /* skip "@define-color " */
    const gchar *space = strchr(p, ' ');
    if (!space) return;

    gchar *name = g_strndup(p, space - p);
    /* skip space, grab the rest minus trailing semicolon/whitespace */
    const gchar *val_start = space + 1;
    gchar *val = g_strstrip(g_strdup(val_start));
    gsize len = strlen(val);
    if (len > 0 && val[len - 1] == ';')
        val[len - 1] = '\0';
    g_strstrip(val);

    g_hash_table_insert(colors, name, val);
}

static GHashTable *load_gtk3_colors(void)
{
    GHashTable *colors = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);

    gchar *path = g_build_filename(
        g_get_home_dir(), ".config", "gtk-3.0", "colors.css", NULL);
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        g_free(path);
        return colors;
    }
    g_free(path);

    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **l = lines; *l; l++)
        parse_gtk_color_line(*l, colors);

    g_strfreev(lines);
    g_free(contents);
    return colors;
}

static const gchar *color_lookup(GHashTable *colors, const gchar *name,
                                  const gchar *fallback)
{
    const gchar *val = g_hash_table_lookup(colors, name);
    return val ? val : fallback;
}

void chat_webview_apply_theme(GtkWidget *webview)
{
    GHashTable *colors = load_gtk3_colors();

    /* Map GTK/Breeze color names to our CSS variables */
    const gchar *bg      = color_lookup(colors, "theme_base_color_breeze", "#1e1e1e");
    const gchar *bg2     = color_lookup(colors, "theme_button_background_normal_breeze", "#2d2d2d");
    const gchar *fg      = color_lookup(colors, "theme_fg_color_breeze", "#d4d4d4");
    const gchar *fg2     = color_lookup(colors, "insensitive_fg_color_breeze", "#808080");
    const gchar *accent  = color_lookup(colors, "theme_selected_bg_color_breeze", "#007acc");
    const gchar *link    = color_lookup(colors, "link_color_breeze", "#4a9eff");
    const gchar *pos     = color_lookup(colors, "success_color_breeze", "#4ec9b0");
    const gchar *neg     = color_lookup(colors, "error_color_breeze", "#f48771");
    const gchar *border  = color_lookup(colors, "borders_breeze", "#404040");
    const gchar *sel_bg  = color_lookup(colors, "theme_hovering_selected_bg_color_breeze", "#264f78");
    const gchar *sel_fg  = color_lookup(colors, "theme_selected_fg_color_breeze", "#ffffff");
    const gchar *tooltip = color_lookup(colors, "tooltip_background_breeze", "#1a1a2e");

    /* Read font from GTK settings */
    GtkSettings *gtk_settings = gtk_settings_get_default();
    gchar *gtk_font = NULL;
    if (gtk_settings)
        g_object_get(gtk_settings, "gtk-font-name", &gtk_font, NULL);

    /* Parse "Font Name, Size" format */
    gchar *font_family = NULL;
    gchar *font_size = NULL;
    if (gtk_font) {
        /* Find last comma — everything before is family, after is size */
        gchar *last_comma = g_strrstr(gtk_font, ",");
        if (last_comma) {
            font_family = g_strndup(gtk_font, last_comma - gtk_font);
            font_size = g_strstrip(g_strdup(last_comma + 1));
        } else {
            font_family = g_strdup(gtk_font);
        }
    }

    GString *script = g_string_new("(function() { var s = document.documentElement.style;\n");
    g_string_append_printf(script, "s.setProperty('--bg-primary', '%s');\n", bg);
    g_string_append_printf(script, "s.setProperty('--bg-secondary', '%s');\n", bg2);
    g_string_append_printf(script, "s.setProperty('--bg-tertiary', '%s');\n", border);
    g_string_append_printf(script, "s.setProperty('--fg-primary', '%s');\n", fg);
    g_string_append_printf(script, "s.setProperty('--fg-secondary', '%s');\n", fg2);
    g_string_append_printf(script, "s.setProperty('--accent', '%s');\n", accent);
    g_string_append_printf(script, "s.setProperty('--link', '%s');\n", link);
    g_string_append_printf(script, "s.setProperty('--positive', '%s');\n", pos);
    g_string_append_printf(script, "s.setProperty('--negative', '%s');\n", neg);
    g_string_append_printf(script, "s.setProperty('--border', '%s');\n", border);
    g_string_append_printf(script, "s.setProperty('--selection-bg', '%s');\n", sel_bg);
    g_string_append_printf(script, "s.setProperty('--selection-fg', '%s');\n", sel_fg);
    g_string_append_printf(script, "s.setProperty('--code-bg', '%s');\n", tooltip);
    if (font_family)
        g_string_append_printf(script,
            "document.body.style.fontFamily = '\"%s\", sans-serif';\n",
            font_family);
    if (font_size)
        g_string_append_printf(script,
            "document.body.style.fontSize = '%spx';\n", font_size);
    g_string_append(script, "})();");

    g_free(gtk_font);
    g_free(font_family);
    g_free(font_size);

    run_js(webview, script->str);
    g_string_free(script, TRUE);
    g_hash_table_unref(colors);
}

/* ── Callback setters ────────────────────────────────────────────── */

void chat_webview_set_permission_callback(GtkWidget *webview,
                                          ChatWebViewPermissionCb cb,
                                          gpointer user_data)
{
    WebViewPrivate *priv = get_priv(webview);
    if (priv) {
        priv->permission_cb = cb;
        priv->permission_data = user_data;
    }
}

void chat_webview_set_jump_callback(GtkWidget *webview,
                                    ChatWebViewJumpToEditCb cb,
                                    gpointer user_data)
{
    WebViewPrivate *priv = get_priv(webview);
    if (priv) {
        priv->jump_cb = cb;
        priv->jump_data = user_data;
    }
}
