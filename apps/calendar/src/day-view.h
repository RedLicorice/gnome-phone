/* CalDayView: Etar's DayView, the Week (7 days) and Day (1 day) views (spec/calendar.md §3).
 *
 * The hours run down the left; each day is a column of hour cells scrolled together, with the timed events laid
 * out in them, the all-day events in an area above, and in the week a header of weekday and date. It draws itself
 * the way DayView draws its canvas: in device pixels where Etar does (the 1 px hour and day gaps), dp otherwise.
 *
 * Signals:
 *   "event-activated" (CalEvent *)          a tap on an event (after its clicked flash)
 *   "create-event"    (gint64 start, gboolean all_day)   a tap on the selected empty hour (or all-day slot)
 *   "day-activated"   (int day)             a tap on a date in the week's header
 *   "page"            (int delta)           a swipe moved to the next (1) or previous (-1) week or day; the view
 *                                           already shows it, the owner reads its events
 *   "cell-height-changed" (double)          a pinch ended: the new hour height to keep */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_DAY_VIEW (cal_day_view_get_type ())
G_DECLARE_FINAL_TYPE (CalDayView, cal_day_view, CAL, DAY_VIEW, GtkWidget)

GtkWidget *cal_day_view_new            (int num_days);
int        cal_day_view_get_num_days   (CalDayView *self);
/* Days per week: the Week view's days (2 … 7); the view keeps its first day */
void       cal_day_view_set_num_days   (CalDayView *self, int num_days);
/* the first day shown (a day number, cal_day_number); the selection moves to @selected (a day in the period) */
void       cal_day_view_set_first_day  (CalDayView *self, int first_day);
int        cal_day_view_get_first_day  (CalDayView *self);
/* the events of the shown period and the periods either side (for a swipe), sorted by cal_event_compare */
void       cal_day_view_set_events     (CalDayView *self, GPtrArray *events);
/* where Minerva's run history begins (0: none) and the text its line carries */
void       cal_day_view_set_history    (CalDayView *self, gint64 history, const char *text);
void       cal_day_view_set_cell_height (CalDayView *self, double cell_height);
/* Staggered display: overlapping timed events overlap, each later column narrower and shadowed, its title where it
 * shows (Event.computePositions with drawStaggered, EventGeometry) */
void       cal_day_view_set_staggered  (CalDayView *self, gboolean staggered);
/* scroll so that @hour shows a little below the top (DayView.initFirstHour: hour − visible hours / 5) */
void       cal_day_view_scroll_to_hour (CalDayView *self, int hour);
/* the hour a new event from the FAB starts at: the selected slot, if any */
gboolean   cal_day_view_get_selection  (CalDayView *self, gint64 *start, gboolean *all_day);
void       cal_day_view_clear_selection (CalDayView *self);

G_END_DECLS
