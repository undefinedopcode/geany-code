#ifndef GEANY_CODE_CLI_SESSION_H
#define GEANY_CODE_CLI_SESSION_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/*
 * CLISession - Manages a `claude` CLI subprocess.
 *
 * Spawns `claude` in streaming JSON mode and communicates over stdin/stdout.
 * Parses streaming responses and emits callbacks for messages, tool calls,
 * permission requests, etc.
 */

typedef struct _CLISession CLISession;

CLISession *cli_session_new(void);
void        cli_session_free(CLISession *session);

/* Start the claude process in the given working directory */
gboolean cli_session_start(CLISession *session, const gchar *working_dir);

/* Send a message (with optional file context and image attachments).
 * images is a GList of gchar* base64 PNG strings (ownership NOT taken). */
void cli_session_send_message(CLISession *session, const gchar *text,
                              const gchar *file_path,
                              const gchar *selection,
                              GList *images);

/* Respond to a permission request */
void cli_session_respond_permission(CLISession *session,
                                    const gchar *request_id,
                                    const gchar *option_id);

/* Switch permission mode (stops and restarts with --resume) */
void cli_session_set_mode(CLISession *session, const gchar *mode_id);

/* Switch model via control_request (no restart needed) */
void cli_session_set_model(CLISession *session, const gchar *model_value);

/* Gracefully interrupt the current response (keeps session alive) */
void cli_session_interrupt(CLISession *session);

/* Set session_id for resume (takes a copy). Next start() will use --resume. */
void cli_session_set_session_id(CLISession *session, const gchar *session_id);

/* Kill the process */
void cli_session_stop(CLISession *session);

/* Check if the session is active */
gboolean cli_session_is_running(CLISession *session);

/* Get the current working directory (may be NULL before first start) */
const gchar *cli_session_get_working_dir(CLISession *session);

/* ── Callbacks ───────────────────────────────────────────────────── */

typedef void (*CLIMessageCb)(const gchar *msg_id, const gchar *role,
                              const gchar *content, gboolean is_streaming,
                              gpointer user_data);
typedef void (*CLIToolCallCb)(const gchar *msg_id, const gchar *tool_id,
                               const gchar *tool_name, const gchar *input_json,
                               const gchar *result,
                               gpointer user_data);
typedef void (*CLIPermissionCb)(const gchar *request_id,
                                const gchar *tool_name,
                                const gchar *description,
                                const gchar *options_json,
                                gpointer user_data);
typedef void (*CLIInitCb)(const gchar *model, const gchar *permission_mode,
                          gpointer user_data);
typedef void (*CLIModelsCb)(const gchar *models_json, gpointer user_data);
typedef void (*CLICommandsCb)(const gchar *commands_json, gpointer user_data);
typedef void (*CLITodosCb)(const gchar *todos_json, gpointer user_data);
typedef void (*CLIThinkingCb)(const gchar *msg_id, guint fragment_index,
                               const gchar *text, gboolean is_streaming,
                               gpointer user_data);
typedef void (*CLIErrorCb)(const gchar *error_msg, gpointer user_data);
typedef void (*CLIFinishedCb)(gpointer user_data);

void cli_session_set_message_callback(CLISession *session, CLIMessageCb cb,
                                      gpointer data);
void cli_session_set_tool_call_callback(CLISession *session, CLIToolCallCb cb,
                                        gpointer data);
void cli_session_set_permission_callback(CLISession *session, CLIPermissionCb cb,
                                         gpointer data);
void cli_session_set_init_callback(CLISession *session, CLIInitCb cb,
                                   gpointer data);
void cli_session_set_models_callback(CLISession *session, CLIModelsCb cb,
                                     gpointer data);
void cli_session_set_commands_callback(CLISession *session, CLICommandsCb cb,
                                       gpointer data);
void cli_session_set_todos_callback(CLISession *session, CLITodosCb cb,
                                    gpointer data);
void cli_session_set_thinking_callback(CLISession *session, CLIThinkingCb cb,
                                       gpointer data);
void cli_session_set_error_callback(CLISession *session, CLIErrorCb cb,
                                    gpointer data);
void cli_session_set_finished_callback(CLISession *session, CLIFinishedCb cb,
                                       gpointer data);

#endif /* GEANY_CODE_CLI_SESSION_H */
