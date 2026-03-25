#ifndef GEANY_CODE_EDITOR_BRIDGE_H
#define GEANY_CODE_EDITOR_BRIDGE_H

#include <geanyplugin.h>

/*
 * EditorBridge - Interface between the chat and Geany's editor.
 *
 * Provides document access (current file, selection, project root)
 * and navigation (jump to file/line).
 */

/* Get the file path of the active document (or NULL) */
const gchar *editor_bridge_get_current_file(void);

/* Get the selected text in the active document (caller must g_free) */
gchar *editor_bridge_get_selection(void);

/* Get the project root (VCS markers or Geany project dir) */
gchar *editor_bridge_get_project_root(void);

/* Get the git root directory (walks up looking for .git).
 * Falls back to project root if no .git found. Caller must g_free. */
gchar *editor_bridge_get_git_root(void);

/* Open a file and optionally highlight a line range */
void editor_bridge_jump_to(const gchar *file_path,
                           gint start_line, gint end_line);

#endif /* GEANY_CODE_EDITOR_BRIDGE_H */
