/* NeoNavBar: Material 3's bottom navigation bar (BottomNavigationView, labelVisibilityMode labeled).
 *
 * 80 dp tall in surfaceContainerHighest, the items in equal columns. The selected item has a 64 x 32 dp pill in
 * secondaryContainer behind its 24 dp icon, and its icon and label in primary; the others are white at 50 %.
 * A tap selects an item and emits "selected" (its index); there is no swipe between tabs. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_TYPE_NAV_BAR (neo_nav_bar_get_type ())
G_DECLARE_FINAL_TYPE (NeoNavBar, neo_nav_bar, NEO, NAV_BAR, GtkWidget)

GtkWidget *neo_nav_bar_new          (void);
/* returns the item's index */
int        neo_nav_bar_append       (NeoNavBar *self, const char *icon_name, const char *label);
/* selects without emitting "selected" and without the indicator's animation */
void       neo_nav_bar_set_selected (NeoNavBar *self, int index);
int        neo_nav_bar_get_selected (NeoNavBar *self);
/* the rail: the items stacked down the start edge (Material 3's NavigationRail, for a landscape window); FALSE: the
 * bottom bar again. The items, the selection and the signal stay the same. */
void       neo_nav_bar_set_rail     (NeoNavBar *self, gboolean rail);

/* Signals: "selected" (int index) */

G_END_DECLS
