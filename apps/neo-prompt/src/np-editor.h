/* The editor: one text view for every document, with Markdown and Jinja2 coloured by tags, the headings of the
 * document as an index, a search, the refusal of `minerva-config` shown under the text, a draft while it is dirty,
 * and the history of what a save overwrites.
 *
 * A save is: the check (`validate`, e.g. `state preview`), a fresh read of the source compared with the text as it
 * was loaded (the agent writes these files too), the snapshot, then the write (`save`, e.g. `state save`) — or an
 * atomic file replace. Nothing is written before the check passes. */
#pragma once

#include <gtk/gtk.h>

#include "np-store.h"

G_BEGIN_DECLS

typedef struct _NpEditor NpEditor;

NpEditor   *np_editor_new        (NpStore *store, GtkWindow *window, GtkWidget *snackbar);
GtkWidget  *np_editor_root       (NpEditor *self);
void        np_editor_open       (NpEditor *self, NpDoc *doc);
NpDoc      *np_editor_doc        (NpEditor *self);
gboolean    np_editor_modified   (NpEditor *self);
void        np_editor_save       (NpEditor *self);
void        np_editor_reload     (NpEditor *self);
/* the restored snapshot goes into the buffer, dirty: the user saves it through the normal door */
void        np_editor_put_text   (NpEditor *self, const char *text);
char       *np_editor_text       (NpEditor *self);
void        np_editor_search     (NpEditor *self);
void        np_editor_index      (NpEditor *self, GtkWidget *anchor);
void        np_editor_set_saved_cb (NpEditor *self, void (*cb) (gpointer), gpointer data);
void        np_editor_set_changed_cb (NpEditor *self, void (*cb) (gpointer), gpointer data);

G_END_DECLS
