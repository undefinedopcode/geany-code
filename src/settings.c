#include "settings.h"
#include "plugin.h"
#include <string.h>

struct _GeanyCodeSettings {
    GKeyFile *keyfile;
    gchar    *config_path;

    gchar    *claude_path;
    gchar    *permission_mode;
};

static gchar *get_config_dir(void)
{
    return g_build_filename(geany_data->app->configdir,
                            "plugins", "geany-code", NULL);
}

GeanyCodeSettings *settings_new(void)
{
    GeanyCodeSettings *s = g_new0(GeanyCodeSettings, 1);
    s->keyfile = g_key_file_new();

    gchar *dir = get_config_dir();
    s->config_path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);

    /* Defaults */
    s->claude_path = g_strdup("claude");
    s->permission_mode = g_strdup("approve-edits");

    return s;
}

void settings_free(GeanyCodeSettings *settings)
{
    if (!settings) return;
    g_key_file_free(settings->keyfile);
    g_free(settings->config_path);
    g_free(settings->claude_path);
    g_free(settings->permission_mode);
    g_free(settings);
}

void settings_load(GeanyCodeSettings *settings)
{
    if (!g_key_file_load_from_file(settings->keyfile, settings->config_path,
                                    G_KEY_FILE_NONE, NULL))
        return;  /* file doesn't exist yet, use defaults */

    gchar *val;

    val = g_key_file_get_string(settings->keyfile, "general",
                                "claude_path", NULL);
    if (val) { g_free(settings->claude_path); settings->claude_path = val; }

    val = g_key_file_get_string(settings->keyfile, "general",
                                "permission_mode", NULL);
    if (val) { g_free(settings->permission_mode); settings->permission_mode = val; }
}

void settings_save(GeanyCodeSettings *settings)
{
    g_key_file_set_string(settings->keyfile, "general",
                          "claude_path", settings->claude_path);
    g_key_file_set_string(settings->keyfile, "general",
                          "permission_mode", settings->permission_mode);

    /* Ensure directory exists */
    gchar *dir = g_path_get_dirname(settings->config_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    g_key_file_save_to_file(settings->keyfile, settings->config_path, NULL);
}

const gchar *settings_get_claude_path(GeanyCodeSettings *settings)
{
    return settings->claude_path;
}

void settings_set_claude_path(GeanyCodeSettings *settings, const gchar *path)
{
    g_free(settings->claude_path);
    settings->claude_path = g_strdup(path);
}

const gchar *settings_get_permission_mode(GeanyCodeSettings *settings)
{
    return settings->permission_mode;
}

void settings_set_permission_mode(GeanyCodeSettings *settings, const gchar *mode)
{
    g_free(settings->permission_mode);
    settings->permission_mode = g_strdup(mode);
}
