/* Small pieces every Contacts screen uses: colours from the spec's ARGB constants, Roboto text in Android's line
 * boxes (the Dialer's text.c), the toolbar's icon buttons, the AppCompat checkbox, and handing a tel:, sms:,
 * mailto: or web address to the app that takes it. */
#pragma once

#include "ct-constants.h"
#include "ct-store.h"
#include "text.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* an ARGB constant (0xAARRGGBB) as a GdkRGBA */
GdkRGBA    ct_rgba (guint32 argb);

/* Roboto at an effective weight (Bold text adds 300: regular 700, medium 800, bold 900, spec §0.4) */
#define CT_TEXT(size, weight) ((TextStyle) { TEXT_ROBOTO, (size), (weight) })

/* A toolbar icon button: @width wide and the toolbar's height, a 24 dp icon centred, an unbounded ripple of
 * @ripple (ARGB) as ?selectableItemBackgroundBorderless draws it (radius half the button's diagonal, §7.1), and
 * @name for assistive technology. "Back" is the name the shell's back gesture presses (extension/a11y.js). */
GtkWidget *ct_icon_button (const char *icon_name, const char *name, double width, double height, guint32 ripple);

/* An icon in a colour of its own rather than the CSS colour: QuickContact's kind icons take the contact's theme
 * colour (SRC_ATOP), which no fixed style sheet knows. @size dp square. */
GtkWidget *ct_tinted_icon_new (const char *icon_name, double size, guint32 argb);
void       ct_tinted_icon_set (GtkWidget *icon, const char *icon_name, guint32 argb);

/* a GtkLabel in one of the app's CSS text styles, Android's font padding added (neo_label_set_font_padding) */
GtkWidget *ct_label (const char *text, const char *css_class, double size);

/* A borderless text button: an AlertDialog's or the empty view's (14 sp medium caps colorAccent, 64 dp min width,
 * 8 dp padding, a bounded ripple with 2 dp corners). The label is upper-cased as allCaps does. */
GtkWidget *ct_text_button (const char *label, const char *css_class);

/* AppCompat's abc_btn_check_material at (x, y), its 32 dp box's top-left: an 18 dp square with 2 dp corners, a 2 dp
 * outline in @off, or filled with @on and the check mark knocked out */
void       ct_snapshot_checkbox (GtkSnapshot *snapshot, double x, double y, gboolean checked, guint32 on,
                                 guint32 off);

/* the same check box as a widget (32 dp square), for rows that are not drawn by hand */
GtkWidget *ct_check_new        (guint32 on, guint32 off);
void       ct_check_set_active (GtkWidget *check, gboolean active);
gboolean   ct_check_get_active (GtkWidget *check);

/* whether an app takes @scheme ("tel", "sms", "mailto", "geo", "sip"); an action nothing could take is not shown */
gboolean   ct_can_open_scheme (const char *scheme);
/* hands @uri to its app; FALSE (and a toast from the caller) when none takes it */
gboolean   ct_open_uri (GtkWidget *widget, const char *uri);

/* text into the clipboard (QuickContact's long press: "Copy to clipboard") */
void       ct_copy_text (GtkWidget *widget, const char *text);

/* Settings' choices (§10.6): "Name format" is the order names show in, "Sort by" the order the list is in */
CtNameOrder ct_display_order (GSettings *settings);
CtNameOrder ct_sort_order    (GSettings *settings);
/* "Contacts to display": whether the contacts of @book_uid are in the list (all, or the books Customize kept) */
gboolean    ct_book_shown    (GSettings *settings, const char *book_uid);
/* the contacts the list is for: those "Contacts to display" keeps (CtEntry, owned) */
GPtrArray  *ct_shown_entries (CtStore *store, GSettings *settings);
/* ICU's {count, plural, =1 {# contact} other {# contacts}} for the two forms the app has */
char       *ct_contacts_count (guint count, const char *account);

/* The busy line of anything that is not instant (enrolling a voice, merging, creating a notebook): a spinner and
 * what is being done, hidden until ct_busy_start. After @slow_s seconds the line becomes @slow ("{s}" is the seconds so
 * far), so a long wait says it is still going. Starting it again with the same @what keeps the clock. The texts are
 * the caller's (data). */
GtkWidget *ct_busy_new    (void);
void       ct_busy_start  (GtkWidget *busy, const char *what, const char *slow, guint slow_s);
void       ct_busy_stop   (GtkWidget *busy);
gboolean   ct_busy_active (GtkWidget *busy);

G_END_DECLS
