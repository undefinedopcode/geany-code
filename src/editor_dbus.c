#include "editor_dbus.h"
#include "plugin.h"
#include <Scintilla.h>
#include <string.h>
#include <sys/stat.h>

/* Reload document from disk only if the file on disk differs
 * from the buffer (by size). Avoids marking all lines as changed. */
static void reload_if_stale(GeanyDocument *doc)
{
    if (!doc || !doc->is_valid || !doc->file_name)
        return;

    struct stat st;
    if (stat(doc->file_name, &st) != 0)
        return;  /* file doesn't exist on disk yet */

    gint buf_len = sci_get_length(doc->editor->sci);
    if ((gint)st.st_size != buf_len)
        document_reload_force(doc, NULL);
}

static GDBusConnection *connection = NULL;
static guint registration_id = 0;
static guint owner_id = 0;

/* Pending question state */
typedef struct {
    GMainLoop *loop;
    gchar     *response;
    gboolean   completed;
} PendingQuestion;

static GHashTable *pending_questions = NULL;  /* request_id -> PendingQuestion* */
static guint next_question_id = 0;

/* Callback for showing questions in the chat UI */
static void (*question_requested_cb)(const gchar *request_id,
                                     const gchar *questions_json,
                                     gpointer user_data) = NULL;
static gpointer question_requested_data = NULL;

/* DBus introspection XML */
static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='org.geanycode.Editor'>"
    "    <method name='ListDocuments'>"
    "      <arg type='as' name='documents' direction='out'/>"
    "    </method>"
    "    <method name='ReadDocument'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='s' name='content' direction='out'/>"
    "    </method>"
    "    <method name='EditDocument'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='s' name='oldText' direction='in'/>"
    "      <arg type='s' name='newText' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='WriteDocument'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='s' name='content' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='GotoLine'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='i' name='line' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='Build'>"
    "      <arg type='s' name='command' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='GetProject'>"
    "      <arg type='s' name='info' direction='out'/>"
    "    </method>"
    "    <method name='AskUserQuestion'>"
    "      <arg type='s' name='questionsJson' direction='in'/>"
    "      <arg type='s' name='responseJson' direction='out'/>"
    "    </method>"
    "    <method name='ProvideQuestionResponse'>"
    "      <arg type='s' name='requestId' direction='in'/>"
    "      <arg type='s' name='responseJson' direction='in'/>"
    "    </method>"
    "    <method name='Search'>"
    "      <arg type='s' name='pattern' direction='in'/>"
    "      <arg type='b' name='regex' direction='in'/>"
    "      <arg type='s' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SetIndicator'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='i' name='line' direction='in'/>"
    "      <arg type='i' name='indicatorType' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='ClearIndicators'>"
    "      <arg type='s' name='filePath' direction='in'/>"
    "      <arg type='s' name='result' direction='out'/>"
    "    </method>"
    "    <method name='GetSelection'>"
    "      <arg type='s' name='info' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static GDBusNodeInfo *introspection_data = NULL;

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Ensure a file is open in Geany and up to date. Returns the document. */
static GeanyDocument *ensure_document(const gchar *file_path)
{
    /* Check if already open */
    guint n = geany_data->documents_array->len;
    for (guint i = 0; i < n; i++) {
        GeanyDocument *doc = g_ptr_array_index(geany_data->documents_array, i);
        if (doc && doc->is_valid && doc->file_name &&
            g_strcmp0(doc->file_name, file_path) == 0) {
            return doc;
        }
    }

    /* Not open — open it */
    return document_open_file(file_path, FALSE, NULL, NULL);
}

/* ── Method handler ──────────────────────────────────────────────── */

static void handle_method_call(GDBusConnection       *conn,
                               const gchar           *sender,
                               const gchar           *object_path,
                               const gchar           *interface_name,
                               const gchar           *method_name,
                               GVariant              *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer               user_data)
{
    (void)conn; (void)sender; (void)object_path;
    (void)interface_name; (void)user_data;

    if (g_strcmp0(method_name, "ListDocuments") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

        guint n = geany_data->documents_array->len;
        for (guint i = 0; i < n; i++) {
            GeanyDocument *doc = g_ptr_array_index(geany_data->documents_array, i);
            if (doc && doc->is_valid && doc->file_name)
                g_variant_builder_add(&builder, "s", doc->file_name);
        }

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(as)", &builder));

    } else if (g_strcmp0(method_name, "ReadDocument") == 0) {
        const gchar *file_path = NULL;
        g_variant_get(parameters, "(&s)", &file_path);

        /* Open or find the document */
        GeanyDocument *doc = ensure_document(file_path);
        if (doc && doc->is_valid) {
            /* Reload only if disk content differs */
            reload_if_stale(doc);

            ScintillaObject *sci = doc->editor->sci;
            gchar *content = sci_get_contents(sci, -1);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", content));
            g_free(content);
        } else {
            /* Try reading from disk directly */
            gchar *content = NULL;
            if (g_file_get_contents(file_path, &content, NULL, NULL)) {
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(s)", content));
                g_free(content);
            } else {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.geanycode.Error.NotFound",
                    "File not found or not readable");
            }
        }

    } else if (g_strcmp0(method_name, "EditDocument") == 0) {
        const gchar *file_path = NULL, *old_text = NULL, *new_text = NULL;
        g_variant_get(parameters, "(&s&s&s)", &file_path, &old_text, &new_text);

        GeanyDocument *doc = ensure_document(file_path);
        if (!doc || !doc->is_valid) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.NotFound",
                "Could not open file");
            return;
        }

        /* Reload only if disk content differs from buffer */
        reload_if_stale(doc);

        ScintillaObject *sci = doc->editor->sci;

        /* Use Scintilla's own search to find the exact match position.
         * This handles UTF-8 correctly and gives us Scintilla positions. */
        gint doc_len = sci_get_length(sci);
        sci_set_target_start(sci, 0);
        sci_set_target_end(sci, doc_len);

        /* sci_find_text with struct */
        struct Sci_TextToFind ttf;
        ttf.chrg.cpMin = 0;
        ttf.chrg.cpMax = doc_len;
        ttf.lpstrText = (gchar *)old_text;

        gint found = scintilla_send_message(sci, SCI_FINDTEXT,
            SCFIND_MATCHCASE, (sptr_t)&ttf);

        if (found < 0) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.NotFound",
                "old_text not found in document (must be unique exact match)");
            return;
        }

        gint start_pos = ttf.chrgText.cpMin;
        gint end_pos = ttf.chrgText.cpMax;

        /* Check uniqueness — search again from after the match */
        struct Sci_TextToFind ttf2;
        ttf2.chrg.cpMin = end_pos;
        ttf2.chrg.cpMax = doc_len;
        ttf2.lpstrText = (gchar *)old_text;

        if (scintilla_send_message(sci, SCI_FINDTEXT,
                SCFIND_MATCHCASE, (sptr_t)&ttf2) >= 0) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.Ambiguous",
                "old_text appears multiple times in document");
            return;
        }

        /* Replace only the matched range using target replace.
         * This is the most surgical edit — only changed lines get flagged. */
        sci_set_target_start(sci, start_pos);
        sci_set_target_end(sci, end_pos);
        sci_replace_target(sci, new_text, FALSE);

        /* Save the document */
        document_save_file(doc, FALSE);

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", "Edit applied and saved"));

    } else if (g_strcmp0(method_name, "WriteDocument") == 0) {
        const gchar *file_path = NULL, *content = NULL;
        g_variant_get(parameters, "(&s&s)", &file_path, &content);

        /* Open or create the document */
        GeanyDocument *doc = ensure_document(file_path);
        if (!doc) {
            /* File doesn't exist — create it */
            doc = document_new_file(file_path, NULL, content);
        }

        if (!doc || !doc->is_valid) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.Failed",
                "Could not open or create file");
            return;
        }

        /* Set the content */
        ScintillaObject *sci = doc->editor->sci;
        sci_set_text(sci, content);

        /* Save */
        document_save_file(doc, FALSE);

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", "File written and saved"));

    } else if (g_strcmp0(method_name, "GotoLine") == 0) {
        const gchar *file_path = NULL;
        gint line = 0;
        g_variant_get(parameters, "(&si)", &file_path, &line);

        GeanyDocument *doc = ensure_document(file_path);
        if (!doc || !doc->is_valid) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.NotFound",
                "Could not open file");
            return;
        }

        /* Navigate with history */
        navqueue_goto_line(doc, NULL, line > 0 ? line : 1);

        gchar *result = g_strdup_printf("Navigated to %s:%d", file_path, line);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", result));
        g_free(result);

    } else if (g_strcmp0(method_name, "Build") == 0) {
        const gchar *command = NULL;
        g_variant_get(parameters, "(&s)", &command);

        /* Try to find the command in the .geany project file first */
        gchar *build_cmd = NULL;
        gchar *build_wd = NULL;

        if (geany_data->app->project &&
            geany_data->app->project->file_name) {
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf,
                    geany_data->app->project->file_name,
                    G_KEY_FILE_NONE, NULL)) {

                /* Map command name to key prefix:
                 * compile -> FT_00, build/make -> NF_00, run -> EX_00 */
                const gchar *prefix = NULL;
                if (g_strcmp0(command, "compile") == 0)
                    prefix = "FT_00";
                else if (g_strcmp0(command, "build") == 0 ||
                         g_strcmp0(command, "make") == 0)
                    prefix = "NF_00";
                else if (g_strcmp0(command, "run") == 0)
                    prefix = "EX_00";

                if (prefix) {
                    gchar *cm_key = g_strdup_printf("%s_CM", prefix);
                    gchar *wd_key = g_strdup_printf("%s_WD", prefix);

                    build_cmd = g_key_file_get_string(kf, "build-menu",
                                                       cm_key, NULL);
                    build_wd = g_key_file_get_string(kf, "build-menu",
                                                      wd_key, NULL);
                    g_free(cm_key);
                    g_free(wd_key);

                    /* Empty string means not configured */
                    if (build_cmd && strlen(build_cmd) == 0) {
                        g_free(build_cmd);
                        build_cmd = NULL;
                    }
                    if (build_wd && strlen(build_wd) == 0) {
                        g_free(build_wd);
                        build_wd = NULL;
                    }
                }
            }
            g_key_file_free(kf);
        }

        if (build_cmd) {
            /* Run the command ourselves and capture output */
            const gchar *wd = build_wd ? build_wd
                : (geany_data->app->project
                   ? geany_data->app->project->base_path
                   : NULL);

            msgwin_status_add("[geany-code] Running build: %s (in %s)",
                              build_cmd, wd ? wd : ".");

            GSubprocessLauncher *launcher = g_subprocess_launcher_new(
                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_MERGE);
            if (wd)
                g_subprocess_launcher_set_cwd(launcher, wd);

            GError *err = NULL;
            GSubprocess *proc = g_subprocess_launcher_spawn(
                launcher, &err, "/bin/sh", "-c", build_cmd, NULL);
            g_object_unref(launcher);

            if (!proc) {
                gchar *errmsg = g_strdup_printf("Failed to run '%s': %s",
                                                 build_cmd, err->message);
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.geanycode.Error.BuildFailed", errmsg);
                g_free(errmsg);
                g_error_free(err);
                g_free(build_cmd);
                g_free(build_wd);
                return;
            }

            /* Wait for completion and read output */
            gchar *stdout_buf = NULL;
            g_subprocess_communicate_utf8(proc, NULL, NULL,
                                          &stdout_buf, NULL, &err);

            gint exit_status = g_subprocess_get_exit_status(proc);
            g_object_unref(proc);

            GString *result = g_string_new("");
            g_string_append_printf(result, "Command: %s\n", build_cmd);
            g_string_append_printf(result, "Working dir: %s\n", wd ? wd : ".");
            g_string_append_printf(result, "Exit code: %d\n\n", exit_status);
            if (stdout_buf && strlen(stdout_buf) > 0)
                g_string_append(result, stdout_buf);
            else
                g_string_append(result, "(no output)");

            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", result->str));

            g_string_free(result, TRUE);
            g_free(stdout_buf);
            g_free(build_cmd);
            g_free(build_wd);

        } else {
            /* No project command found — fall back to Geany menu trigger */
            if (g_strcmp0(command, "compile") == 0) {
                build_activate_menu_item(GEANY_GBG_FT, 0);
            } else if (g_strcmp0(command, "build") == 0 ||
                       g_strcmp0(command, "make") == 0) {
                build_activate_menu_item(GEANY_GBG_NON_FT, 0);
            } else if (g_strcmp0(command, "run") == 0) {
                build_activate_menu_item(GEANY_GBG_EXEC, 0);
            } else {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.geanycode.Error.InvalidCommand",
                    "Unknown build command. Use: compile, build, make, run");
                return;
            }

            gchar *result = g_strdup_printf(
                "Triggered via Geany menu: %s (no project command found)",
                command);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", result));
            g_free(result);
        }

    } else if (g_strcmp0(method_name, "GetProject") == 0) {
        GString *info = g_string_new("");

        if (geany_data->app->project) {
            GeanyProject *proj = geany_data->app->project;
            g_string_append_printf(info, "Project: %s\n",
                proj->name ? proj->name : "(unnamed)");
            g_string_append_printf(info, "Base path: %s\n",
                proj->base_path ? proj->base_path : ".");
            if (proj->file_name)
                g_string_append_printf(info, "Config: %s\n", proj->file_name);
            if (proj->description && strlen(proj->description) > 0)
                g_string_append_printf(info, "Description: %s\n", proj->description);
        } else {
            g_string_append(info, "No project open\n");
        }

        /* List open files count */
        guint n = geany_data->documents_array->len;
        guint open = 0;
        for (guint i = 0; i < n; i++) {
            GeanyDocument *doc = g_ptr_array_index(geany_data->documents_array, i);
            if (doc && doc->is_valid) open++;
        }
        g_string_append_printf(info, "Open documents: %u\n", open);

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", info->str));
        g_string_free(info, TRUE);

    } else if (g_strcmp0(method_name, "AskUserQuestion") == 0) {
        const gchar *questions_json = NULL;
        g_variant_get(parameters, "(&s)", &questions_json);

        gchar *request_id = g_strdup_printf("q_%u_%u",
            (guint)getpid(), next_question_id++);

        /* Create pending question with a nested main loop */
        PendingQuestion *pq = g_new0(PendingQuestion, 1);
        pq->loop = g_main_loop_new(NULL, FALSE);
        pq->completed = FALSE;

        g_hash_table_insert(pending_questions, g_strdup(request_id), pq);

        /* Notify the plugin UI to show the question */
        if (question_requested_cb)
            question_requested_cb(request_id, questions_json,
                                  question_requested_data);

        /* Block until response or timeout (5 minutes) */
        guint timeout_id = g_timeout_add_seconds(300, (GSourceFunc)(void(*)(void))g_main_loop_quit, pq->loop);
        g_main_loop_run(pq->loop);
        g_source_remove(timeout_id);

        if (pq->completed && pq->response) {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", pq->response));
        } else {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)",
                    "ERROR: Question timeout or cancelled"));
        }

        g_hash_table_remove(pending_questions, request_id);
        g_main_loop_unref(pq->loop);
        g_free(pq->response);
        g_free(pq);
        g_free(request_id);

    } else if (g_strcmp0(method_name, "ProvideQuestionResponse") == 0) {
        const gchar *request_id = NULL, *response_json = NULL;
        g_variant_get(parameters, "(&s&s)", &request_id, &response_json);

        PendingQuestion *pq = g_hash_table_lookup(pending_questions, request_id);
        if (pq) {
            pq->response = g_strdup(response_json);
            pq->completed = TRUE;
            g_main_loop_quit(pq->loop);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "Search") == 0) {
        const gchar *pattern = NULL;
        gboolean use_regex = FALSE;
        g_variant_get(parameters, "(&sb)", &pattern, &use_regex);

        GString *results = g_string_new("");
        gint total_matches = 0;
        gint search_flags = SCFIND_MATCHCASE;
        if (use_regex) search_flags |= SCFIND_REGEXP;

        /* Search across all open documents */
        guint n = geany_data->documents_array->len;
        for (guint i = 0; i < n; i++) {
            GeanyDocument *doc = g_ptr_array_index(geany_data->documents_array, i);
            if (!doc || !doc->is_valid || !doc->file_name)
                continue;

            ScintillaObject *sci = doc->editor->sci;
            gint doc_len = sci_get_length(sci);
            gint pos = 0;

            while (pos < doc_len) {
                struct Sci_TextToFind ttf;
                ttf.chrg.cpMin = pos;
                ttf.chrg.cpMax = doc_len;
                ttf.lpstrText = (gchar *)pattern;

                gint found = scintilla_send_message(sci, SCI_FINDTEXT,
                    search_flags, (sptr_t)&ttf);
                if (found < 0) break;

                gint line = sci_get_line_from_position(sci, ttf.chrgText.cpMin) + 1;
                gchar *line_text = sci_get_line(sci, line - 1);

                /* Trim trailing newline */
                if (line_text) {
                    gsize len = strlen(line_text);
                    while (len > 0 && (line_text[len-1] == '\n' || line_text[len-1] == '\r'))
                        line_text[--len] = '\0';
                }

                g_string_append_printf(results, "%s:%d: %s\n",
                    doc->file_name, line, line_text ? line_text : "");
                g_free(line_text);

                total_matches++;

                /* Skip to next line to avoid duplicate entries for
                 * multiple matches on the same line */
                gint next_line_pos = sci_get_position_from_line(sci, line);
                pos = (next_line_pos > ttf.chrgText.cpMax)
                    ? next_line_pos : ttf.chrgText.cpMax;

                if (total_matches >= 100) {
                    g_string_append(results, "... (truncated at 100 matches)\n");
                    goto search_done;
                }
            }
        }

search_done:
        if (total_matches == 0)
            g_string_append(results, "No matches found\n");
        else
            g_string_append_printf(results, "\n%d match%s found\n",
                total_matches, total_matches == 1 ? "" : "es");

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", results->str));
        g_string_free(results, TRUE);

    } else if (g_strcmp0(method_name, "SetIndicator") == 0) {
        const gchar *file_path = NULL;
        gint line = 0, indicator_type = 0;
        g_variant_get(parameters, "(&sii)", &file_path, &line, &indicator_type);

        GeanyDocument *doc = ensure_document(file_path);
        if (!doc || !doc->is_valid) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.NotFound",
                "Could not open file");
            return;
        }

        /* indicator_type: 0 = error (red), 1 = warning (yellow) */
        gint indic = (indicator_type == 1)
            ? GEANY_INDICATOR_SNIPPET : GEANY_INDICATOR_ERROR;

        editor_indicator_set_on_line(doc->editor, indic, line > 0 ? line - 1 : 0);

        gchar *result = g_strdup_printf("Indicator set on %s:%d", file_path, line);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", result));
        g_free(result);

    } else if (g_strcmp0(method_name, "ClearIndicators") == 0) {
        const gchar *file_path = NULL;
        g_variant_get(parameters, "(&s)", &file_path);

        GeanyDocument *doc = ensure_document(file_path);
        if (!doc || !doc->is_valid) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.geanycode.Error.NotFound",
                "Could not open file");
            return;
        }

        editor_indicator_clear(doc->editor, GEANY_INDICATOR_ERROR);

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", "Indicators cleared"));

    } else if (g_strcmp0(method_name, "GetSelection") == 0) {
        GeanyDocument *doc = document_get_current();
        GString *info = g_string_new("");

        if (doc && doc->is_valid) {
            g_string_append_printf(info, "File: %s\n",
                doc->file_name ? doc->file_name : "(untitled)");

            ScintillaObject *sci = doc->editor->sci;
            gint pos = sci_get_current_position(sci);
            gint line = sci_get_line_from_position(sci, pos) + 1;
            gint col = sci_get_col_from_position(sci, pos);

            g_string_append_printf(info, "Cursor: line %d, col %d\n", line, col);

            if (sci_has_selection(sci)) {
                gchar *sel = sci_get_selection_contents(sci);
                gint sel_start = sci_get_selection_start(sci);
                gint sel_end = sci_get_selection_end(sci);
                gint start_line = sci_get_line_from_position(sci, sel_start) + 1;
                gint end_line = sci_get_line_from_position(sci, sel_end) + 1;

                g_string_append_printf(info, "Selection: lines %d-%d (%d chars)\n",
                    start_line, end_line, (int)strlen(sel));
                g_string_append_printf(info, "Selected text:\n%s\n", sel);
                g_free(sel);
            } else {
                g_string_append(info, "No selection\n");

                /* Include the current line for context */
                gchar *cur_line = sci_get_line(sci, line - 1);
                if (cur_line) {
                    gsize len = strlen(cur_line);
                    while (len > 0 && (cur_line[len-1] == '\n' || cur_line[len-1] == '\r'))
                        cur_line[--len] = '\0';
                    g_string_append_printf(info, "Current line: %s\n", cur_line);
                    g_free(cur_line);
                }
            }
        } else {
            g_string_append(info, "No document open\n");
        }

        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", info->str));
        g_string_free(info, TRUE);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call, NULL, NULL
};

/* ── Bus acquired / name callbacks ───────────────────────────────── */

static void on_bus_acquired(GDBusConnection *conn, const gchar *name,
                            gpointer user_data)
{
    (void)name; (void)user_data;
    connection = conn;

    GError *error = NULL;
    registration_id = g_dbus_connection_register_object(
        conn,
        "/GeanyCode/Editor",
        introspection_data->interfaces[0],
        &interface_vtable,
        NULL, NULL, &error);

    if (error) {
        g_warning("Failed to register DBus object: %s", error->message);
        g_error_free(error);
    }
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name,
                             gpointer user_data)
{
    (void)conn; (void)user_data;
    g_message("geany-code: DBus name acquired: %s", name);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name,
                         gpointer user_data)
{
    (void)conn; (void)user_data;
    g_warning("geany-code: DBus name lost: %s", name);
}

/* ── Public API ──────────────────────────────────────────────────── */

void editor_dbus_start(void)
{
    pending_questions = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_assert(introspection_data != NULL);

    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                              "org.geanycode.editor",
                              G_BUS_NAME_OWNER_FLAGS_NONE,
                              on_bus_acquired,
                              on_name_acquired,
                              on_name_lost,
                              NULL, NULL);
}

void editor_dbus_stop(void)
{
    if (registration_id > 0 && connection) {
        g_dbus_connection_unregister_object(connection, registration_id);
        registration_id = 0;
    }

    if (owner_id > 0) {
        g_bus_unown_name(owner_id);
        owner_id = 0;
    }

    if (introspection_data) {
        g_dbus_node_info_unref(introspection_data);
        introspection_data = NULL;
    }

    if (pending_questions) {
        g_hash_table_unref(pending_questions);
        pending_questions = NULL;
    }
}

void editor_dbus_set_question_callback(
    void (*cb)(const gchar *, const gchar *, gpointer),
    gpointer user_data)
{
    question_requested_cb = cb;
    question_requested_data = user_data;
}

void editor_dbus_provide_response(const gchar *request_id,
                                  const gchar *response_json)
{
    PendingQuestion *pq = g_hash_table_lookup(pending_questions, request_id);
    if (pq) {
        pq->response = g_strdup(response_json);
        pq->completed = TRUE;
        g_main_loop_quit(pq->loop);
    }
}
