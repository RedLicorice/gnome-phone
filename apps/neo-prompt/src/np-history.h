/* The history of the document in the editor: a snapshot for every save (the text the save replaces), newest first.
 * A snapshot opens as a line diff against the text in the editor; "Ripristina" puts it in the editor dirty, so it
 * goes back through the normal save (the check, the snapshot, the write), not around them. */
#pragma once

#include <gtk/gtk.h>

#include "np-store.h"

G_BEGIN_DECLS

typedef struct _NpHistory NpHistory;

NpHistory  *np_history_new       (NpStore *store, GtkWindow *window, GtkWidget *snackbar);
GtkWidget  *np_history_root      (NpHistory *self);
void        np_history_set_doc   (NpHistory *self, NpDoc *doc);
void        np_history_refresh   (NpHistory *self);
/* the diff goes back to the list of snapshots: TRUE when there was a diff to close, so Back stops there */
gboolean    np_history_back      (NpHistory *self);
/* the text the snapshot is compared with (the editor's) */
void        np_history_set_current_cb (NpHistory *self, char *(*cb) (gpointer), gpointer data);
/* the snapshot to put back in the editor */
void        np_history_set_restore_cb (NpHistory *self, void (*cb) (gpointer, const char *text), gpointer data);

G_END_DECLS
