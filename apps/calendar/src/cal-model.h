/* The calendar's data as the views see it: calendars, the event occurrences in a time range, their reminders, and
 * the rules every view shares (an event's display colour, its days, how overlapping events share a day's width).
 *
 * This is Etar's Event and CalendarContract rows rebuilt over evolution-data-server (spec/calendar.md §12): one
 * CalEvent is one occurrence (Etar's Instances row), a recurring event giving one per repeat. Nothing here talks to
 * EDS; cal-store.c fills these from it. Pure C and GLib, so the tests can use it. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* --- calendars (Etar's Calendars rows; EDS's ESources with a calendar extension) */

typedef struct {
  char *uid;            /* the ESource's uid */
  char *name;           /* its display name */
  char *account;        /* the parent source's name: "On This Computer", an account */
  guint32 color;        /* 0xRRGGBB, ESourceSelectable's colour */
  gboolean readonly;    /* the backend refuses writes (a holiday or subscribed calendar) */
  gboolean visible;     /* shown in the views (ESourceSelectable:selected) */
  gboolean alarms;      /* its reminders ring (ESourceAlarms:include-me) */
  char *email;          /* its owner's address (the backend's cal-email-address), to find "me" among the guests;
                         * NULL for a calendar without one (a local calendar) */
  gboolean local;       /* "On This Computer" (backend "local"): only these take guests' status from the app */
} CalCalendar;

void cal_calendar_free (CalCalendar *calendar);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CalCalendar, cal_calendar_free)

/* --- reminders (Etar's Reminders rows; VALARMs) */

typedef enum {
  CAL_METHOD_NOTIFICATION = 1,  /* Reminders.METHOD_ALERT; VALARM ACTION:DISPLAY */
  CAL_METHOD_EMAIL = 2,         /* METHOD_EMAIL; ACTION:EMAIL */
  CAL_METHOD_SMS = 3,           /* METHOD_SMS: no iCalendar form, never read from EDS */
  CAL_METHOD_ALARM = 4,         /* METHOD_ALARM; ACTION:AUDIO */
} CalMethod;

typedef struct {
  int minutes;          /* before the start */
  CalMethod method;
} CalReminder;

/* --- guests (Etar's Attendees rows; ATTENDEE properties) */

typedef enum {
  CAL_STATUS_NONE,              /* Attendees.ATTENDEE_STATUS_NONE: no PARTSTAT, or no ATTENDEE for the owner */
  CAL_STATUS_ACCEPTED,          /* PARTSTAT=ACCEPTED */
  CAL_STATUS_DECLINED,          /* PARTSTAT=DECLINED */
  CAL_STATUS_INVITED,           /* PARTSTAT=NEEDS-ACTION (the default) */
  CAL_STATUS_TENTATIVE,         /* PARTSTAT=TENTATIVE */
} CalStatus;

typedef struct {
  char *email;          /* the address, without "mailto:" */
  char *name;           /* CN, or NULL */
  CalStatus status;
  gboolean organizer;   /* the ORGANIZER, listed as Etar lists it among the guests */
} CalAttendee;

void         cal_attendee_free (CalAttendee *attendee);
CalAttendee *cal_attendee_copy (const CalAttendee *attendee);

/* Events.AVAILABILITY: TRANSP (OPAQUE busy, the default; TRANSPARENT free) */
typedef enum { CAL_AVAILABILITY_BUSY, CAL_AVAILABILITY_FREE } CalAvailability;
/* Events.ACCESS_LEVEL: CLASS (none: the calendar's default) */
typedef enum { CAL_ACCESS_DEFAULT, CAL_ACCESS_CONFIDENTIAL, CAL_ACCESS_PRIVATE, CAL_ACCESS_PUBLIC } CalAccess;

/* --- Minerva's scheduler (docs/calendar-scheduler.md of Minerva's repository): what an occurrence of the Minerva
 * calendar carries besides an event's fields. The scheduler is its one store; cal-minerva.c fills this. */

typedef struct {
  char *task;           /* the scheduler's task id */
  char *kind;           /* what the app calls it: "prompt", "reminder", or "other" (made by Minerva, read-only) */
  char *state;          /* the task's state (a future one) or the run's outcome (a past one) */
  char *repeat;         /* "none", "daily", "weekdays", "weekly", "monthly", or "custom" (not editable here) */
  char *goal;           /* the whole prompt or reminder text (shown to the owner only) */
  char *error;          /* a failed run's error, or NULL */
  gboolean past;        /* a run of the run log, not a time still to come */
  gboolean approval;    /* the body is a call the owner is asked about when it runs */
  int run;              /* a past run's number */
} CalMinervaInfo;

void            cal_minerva_info_free (CalMinervaInfo *info);
CalMinervaInfo *cal_minerva_info_copy (const CalMinervaInfo *info);

/* --- events (one occurrence) */

typedef struct {
  int ref;
  char *calendar;       /* the calendar's uid */
  char *uid;            /* the iCalendar UID */
  char *rid;            /* this occurrence's RECURRENCE-ID as EDS spells it, or NULL for a single event */
  char *title, *location, *description, *url;
  gint64 start, end;    /* Unix seconds; an all-day event's are local midnights, the end exclusive */
  gboolean all_day;
  guint32 color;        /* 0xRRGGBB: the event's own COLOR, else its calendar's */
  gboolean recurring;   /* it has an RRULE/RDATE, or it is a moved occurrence of one */
  char *rrule;          /* the RRULE value, "FREQ=WEEKLY;BYDAY=MO", or NULL */
  char *tzid;           /* the zone of DTSTART ("Europe/Berlin"), or NULL for UTC, floating and all-day */
  GArray *reminders;    /* CalReminder, in the event's order */
  gboolean readonly;    /* its calendar is read-only */
  gboolean own_color;   /* the colour is the event's COLOR, not its calendar's */
  CalAvailability availability;
  CalAccess access;
  GPtrArray *attendees; /* CalAttendee: the guests, the organizer first when there is one */
  char *organizer;      /* ORGANIZER's address, or NULL */
  CalStatus self_status;  /* the calendar owner's PARTSTAT (Instances.SELF_ATTENDEE_STATUS) */
  gboolean cancelled;   /* STATUS:CANCELLED (Events.STATUS_CANCELED): the views strike its title through */
  CalMinervaInfo *minerva;  /* an occurrence of the Minerva calendar (the scheduler's), NULL for an EDS event */

  /* the layout of the views, filled by cal_layout_events (Etar's Event.column, maxColumns, textOffsetMillis,
   * drawStaggered) */
  int column, max_columns;
  gint64 text_offset;   /* staggered: seconds from the start to where the title shows (unobscured) */
  gboolean staggered;
} CalEvent;

CalEvent *cal_event_new   (void);
CalEvent *cal_event_ref   (CalEvent *event);
/* a new event with all of @event's fields (the layout's reset) */
CalEvent *cal_event_copy  (const CalEvent *event);
/* "Email guests" (AlertReceiver.createEmailIntent, Utils.createEmailAttendeesIntent): a mailto: URI to the guests
 * other than @me (the declined ones on cc), else to the organizer; "Re: <title>" and @body; NULL when there is no
 * one to write to */
char     *cal_event_mail_uri (const CalEvent *event, const char *me, const char *body);
void      cal_event_unref (CalEvent *event);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CalEvent, cal_event_unref)

/* Etar's Event.drawAsAllday: all-day, or lasting a day or more; drawn in the all-day area, not the grid */
gboolean cal_event_draws_as_all_day (const CalEvent *event);

/* the days an event covers, as day numbers (cal_day_number); an end exactly at midnight ends the day before */
int cal_event_first_day (const CalEvent *event);
int cal_event_last_day  (const CalEvent *event);

/* begin ASC, end DESC, title ASC (Etar's EventLoader sort order); for g_ptr_array_sort */
int cal_event_compare (gconstpointer a, gconstpointer b);

/* --- colours (E/Utils.java:891-966) */

/* "Use real event colors" off: saturation ×1.3 (to 1 at most) and value ×0.8 */
guint32 cal_display_color (guint32 rgb, gboolean real_colors);
/* the colour the views draw @event in: cal_display_color with the "Use real event colors" setting, which the app
 * sets once for all views (Etar reads the preference in Utils.getDisplayColorFromColor) */
guint32 cal_event_display_color   (const CalEvent *event);
void    cal_set_real_event_colors (gboolean real);
/* Utils.getDeclinedColorFromColor: a declined event's colour, @rgb at 0x66 over white */
guint32 cal_declined_color (guint32 rgb);
/* Utils.getAdaptiveTextColor with getAdaptiveTextAlpha: with real event colours, an event's text is black at 70 %
 * on a bright colour (brightness over 130) and white at 90 % on a dark one; @argb (the views' text colour)
 * otherwise */
guint32 cal_adaptive_text_color (guint32 argb, guint32 event_rgb);

/* --- days: a local calendar day as one number, as Etar's Julian days */

int     cal_day_number (gint64 time);          /* the local day @time falls on */
gint64  cal_day_start  (int day);              /* that day's local midnight, in Unix seconds */
int     cal_day_of_week (int day);              /* 0 Sunday … 6 Saturday */
int     cal_minute_of_day (gint64 time);        /* 0 … 1439, local */

/* --- layout (Etar's Event.computePositions, non-staggered): for the timed events of one day (or the all-day ones
 * of a span of days) sorted by cal_event_compare, each event's column and the columns of its overlap group.
 * @min_seconds is the shortest duration an event takes up on screen (its minimum height in time). */
void cal_layout_events (GPtrArray *events, gboolean all_day, gint64 min_seconds);
/* "Staggered display" (computeGroupLayout with drawStaggered, timed events only): an event goes in the lowest column
 * where neither it nor any event below it is wholly covered by later columns for @min_seconds, a column inserted
 * where needed; each event's text_offset is where its uncovered part begins. A group it cannot stagger falls back to
 * cal_layout_events' columns. */
void cal_layout_events_staggered (GPtrArray *events, gint64 min_seconds);

G_END_DECLS
