/* Neo Voice's window: the detections newest first, and the selected one (play, trim, label, note, delete).
 * Landscape is master-detail (the list beside the clip); portrait is one column, the detail over the list.
 * Read first: browsing and playing change nothing; every change is a button, then one Voice1 call. */
#pragma once

#include "nv-store.h"

G_BEGIN_DECLS

GtkWidget *nv_window_new (GtkApplication *app, NvStore *store);
/* a test run: select the record with this id once the list is loaded */
void       nv_window_select (GtkWidget *window, const char *id);

G_END_DECLS
