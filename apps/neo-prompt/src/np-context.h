/* The context view: what the agent is given, block by block, with the source file of each, an estimate of its
 * tokens, and how often it changes.
 *
 * opencode assembles the request in memory and exposes no copy of it (no endpoint, no log body, only the messages in
 * its store), so this page is a RECONSTRUCTION from the parts on disk. The page says so: the blocks whose text cannot
 * be read are shown as "non ricostruibile" with no token figure, and the total is a lower bound.
 *
 * The cache line at the top is prefix caching: a block that changes every message in front of stable ones throws
 * away every cached token after it. */
#pragma once

#include <gtk/gtk.h>

#include "np-store.h"

G_BEGIN_DECLS

typedef struct _NpContext NpContext;

NpContext  *np_context_new    (NpStore *store, GtkWindow *window, GtkWidget *snackbar);
GtkWidget  *np_context_root   (NpContext *self);
void        np_context_refresh (NpContext *self);
/* a block whose source is a document of the registry opens it */
void        np_context_set_open_cb (NpContext *self, void (*cb) (gpointer, const char *id, const char *person),
                                    gpointer data);

G_END_DECLS
