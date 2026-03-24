#ifndef GEANY_CODE_CHAT_INPUT_H
#define GEANY_CODE_CHAT_INPUT_H

#include <gtk/gtk.h>

/*
 * ChatInput - Text input area with send/stop buttons, mode/model selectors,
 * and image attachment chips.
 *
 * Layout: [image chips ...]
 *         [Mode combo] [Model combo]
 *         [text entry] [stop] [send]
 */

GtkWidget *chat_input_new(void);

/* Get/set the input text */
const gchar *chat_input_get_text(GtkWidget *input);
void         chat_input_set_text(GtkWidget *input, const gchar *text);
void         chat_input_clear(GtkWidget *input);

/* Focus the text entry */
void chat_input_grab_focus(GtkWidget *input);

/* Enable/disable send vs stop button */
void chat_input_set_busy(GtkWidget *input, gboolean busy);

/* Set the active permission mode by ID */
void chat_input_set_mode(GtkWidget *input, const gchar *mode_id);
gchar *chat_input_get_mode(GtkWidget *input);

/* Slash command completion — commands_json is a JSON array of {name, description} */
void chat_input_set_commands(GtkWidget *input, const gchar *commands_json);

/* @ file completion — set project root to enable file scanning */
void chat_input_set_project_root(GtkWidget *input, const gchar *root);

/* Model dropdown */
void chat_input_set_models(GtkWidget *input, const gchar *models_json);
void chat_input_set_model(GtkWidget *input, const gchar *model_value);
gchar *chat_input_get_model(GtkWidget *input);

/* Image attachments */
void   chat_input_add_image(GtkWidget *input, GdkPixbuf *pixbuf, gchar *b64_data);
GList *chat_input_take_images(GtkWidget *input);

/* Context chunks — each chunk is a ContextChunk struct */
typedef struct {
    gchar *file_path;
    gchar *content;
    gint   start_line;
    gint   end_line;
} ContextChunk;

void   chat_input_add_context(GtkWidget *input, const gchar *file_path,
                               const gchar *content,
                               gint start_line, gint end_line);
GList *chat_input_take_contexts(GtkWidget *input);

/* Callbacks */
typedef void (*ChatInputSendCb)(const gchar *text, gpointer user_data);
typedef void (*ChatInputStopCb)(gpointer user_data);
typedef void (*ChatInputModeChangedCb)(const gchar *mode_id, gpointer user_data);
typedef void (*ChatInputModelChangedCb)(const gchar *model_value, gpointer user_data);

void chat_input_set_mode_changed_callback(GtkWidget *input,
                                           ChatInputModeChangedCb cb,
                                           gpointer user_data);
void chat_input_set_model_changed_callback(GtkWidget *input,
                                            ChatInputModelChangedCb cb,
                                            gpointer user_data);

void chat_input_set_send_callback(GtkWidget *input, ChatInputSendCb cb,
                                  gpointer user_data);
void chat_input_set_stop_callback(GtkWidget *input, ChatInputStopCb cb,
                                  gpointer user_data);

#endif /* GEANY_CODE_CHAT_INPUT_H */
