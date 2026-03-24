#ifndef GEANY_CODE_PLUGIN_H
#define GEANY_CODE_PLUGIN_H

#include <geanyplugin.h>

/* Geany globals (defined in plugin.c) */
extern GeanyPlugin *geany_plugin;
extern GeanyData   *geany_data;

/* Forward declarations */
typedef struct _GeanyCodePlugin GeanyCodePlugin;

/* Global plugin data - accessible to all modules */
struct _GeanyCodePlugin {
    GeanyPlugin  *geany_plugin;
    GtkWidget    *sidebar_page;     /* our page in the sidebar notebook */
    GtkWidget    *chat_widget;      /* main chat container */

    /* Menu items (so we can destroy on cleanup) */
    GtkWidget    *menu_item;
    GtkWidget    *submenu;
};

/* The single global plugin instance */
extern GeanyCodePlugin *geany_code;

#endif /* GEANY_CODE_PLUGIN_H */
