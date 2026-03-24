#ifndef GEANY_CODE_CHAT_WEBVIEW_H
#define GEANY_CODE_CHAT_WEBVIEW_H

#include <gtk/gtk.h>

/*
 * ChatWebView - WebKit2GTK-based chat renderer.
 *
 * Wraps a WebKitWebView that loads our chat.html/css/js assets and provides
 * a C↔JavaScript bridge for message rendering, tool call display, permission
 * requests, and user questions.
 *
 * JS→C communication uses a custom URI scheme ("geanycode://action/data").
 * C→JS communication uses webkit_web_view_evaluate_javascript().
 */

GtkWidget *chat_webview_new(void);

/* ── C → JavaScript calls ────────────────────────────────────────── */

/* Add or update a chat message */
void chat_webview_add_message(GtkWidget *webview,
                              const gchar *id, const gchar *role,
                              const gchar *content, const gchar *timestamp,
                              gboolean is_streaming);
void chat_webview_update_message(GtkWidget *webview,
                                 const gchar *id, const gchar *content,
                                 gboolean is_streaming);

/* Append an inline image to a message */
void chat_webview_add_message_image(GtkWidget *webview,
                                    const gchar *msg_id,
                                    const gchar *b64_png);

/* Tool calls and results */
void chat_webview_add_tool_call(GtkWidget *webview,
                                const gchar *msg_id, const gchar *tool_id,
                                const gchar *tool_name, const gchar *input_json,
                                const gchar *result);

/* Permission requests */
void chat_webview_show_permission(GtkWidget *webview,
                                  const gchar *request_id,
                                  const gchar *tool_name,
                                  const gchar *description,
                                  const gchar *options_json);

/* Update todos display */
void chat_webview_update_todos(GtkWidget *webview, const gchar *todos_json);

/* Show a user question inline */
void chat_webview_show_user_question(GtkWidget *webview,
                                     const gchar *request_id,
                                     const gchar *questions_json);

/* Add a dimmed historical message (for session resume) */
void chat_webview_add_history_message(GtkWidget *webview,
                                      const gchar *id, const gchar *role,
                                      const gchar *content,
                                      const gchar *timestamp);
/* Add a visual separator after history messages */
void chat_webview_add_history_separator(GtkWidget *webview);

/* Clear all messages */
void chat_webview_clear(GtkWidget *webview);

/* Set theme colors from GTK */
void chat_webview_apply_theme(GtkWidget *webview);

/* ── Callbacks from JavaScript ───────────────────────────────────── */

typedef void (*ChatWebViewPermissionCb)(const gchar *request_id,
                                        const gchar *option_id,
                                        gpointer user_data);
typedef void (*ChatWebViewJumpToEditCb)(const gchar *file_path,
                                        gint start_line, gint end_line,
                                        gpointer user_data);

void chat_webview_set_permission_callback(GtkWidget *webview,
                                          ChatWebViewPermissionCb cb,
                                          gpointer user_data);
void chat_webview_set_jump_callback(GtkWidget *webview,
                                    ChatWebViewJumpToEditCb cb,
                                    gpointer user_data);

#endif /* GEANY_CODE_CHAT_WEBVIEW_H */
