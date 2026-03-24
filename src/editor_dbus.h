#ifndef GEANY_CODE_EDITOR_DBUS_H
#define GEANY_CODE_EDITOR_DBUS_H

#include <gio/gio.h>

void editor_dbus_start(void);
void editor_dbus_stop(void);

/* Set callback for when MCP asks a user question.
 * The callback receives (request_id, questions_json, user_data). */
void editor_dbus_set_question_callback(
    void (*cb)(const gchar *request_id, const gchar *questions_json,
               gpointer user_data),
    gpointer user_data);

/* Provide the user's answer to a pending question */
void editor_dbus_provide_response(const gchar *request_id,
                                  const gchar *response_json);

#endif /* GEANY_CODE_EDITOR_DBUS_H */
