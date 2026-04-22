#ifndef GEANY_CODE_CHAT_WIDGET_H
#define GEANY_CODE_CHAT_WIDGET_H

#include <gtk/gtk.h>
#include <geanyplugin.h>

/*
 * ChatWidget - Main container that combines ChatWebView + ChatInput
 * and manages the CLISession.
 *
 * This is the top-level widget added to Geany's sidebar.
 */

GtkWidget *chat_widget_new(void);

/* Focus the input field */
void chat_widget_focus_input(GtkWidget *widget);

/* Send the current selection with a prompt prefix */
void chat_widget_send_selection(GtkWidget *widget, GeanyDocument *doc);

/* Quick action: send selection with a specific instruction */
void chat_widget_quick_action(GtkWidget *widget, GeanyDocument *doc,
                              const gchar *instruction);

/* Paste an image from the clipboard into the input area */
void chat_widget_paste_image(GtkWidget *widget);

/* Show a user question inline in the chat (from MCP ask_user tool) */
void chat_widget_show_user_question(GtkWidget *widget,
                                    const gchar *request_id,
                                    const gchar *questions_json);

/* Add current selection as a context chunk */
void chat_widget_add_context_from_editor(GtkWidget *widget);

/* Open the command palette (Ctrl+Shift+P by default) */
void chat_widget_show_command_palette(GtkWidget *widget);

/* Copy the most recent assistant response to the clipboard. */
gboolean chat_widget_copy_last_response(GtkWidget *widget);

#endif /* GEANY_CODE_CHAT_WIDGET_H */
