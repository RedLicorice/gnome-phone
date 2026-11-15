/* Neo Prompt's small building blocks: a card, a tappable row, a label, an inline message, a colour looked up from
 * the stylesheet (a GtkTextTag takes an RGBA, not a CSS name).
 *
 * They are the shapes the phone's own apps share (libneo's cards and ripple), private to this app: libneo has no
 * list row. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget  *np_label       (const char *text, const char *css);
GtkWidget  *np_sub         (const char *text);
/* a card: rows on the window's surfaceContainerHighest, 16 dp corners, inset from the edges */
GtkWidget  *np_card        (void);
/* a tappable row of a title and a second line; returns the NeoPressable */
GtkWidget  *np_row         (const char *title, const char *subtitle, const char *css);
void        np_row_set_sub (GtkWidget *row, const char *subtitle);
/* a message under a card's rows: hidden while empty; red when `error` */
GtkWidget  *np_msg         (void);
void        np_msg_set     (GtkWidget *label, const char *text, gboolean error);
/* a small pill of secondary text */
GtkWidget  *np_badge       (const char *text, const char *tone);
GtkWidget  *np_scroller    (GtkWidget *child);
/* a stylesheet colour on `widget` ("neo_primary"), FALSE when the look has no such name */
gboolean    np_lookup      (GtkWidget *widget, const char *name, GdkRGBA *out);
/* a path with the home directory as ~ (one shared buffer) */
const char *np_short_path  (const char *path);

G_END_DECLS
