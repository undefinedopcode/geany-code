#include "editor_bridge.h"
#include "plugin.h"
#include <string.h>

const gchar *editor_bridge_get_current_file(void)
{
    GeanyDocument *doc = document_get_current();
    if (doc && doc->is_valid && doc->file_name)
        return doc->file_name;
    return NULL;
}

gchar *editor_bridge_get_selection(void)
{
    GeanyDocument *doc = document_get_current();
    if (!doc || !doc->is_valid)
        return NULL;

    ScintillaObject *sci = doc->editor->sci;
    if (!sci_has_selection(sci))
        return NULL;

    return sci_get_selection_contents(sci);
}

gchar *editor_bridge_get_project_root(void)
{
    /* First: check if Geany has a project open */
    if (geany_data->app->project) {
        const gchar *base = geany_data->app->project->base_path;
        if (base && strlen(base) > 0)
            return g_strdup(base);
    }

    /* Second: walk up from current file looking for VCS markers */
    const gchar *file = editor_bridge_get_current_file();
    if (file) {
        gchar *dir = g_path_get_dirname(file);

        static const gchar *markers[] = {
            ".git", ".hg", ".svn",
            "Makefile", "CMakeLists.txt", "meson.build",
            "package.json", "Cargo.toml", "go.mod",
            NULL
        };

        while (dir && strcmp(dir, "/") != 0) {
            for (const gchar **m = markers; *m; m++) {
                gchar *path = g_build_filename(dir, *m, NULL);
                gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
                g_free(path);
                if (exists)
                    return dir;
            }

            gchar *parent = g_path_get_dirname(dir);
            g_free(dir);
            dir = parent;
        }

        g_free(dir);

        /* Fallback: directory of current file */
        return g_path_get_dirname(file);
    }

    /* Third: read last session from Geany's session.conf — available at
     * startup before the project object is populated */
    {
        gchar *session_conf = g_build_filename(
            g_get_user_config_dir(), "geany", "session.conf", NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, session_conf, G_KEY_FILE_NONE, NULL)) {
            gchar *session_file = g_key_file_get_string(
                kf, "project", "session_file", NULL);
            if (session_file && *session_file) {
                gchar *dir = g_path_get_dirname(session_file);
                g_free(session_file);
                g_key_file_free(kf);
                g_free(session_conf);
                return dir;
            }
            g_free(session_file);
        }
        g_key_file_free(kf);
        g_free(session_conf);
    }

    return g_strdup(g_get_home_dir());
}

void editor_bridge_jump_to(const gchar *file_path,
                           gint start_line, gint end_line)
{
    if (!file_path)
        return;

    /* Open the file (or switch to it if already open) */
    GeanyDocument *doc = document_open_file(file_path, FALSE, NULL, NULL);
    if (!doc || !doc->is_valid)
        return;

    ScintillaObject *sci = doc->editor->sci;

    /* Navigate to line (Scintilla lines are 0-based) */
    gint line = (start_line > 0) ? start_line - 1 : 0;
    sci_goto_line(sci, line, TRUE);

    /* Optionally highlight the range */
    if (start_line > 0 && end_line > 0 && end_line >= start_line) {
        gint start_pos = sci_get_position_from_line(sci, start_line - 1);
        gint end_pos = sci_get_line_end_position(sci, end_line - 1);
        sci_set_selection_start(sci, start_pos);
        sci_set_selection_end(sci, end_pos);
    }
}
