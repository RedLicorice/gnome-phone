/* The calendar's data and the rules the views share (cal-model.h). */
#include "cal-model.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

void
cal_calendar_free (CalCalendar *calendar)
{
  g_free (calendar->uid);
  g_free (calendar->name);
  g_free (calendar->account);
  g_free (calendar->email);
  g_free (calendar);
}

void
cal_attendee_free (CalAttendee *attendee)
{
  g_free (attendee->email);
  g_free (attendee->name);
  g_free (attendee);
}

CalEvent *
cal_event_new (void)
{
  CalEvent *event = g_new0 (CalEvent, 1);

  event->ref = 1;
  event->reminders = g_array_new (FALSE, FALSE, sizeof (CalReminder));
  event->attendees = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_attendee_free);
  event->max_columns = 1;
  return event;
}

CalEvent *
cal_event_ref (CalEvent *event)
{
  event->ref++;
  return event;
}

CalAttendee *
cal_attendee_copy (const CalAttendee *attendee)
{
  CalAttendee *copy = g_memdup2 (attendee, sizeof *attendee);

  copy->email = g_strdup (attendee->email);
  copy->name = g_strdup (attendee->name);
  return copy;
}

/* addIfEmailable: an address, not the owner's, nor a machine-made one (Utils.MACHINE_GENERATED_ADDRESS) */
static void
add_if_mailable (GPtrArray *list, const char *email, const char *me)
{
  if (!email || !strchr (email, '@') || g_str_has_suffix (email, "calendar.google.com")
      || (me && g_ascii_strcasecmp (email, me) == 0))
    return;
  g_ptr_array_add (list, (gpointer) email);
}

char *
cal_event_mail_uri (const CalEvent *event, const char *me, const char *body)
{
  g_autoptr(GPtrArray) to = g_ptr_array_new (), cc = g_ptr_array_new ();
  g_autoptr(GString) uri = g_string_new ("mailto:");
  g_autofree char *subject = NULL;
  GPtrArray *list;
  gboolean query = FALSE;

  for (guint i = 0; i < event->attendees->len; i++)
    {
      const CalAttendee *guest = event->attendees->pdata[i];

      add_if_mailable (guest->status == CAL_STATUS_DECLINED ? cc : to, guest->email, me);
    }
  if (to->len == 0 && cc->len == 0)
    add_if_mailable (to, event->organizer, me);
  if (to->len == 0 && cc->len == 0)
    return NULL;
  /* no one to write to but declined guests: they are written to instead */
  list = to->len > 0 ? to : cc;
#define ADD(key, value) G_STMT_START { \
    g_autofree char *escaped = g_uri_escape_string ((value), NULL, FALSE); \
    g_string_append_printf (uri, "%s%s=%s", query ? "&" : "?", (key), escaped); \
    query = TRUE; } G_STMT_END
  {
    g_autofree char *first = g_uri_escape_string (list->pdata[0], "@", FALSE);

    g_string_append (uri, first);
  }
  for (guint i = 1; i < list->len; i++)
    ADD ("to", (const char *) list->pdata[i]);
  subject = g_strconcat ("Re: ", event->title && *event->title ? event->title : _("(No title)"), NULL);
  ADD ("subject", subject);
  if (body)
    ADD ("body", body);
  if (list == to)
    for (guint i = 0; i < cc->len; i++)
      ADD ("cc", (const char *) cc->pdata[i]);
#undef ADD
  return g_string_free (g_steal_pointer (&uri), FALSE);
}

CalEvent *
cal_event_copy (const CalEvent *event)
{
  CalEvent *copy = cal_event_new ();
  GArray *reminders = copy->reminders;
  GPtrArray *attendees = copy->attendees;

  *copy = *event;
  copy->ref = 1;
  copy->calendar = g_strdup (event->calendar);
  copy->uid = g_strdup (event->uid);
  copy->rid = g_strdup (event->rid);
  copy->title = g_strdup (event->title);
  copy->location = g_strdup (event->location);
  copy->description = g_strdup (event->description);
  copy->url = g_strdup (event->url);
  copy->rrule = g_strdup (event->rrule);
  copy->tzid = g_strdup (event->tzid);
  copy->organizer = g_strdup (event->organizer);
  copy->reminders = reminders;
  g_array_append_vals (reminders, event->reminders->data, event->reminders->len);
  copy->attendees = attendees;
  for (guint i = 0; i < event->attendees->len; i++)
    g_ptr_array_add (attendees, cal_attendee_copy (event->attendees->pdata[i]));
  copy->minerva = event->minerva ? cal_minerva_info_copy (event->minerva) : NULL;
  copy->column = 0;
  copy->max_columns = 1;
  return copy;
}

CalMinervaInfo *
cal_minerva_info_copy (const CalMinervaInfo *info)
{
  CalMinervaInfo *copy = g_memdup2 (info, sizeof *info);

  copy->task = g_strdup (info->task);
  copy->kind = g_strdup (info->kind);
  copy->state = g_strdup (info->state);
  copy->repeat = g_strdup (info->repeat);
  copy->goal = g_strdup (info->goal);
  copy->error = g_strdup (info->error);
  return copy;
}

void
cal_minerva_info_free (CalMinervaInfo *info)
{
  if (!info)
    return;
  g_free (info->task);
  g_free (info->kind);
  g_free (info->state);
  g_free (info->repeat);
  g_free (info->goal);
  g_free (info->error);
  g_free (info);
}

void
cal_event_unref (CalEvent *event)
{
  if (--event->ref > 0)
    return;
  g_free (event->calendar);
  g_free (event->uid);
  g_free (event->rid);
  g_free (event->title);
  g_free (event->location);
  g_free (event->description);
  g_free (event->url);
  g_free (event->rrule);
  g_free (event->tzid);
  g_array_unref (event->reminders);
  g_ptr_array_unref (event->attendees);
  g_free (event->organizer);
  cal_minerva_info_free (event->minerva);
  g_free (event);
}

gboolean
cal_event_draws_as_all_day (const CalEvent *event)
{
  /* Event.drawAsAllday: allDay || duration >= DateUtils.DAY_IN_MILLIS */
  return event->all_day || event->end - event->start >= 24 * 3600;
}

int
cal_event_first_day (const CalEvent *event)
{
  return cal_day_number (event->start);
}

int
cal_event_last_day (const CalEvent *event)
{
  /* the end is exclusive: an event to midnight, as every all-day one, ends on the day before */
  if (event->end <= event->start)
    return cal_day_number (event->start);
  return cal_day_number (event->end - 1);
}

int
cal_event_compare (gconstpointer a, gconstpointer b)
{
  const CalEvent *x = *(CalEvent * const *) a, *y = *(CalEvent * const *) b;

  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  if (x->end != y->end)
    return x->end > y->end ? -1 : 1;
  return g_strcmp0 (x->title, y->title);
}

/* --- colours */

static gboolean real_event_colors;

void
cal_set_real_event_colors (gboolean real)
{
  real_event_colors = real;
}

guint32
cal_event_display_color (const CalEvent *event)
{
  return cal_display_color (event->color, real_event_colors);
}

guint32
cal_declined_color (guint32 rgb)
{
  /* as Etar's integer arithmetic: each channel (c × 0x66 + 0xff × 0x99) >> 8 */
  guint32 out = 0xff000000;

  for (int shift = 0; shift <= 16; shift += 8)
    out |= ((((rgb >> shift) & 0xff) * 0x66 + 0xff * (0xff - 0x66)) >> 8) << shift;
  return out;
}

guint32
cal_adaptive_text_color (guint32 argb, guint32 event_rgb)
{
  double r = (event_rgb >> 16) & 0xff, g = (event_rgb >> 8) & 0xff, b = event_rgb & 0xff;
  int brightness = (int) sqrt (r * r * .299 + g * g * .587 + b * b * .114);
  double alpha = (argb >> 24) & 0xff;

  if (!real_event_colors)
    return argb;
  if (brightness > 130)
    return ((guint32) lround (alpha * 0.7f) << 24) | 0x000000;
  return ((guint32) lround (alpha * 0.9f) << 24) | 0xffffff;
}

guint32
cal_display_color (guint32 rgb, gboolean real_colors)
{
  double r = ((rgb >> 16) & 0xff) / 255.0, g = ((rgb >> 8) & 0xff) / 255.0, b = (rgb & 0xff) / 255.0;
  double max = MAX (r, MAX (g, b)), min = MIN (r, MIN (g, b));
  double h = 0, s = max > 0 ? (max - min) / max : 0, v = max;

  if (real_colors)
    return rgb & 0xffffff;
  /* Utils.getDisplayColorFromColor: Color.colorToHSV, s × 1.3 (max 1), v × 0.8, Color.HSVToColor */
  if (max > min)
    {
      if (max == r)
        h = fmod ((g - b) / (max - min), 6);
      else if (max == g)
        h = (b - r) / (max - min) + 2;
      else
        h = (r - g) / (max - min) + 4;
      h *= 60;
      if (h < 0)
        h += 360;
    }
  s = MIN (1.0, s * 1.3);
  v = v * 0.8;

  /* HSV back to RGB, as android.graphics.Color does (rounding each channel) */
  {
    double c = v * s, x = c * (1 - fabs (fmod (h / 60, 2) - 1)), m = v - c, rr, gg, bb;
    int sector = (int) (h / 60) % 6;

    switch (sector)
      {
      case 0: rr = c; gg = x; bb = 0; break;
      case 1: rr = x; gg = c; bb = 0; break;
      case 2: rr = 0; gg = c; bb = x; break;
      case 3: rr = 0; gg = x; bb = c; break;
      case 4: rr = x; gg = 0; bb = c; break;
      default: rr = c; gg = 0; bb = x; break;
      }
    return ((guint32) lround ((rr + m) * 255) << 16) | ((guint32) lround ((gg + m) * 255) << 8)
           | (guint32) lround ((bb + m) * 255);
  }
}

/* --- days */

int
cal_day_number (gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                  g_date_time_get_year (local));
  return (int) g_date_get_julian (&date);
}

gint64
cal_day_start (int day)
{
  GDate date;
  g_autoptr(GDateTime) midnight = NULL;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  midnight = g_date_time_new_local (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date), 0, 0,
                                    0);
  return g_date_time_to_unix (midnight);
}

int
cal_day_of_week (int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  /* GDateWeekday is Monday 1 … Sunday 7; Android's Time.weekDay is Sunday 0 … Saturday 6 */
  return g_date_get_weekday (&date) % 7;
}

int
cal_minute_of_day (gint64 time)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (time);

  return g_date_time_get_hour (local) * 60 + g_date_time_get_minute (local);
}

/* --- layout */

/* removeNonAlldayActiveEvents / removeAlldayActiveEvents: the events of the active list that ended before @event
 * starts free their columns */
static guint64
remove_inactive (CalEvent *event, GPtrArray *active, gboolean all_day, gint64 min_seconds, guint64 mask)
{
  for (guint i = 0; i < active->len;)
    {
      CalEvent *other = active->pdata[i];
      gboolean over;

      if (all_day)
        over = cal_event_last_day (other) < cal_event_first_day (event);
      else
        over = other->start + MAX (other->end - other->start, min_seconds) <= event->start;
      if (over)
        {
          mask &= ~(G_GUINT64_CONSTANT (1) << other->column);
          g_ptr_array_remove_index (active, i);
        }
      else
        i++;
    }
  return mask;
}

/* computeGroupLayout, non-staggered: each event takes the lowest free column; the group shares the widest count */
static void
layout_group (GPtrArray *group, gboolean all_day, gint64 min_seconds)
{
  g_autoptr(GPtrArray) active = g_ptr_array_new ();
  guint64 mask = 0;
  int max_columns = 0;

  for (guint i = 0; i < group->len; i++)
    {
      CalEvent *event = group->pdata[i];
      int column;

      mask = remove_inactive (event, active, all_day, min_seconds, mask);
      g_ptr_array_add (active, event);
      for (column = 0; column < 64 && (mask & (G_GUINT64_CONSTANT (1) << column)); column++)
        ;
      /* "No column available. What do we do now? Use last column" (Event.java:572-576) */
      if (column == 64)
        column = 63;
      else
        mask |= G_GUINT64_CONSTANT (1) << column;
      event->column = column;
      max_columns = MAX (max_columns, column + 1);
    }
  for (guint i = 0; i < group->len; i++)
    {
      ((CalEvent *) group->pdata[i])->max_columns = max_columns;
      ((CalEvent *) group->pdata[i])->text_offset = 0;
      ((CalEvent *) group->pdata[i])->staggered = FALSE;
    }
}

/* getUnobscuredDurationOffset: from the start of @event, where a stretch of @min_seconds begins that no event of a
 * higher column covers; -1 if there is none */
static gint64
unobscured_offset (CalEvent *event, GPtrArray *group, gint64 min_seconds)
{
  gint64 start = event->start;
  gint64 end = MAX (event->end, event->start + min_seconds);

  for (guint i = 0; i < group->len; i++)
    {
      CalEvent *other = group->pdata[i];

      if (other == event || other->column <= event->column)
        continue;
      if (other->start >= end)
        break;
      if (other->start - start >= min_seconds)
        return start - event->start;
      start = MAX (start, MAX (other->end, other->start + min_seconds));
    }
  return end - start >= min_seconds ? start - event->start : -1;
}

/* computeEventColAndTextOffsetStaggered: the column for @event, moving others up when one is inserted; the new mask,
 * or 0 with *failed when it fits nowhere */
static guint64
stagger_one (CalEvent *event, GPtrArray *active, GPtrArray *group, guint64 mask, int max_columns, gint64 min_seconds,
             gboolean *failed)
{
  for (int column = 0; column < 64; column++)
    {
      gboolean obscures = FALSE;

      event->column = column;
      /* does it wholly cover an event of the column below? */
      for (guint i = 0; i < active->len; i++)
        {
          CalEvent *other = active->pdata[i];
          gint64 offset;

          if (other == event || other->column != column - 1)
            continue;
          offset = unobscured_offset (other, group, min_seconds);
          if (offset == -1)
            {
              obscures = TRUE;
              break;
            }
          other->text_offset = offset;
        }
      if (obscures)
        {
          gint64 offset;
          guint64 low, high;

          /* a column between column - 2 and column - 1, if the event is not itself covered there */
          event->column = column - 2;
          offset = unobscured_offset (event, group, min_seconds);
          if (offset == -1)
            {
              *failed = TRUE;
              return 0;
            }
          event->column = column - 1;
          event->text_offset = offset;
          for (guint i = 0; i < active->len; i++)
            {
              CalEvent *other = active->pdata[i];
              gint64 other_offset;

              if (other == event || other->column != column - 1 || other->text_offset == 0)
                continue;
              other_offset = unobscured_offset (other, group, min_seconds);
              if (other_offset == -1)
                {
                  *failed = TRUE;
                  return 0;
                }
              other->text_offset = other_offset;
            }
          for (guint i = 0; i < group->len; i++)
            {
              CalEvent *other = group->pdata[i];

              if (other != event && other->column >= column - 1)
                other->column++;
            }
          low = mask & ((G_GUINT64_CONSTANT (1) << column) - 1);
          high = column < 63 ? (mask >> column) << (column + 1) : 0;
          mask = low | high;
          break;
        }
      else if (mask & (G_GUINT64_CONSTANT (1) << column))
        continue;
      else if (column >= max_columns)
        {
          event->text_offset = 0;
          break;
        }
      else
        {
          gint64 offset = unobscured_offset (event, group, min_seconds);

          if (offset != -1)
            {
              event->text_offset = offset;
              break;
            }
        }
    }
  return mask | (G_GUINT64_CONSTANT (1) << event->column);
}

static void
stagger_group (GPtrArray *group, gint64 min_seconds)
{
  g_autoptr(GPtrArray) active = g_ptr_array_new ();
  guint64 mask = 0;
  int max_columns = 0;

  /* as Etar's freshly loaded Events: an event not placed yet sits in column 0 */
  for (guint i = 0; i < group->len; i++)
    {
      ((CalEvent *) group->pdata[i])->column = 0;
      ((CalEvent *) group->pdata[i])->text_offset = 0;
    }
  for (guint i = 0; i < group->len; i++)
    {
      CalEvent *event = group->pdata[i];
      gboolean failed = FALSE;

      mask = remove_inactive (event, active, FALSE, min_seconds, mask);
      g_ptr_array_add (active, event);
      mask = stagger_one (event, active, group, mask, max_columns, min_seconds, &failed);
      if (failed)
        {
          /* no valid column: the group is laid out side by side instead */
          for (guint j = 0; j < group->len; j++)
            {
              ((CalEvent *) group->pdata[j])->text_offset = 0;
              ((CalEvent *) group->pdata[j])->staggered = FALSE;
            }
          layout_group (group, FALSE, min_seconds);
          return;
        }
      max_columns = MAX (max_columns, event->column + 1);
    }
  /* an inserted column moves events up past the max_columns Etar keeps, which then draws them beyond their day
   * (EventGeometry's right edge); the group's columns counted after the moves keep them in it */
  for (guint i = 0; i < group->len; i++)
    max_columns = MAX (max_columns, ((CalEvent *) group->pdata[i])->column + 1);
  for (guint i = 0; i < group->len; i++)
    {
      ((CalEvent *) group->pdata[i])->max_columns = max_columns;
      ((CalEvent *) group->pdata[i])->staggered = TRUE;
    }
}

void
cal_layout_events_staggered (GPtrArray *events, gint64 min_seconds)
{
  g_autoptr(GPtrArray) group = g_ptr_array_new ();
  gint64 group_end = G_MININT64;

  min_seconds = MAX (min_seconds, 0);
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];

      if (group->len > 0 && event->start >= group_end)
        {
          stagger_group (group, min_seconds);
          g_ptr_array_set_size (group, 0);
          group_end = G_MININT64;
        }
      g_ptr_array_add (group, event);
      group_end = MAX (group_end, event->end);
    }
  stagger_group (group, min_seconds);
}

void
cal_layout_events (GPtrArray *events, gboolean all_day, gint64 min_seconds)
{
  g_autoptr(GPtrArray) group = g_ptr_array_new ();
  gint64 group_end = G_MININT64;
  int group_last_day = G_MININT;

  /* doComputePositions: events are grouped while they overlap the group so far */
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];
      gboolean separate;

      if (all_day)
        separate = group->len > 0 && cal_event_first_day (event) > group_last_day;
      else
        separate = group->len > 0 && event->start >= group_end;
      if (separate)
        {
          layout_group (group, all_day, min_seconds);
          g_ptr_array_set_size (group, 0);
          group_end = G_MININT64;
          group_last_day = G_MININT;
        }
      g_ptr_array_add (group, event);
      group_end = MAX (group_end, event->end);
      group_last_day = MAX (group_last_day, cal_event_last_day (event));
    }
  layout_group (group, all_day, min_seconds);
}
