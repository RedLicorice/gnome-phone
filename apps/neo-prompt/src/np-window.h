/* Neo Prompt's window: four tabs on libneo's bottom bar — the files it may open, the editor, the context view and
 * the history of the open document. The bar at the top carries the title and the tab's actions (save, search,
 * index, refresh). */
#pragma once

#include <gtk/gtk.h>

#include "np-store.h"

G_BEGIN_DECLS

GtkWidget *np_window_new      (GtkApplication *app, NpStore *store);
/* the document `id` for `person` (a notebook), asking first when the editor holds unsaved text */
void       np_window_open_doc (GtkWidget *window, const char *id, const char *person);
void       np_window_present_tab (GtkWidget *window, int tab);

G_END_DECLS
