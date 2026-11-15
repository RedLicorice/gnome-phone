/* Home-screen widgets, described as data: which widget, on which page, how many grid rows it takes at the top of that
 * page (the pages' icons start under them). The clock is one, the omnisearch bar (omni.c) the second, under it.
 * Adding a widget is one line in the table of widgets.c plus its draw and tap functions; adding and removing them
 * with a long press can later write the same table from a setting. */
#pragma once

#include <stdbool.h>

/* the grid slots the widgets of `page` take at its top (rows × cols) */
int   widgets_slots (int page, int cols);
/* draws the widgets of `page`, the page's left edge at x0 */
void  widgets_draw  (int page, float x0);
/* a tap at (x, y) of the workspace at rest, page 0: the widget under it takes it; true when one did */
bool  widgets_tap   (float x, float y);
