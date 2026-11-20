/* Minerva's scheduler as one more calendar (docs/calendar-scheduler.md of Minerva's repository). The harness
 * scheduler is the one store: this turns what `minerva-config schedule range` answers into CalEvents of the "Minerva"
 * calendar, and an edited event back into the entry `schedule create|update` takes. No EDS, no GTK: the tests use it.
 *
 * The repeat names ("none", "daily", "weekdays", "weekly", "monthly", "custom") are minerva-config's (its [schedule]
 * repeats): the contract between the two, like the JSON members. */
#pragma once

#include "cal-model.h"

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CAL_MINERVA_CALENDAR "minerva"   /* the Minerva calendar's uid among the calendars */

/* The occurrences of a `schedule range` answer as events of the Minerva calendar in @color: a future due time is
 * writable when it is a prompt or a reminder with a repeat the app can make; a past run (and a task Minerva made of
 * another kind) is read-only. @history_from: the oldest time the run log covers (0 when unknown). NULL with @error
 * when the answer cannot be read. */
GPtrArray *cal_minerva_events (const char *json, guint32 color, gint64 *history_from, GError **error);
/* the same from the answer already parsed (minerva-config's JSON object) */
GPtrArray *cal_minerva_events_of (JsonObject *root, guint32 color, gint64 *history_from);

/* The entry `schedule create|update` reads on stdin for @event: {kind, text (the title, else the description),
 * start (ISO 8601 with its offset), repeat}. NULL when its repeat has no form the scheduler takes (custom). */
char *cal_minerva_entry (const CalEvent *event, const char *kind);

/* The repeat an RRULE is ("none" for none), NULL when no repeat makes it (an interval, a count, an end, several
 * days but the weekdays). @start: the first occurrence, to check a weekly day or a monthly date. */
const char *cal_minerva_repeat_of_rrule (const char *rrule, gint64 start);

/* The RRULE of a repeat starting at @start, NULL for "none" and for one the app cannot make */
char *cal_minerva_rrule_of_repeat (const char *repeat, gint64 start);

/* A past run's outcome before its title: @ui's "mark_<state>" (minerva-config schedule ui), when it has one */
void cal_minerva_mark_outcomes (GPtrArray *events, JsonObject *ui);

/* @template with "{@key}" replaced by @value (minerva-config's texts) */
char *cal_minerva_fill (const char *template, const char *key, const char *value);

G_END_DECLS
