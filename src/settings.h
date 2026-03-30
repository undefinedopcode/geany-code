#ifndef GEANY_CODE_SETTINGS_H
#define GEANY_CODE_SETTINGS_H

#include <glib.h>

/*
 * Settings - Plugin configuration backed by a GKeyFile.
 *
 * Stored in ~/.config/geany/plugins/geany-code/settings.ini
 */

typedef struct _GeanyCodeSettings GeanyCodeSettings;

GeanyCodeSettings *settings_new(void);
void               settings_free(GeanyCodeSettings *settings);

/* Load/save from disk */
void settings_load(GeanyCodeSettings *settings);
void settings_save(GeanyCodeSettings *settings);

/* Accessors */
const gchar *settings_get_claude_path(GeanyCodeSettings *settings);
void         settings_set_claude_path(GeanyCodeSettings *settings,
                                      const gchar *path);

const gchar *settings_get_permission_mode(GeanyCodeSettings *settings);
void         settings_set_permission_mode(GeanyCodeSettings *settings,
                                          const gchar *mode);

/* Diff color scheme: "green-red" (default), "blue-red", "purple-orange" */
const gchar *settings_get_diff_colors(GeanyCodeSettings *settings);
void         settings_set_diff_colors(GeanyCodeSettings *settings,
                                      const gchar *scheme);

#endif /* GEANY_CODE_SETTINGS_H */
