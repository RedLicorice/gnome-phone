/* The editor: one text view for every document, with Markdown and Jinja2 coloured by tags, the headings of the
 * document as an index, a search, the refusal of `minerva-config` shown under the text, a draft while it is dirty,
 * and the history of what a save overwrites.
 *
 * The text has two modes, chosen per kind of text and remembered (np_mode_for). Source draws the Markdown as written;
 * rendered impaginates it - the markers get an invisible tag, the runs their weight and size - so the buffer keeps
 * every byte and a save writes what a save writes in source mode. The line the cursor is on keeps its markers.
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
gboolean    np_editor_saving     (NpEditor *self);
/* TRUE when the text is drawn impaginated (the Markdown markers hidden, the run styled) rather than as source */
gboolean    np_editor_rendered   (NpEditor *self);
/* the mode is a choice per kind of text: the editor remembers it (np_mode_set) and draws the buffer again */
void        np_editor_set_rendered (NpEditor *self, gboolean rendered);
/* reading (the default when a text opens: no cursor, no keyboard, the kind's read mode) or editing (after Edit: the
 * keyboard, the cursor, the mode chosen for editing). A text put back (a draft, a snapshot) switches to editing. */
gboolean    np_editor_editing    (NpEditor *self);
void        np_editor_set_editing (NpEditor *self, gboolean editing);
void        np_editor_save       (NpEditor *self);
/* a save that says how it ended, for a Back that has somewhere to go only once the text is safe; `ok` is FALSE
 * when nothing was written (the refusal is on screen) and the caller must stay where it is */
void        np_editor_save_then  (NpEditor *self, void (*done) (gboolean ok, gpointer data), gpointer data);
void        np_editor_reload     (NpEditor *self);
/* throw the buffer away: the draft too, then the source as it is on disk */
void        np_editor_discard    (NpEditor *self);
/* the restored snapshot goes into the buffer, dirty: the user saves it through the normal door */
void        np_editor_put_text   (NpEditor *self, const char *text);
char       *np_editor_text       (NpEditor *self);
void        np_editor_search     (NpEditor *self);
gboolean    np_editor_search_open (NpEditor *self);
void        np_editor_index      (NpEditor *self, GtkWidget *anchor);
void        np_editor_set_saved_cb (NpEditor *self, void (*cb) (gpointer), gpointer data);
void        np_editor_set_changed_cb (NpEditor *self, void (*cb) (gpointer), gpointer data);

G_END_DECLS
