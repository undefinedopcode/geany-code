#ifndef GEANY_CODE_SESSION_PICKER_H
#define GEANY_CODE_SESSION_PICKER_H

#include <gtk/gtk.h>

typedef struct {
    gchar *session_id;     /* UUID from filename (minus .jsonl) */
    gchar *slug;           /* human-readable slug, may be NULL */
    gchar *timestamp;      /* ISO8601 timestamp, may be NULL */
    gchar *first_message;  /* first external user message, truncated */
    gint64 mtime;          /* file modification time (unix epoch) */
} SessionInfo;

/* Discover sessions for the given working directory.
 * Returns a GList of SessionInfo*, sorted newest-first.
 * Caller frees with session_info_list_free(). */
GList *session_discover(const gchar *working_dir);

void session_info_free(SessionInfo *info);
void session_info_list_free(GList *list);

/* Show the session picker dialog.
 * Returns the selected session_id (caller frees with g_free),
 * empty string "" if "Start new session" was chosen, or NULL if cancelled. */
gchar *session_picker_run(GtkWindow *parent, const gchar *working_dir);

/* ── Session history ─────────────────────────────────────────────── */

typedef struct {
    gchar *tool_id;
    gchar *tool_name;
    gchar *input_json;
} HistoryToolCall;

typedef struct {
    gchar *uuid;       /* message UUID from JSONL */
    gchar *role;       /* "user" or "assistant" */
    gchar *content;    /* extracted text content (no tool blocks) */
    gchar *timestamp;  /* ISO8601 timestamp, may be NULL */
    GList *tool_calls; /* GList of HistoryToolCall* (may be NULL) */
} HistoryMessage;

void history_message_free(HistoryMessage *msg);
void history_message_list_free(GList *list);

/* Load last max_messages displayable messages from a session file.
 * Returns a GList of HistoryMessage*, in chronological order (oldest first).
 * Caller frees with history_message_list_free(). */
GList *session_load_history(const gchar *working_dir,
                            const gchar *session_id,
                            guint max_messages);

#endif /* GEANY_CODE_SESSION_PICKER_H */
