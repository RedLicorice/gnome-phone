/* Neo Remote's window: the remotes and macros, a remote's buttons, the scan by brand, the database search. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* `ui`: the app's words, icons and settings (ui.json) */
GtkWidget *nr_window_new (GtkApplication *app, JsonObject *ui);

G_END_DECLS
