/* Etar's RecurrencePickerDialog, WeekButton, LinearLayoutWithMaxWidth and EventRecurrence (cal-recurrence-picker.h;
 * the recurrencepicker package, calendarcommon2/EventRecurrence.java, layout/recurrencepicker*.xml).
 *
 * Built without CAL_RECURRENCE_NO_UI it is the dialog; with it, only the rule and the model, for the tests.
 *
 * Left out, each at its place below too:
 *  - the switch's own thumb colours (app:thumbTint preference_material_switch_color: white on, darker grey off):
 *    CalMaterialSwitch draws Material 3's, and has no way to take others;
 *  - a pressed day showing filled before it toggles (recurrence_bubble_fill's state_pressed): NeoPressable keeps its
 *    pressed state to itself;
 *  - the toast of the rule (doToast): Etar never calls it;
 *  - keeping the dialog's state across a rotation (onSaveInstanceState): a GTK window keeps its widgets. */
#include "cal-recurrence-picker.h"

#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>

/* --- EventRecurrence */

static const char *const day_codes[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
static const char *const freq_names[] = {
  NULL, "SECONDLY", "MINUTELY", "HOURLY", "DAILY", "WEEKLY", "MONTHLY", "YEARLY",
};

enum {
  PARSED_FREQ = 1 << 0, PARSED_UNTIL = 1 << 1, PARSED_COUNT = 1 << 2, PARSED_INTERVAL = 1 << 3,
  PARSED_BYSECOND = 1 << 4, PARSED_BYMINUTE = 1 << 5, PARSED_BYHOUR = 1 << 6, PARSED_BYDAY = 1 << 7,
  PARSED_BYMONTHDAY = 1 << 8, PARSED_BYYEARDAY = 1 << 9, PARSED_BYWEEKNO = 1 << 10, PARSED_BYMONTH = 1 << 11,
  PARSED_BYSETPOS = 1 << 12, PARSED_WKST = 1 << 13,
};

static void
list_set (CalRecurList *list, int n, const int *values)
{
  g_free (list->values);
  list->n = n;
  list->values = n ? g_memdup2 (values, n * sizeof (int)) : NULL;
}

void
cal_recurrence_free (CalRecurrence *er)
{
  CalRecurList *lists[] = { &er->bysecond, &er->byminute, &er->byhour, &er->bymonthday, &er->byyearday,
                            &er->byweekno, &er->bymonth, &er->bysetpos };

  for (guint i = 0; i < G_N_ELEMENTS (lists); i++)
    g_free (lists[i]->values);
  g_free (er->until);
  g_free (er->byday);
  g_free (er->byday_num);
  g_free (er);
}

CalRecurrence *
cal_recurrence_new (int wkst)
{
  CalRecurrence *er = g_new0 (CalRecurrence, 1);

  er->wkst = wkst;
  return er;
}

/* PartParser.parseIntRange: an int (a leading '+' allowed), within [min, max], 0 only when allowed */
static gboolean
parse_int_range (const char *str, gint64 min, gint64 max, gboolean allow_zero, int *out)
{
  const char *digits;
  char *end;
  gint64 value;

  if (*str == '+')
    str++;
  digits = *str == '-' ? str + 1 : str;
  if (!g_ascii_isdigit (*digits))
    return FALSE;
  value = g_ascii_strtoll (str, &end, 10);
  /* Integer.parseInt: digits to the end, within an int */
  if (*end || value < G_MININT32 || value > G_MAXINT32)
    return FALSE;
  if (value < min || value > max || (value == 0 && !allow_zero))
    return FALSE;
  *out = (int) value;
  return TRUE;
}

/* String.split (","), which drops empty strings at the end only */
static GStrv
split_list (const char *text)
{
  GStrv items = g_strsplit (text, ",", -1);
  int n = (int) g_strv_length (items);

  while (n > 0 && !*items[n - 1])
    {
      g_free (items[n - 1]);
      items[--n] = NULL;
    }
  return items;
}

/* PartParser.parseNumberList */
static gboolean
parse_number_list (const char *text, gint64 min, gint64 max, gboolean allow_zero, CalRecurList *list)
{
  g_auto(GStrv) items = split_list (text);
  int n = (int) g_strv_length (items);
  g_autofree int *values = g_new0 (int, MAX (n, 1));

  if (n == 0)
    return FALSE;
  for (int i = 0; i < n; i++)
    if (!parse_int_range (items[i], min, max, allow_zero, &values[i]))
      return FALSE;
  list_set (list, n, values);
  return TRUE;
}

static int
weekday_of (const char *code)
{
  for (int i = 0; i < 7; i++)
    if (g_str_equal (code, day_codes[i]))
      return i;
  return -1;
}

/* ParseByDay: "1SU,-1SU" into weekdays and their numbers */
static gboolean
parse_byday (const char *value, CalRecurrence *er)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (value));
  g_auto(GStrv) items = split_list (trimmed);
  int n = (int) g_strv_length (items);

  if (n == 0)
    return FALSE;
  g_free (er->byday);
  g_free (er->byday_num);
  er->byday = g_new0 (int, n);
  er->byday_num = g_new0 (int, n);
  er->byday_count = n;
  for (int i = 0; i < n; i++)
    {
      const char *item = items[i];
      int start = (int) strlen (item) - 2;

      /* parseWday: a number before the weekday's two letters */
      if (start > 0)
        {
          g_autofree char *number = g_strndup (item, start);

          if (!parse_int_range (number, -53, 53, FALSE, &er->byday_num[i]))
            return FALSE;
          item += start;
        }
      if ((er->byday[i] = weekday_of (item)) < 0)
        return FALSE;
    }
  return TRUE;
}

/* one part: its PARSED_ flag, 0 for a name without a parser, −1 for a value it refuses */
static int
parse_part (const char *name, const char *value, CalRecurrence *er)
{
  if (g_str_equal (name, "FREQ"))
    {
      for (int i = CAL_RECUR_SECONDLY; i <= CAL_RECUR_YEARLY; i++)
        if (g_str_equal (value, freq_names[i]))
          {
            er->freq = i;
            return PARSED_FREQ;
          }
      return -1;
    }
  if (g_str_equal (name, "UNTIL"))
    {
      /* VALIDATE_UNTIL is off: kept as written */
      g_free (er->until);
      er->until = g_strdup (value);
      return PARSED_UNTIL;
    }
  if (g_str_equal (name, "COUNT"))
    {
      if (!parse_int_range (value, G_MININT32, G_MAXINT32, TRUE, &er->count))
        return -1;
      if (er->count < 0)
        er->count = 1;                  /* "Invalid Count. Forcing COUNT to 1" */
      return PARSED_COUNT;
    }
  if (g_str_equal (name, "INTERVAL"))
    {
      if (!parse_int_range (value, G_MININT32, G_MAXINT32, TRUE, &er->interval))
        return -1;
      if (er->interval < 1)
        er->interval = 1;
      return PARSED_INTERVAL;
    }
  if (g_str_equal (name, "BYSECOND"))
    return parse_number_list (value, 0, 59, TRUE, &er->bysecond) ? PARSED_BYSECOND : -1;
  if (g_str_equal (name, "BYMINUTE"))
    return parse_number_list (value, 0, 59, TRUE, &er->byminute) ? PARSED_BYMINUTE : -1;
  if (g_str_equal (name, "BYHOUR"))
    return parse_number_list (value, 0, 23, TRUE, &er->byhour) ? PARSED_BYHOUR : -1;
  if (g_str_equal (name, "BYDAY"))
    return parse_byday (value, er) ? PARSED_BYDAY : -1;
  if (g_str_equal (name, "BYMONTHDAY"))
    return parse_number_list (value, -31, 31, FALSE, &er->bymonthday) ? PARSED_BYMONTHDAY : -1;
  if (g_str_equal (name, "BYYEARDAY"))
    return parse_number_list (value, -366, 366, FALSE, &er->byyearday) ? PARSED_BYYEARDAY : -1;
  if (g_str_equal (name, "BYWEEKNO"))
    return parse_number_list (value, -53, 53, FALSE, &er->byweekno) ? PARSED_BYWEEKNO : -1;
  if (g_str_equal (name, "BYMONTH"))
    return parse_number_list (value, 1, 12, FALSE, &er->bymonth) ? PARSED_BYMONTH : -1;
  if (g_str_equal (name, "BYSETPOS"))
    return parse_number_list (value, G_MININT32, G_MAXINT32, TRUE, &er->bysetpos) ? PARSED_BYSETPOS : -1;
  if (g_str_equal (name, "WKST"))
    return (er->wkst = weekday_of (value)) >= 0 ? PARSED_WKST : -1;
  return 0;
}

CalRecurrence *
cal_recurrence_parse (const char *rrule)
{
  g_autoptr(CalRecurrence) er = cal_recurrence_new (-1);
  g_autofree char *upper = NULL;
  g_auto(GStrv) parts = NULL;
  int parsed = 0;

  if (!rrule)
    return NULL;
  /* ALLOW_LOWER_CASE: the whole rule upper-cased */
  upper = g_ascii_strup (rrule, -1);
  parts = g_strsplit (upper, ";", -1);
  for (int i = 0; parts[i]; i++)
    {
      char *part = parts[i], *equal;
      int flag;

      if (!*part)                       /* ";;" */
        continue;
      equal = strchr (part, '=');
      if (!equal || equal == part || !equal[1])
        return NULL;                    /* "Missing LHS", "Missing RHS" */
      *equal = '\0';
      flag = parse_part (part, equal + 1, er);
      if (flag == 0 && g_str_has_prefix (part, "X-"))
        continue;                       /* "Ignoring custom part" */
      if (flag <= 0 || (parsed & flag))
        return NULL;                    /* no parser, a bad value, a part given twice */
      parsed |= flag;
    }
  if (!(parsed & PARSED_WKST))
    er->wkst = 1;                       /* "If not specified, week starts on Monday" */
  if (!(parsed & PARSED_FREQ))
    return NULL;
  /* both UNTIL and COUNT: ONLY_ONE_UNTIL_COUNT is off, so Etar only warns (canHandleRecurrenceRule refuses it) */
  return g_steal_pointer (&er);
}

static void
append_numbers (GString *s, const char *label, const CalRecurList *list)
{
  if (list->n <= 0)
    return;
  g_string_append (s, label);
  for (int i = 0; i < list->n; i++)
    g_string_append_printf (s, i ? ",%d" : "%d", list->values[i]);
}

char *
cal_recurrence_to_string (const CalRecurrence *er)
{
  GString *s = g_string_new ("FREQ=");

  if (er->freq >= CAL_RECUR_SECONDLY && er->freq <= CAL_RECUR_YEARLY)
    g_string_append (s, freq_names[er->freq]);
  if (er->until && *er->until)
    g_string_append_printf (s, ";UNTIL=%s", er->until);
  if (er->count != 0)
    g_string_append_printf (s, ";COUNT=%d", er->count);
  if (er->interval != 0)
    g_string_append_printf (s, ";INTERVAL=%d", er->interval);
  if (er->wkst >= 0 && er->wkst < 7)
    g_string_append_printf (s, ";WKST=%s", day_codes[er->wkst]);
  append_numbers (s, ";BYSECOND=", &er->bysecond);
  append_numbers (s, ";BYMINUTE=", &er->byminute);
  /* Etar labels the hours ";BYSECOND=" too (a slip: BYHOUR=9 would come back as a second BYSECOND, a rule its own
   * parser refuses); written as what they are */
  append_numbers (s, ";BYHOUR=", &er->byhour);
  for (int i = 0; i < er->byday_count; i++)
    {
      g_string_append (s, i ? "," : ";BYDAY=");
      if (er->byday_num[i] != 0)
        g_string_append_printf (s, "%d", er->byday_num[i]);
      g_string_append (s, day_codes[er->byday[i]]);
    }
  append_numbers (s, ";BYMONTHDAY=", &er->bymonthday);
  append_numbers (s, ";BYYEARDAY=", &er->byyearday);
  append_numbers (s, ";BYWEEKNO=", &er->byweekno);
  append_numbers (s, ";BYMONTH=", &er->bymonth);
  append_numbers (s, ";BYSETPOS=", &er->bysetpos);
  return g_string_free (s, FALSE);
}

/* --- RecurrencePickerDialog's rule logic */

#define FIFTH_WEEK_IN_A_MONTH 5
#define LAST_NTH_DAY_OF_WEEK  (-1)

static gboolean
is_supported_monthly_by_nth_day_of_week (int num)
{
  /* "greater then 0 but less then 5", or −1 for the last one */
  return (num > 0 && num <= FIFTH_WEEK_IN_A_MONTH) || num == LAST_NTH_DAY_OF_WEEK;
}

/* canHandleRecurrenceRule */
static gboolean
can_handle_recurrence (const CalRecurrence *er)
{
  int nth = 0;

  switch (er->freq)
    {
    case CAL_RECUR_DAILY:
    case CAL_RECUR_MONTHLY:
    case CAL_RECUR_YEARLY:
    case CAL_RECUR_WEEKLY:
      break;
    default:
      return FALSE;
    }
  if (er->count > 0 && er->until && *er->until)
    return FALSE;
  /* the nth of a weekday: one, and only in a monthly rule */
  for (int i = 0; i < er->byday_count; i++)
    if (is_supported_monthly_by_nth_day_of_week (er->byday_num[i]))
      nth++;
  if (nth > 1)
    return FALSE;
  if (nth > 0 && er->freq != CAL_RECUR_MONTHLY)
    return FALSE;
  /* one day of the month, not the 9th and the 10th */
  if (er->bymonthday.n > 1)
    return FALSE;
  if (er->freq == CAL_RECUR_MONTHLY)
    {
      if (er->byday_count > 1)
        return FALSE;
      if (er->byday_count > 0 && er->bymonthday.n > 0)
        return FALSE;
    }
  return TRUE;
}

static GTimeZone *
event_zone (const char *tzid)
{
  GTimeZone *zone = tzid ? g_time_zone_new_identifier (tzid) : NULL;

  return zone ? zone : g_time_zone_new_local ();
}

static int
day_number_of (int year, int month, int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, day, month, year);
  return (int) g_date_get_julian (&date);
}

/* the event's start (mTime): its day, time of day, weekday and day of the month, in its zone */
typedef struct {
  int day, second, weekday, month_day;
} Moment;

static Moment
moment_of (gint64 time, const char *tzid)
{
  g_autoptr(GTimeZone) zone = event_zone (tzid);
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc (time);
  g_autoptr(GDateTime) there = g_date_time_to_timezone (utc, zone);

  return (Moment) {
    day_number_of (g_date_time_get_year (there), g_date_time_get_month (there), g_date_time_get_day_of_month (there)),
    g_date_time_get_hour (there) * 3600 + g_date_time_get_minute (there) * 60 + g_date_time_get_second (there),
    g_date_time_get_day_of_week (there) % 7,
    g_date_time_get_day_of_month (there),
  };
}

static GDateTime *
time_at (int day, int second, const char *tzid)
{
  g_autoptr(GTimeZone) zone = event_zone (tzid);
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  return g_date_time_new (zone, g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                          second / 3600, second / 60 % 60, second % 60);
}

/* Time.setMonth (month + n).normalize (): a day past the month's end runs into the next (Jan 31 + 1 → Mar 3) */
static int
add_months (int day, int months)
{
  GDate date;
  int total;

  g_date_clear (&date, 1);
  g_date_set_julian (&date, day);
  total = g_date_get_month (&date) - 1 + months;
  return day_number_of (g_date_get_year (&date) + total / 12, total % 12 + 1, 1) + g_date_get_day (&date) - 1;
}

/* Time.parse: "YYYYMMDD" (a date: @second −1), "YYYYMMDDTHHMMSS" (in the event's zone) or that with "Z" (UTC);
 * FALSE for anything else (TimeFormatException) */
static gboolean
parse_until (const char *until, const char *tzid, int *day, int *second)
{
  size_t length = strlen (until);
  int y, mo, d, h = 0, mi = 0, s = 0;

  if (length != 8 && length != 15 && length != 16)
    return FALSE;
  for (size_t i = 0; i < length; i++)
    if (i == 8 ? until[i] != 'T' : i == 15 ? until[i] != 'Z' : !g_ascii_isdigit (until[i]))
      return FALSE;
  if (sscanf (until, "%4d%2d%2d", &y, &mo, &d) != 3 || !g_date_valid_dmy (d, mo, y))
    return FALSE;
  if (length > 8 && (sscanf (until + 9, "%2d%2d%2d", &h, &mi, &s) != 3 || h > 23 || mi > 59 || s > 59))
    return FALSE;
  if (length == 16)
    {
      g_autoptr(GDateTime) utc = g_date_time_new_utc (y, mo, d, h, mi, s);
      g_autoptr(GTimeZone) zone = event_zone (tzid);
      g_autoptr(GDateTime) there = g_date_time_to_timezone (utc, zone);

      *day = day_number_of (g_date_time_get_year (there), g_date_time_get_month (there),
                            g_date_time_get_day_of_month (there));
      *second = g_date_time_get_hour (there) * 3600 + g_date_time_get_minute (there) * 60
                + g_date_time_get_second (there);
      return TRUE;
    }
  *day = day_number_of (y, mo, d);
  *second = length == 8 ? -1 : h * 3600 + mi * 60 + s;
  return TRUE;
}

char *
cal_recurrence_fit_until (const char *rrule, gint64 start, const char *tzid)
{
  g_auto(GStrv) parts = rrule ? g_strsplit (rrule, ";", -1) : NULL;

  for (int i = 0; parts && parts[i]; i++)
    {
      const char *value = parts[i] + 6;
      int day, second;
      gboolean date;

      if (g_ascii_strncasecmp (parts[i], "UNTIL=", 6) != 0 || !parse_until (value, tzid, &day, &second))
        continue;
      date = strlen (value) == 8;
      if (!tzid && !date)
        {
          GDate d;

          g_date_clear (&d, 1);
          g_date_set_julian (&d, day);
          g_free (parts[i]);
          parts[i] = g_strdup_printf ("UNTIL=%04d%02d%02d", g_date_get_year (&d), g_date_get_month (&d),
                                      g_date_get_day (&d));
        }
      else if (tzid && date)
        {
          g_autoptr(GDateTime) there = time_at (day, moment_of (start, tzid).second, tzid);
          g_autoptr(GDateTime) utc = g_date_time_to_utc (there);
          g_autofree char *text = g_date_time_format (utc, "%Y%m%dT%H%M%SZ");

          g_free (parts[i]);
          parts[i] = g_strconcat ("UNTIL=", text, NULL);
        }
      return g_strjoinv (";", parts);
    }
  return g_strdup (rrule);
}

/* copyEventRecurrenceToModel; FALSE where it throws IllegalStateException */
static gboolean
copy_recurrence_to_model (const CalRecurrence *er, CalRecurrenceModel *model, const char *tzid)
{
  int nth = 0;

  switch (er->freq)
    {
    case CAL_RECUR_DAILY:   model->freq = CAL_REPEAT_DAILY;   break;
    case CAL_RECUR_MONTHLY: model->freq = CAL_REPEAT_MONTHLY; break;
    case CAL_RECUR_YEARLY:  model->freq = CAL_REPEAT_YEARLY;  break;
    case CAL_RECUR_WEEKLY:  model->freq = CAL_REPEAT_WEEKLY;  break;
    default:
      return FALSE;
    }
  if (er->interval > 0)
    model->interval = er->interval;
  /* the end: by count (a rule without COUNT leaves 0, which "For a number of events" makes 1) … */
  model->end_count = er->count;
  if (model->end_count > 0)
    model->end = CAL_REPEAT_END_BY_COUNT;
  /* … or by date (one Time.parse cannot read is none: the default end date is used) */
  if (er->until && *er->until)
    {
      gboolean parsed = parse_until (er->until, tzid, &model->end_day, &model->end_second);

      if (model->end == CAL_REPEAT_END_BY_COUNT && parsed)
        return FALSE;                   /* "The UI can only handle END_BY_DATE or END_BY_COUNT" */
      if (!parsed)
        model->end_day = 0;
      model->end = CAL_REPEAT_END_BY_DATE;
    }
  /* weekly by weekday, or monthly by the nth weekday */
  memset (model->weekly_by_day_of_week, 0, sizeof model->weekly_by_day_of_week);
  if (er->byday_count > 0)
    {
      for (int i = 0; i < er->byday_count; i++)
        {
          model->weekly_by_day_of_week[er->byday[i]] = TRUE;
          if (model->freq == CAL_REPEAT_MONTHLY && is_supported_monthly_by_nth_day_of_week (er->byday_num[i]))
            {
              model->monthly_by_day_of_week = er->byday[i];
              model->monthly_by_nth_day_of_week = er->byday_num[i];
              model->monthly_repeat = CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK;
              nth++;
            }
        }
      if (model->freq == CAL_REPEAT_MONTHLY && (er->byday_count != 1 || nth != 1))
        return FALSE;                   /* one weekday, and which of them in the month */
    }
  if (model->freq == CAL_REPEAT_MONTHLY)
    {
      if (er->bymonthday.n == 1)
        {
          if (model->monthly_repeat == CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK)
            return FALSE;
          model->monthly_by_month_day = er->bymonthday.values[0];
          model->monthly_repeat = CAL_REPEAT_MONTHLY_BY_DATE;
        }
      else if (er->bymonth.n > 1)
        return FALSE;                   /* Etar tests BYMONTH's count here, meaning BYMONTHDAY's: kept, so a
                                         * monthly rule in two months is refused as Etar's dialog refuses it */
    }
  return TRUE;
}

gboolean
cal_recurrence_model_init (CalRecurrenceModel *model, const CalRecurrence *er, gint64 start, const char *tzid)
{
  Moment first = moment_of (start, tzid);

  memset (model, 0, sizeof *model);
  model->freq = CAL_REPEAT_WEEKLY;
  model->interval = 1;
  model->end = CAL_REPEAT_END_NEVER;
  model->end_count = CAL_REPEAT_COUNT_DEFAULT;
  model->end_second = -1;
  model->weekly_by_day_of_week[first.weekday] = TRUE;
  if (er && er->freq)
    {
      model->repeats = TRUE;
      if (!copy_recurrence_to_model (er, model, tzid))
        return FALSE;
      /* "Leave today's day of week as checked by default in weekly view" */
      if (er->byday_count == 0)
        model->weekly_by_day_of_week[first.weekday] = TRUE;
    }
  /* no end date: the start's, a month on for a daily or weekly rule, three for a monthly one, three years for a
   * yearly one, at the start's time of day */
  if (model->end_day == 0)
    {
      model->end_day = add_months (first.day, model->freq == CAL_REPEAT_MONTHLY ? 3
                                               : model->freq == CAL_REPEAT_YEARLY ? 36 : 1);
      model->end_second = first.second;
    }
  else if (model->end_second < 0)
    model->end_second = first.second;
  /* updateDialog: "on every [nth] [weekday]" of the start, the fifth one being the last */
  if (model->monthly_by_nth_day_of_week == 0)
    {
      model->monthly_by_nth_day_of_week = (first.month_day + 6) / 7;
      if (model->monthly_by_nth_day_of_week >= FIFTH_WEEK_IN_A_MONTH)
        model->monthly_by_nth_day_of_week = LAST_NTH_DAY_OF_WEEK;
      model->monthly_by_day_of_week = first.weekday;
    }
  return TRUE;
}

static char *
until_string (const CalRecurrenceModel *model, const char *tzid)
{
  g_autoptr(GDateTime) local = time_at (model->end_day, MAX (model->end_second, 0), tzid);
  g_autoptr(GDateTime) utc = NULL;

  /* an all-day event: a DATE, as RFC 5545 has an all-day start's UNTIL (a UTC time at the zone's midnight would
   * fall on the day before, and libical would end the rule a day early) */
  if (!tzid)
    return g_date_time_format (local, "%Y%m%d");
  /* switchTimezone (UTC), format2445 */
  utc = g_date_time_to_utc (local);
  return g_date_time_format (utc, "%Y%m%dT%H%M%SZ");
}

char *
cal_recurrence_model_store (const CalRecurrenceModel *model, CalRecurrence *er, const char *tzid)
{
  static const CalRecurFreq freqs[] = { CAL_RECUR_DAILY, CAL_RECUR_WEEKLY, CAL_RECUR_MONTHLY, CAL_RECUR_YEARLY };

  if (!model->repeats)
    return NULL;
  er->freq = freqs[model->freq];
  er->interval = model->interval <= 1 ? 0 : model->interval;
  switch (model->end)
    {
    case CAL_REPEAT_END_BY_DATE:
      g_free (er->until);
      er->until = until_string (model, tzid);
      er->count = 0;
      break;
    case CAL_REPEAT_END_BY_COUNT:
      er->count = model->end_count;
      g_clear_pointer (&er->until, g_free);
      if (er->count <= 0)
        return NULL;                    /* "count is 0" */
      break;
    default:
      er->count = 0;
      g_clear_pointer (&er->until, g_free);
      break;
    }
  /* the weekly and monthly patterns, from what the dialog shows */
  er->byday_count = 0;
  list_set (&er->bymonthday, 0, NULL);
  switch (model->freq)
    {
    case CAL_REPEAT_MONTHLY:
      if (model->monthly_repeat == CAL_REPEAT_MONTHLY_BY_DATE)
        {
          if (model->monthly_by_month_day > 0)
            list_set (&er->bymonthday, 1, &model->monthly_by_month_day);
        }
      else
        {
          if (!is_supported_monthly_by_nth_day_of_week (model->monthly_by_nth_day_of_week))
            return NULL;
          g_free (er->byday);
          g_free (er->byday_num);
          er->byday = g_new (int, 1);
          er->byday_num = g_new (int, 1);
          er->byday[0] = model->monthly_by_day_of_week;
          er->byday_num[0] = model->monthly_by_nth_day_of_week;
          er->byday_count = 1;
        }
      break;
    case CAL_REPEAT_WEEKLY:
      g_free (er->byday);
      g_free (er->byday_num);
      er->byday = g_new0 (int, 7);
      er->byday_num = g_new0 (int, 7);
      /* Sunday first, whatever the week's start */
      for (int i = 0; i < 7; i++)
        if (model->weekly_by_day_of_week[i])
          er->byday[er->byday_count++] = i;
      break;
    default:
      break;
    }
  if (!can_handle_recurrence (er))
    return NULL;                        /* "UI generated recurrence that it can't handle" */
  return cal_recurrence_to_string (er);
}

gboolean
cal_recurrence_model_equal (const CalRecurrenceModel *a, const CalRecurrenceModel *b)
{
  if (a->repeats != b->repeats || a->freq != b->freq || a->interval != b->interval || a->end != b->end
      || a->end_day != b->end_day || a->end_second != b->end_second || a->end_count != b->end_count
      || a->monthly_repeat != b->monthly_repeat || a->monthly_by_month_day != b->monthly_by_month_day
      || a->monthly_by_day_of_week != b->monthly_by_day_of_week
      || a->monthly_by_nth_day_of_week != b->monthly_by_nth_day_of_week)
    return FALSE;
  for (int i = 0; i < 7; i++)
    if (!a->weekly_by_day_of_week[i] != !b->weekly_by_day_of_week[i])
      return FALSE;
  return TRUE;
}

gboolean
cal_recurrence_picker_can_handle (const char *rrule)
{
  g_autoptr(CalRecurrence) er = NULL;
  CalRecurrenceModel model;

  /* EditEventView.populateRepeats: no rule, the button is on */
  if (!rrule || !*rrule)
    return TRUE;
  er = cal_recurrence_parse (rrule);
  /* the start makes no difference to whether the dialog would throw */
  return er && can_handle_recurrence (er) && cal_recurrence_model_init (&model, er, 0, "UTC");
}

#ifndef CAL_RECURRENCE_NO_UI

/* --- the dialog */

#include "cal-colors.h"
#include "cal-date-picker.h"
#include "cal-format.h"
#include "cal-material-switch.h"
#include "cal-popup.h"

#include <neo.h>
#include <spongy-gtk.h>

#define MIN_SCREEN_WIDTH_FOR_SINGLE_ROW_WEEK 450
#define INTERVAL_DEFAULT 1
#define BUBBLE_TEXT_NORMAL   0xff737373    /* recurrence_bubble_text_normal (not themed: the same in every look) */
#define BUBBLE_TEXT_DISABLED 0xffcccccc    /* done_text_color_disabled */
#define BUBBLE_DISABLED      0xff979696    /* ic_recurrence_bubble_*_disabled */
#define BUBBLE_RADIUS 11.6998              /* the bubbles' circle, in their 24 × 24 viewport */
#define BUBBLE_STROKE 0.600365

typedef struct {
  NeoDialog *dialog;
  CalRecurrenceSet done;
  gpointer data;

  char *rrule;                          /* the rule it opened with */
  CalRecurrence *er;                    /* mRecurrence */
  CalRecurrenceModel model, initial;    /* mModel, and as it opened */
  char *tzid;
  int updating;                         /* text set by the dialog, not typed */
  gboolean hide_post_end_count;         /* mHidePostEndCount */

  GtkWidget *freq, *repeat_switch, *options;
  GtkWidget *interval_pre, *interval, *interval_post;
  GtkWidget *week_group, *week_group2, *days[7];
  GtkWidget *month_group, *by_month_day, *by_nth;
  GtkWidget *end_spinner, *end_label, *end_count_field, *end_count, *post_end_count, *end_date;
  GtkWidget *done_button;
} Picker;

static void update_dialog (Picker *picker);

static void
picker_free (gpointer data, GClosure *closure)
{
  Picker *picker = data;

  g_free (picker->rrule);
  g_clear_pointer (&picker->er, cal_recurrence_free);
  g_free (picker->tzid);
  g_free (picker);
}

/* the text before and after a count's marker ("Every %d weeks": "Every", "weeks"), trimmed; FALSE without one */
static gboolean
split_marker (const char *text, const char *marker, char **before, char **after)
{
  const char *at = strstr (text, marker);

  if (!at)
    return FALSE;
  if (before)
    *before = g_strstrip (g_strndup (text, at - text));
  if (after)
    *after = g_strstrip (g_strdup (at + strlen (marker)));
  return TRUE;
}

static const char *
interval_string (CalRepeatFreq freq, int n)
{
  switch (freq)
    {
    case CAL_REPEAT_DAILY:
      return g_dngettext (NULL, "Every %d day", "Every %d days", n);
    case CAL_REPEAT_WEEKLY:
      return g_dngettext (NULL, "Every %d week", "Every %d weeks", n);
    case CAL_REPEAT_MONTHLY:
      return g_dngettext (NULL, "Every %d month", "Every %d months", n);
    default:
      return g_dngettext (NULL, "Every %d year", "Every %d years", n);
    }
}

static const char *
end_count_string (int n)
{
  return g_dngettext (NULL, "For %d event", "For %d events", n);
}

static int
text_value (GtkWidget *text)
{
  return atoi (gtk_editable_get_text (GTK_EDITABLE (text)));
}

static gboolean
text_empty (GtkWidget *text)
{
  return !*gtk_editable_get_text (GTK_EDITABLE (text));
}

static void
set_number (Picker *picker, GtkWidget *text, int value)
{
  g_autofree char *string = g_strdup_printf ("%d", value);

  /* checked first, as Etar does, so the cursor stays where it is */
  if (g_str_equal (string, gtk_editable_get_text (GTK_EDITABLE (text))))
    return;
  picker->updating++;
  gtk_editable_set_text (GTK_EDITABLE (text), string);
  picker->updating--;
}

/* updateIntervalText: "Every" [n] "weeks" */
static void
update_interval_text (Picker *picker)
{
  g_autofree char *before = NULL, *after = NULL;

  if (split_marker (interval_string (picker->model.freq, picker->model.interval), "%d", &before, &after))
    {
      gtk_label_set_label (GTK_LABEL (picker->interval_pre), before);
      gtk_label_set_label (GTK_LABEL (picker->interval_post), after);
    }
}

/* updateEndCountText: [n] "events" */
static void
update_end_count_text (Picker *picker)
{
  g_autofree char *before = NULL, *after = NULL;

  /* a translation with nothing before the count: Etar logs it and leaves the text */
  if (split_marker (end_count_string (picker->model.end_count), "%d", &before, &after) && *before)
    gtk_label_set_label (GTK_LABEL (picker->post_end_count), after);
}

/* the end spinner's own text (EndSpinnerAdapter.getView): "Forever", "Until", "For" */
static void
update_end_label (Picker *picker)
{
  g_autofree char *before = NULL, *after = NULL;
  const char *date = _("Until %s");
  gboolean form;

  /* mUseFormStrings: a translation that puts nothing before the date or the count shows the whole labels */
  form = !split_marker (date, "%s", &before, NULL) || !*before;
  g_clear_pointer (&before, g_free);
  form = form || !split_marker (end_count_string (1), "%d", &before, NULL) || !*before;
  g_clear_pointer (&before, g_free);
  picker->hide_post_end_count = FALSE;
  switch (picker->model.end)
    {
    case CAL_REPEAT_END_BY_DATE:
      if (form)
        gtk_label_set_label (GTK_LABEL (picker->end_label), _("Until a date"));
      else if (split_marker (date, "%s", &before, NULL))
        gtk_label_set_label (GTK_LABEL (picker->end_label), before);
      break;
    case CAL_REPEAT_END_BY_COUNT:
      if (form)
        {
          gtk_label_set_label (GTK_LABEL (picker->end_label), _("For a number of events"));
          /* and no " events" after the count */
          picker->hide_post_end_count = TRUE;
        }
      else if (split_marker (end_count_string (picker->model.end_count), "%d", &before, &after))
        {
          gtk_label_set_label (GTK_LABEL (picker->end_label), before);
          gtk_label_set_label (GTK_LABEL (picker->post_end_count), after);
        }
      break;
    default:
      gtk_label_set_label (GTK_LABEL (picker->end_label), _("Forever"));
      break;
    }
}

/* updateDoneButtonState */
static void
update_done_state (Picker *picker)
{
  gboolean enabled = TRUE;

  if (picker->model.repeats)
    {
      if (text_empty (picker->interval))
        enabled = FALSE;
      else if (gtk_widget_get_visible (picker->end_count_field) && text_empty (picker->end_count))
        enabled = FALSE;
      else if (picker->model.freq == CAL_REPEAT_WEEKLY)
        {
          enabled = FALSE;
          for (int i = 0; i < 7; i++)
            enabled = enabled || picker->model.weekly_by_day_of_week[i];
        }
    }
  gtk_widget_set_sensitive (picker->done_button, enabled);
}

/* togglePickerOptions: with the switch off everything but it is greyed */
static void
toggle_picker_options (Picker *picker)
{
  gtk_widget_set_sensitive (picker->freq, picker->model.repeats);
  gtk_widget_set_sensitive (picker->options, picker->model.repeats);
  for (int i = 0; i < 7; i++)
    if (picker->days[i])
      gtk_widget_queue_draw (picker->days[i]);
  update_done_state (picker);
}

static void
set_radio (GtkWidget *row, gboolean checked)
{
  GtkWidget *box = neo_pressable_get_child (NEO_PRESSABLE (row));

  gtk_image_set_from_icon_name (GTK_IMAGE (gtk_widget_get_first_child (box)),
                                checked ? "cal-radio-on-symbolic" : "cal-radio-off-symbolic");
  if (checked)
    gtk_widget_add_css_class (row, "checked");
  else
    gtk_widget_remove_css_class (row, "checked");
}

static void
update_dialog (Picker *picker)
{
  CalRecurrenceModel *model = &picker->model;

  set_number (picker, picker->interval, model->interval);
  cal_spinner_set_selected (CAL_SPINNER (picker->freq), model->freq);
  gtk_widget_set_visible (picker->week_group, model->freq == CAL_REPEAT_WEEKLY);
  gtk_widget_set_visible (picker->week_group2, model->freq == CAL_REPEAT_WEEKLY
                                               && gtk_widget_get_first_child (picker->week_group2));
  gtk_widget_set_visible (picker->month_group, model->freq == CAL_REPEAT_MONTHLY);
  if (model->freq == CAL_REPEAT_WEEKLY)
    for (int i = 0; i < 7; i++)
      {
        gtk_widget_queue_draw (picker->days[i]);
        gtk_accessible_update_state (GTK_ACCESSIBLE (picker->days[i]), GTK_ACCESSIBLE_STATE_PRESSED,
                                     model->weekly_by_day_of_week[i] ? GTK_ACCESSIBLE_TRISTATE_TRUE
                                                                     : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
      }
  if (model->freq == CAL_REPEAT_MONTHLY)
    {
      set_radio (picker->by_month_day, model->monthly_repeat == CAL_REPEAT_MONTHLY_BY_DATE);
      set_radio (picker->by_nth, model->monthly_repeat == CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK);
    }
  update_interval_text (picker);
  update_done_state (picker);

  /* the end */
  update_end_label (picker);
  gtk_widget_set_visible (picker->end_count_field, model->end == CAL_REPEAT_END_BY_COUNT);
  gtk_widget_set_visible (picker->post_end_count, model->end == CAL_REPEAT_END_BY_COUNT
                                                  && !picker->hide_post_end_count);
  gtk_widget_set_visible (picker->end_date, model->end == CAL_REPEAT_END_BY_DATE);
  if (model->end == CAL_REPEAT_END_BY_DATE)
    {
      g_autoptr(GDateTime) date = time_at (model->end_day, 12 * 3600, NULL);
      /* FORMAT_NUMERIC_DATE, as the editor's "; until 12/31/2026" */
      g_autofree char *text = cal_format_skeleton (date, "yMd");

      gtk_label_set_label (GTK_LABEL (picker->end_date), text);
    }
  else if (model->end == CAL_REPEAT_END_BY_COUNT)
    set_number (picker, picker->end_count, model->end_count);
  update_done_state (picker);
}

/* --- the controls */

static void
on_switch (GObject *object, GParamSpec *pspec, Picker *picker)
{
  picker->model.repeats = cal_material_switch_get_active (CAL_MATERIAL_SWITCH (object));
  toggle_picker_options (picker);
}

static void
on_freq (CalSpinner *spinner, Picker *picker)
{
  picker->model.freq = cal_spinner_get_selected (spinner);
  update_dialog (picker);
}

/* minMaxTextWatcher: the number typed, @def for none, held within [min, max] (the text set to the bound) */
static int
watch_number (Picker *picker, GtkWidget *text, int min, int def, int max)
{
  int value = text_empty (text) ? def : text_value (text);

  if (value < min || value > max)
    {
      value = CLAMP (value, min, max);
      set_number (picker, text, value);
    }
  update_done_state (picker);
  return value;
}

static void
on_interval_changed (GtkEditable *text, Picker *picker)
{
  int value;

  if (picker->updating)
    return;
  value = watch_number (picker, picker->interval, 1, INTERVAL_DEFAULT, CAL_REPEAT_INTERVAL_MAX);
  if (!text_empty (picker->interval))
    {
      picker->model.interval = value;
      update_interval_text (picker);
    }
}

static void
on_count_changed (GtkEditable *text, Picker *picker)
{
  int value;

  if (picker->updating)
    return;
  value = watch_number (picker, picker->end_count, 1, CAL_REPEAT_COUNT_DEFAULT, CAL_REPEAT_COUNT_MAX);
  if (picker->model.end_count != value)
    {
      picker->model.end_count = value;
      update_end_count_text (picker);
    }
}

/* onItemSelected for the end spinner */
static void
on_end_chosen (int index, gpointer data)
{
  Picker *picker = data;

  picker->model.end = index;
  if (index == CAL_REPEAT_END_BY_COUNT)
    {
      picker->model.end_count = CLAMP (picker->model.end_count, 1, CAL_REPEAT_COUNT_MAX);
      update_end_count_text (picker);
    }
  update_dialog (picker);
}

static void
on_end_spinner (NeoPressable *pressable, Picker *picker)
{
  /* the drop-down's rows are the long forms (getDropDownView), as wide as the widest */
  const char *labels[] = { _("Forever"), _("Until a date"), _("For a number of events") };
  double width = gtk_widget_get_width (GTK_WIDGET (pressable));

  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout (picker->end_label, labels[i]);
      int text_width;

      pango_layout_get_pixel_size (layout, &text_width, NULL);
      /* the popup's 16 sp rows, 16 dp in at each side, from this 15 sp text */
      width = MAX (width, text_width * 16.0 / 15.0 + 2 * 16 + 1);
    }
  cal_popup_show (GTK_WIDGET (pressable), labels, G_N_ELEMENTS (labels), picker->model.end, width, FALSE,
                  on_end_chosen, picker);
}

static void
on_end_date_chosen (int day, gpointer data)
{
  Picker *picker = data;

  /* onDateSet: the day changes, the time of day stays */
  picker->model.end_day = day;
  update_dialog (picker);
}

static void
on_end_date_pressed (GtkGestureClick *click, int n_press, double x, double y, Picker *picker)
{
  if (!gtk_widget_is_sensitive (picker->end_date))
    return;
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  /* Etar's DatePickerDialog is the platform one, without a title; this app's date dialog is MaterialDatePicker,
   * whose own title is "Select date" */
  cal_date_picker_show (picker->end_date, _("Select date"), picker->model.end_day, cal_week_start (-1),
                        on_end_date_chosen, picker);
}

static void
on_month_radio (NeoPressable *row, Picker *picker)
{
  picker->model.monthly_repeat = GTK_WIDGET (row) == picker->by_nth ? CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK
                                                                    : CAL_REPEAT_MONTHLY_BY_DATE;
  update_dialog (picker);
}

static void
on_day (NeoPressable *pressable, Picker *picker)
{
  int weekday = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (pressable), "cal-weekday"));

  picker->model.weekly_by_day_of_week[weekday] = !picker->model.weekly_by_day_of_week[weekday];
  update_dialog (picker);
}

static void
on_done (NeoPressable *button, Picker *picker)
{
  g_autofree char *rrule = NULL;

  if (!gtk_widget_is_sensitive (picker->done_button))
    return;
  /* the rule as it opened, untouched: answered as it was written, rather than rewritten as Etar would (which
   * drops X- parts, a daily rule's BYDAY, a negative BYMONTHDAY …) */
  if (cal_recurrence_model_equal (&picker->model, &picker->initial))
    rrule = g_strdup (picker->model.repeats ? picker->rrule : NULL);
  else
    rrule = cal_recurrence_model_store (&picker->model, picker->er, picker->tzid);
  if (picker->model.repeats && !rrule)
    return;                             /* Etar throws here; Done stays */
  picker->done (rrule, picker->data);
  neo_dialog_respond (picker->dialog, NEO_RESPONSE_OK);
}

static void
on_response (NeoDialog *dialog, int response, Picker *picker)
{
  /* Done answers itself (on_done); a tap outside, Back: nothing set (onRecurrenceSet is not called) */
}

/* digits only (inputType number) */
static void
on_insert_digits (GtkEditable *editable, const char *text, int length, int *position, gpointer data)
{
  if (length < 0)
    length = (int) strlen (text);
  for (int i = 0; i < length; i++)
    if (!g_ascii_isdigit (text[i]))
      {
        g_signal_stop_emission_by_name (editable, "insert-text");
        return;
      }
}

static gboolean
select_all (gpointer data)
{
  GtkWidget *text = data;

  if (gtk_widget_has_focus (text))
    gtk_editable_select_region (GTK_EDITABLE (text), 0, -1);
  g_object_unref (text);
  return G_SOURCE_REMOVE;
}

static void
on_number_focus (GtkEventControllerFocus *focus, gpointer data)
{
  /* selectAllOnFocus: after the tap that focused it has placed the cursor */
  g_idle_add (select_all, g_object_ref (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (focus))));
}

/* an EditText for a number (ems, maxLength): the text over an underline, @text_out the text itself */
static GtkWidget *
number_field (int max_length, const char *css_class, GtkWidget **text_out)
{
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_text_new ();
  GtkEventController *focus = gtk_event_controller_focus_new ();

  gtk_text_set_max_length (GTK_TEXT (text), max_length);
  gtk_text_set_input_purpose (GTK_TEXT (text), GTK_INPUT_PURPOSE_DIGITS);
  gtk_editable_set_alignment (GTK_EDITABLE (text), 0.5);
  gtk_editable_set_width_chars (GTK_EDITABLE (text), max_length);
  gtk_editable_set_max_width_chars (GTK_EDITABLE (text), max_length);   /* else its natural width is 150 */
  gtk_widget_set_valign (text, GTK_ALIGN_BASELINE_FILL);
  g_signal_connect (text, "insert-text", G_CALLBACK (on_insert_digits), NULL);
  g_signal_connect (focus, "enter", G_CALLBACK (on_number_focus), NULL);
  gtk_widget_add_controller (text, focus);
  gtk_box_append (GTK_BOX (field), text);
  gtk_widget_add_css_class (field, "cal-recur-number");
  gtk_widget_add_css_class (field, css_class);
  gtk_widget_set_hexpand (field, FALSE);
  gtk_widget_set_valign (field, GTK_ALIGN_BASELINE_FILL);
  *text_out = text;
  return field;
}

static GtkWidget *
text_label (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_add_css_class (label, "cal-recur-text");
  return label;
}

/* WeekButton: a weekday's ToggleButton, its circle (recurrence_bubble_fill) stretched to the button as Android
 * stretches a background: filled in colorPrimary when checked, a thin ring otherwise, grey when disabled */
static void
draw_day (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Picker *picker = data;
  GtkWidget *widget = GTK_WIDGET (area);
  int weekday = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (area), "cal-weekday"));
  gboolean checked = picker->model.weekly_by_day_of_week[weekday];
  gboolean enabled = gtk_widget_is_sensitive (widget);
  GdkRGBA ring = cal_argb (enabled ? CAL_PRIMARY : BUBBLE_DISABLED);
  GdkRGBA ink = cal_argb (checked ? 0xffffffff : enabled ? BUBBLE_TEXT_NORMAL : BUBBLE_TEXT_DISABLED);
  g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout (widget,
                                                                  g_object_get_data (G_OBJECT (area), "cal-name"));
  int text_width, text_height;

  cairo_save (cr);
  cairo_translate (cr, width / 2.0, height / 2.0);
  cairo_scale (cr, width / 24.0, height / 24.0);
  cairo_arc (cr, 0, 0, BUBBLE_RADIUS, 0, 2 * G_PI);
  cairo_restore (cr);
  gdk_cairo_set_source_rgba (cr, &ring);
  if (checked)
    cairo_fill (cr);
  else
    {
      /* the vector's stroke, scaled as VectorDrawable scales it (by the smaller of its two scales) */
      cairo_set_line_width (cr, BUBBLE_STROKE * MIN (width, height) / 24.0);
      cairo_stroke (cr);
    }
  pango_layout_set_width (layout, MAX (width - 8, 1) * PANGO_SCALE);  /* 4 dp padding at each side */
  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
  pango_layout_get_pixel_size (layout, &text_width, &text_height);
  gdk_cairo_set_source_rgba (cr, &ink);
  cairo_move_to (cr, 4, (height - text_height) / 2.0);
  pango_cairo_show_layout (cr, layout);
}

static GtkWidget *
day_button (Picker *picker, int weekday)
{
  GtkWidget *area = g_object_new (GTK_TYPE_DRAWING_AREA, "accessible-role", GTK_ACCESSIBLE_ROLE_TOGGLE_BUTTON, NULL);
  GtkWidget *button = neo_pressable_new (area);
  /* DateFormatSymbols.getShortWeekdays, textAllCaps: "SUN" … (2023-01-01 was a Sunday) */
  g_autoptr(GDateTime) date = g_date_time_new_local (2023, 1, 1 + weekday, 12, 0, 0);
  g_autofree char *name = g_date_time_format (date, "%a");
  g_autofree char *full = g_date_time_format (date, "%A");

  g_object_set_data_full (G_OBJECT (area), "cal-name", g_utf8_strup (name, -1), g_free);
  g_object_set_data (G_OBJECT (area), "cal-weekday", GINT_TO_POINTER (weekday));
  g_object_set_data (G_OBJECT (button), "cal-weekday", GINT_TO_POINTER (weekday));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_day, picker, NULL);
  gtk_widget_set_hexpand (area, TRUE);
  gtk_widget_set_vexpand (area, TRUE);
  gtk_widget_add_css_class (area, "cal-recur-day-face");
  gtk_accessible_update_property (GTK_ACCESSIBLE (area), GTK_ACCESSIBLE_PROPERTY_LABEL, full, -1);
  /* a ToggleButton has no ripple: its background is the bubble */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_NONE, 0);
  gtk_widget_add_css_class (button, "cal-recur-day");
  gtk_widget_set_hexpand (button, TRUE);
  g_signal_connect (button, "clicked", G_CALLBACK (on_day), picker);
  picker->days[weekday] = area;
  return button;
}

/* a RadioButton of the monthly group: the radio, then its text (15 sp) */
static GtkWidget *
radio_row (Picker *picker, const char *text)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *mark = gtk_image_new_from_icon_name ("cal-radio-off-symbolic");
  GtkWidget *label = text_label (text);
  GtkWidget *row;

  gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
  gtk_widget_add_css_class (mark, "cal-recur-radio-mark");
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  /* wrap_content: one line when it fits, wrapped when it does not */
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 200);
  gtk_box_append (GTK_BOX (box), mark);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  gtk_widget_add_css_class (row, "cal-recur-radio");
  gtk_widget_set_halign (row, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_month_radio), picker);
  return row;
}

/* the layout's fixed size (recurrence_picker_width, _height): its child gets that, its scroll view what is left */
static void
fixed_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
               int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), orientation == GTK_ORIENTATION_HORIZONTAL
                                                                                ? "cal-width" : "cal-height"));
}

static void
fixed_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_allocate (child, width, height, -1, NULL);
}

/* the screen's size class: the dialog's size (recurrence_picker_width/_height of values, -land, -sw600dp,
 * -sw600dp-land), the card's least width, and whether the week fits in one row */
static void
dialog_geometry (GtkWidget *parent, int *width, int *height, int *card_width, gboolean *one_row)
{
  GtkRoot *root = gtk_widget_get_root (parent);
  int w = root ? gtk_widget_get_width (GTK_WIDGET (root)) : 0, h = root ? gtk_widget_get_height (GTK_WIDGET (root)) : 0;
  gboolean landscape;

  if (w <= 0 || h <= 0)
    w = 360, h = 761;
  landscape = w > h;
  if (MIN (w, h) >= 600)
    {
      *width = landscape ? 600 : 480;
      *height = 300;
    }
  else
    {
      *width = landscape ? 500 : 300;
      *height = landscape ? 281 : 340;
    }
  /* the dialog window's least width (ThemeOverlay.AppCompat.Dialog's windowMinWidthMinor 95 % of the screen held
   * upright, windowMinWidthMajor 65 % on its side) less its 16 dp insets: the layout sits at its start, the window
   * background (the card's colour here) fills the rest */
  *card_width = MAX (*width, (int) ((landscape ? 0.65 : 0.95) * w) - 2 * 16);
  *one_row = w > MIN_SCREEN_WIDTH_FOR_SINGLE_ROW_WEEK;
}

static int
first_day_of_week (void)
{
  /* Utils.getFirstDayOfWeek: the "Week starts on" setting */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "de.yesman.Calendar", TRUE)
                                             : NULL;
  g_autoptr(GSettings) settings = schema ? g_settings_new ("de.yesman.Calendar") : NULL;

  return cal_week_start (settings ? g_settings_get_int (settings, "week-start") : -1);
}

void
cal_recurrence_picker_show (GtkWidget *parent, const char *rrule, gint64 start, const char *tzid,
                            CalRecurrenceSet done, gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  const char *freqs[] = { _("REPEAT DAILY"), _("REPEAT WEEKLY"), _("REPEAT MONTHLY"), _("REPEAT YEARLY") };
  int week_start = first_day_of_week ();
  int width, height, card_width, row1, idx;
  gboolean one_row;
  GtkWidget *card, *layout, *scroll, *body, *top, *interval_group, *end_group, *box, *arrow, *bar, *divider;
  GtkGesture *click;
  g_autofree char *nth_text = NULL;

  picker->done = done;
  picker->data = data;
  picker->tzid = g_strdup (tzid);
  picker->rrule = rrule && *rrule ? g_strdup (rrule) : NULL;
  /* onCreateView: the rule's parts, else a new one with the week's start as WKST */
  picker->er = picker->rrule ? cal_recurrence_parse (picker->rrule) : NULL;
  if (!picker->er || !cal_recurrence_model_init (&picker->model, picker->er, start, tzid))
    {
      /* the editor's button is off for these (cal_recurrence_picker_can_handle) */
      g_clear_pointer (&picker->er, cal_recurrence_free);
      if (picker->rrule)
        {
          g_warning ("the repeat dialog cannot show %s", picker->rrule);
          picker_free (picker, NULL);
          return;
        }
      picker->er = cal_recurrence_new (week_start);
      cal_recurrence_model_init (&picker->model, NULL, start, tzid);
    }
  dialog_geometry (parent, &width, &height, &card_width, &one_row);

  /* the top row: the frequency, the switch 4 dp after it */
  top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (top, "cal-recur-top");
  picker->freq = cal_spinner_new ();
  cal_spinner_set_items (CAL_SPINNER (picker->freq), freqs, G_N_ELEMENTS (freqs));
  gtk_widget_set_hexpand (picker->freq, TRUE);
  gtk_widget_add_css_class (picker->freq, "cal-recur-freq");
  gtk_box_append (GTK_BOX (top), picker->freq);
  /* app:thumbTint (white on, darker grey off) left out: CalMaterialSwitch draws Material 3's thumb */
  picker->repeat_switch = cal_material_switch_new ();
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->repeat_switch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Repeat event"), -1);
  gtk_widget_set_valign (picker->repeat_switch, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (picker->repeat_switch, "cal-recur-switch");
  gtk_box_append (GTK_BOX (top), picker->repeat_switch);

  picker->options = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (picker->options, "cal-recur-options");

  /* "Every [1] week" */
  interval_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (interval_group, "cal-recur-interval");
  picker->interval_pre = text_label (NULL);
  picker->interval_post = text_label (NULL);
  gtk_widget_set_valign (picker->interval_pre, GTK_ALIGN_BASELINE_FILL);
  gtk_widget_set_valign (picker->interval_post, GTK_ALIGN_BASELINE_FILL);
  gtk_box_append (GTK_BOX (interval_group), picker->interval_pre);
  gtk_box_append (GTK_BOX (interval_group), number_field (2, "cal-recur-interval-field", &picker->interval));
  gtk_box_append (GTK_BOX (interval_group), picker->interval_post);
  gtk_box_append (GTK_BOX (picker->options), interval_group);

  /* the weekdays from the week's start: all seven in a row on a wide screen, else four over three with an
   * invisible fourth keeping the columns (LinearLayoutWithMaxWidth shares the row out) */
  picker->week_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  picker->week_group2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (picker->week_group), TRUE);
  gtk_box_set_homogeneous (GTK_BOX (picker->week_group2), TRUE);
  gtk_widget_add_css_class (picker->week_group, "cal-recur-week");
  gtk_widget_add_css_class (picker->week_group2, "cal-recur-week2");
  row1 = one_row ? 7 : 4;
  idx = week_start;
  for (int i = 0; i < 7; i++, idx = (idx + 1) % 7)
    gtk_box_append (GTK_BOX (i < row1 ? picker->week_group : picker->week_group2), day_button (picker, idx));
  if (!one_row)
    {
      GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

      gtk_widget_add_css_class (spacer, "cal-recur-day");
      gtk_box_append (GTK_BOX (picker->week_group2), spacer);
    }
  gtk_box_append (GTK_BOX (picker->options), picker->week_group);
  gtk_box_append (GTK_BOX (picker->options), picker->week_group2);

  /* monthly: by the date, or by the nth weekday (its text set once, from the model) */
  picker->month_group = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  picker->by_month_day = radio_row (picker, _("on the same day each month"));
  nth_text = g_strdup (cal_format_nth_weekday (picker->model.monthly_by_day_of_week,
                                               (picker->model.monthly_by_nth_day_of_week < 0 ? FIFTH_WEEK_IN_A_MONTH
                                                : picker->model.monthly_by_nth_day_of_week) - 1));
  picker->by_nth = radio_row (picker, nth_text);
  gtk_box_append (GTK_BOX (picker->month_group), picker->by_month_day);
  gtk_box_append (GTK_BOX (picker->month_group), picker->by_nth);
  gtk_box_append (GTK_BOX (picker->options), picker->month_group);

  /* the end: "Forever ▾", "Until ▾ 12/10/2026", "For ▾ [5] events" */
  end_group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (end_group, "cal-recur-end");
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  picker->end_label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (picker->end_label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (picker->end_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (picker->end_label, "cal-spinner-text");
  arrow = gtk_image_new_from_icon_name ("cal-drop-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (arrow), 24);
  gtk_widget_add_css_class (arrow, "cal-spinner-arrow");
  gtk_box_append (GTK_BOX (box), picker->end_label);
  gtk_box_append (GTK_BOX (box), arrow);
  /* not a CalSpinner: Etar's shows a short form ("Until") of the row chosen ("Until a date") */
  picker->end_spinner = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (picker->end_spinner), NEO_RIPPLE_BOUNDED, 4);
  gtk_widget_add_css_class (picker->end_spinner, "cal-recur-end-spinner");
  gtk_widget_set_valign (picker->end_spinner, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->end_spinner), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("End"), -1);
  g_signal_connect (picker->end_spinner, "clicked", G_CALLBACK (on_end_spinner), picker);
  gtk_box_append (GTK_BOX (end_group), picker->end_spinner);
  picker->end_count_field = number_field (3, "cal-recur-count-field", &picker->end_count);
  gtk_widget_set_valign (picker->end_count_field, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (end_group), picker->end_count_field);
  picker->post_end_count = text_label (NULL);
  gtk_label_set_single_line_mode (GTK_LABEL (picker->post_end_count), TRUE);
  gtk_widget_set_valign (picker->post_end_count, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (end_group), picker->post_end_count);
  picker->end_date = gtk_label_new (NULL);
  gtk_widget_add_css_class (picker->end_date, "cal-recur-end-date");
  gtk_label_set_yalign (GTK_LABEL (picker->end_date), 1);
  gtk_widget_set_valign (picker->end_date, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (picker->end_date), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  _("change end date"), -1);
  click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_end_date_pressed), picker);
  gtk_widget_add_controller (picker->end_date, GTK_EVENT_CONTROLLER (click));
  gtk_box_append (GTK_BOX (end_group), picker->end_date);
  gtk_box_append (GTK_BOX (picker->options), end_group);

  /* focusable before its children (descendantFocusability beforeDescendants): the dialog opens with no field
   * focused, and no keyboard */
  body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_focusable (body, TRUE);
  gtk_box_append (GTK_BOX (body), top);
  gtk_box_append (GTK_BOX (body), picker->options);
  scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), body);
  gtk_widget_set_vexpand (scroll, TRUE);

  /* a line, then Done across the bottom */
  divider = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (divider, "cal-recur-divider");
  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (bar, "cal-recur-bar");
  picker->done_button = neo_text_button_new (_("Done"));
  gtk_widget_add_css_class (picker->done_button, "cal-recur-done");
  gtk_widget_set_hexpand (picker->done_button, TRUE);
  g_signal_connect (picker->done_button, "clicked", G_CALLBACK (on_done), picker);
  gtk_box_append (GTK_BOX (bar), picker->done_button);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), scroll);
  gtk_box_append (GTK_BOX (box), divider);
  gtk_box_append (GTK_BOX (box), bar);
  layout = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_layout_manager (layout, gtk_custom_layout_new (NULL, fixed_measure, fixed_allocate));
  g_object_set_data (G_OBJECT (layout), "cal-width", GINT_TO_POINTER (width));
  g_object_set_data (G_OBJECT (layout), "cal-height", GINT_TO_POINTER (height));
  gtk_widget_add_css_class (layout, "cal-recur");
  gtk_widget_set_halign (layout, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (layout), box);
  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (card, card_width, -1);
  gtk_box_append (GTK_BOX (card), layout);

  /* the state: the switch, the texts, what shows */
  cal_material_switch_set_active (CAL_MATERIAL_SWITCH (picker->repeat_switch), picker->model.repeats);
  update_end_count_text (picker);
  update_dialog (picker);
  toggle_picker_options (picker);
  /* the fields hold at most 2 and 3 digits (maxLength): a longer number in the rule shows cut, as Etar's does; the
   * count is then held to 730 (its watcher runs on the text set) */
  if (picker->model.interval > CAL_REPEAT_INTERVAL_MAX)
    picker->model.interval = text_value (picker->interval);
  if (picker->model.end == CAL_REPEAT_END_BY_COUNT && picker->model.end_count != text_value (picker->end_count))
    {
      picker->model.end_count = CLAMP (text_value (picker->end_count), 1, CAL_REPEAT_COUNT_MAX);
      set_number (picker, picker->end_count, picker->model.end_count);
      update_end_count_text (picker);
      update_end_label (picker);
    }
  picker->initial = picker->model;
  g_signal_connect (picker->repeat_switch, "notify::active", G_CALLBACK (on_switch), picker);
  g_signal_connect (picker->freq, "changed", G_CALLBACK (on_freq), picker);
  g_signal_connect (picker->interval, "changed", G_CALLBACK (on_interval_changed), picker);
  g_signal_connect (picker->end_count, "changed", G_CALLBACK (on_count_changed), picker);

  /* no title (FEATURE_NO_TITLE), no buttons of the dialog's own */
  picker->dialog = neo_dialog_new (NULL);
  neo_dialog_set_flush (picker->dialog, TRUE);
  neo_dialog_set_content (picker->dialog, card);
  gtk_widget_add_css_class (gtk_widget_get_parent (card), "cal-recur-card");
  g_signal_connect_data (picker->dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  neo_dialog_present (picker->dialog, parent);
  gtk_widget_grab_focus (body);
  /* and nothing selected in them, as the dialog's focus on the first field leaves it */
  gtk_editable_select_region (GTK_EDITABLE (picker->interval), 0, 0);
  gtk_editable_select_region (GTK_EDITABLE (picker->end_count), 0, 0);
}

#endif /* CAL_RECURRENCE_NO_UI */
