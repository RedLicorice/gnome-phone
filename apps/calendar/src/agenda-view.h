/* CalAgendaView: Etar's Agenda (AgendaFragment, AgendaListView with AgendaByDayAdapter; spec/calendar.md §5): the
 * days that have events (and today), each a header with its weekday and date, then its events; the header of the
 * day at the top stays pinned while its events scroll under it (StickyHeaderListView). A red line marks where the
 * past ends.
 *
 * It holds a window of days and asks for more as the list nears either end ("range-changed"), keeping what is on
 * screen where it is when the new events arrive.
 *
 * Signals:
 *   "event-activated" (CalEvent *)   a tap on an event
 *   "range-changed" ()               the days it wants changed (cal_agenda_view_get_range): read them
 *   "top-day-changed" (int day)      the day at the top changed (the toolbar shows it) */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_AGENDA_VIEW (cal_agenda_view_get_type ())
G_DECLARE_FINAL_TYPE (CalAgendaView, cal_agenda_view, CAL, AGENDA_VIEW, GtkWidget)

GtkWidget *cal_agenda_view_new       (void);
/* @day at the top, the days around it loaded */
void       cal_agenda_view_go_to     (CalAgendaView *self, int day);
void       cal_agenda_view_get_range (CalAgendaView *self, int *first, int *last);
void       cal_agenda_view_set_events (CalAgendaView *self, GPtrArray *events);
/* the day Minerva's run history begins on (0: none) and its row's text: a row there from the next set_events */
void       cal_agenda_view_set_history (CalAgendaView *self, int day, const char *text);

G_END_DECLS
