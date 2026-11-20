/* The calendar's dates and times as text (cal-format.h). */
#include "cal-format.h"
#include <time.h>
#include "cal-model.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <langinfo.h>
#include <locale.h>
#include <unicode/udat.h>
#include <unicode/udatpg.h>
#include <unicode/udateintervalformat.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ICU's interval separator: an en dash between thin spaces */
#define RANGE " – "

gboolean
cal_format_uses_24h (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                              "org.gnome.desktop.interface", TRUE);
  g_autoptr(GSettings) settings = NULL;
  g_autofree char *format = NULL;

  /* no desktop schema (a test): 24 hours, as the phone */
  if (!schema)
    return TRUE;
  g_settings_schema_unref (schema);
  settings = g_settings_new ("org.gnome.desktop.interface");
  format = g_settings_get_string (settings, "clock-format");
  return g_strcmp0 (format, "12h") != 0;
}

/* the ICU locale of a POSIX category ("it_IT.UTF-8@euro" is it_IT; the POSIX locale is ICU's en_US, as glibc's C) */
static const char *
icu_locale (int category, char *id, size_t size)
{
  const char *posix = setlocale (category, NULL);
  size_t n;

  if (!posix || g_str_equal (posix, "C") || g_str_has_prefix (posix, "C.") || g_str_equal (posix, "POSIX"))
    return "en_US";
  n = strcspn (posix, ".@");
  if (n >= size)
    return "en_US";
  memcpy (id, posix, n);
  id[n] = 0;
  return id;
}

const char *
cal_format_date_locale (void)
{
  static char id[ULOC_FULLNAME_CAPACITY];

  return icu_locale (LC_TIME, id, sizeof id);
}

/* the language of the sentences (LC_MESSAGES): a weekday inside "Weekly on Sunday" is in it */
static const char *
messages_locale (void)
{
  static char id[ULOC_FULLNAME_CAPACITY];

  return icu_locale (LC_MESSAGES, id, sizeof id);
}

/* an ICU format for the LC_TIME locale, a skeleton and a zone, made once (the views format at every draw) */
static UDateFormat *
skeleton_format (const char *locale, const char *skeleton)
{
  static GHashTable *formats;
  g_autofree char *key = g_strjoin ("|", locale, skeleton, NULL);
  UDateFormat *format;
  UErrorCode status = U_ZERO_ERROR;
  UDateTimePatternGenerator *generator;
  UChar wanted[32], pattern[128], utc[] = { 'U', 'T', 'C', 0 };
  int32_t length;

  if (!formats)
    formats = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) udat_close);
  if ((format = g_hash_table_lookup (formats, key)))
    return format;
  if (strlen (skeleton) >= G_N_ELEMENTS (wanted))
    return NULL;
  generator = udatpg_open (locale, &status);
  if (U_FAILURE (status))
    return NULL;
  u_uastrcpy (wanted, skeleton);
  length = udatpg_getBestPattern (generator, wanted, -1, pattern, G_N_ELEMENTS (pattern), &status);
  udatpg_close (generator);
  if (U_FAILURE (status))
    return NULL;
  /* in UTC: the caller's wall clock is shifted there (format_skeleton), whatever zone it is in */
  format = udat_open (UDAT_PATTERN, UDAT_PATTERN, locale, utc, -1, pattern, length, &status);
  if (U_FAILURE (status))
    return NULL;
  g_hash_table_insert (formats, g_steal_pointer (&key), format);
  return format;
}

/* a day or a time in the LC_TIME locale's own order for an ICU skeleton (DateFormat.getBestDateTimePattern) */
static char *
format_skeleton_in (GDateTime *time, const char *skeleton, const char *locale)
{
  UDateFormat *format = skeleton_format (locale, skeleton);
  /* the wall clock of the time's own zone, as a UTC instant */
  UDate wall = ((double) g_date_time_to_unix (time) + g_date_time_get_utc_offset (time) / G_TIME_SPAN_SECOND) * 1000.0;
  UErrorCode status = U_ZERO_ERROR;
  UChar text[256];
  int32_t length;
  char *out = NULL;

  if (!format)
    return g_date_time_format (time, "%x");
  length = udat_format (format, wall, text, G_N_ELEMENTS (text), NULL, &status);
  if (U_SUCCESS (status))
    out = g_utf16_to_utf8 ((const gunichar2 *) text, length, NULL, NULL, NULL);
  return out ? out : g_date_time_format (time, "%x");
}

char *
cal_format_skeleton (GDateTime *time, const char *skeleton)
{
  return format_skeleton_in (time, skeleton, cal_format_date_locale ());
}

static GDateTime *local_day (int day);
static char *weekday_name (int weekday, gboolean abbreviated);

/* two days as one interval in the LC_TIME locale ("November 9 – 11", "9–11 novembre"; DateUtils.formatDateRange) */
static char *
format_day_interval (int first, int last, const char *skeleton)
{
  UErrorCode status = U_ZERO_ERROR;
  UChar wanted[32], text[256], utc[] = { 'U', 'T', 'C', 0 };
  g_autoptr(GDateTime) a = local_day (first), b = local_day (last);
  UDateIntervalFormat *format;
  int32_t length;
  char *out = NULL;

  u_uastrcpy (wanted, skeleton);
  format = udtitvfmt_open (cal_format_date_locale (), wanted, -1, utc, -1, &status);
  if (U_FAILURE (status))
    return NULL;
  /* the wall clocks as UTC instants, as format_skeleton_in */
  length = udtitvfmt_format (format,
                             ((double) g_date_time_to_unix (a) + g_date_time_get_utc_offset (a) / G_TIME_SPAN_SECOND) * 1000.0,
                             ((double) g_date_time_to_unix (b) + g_date_time_get_utc_offset (b) / G_TIME_SPAN_SECOND) * 1000.0,
                             text, G_N_ELEMENTS (text), NULL, &status);
  if (U_SUCCESS (status))
    out = g_utf16_to_utf8 ((const gunichar2 *) text, length, NULL, NULL, NULL);
  udtitvfmt_close (format);
  return out;
}

static GDateTime *
local_time (gint64 time)
{
  return g_date_time_new_from_unix_local (time);
}

static GDateTime *
local_day (int day)
{
  return g_date_time_new_from_unix_local (cal_day_start (day));
}

char *
cal_format_time (gint64 time)
{
  g_autoptr(GDateTime) local = local_time (time);

  /* the 12-hour form drops the hour's leading zero, as Android's "h:mm a" */
  return g_date_time_format (local, cal_format_uses_24h () ? "%H:%M" : "%-I:%M %p");
}

char *
cal_format_time_range (gint64 start, gint64 end)
{
  g_autofree char *a = cal_format_time (start), *b = cal_format_time (end);

  if (end <= start)
    return g_steal_pointer (&a);
  return g_strconcat (a, RANGE, b, NULL);
}

static char *
format_day (int day, const char *skeleton)
{
  g_autoptr(GDateTime) local = local_day (day);

  return cal_format_skeleton (local, skeleton);
}

char *
cal_format_month_year (int day)
{
  /* the toolbar of the Week and Month views, "November 2026" */
  return format_day (day, "MMMMy");
}

char *
cal_format_full_date (int day)
{
  /* "November 7, 2026" (DateUtils FORMAT_SHOW_DATE | FORMAT_SHOW_YEAR) */
  return format_day (day, "MMMMdy");
}

char *
cal_format_month_day (int day)
{
  /* "November 7" */
  return format_day (day, "MMMMd");
}

char *
cal_format_weekday (int day)
{
  return format_day (day, "EEEE");
}

char *
cal_format_weekday_short (int day)
{
  return format_day (day, "EEE");
}

char *
cal_format_editor_date (int day)
{
  /* "Sun, Nov 1, 2026" (SHOW_DATE | SHOW_YEAR | SHOW_WEEKDAY | ABBREV_MONTH | ABBREV_WEEKDAY) */
  return format_day (day, "EEEMMMdy");
}

char *
cal_format_picker_date (int day)
{
  /* the date picker's header, "Nov 1, 2026" */
  return format_day (day, "MMMdy");
}

char *
cal_format_relative_weekday (int day, int today)
{
  g_autoptr(GDateTime) date = local_day (day);
  /* alone, the weekday is a date (LC_TIME); inside "Today, %s" it is a word of the sentence (LC_MESSAGES) */
  g_autofree char *weekday = day >= today - 1 && day <= today + 1
                             ? weekday_name (g_date_time_get_day_of_week (date) % 7, FALSE)
                             : cal_format_weekday (day);

  /* agenda_today, agenda_yesterday, agenda_tomorrow (S:329-333) */
  if (day == today)
    return g_strdup_printf (_("Today, %s"), weekday);
  if (day == today - 1)
    return g_strdup_printf (_("Yesterday, %s"), weekday);
  if (day == today + 1)
    return g_strdup_printf (_("Tomorrow, %s"), weekday);
  return g_steal_pointer (&weekday);
}

/* "Thursday, April 12" (FORMAT_SHOW_DATE | FORMAT_SHOW_WEEKDAY: the year only when it is not this one) */
static char *
weekday_date (int day, int today)
{
  g_autoptr(GDateTime) a = local_day (day), b = local_day (today);

  /* "Thursday, April 12" */
  if (g_date_time_get_year (a) == g_date_time_get_year (b))
    return cal_format_skeleton (a, "EEEEMMMMd");
  /* "Thursday, April 12, 2027" */
  return cal_format_skeleton (a, "EEEEMMMMdy");
}

char *
cal_format_event_when (gint64 start, gint64 end, gboolean all_day, gint64 now)
{
  int today = cal_day_number (now), first = cal_day_number (start);
  int last = end > start ? cal_day_number (end - 1) : first;

  if (all_day)
    {
      /* single day: "Today" / "Tomorrow" when it is, else the date; several: the range of dates */
      if (first == last && first == today)
        return g_strdup (_("Today"));
      if (first == last && first == today + 1)
        return g_strdup (_("Tomorrow"));
      if (first == last)
        return weekday_date (first, today);
      {
        g_autofree char *a = weekday_date (first, today), *b = weekday_date (last, today);

        return g_strconcat (a, RANGE, b, NULL);
      }
    }
  /* a timed event within one day ("singleDayEvent"): "Today at 10:00 – 11:00", "Thursday, April 12, 10:00 – …" */
  if (first == last)
    {
      g_autofree char *times = cal_format_time_range (start, end);

      if (first == today)
        return g_strdup_printf (_("Today at %s"), times);
      if (first == today + 1)
        return g_strdup_printf (_("Tomorrow at %s"), times);
      {
        g_autofree char *date = weekday_date (first, today);

        /* date_time_fmt: "%1$s, %2$s" */
        return g_strdup_printf (_("%s, %s"), date, times);
      }
    }
  /* over several days: "Fri, Apr 6, 17:00 – Sun, Apr 8, 18:00" (ABBREV_MONTH | ABBREV_WEEKDAY) */
  {
    g_autoptr(GDateTime) a = local_time (start), b = local_time (end);
  /* the date of a multi-day event's start or end, "Fri, Apr 6" */
    g_autofree char *da = cal_format_skeleton (a, "EEEMMMd");
    g_autofree char *db = cal_format_skeleton (b, "EEEMMMd");
    g_autofree char *ta = cal_format_time (start), *tb = cal_format_time (end);

    return g_strdup_printf ("%s, %s" RANGE "%s, %s", da, ta, db, tb);
  }
}

char *
cal_format_agenda_when (gint64 start, gint64 end, gboolean all_day)
{
  if (all_day)
    {
      int first = cal_day_number (start), last = end > start ? cal_day_number (end - 1) : first;
      char *range;

      if (first == last)
        return cal_format_month_day (first);
      if ((range = format_day_interval (first, last, "MMMMd")))
        return range;
      {
        g_autofree char *a = cal_format_month_day (first), *b = cal_format_month_day (last);

        return g_strconcat (a, RANGE, b, NULL);
      }
    }
  if (cal_day_number (start) != cal_day_number (end > start ? end - 1 : start))
    {
      /* over midnight: DateUtils adds the dates, "Nov 9, 22:00 – Nov 10, 02:00" */
      g_autoptr(GDateTime) a = local_time (start), b = local_time (end);
  /* "Nov 9" */
      g_autofree char *da = cal_format_skeleton (a, "MMMd");
      g_autofree char *db = cal_format_skeleton (b, "MMMd");
      g_autofree char *ta = cal_format_time (start), *tb = cal_format_time (end);

      return g_strdup_printf ("%s, %s" RANGE "%s, %s", da, ta, db, tb);
    }
  return cal_format_time_range (start, end);
}

char *
cal_format_reminder_text (gint64 start, gboolean all_day, const char *location, gint64 now)
{
  int today = cal_day_number (now), day = cal_day_number (start);
  GString *text = g_string_new (NULL);
  g_autofree char *trimmed = location ? g_strstrip (g_strdup (location)) : NULL;

  /* FORMAT_ABBREV_ALL; the time unless all-day; the date unless today or tomorrow */
  if (day < today || day > today + 1)
    {
      g_autoptr(GDateTime) local = local_time (start);
  /* "Nov 9" */
      g_autofree char *date = cal_format_skeleton (local, "MMMd");

      g_string_append (text, date);
      if (!all_day)
        g_string_append (text, ", ");
    }
  if (!all_day)
    {
      g_autofree char *time = cal_format_time (start);

      g_string_append (text, time);
    }
  if (day == today + 1)
    {
      if (text->len)
        g_string_append (text, ", ");
      g_string_append (text, _("Tomorrow"));
    }
  if (trimmed && *trimmed)
    {
      if (text->len)
        g_string_append (text, ", ");
      g_string_append (text, trimmed);
    }
  return g_string_free (text, FALSE);
}

char *
cal_format_reminder_minutes (int minutes)
{
  /* EventViewUtils.constructReminderLabel: weeks, days or hours when it divides evenly, else minutes */
  if (minutes != 0 && minutes % (7 * 24 * 60) == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d week", "%d weeks", minutes / (7 * 24 * 60)),
                            minutes / (7 * 24 * 60));
  if (minutes != 0 && minutes % (24 * 60) == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d day", "%d days", minutes / (24 * 60)), minutes / (24 * 60));
  if (minutes != 0 && minutes % 60 == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d hour", "%d hours", minutes / 60), minutes / 60);
  return g_strdup_printf (g_dngettext (NULL, "%d minute", "%d minutes", minutes), minutes);
}


int
cal_week_start (int setting)
{
  /* Etar's values are Calendar.SUNDAY 1 … SATURDAY 7 */
  if (setting >= 1 && setting <= 7)
    return setting - 1;
#ifdef _NL_TIME_FIRST_WEEKDAY
  {
    /* GtkCalendar's reading of glibc's locale: the week's origin (a Sunday or a Monday) and the first weekday
     * counted from it */
    union { unsigned int word; char *string; } info;
    int origin = 0, first;

    info.string = nl_langinfo (_NL_TIME_FIRST_WEEKDAY);
    first = info.string[0];
    info.string = nl_langinfo (_NL_TIME_WEEK_1STDAY);
    if (info.word == 19971201)
      origin = 1;
    return ((origin + first - 1) % 7 + 7) % 7;
  }
#else
  return 0;
#endif
}

/* --- repeat rules in words */

static const char *byday_names[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

static char *
weekday_name (int weekday, gboolean abbreviated)
{
  /* 2023-01-01 was a Sunday */
  g_autoptr(GDateTime) date = g_date_time_new_local (2023, 1, 1 + weekday, 12, 0, 0);

  return format_skeleton_in (date, abbreviated ? "EEE" : "EEEE", messages_locale ());
}

/* repeat_by_nth_sun … _sat: "on every [first … fourth, last] [weekday]", one string each for the languages whose
 * words change with the weekday's gender */
static const char *const repeat_by_nth[7][5] = {
  { N_("on every first Sunday"), N_("on every second Sunday"), N_("on every third Sunday"),
    N_("on every fourth Sunday"), N_("on every last Sunday") },
  { N_("on every first Monday"), N_("on every second Monday"), N_("on every third Monday"),
    N_("on every fourth Monday"), N_("on every last Monday") },
  { N_("on every first Tuesday"), N_("on every second Tuesday"), N_("on every third Tuesday"),
    N_("on every fourth Tuesday"), N_("on every last Tuesday") },
  { N_("on every first Wednesday"), N_("on every second Wednesday"), N_("on every third Wednesday"),
    N_("on every fourth Wednesday"), N_("on every last Wednesday") },
  { N_("on every first Thursday"), N_("on every second Thursday"), N_("on every third Thursday"),
    N_("on every fourth Thursday"), N_("on every last Thursday") },
  { N_("on every first Friday"), N_("on every second Friday"), N_("on every third Friday"),
    N_("on every fourth Friday"), N_("on every last Friday") },
  { N_("on every first Saturday"), N_("on every second Saturday"), N_("on every third Saturday"),
    N_("on every fourth Saturday"), N_("on every last Saturday") },
};

const char *
cal_format_nth_weekday (int weekday, int nth)
{
  return _(repeat_by_nth[CLAMP (weekday, 0, 6)][CLAMP (nth, 0, 4)]);
}

char *
cal_format_rrule (const char *rrule, gint64 start)
{
  g_auto(GStrv) parts = NULL;
  const char *freq = NULL, *until = NULL;
  int interval = 1, count = 0, n_days = 0, days[7] = { 0 };
  gboolean nth_byday = FALSE;
  g_autoptr(GString) end = g_string_new (NULL);
  g_autoptr(GDateTime) first = g_date_time_new_from_unix_local (start);
  char *text = NULL;

  if (!rrule || !*rrule)
    return NULL;
  parts = g_strsplit (rrule, ";", -1);
  for (int i = 0; parts[i]; i++)
    {
      char *value = strchr (parts[i], '=');

      if (!value)
        continue;
      *value++ = '\0';
      if (g_str_equal (parts[i], "FREQ"))
        freq = value;
      else if (g_str_equal (parts[i], "INTERVAL"))
        interval = MAX (1, atoi (value));
      else if (g_str_equal (parts[i], "COUNT"))
        count = atoi (value);
      else if (g_str_equal (parts[i], "UNTIL"))
        until = value;
      else if (g_str_equal (parts[i], "BYDAY"))
        {
          g_auto(GStrv) list = g_strsplit (value, ",", -1);

          for (int j = 0; list[j] && n_days < 7; j++)
            {
              const char *code = list[j];

              /* "1MO", "-1FR": the nth weekday of a month */
              if (*code == '+' || *code == '-' || g_ascii_isdigit (*code))
                nth_byday = TRUE;
              while (*code == '+' || *code == '-' || g_ascii_isdigit (*code))
                code++;
              for (int k = 0; k < 7; k++)
                if (g_str_equal (code, byday_names[k]))
                  days[n_days++] = k;
            }
        }
    }
  if (!freq)
    return NULL;

  /* the end: "; until 12/31/2026" (FORMAT_NUMERIC_DATE), "; for 5 times" */
  if (until)
    {
      int y = 0, m = 0, d = 0, h = 0, mi = 0, sec = 0;
      g_autoptr(GDateTime) date = NULL;

      /* Time.parse, then formatDateTime in the shown zone: a UTC end ("…T225959Z") is the local date it falls on */
      if (strlen (until) == 16 && until[15] == 'Z'
          && sscanf (until, "%4d%2d%2dT%2d%2d%2d", &y, &m, &d, &h, &mi, &sec) == 6)
        {
          g_autoptr(GDateTime) utc = g_date_time_new_utc (y, m, d, h, mi, sec);

          date = utc ? g_date_time_to_local (utc) : NULL;
        }
      else if (sscanf (until, "%4d%2d%2d", &y, &m, &d) == 3)
        date = g_date_time_new_local (y, m, d, 12, 0, 0);

      if (date)
        {
  /* a repeat rule's end date, numeric (FORMAT_NUMERIC_DATE) */
          g_autofree char *text_date = cal_format_skeleton (date, "yMd");

          g_string_append_printf (end, _("; until %s"), text_date);
        }
    }
  if (count > 0)
    g_string_append_printf (end, g_dngettext (NULL, "; for one time", "; for %d times", count), count);

  if (g_str_equal (freq, "DAILY"))
    text = g_strdup_printf (g_dngettext (NULL, "Daily", "Every %d days", interval), interval);
  else if (g_str_equal (freq, "WEEKLY"))
    {
      g_autoptr(GString) names = g_string_new (NULL);
      gboolean weekdays = n_days == 5 && interval == 1;

      for (int k = 0; k < n_days && weekdays; k++)
        weekdays = days[k] == k + 1;
      if (weekdays)
        return g_strconcat (_("Every weekday (Mon–Fri)"), end->str, NULL);
      if (n_days == 0)
        {
          /* no BYDAY: the first occurrence's weekday */
          days[0] = g_date_time_get_day_of_week (first) % 7;
          n_days = 1;
        }
      for (int k = 0; k < n_days; k++)
        {
          g_autofree char *name = weekday_name (days[k], n_days > 1);

          if (k)
            g_string_append (names, ", ");
          g_string_append (names, name);
        }
      if (interval == 1)
        text = g_strdup_printf (_("Weekly on %s"), names->str);
      else
        text = g_strdup_printf (g_dngettext (NULL, "Every %d week on %s", "Every %d weeks on %s", interval), interval,
                                names->str);
    }
  else if (g_str_equal (freq, "MONTHLY"))
    {
      if (nth_byday && n_days == 1)
        {
          int week = (g_date_time_get_day_of_month (first) - 1) / 7;

          /* repeat_by_nth_*: "Monthly (on every first Monday)", one phrase per weekday and week */
          text = g_strdup_printf (_("Monthly (%s)"),
                                  cal_format_nth_weekday (g_date_time_get_day_of_week (first) % 7, MIN (week, 4)));
        }
      else
        text = g_strdup (_("Monthly"));
    }
  else if (g_str_equal (freq, "YEARLY"))
    text = g_strdup (_("Yearly"));
  else
    return NULL;
  {
    char *full = g_strconcat (text, end->str, NULL);

    g_free (text);
    return full;
  }
}

/* --- the home time zone */

static char *phone_zone, *phone_tz_env;
static gboolean phone_read;

static void
read_phone_zone (void)
{
  g_autoptr(GTimeZone) local = NULL;

  if (phone_read)
    return;
  phone_read = TRUE;
  phone_tz_env = g_strdup (g_getenv ("TZ"));
  local = g_time_zone_new_local ();
  phone_zone = g_strdup (g_time_zone_get_identifier (local));
}

const char *
cal_phone_zone (void)
{
  read_phone_zone ();
  return phone_zone;
}

void
cal_set_home_zone (const char *tzid)
{
  read_phone_zone ();
  if (tzid && *tzid)
    g_setenv ("TZ", tzid, TRUE);
  else if (phone_tz_env)
    g_setenv ("TZ", phone_tz_env, TRUE);
  else
    g_unsetenv ("TZ");
  tzset ();
}

int
cal_week_number (int day, int week_start)
{
  int first = day - (cal_day_of_week (day) - week_start + 7) % 7;
  int thursday = first + (4 - week_start + 7) % 7;
  g_autoptr(GDateTime) time = g_date_time_new_from_unix_local (cal_day_start (thursday) + 12 * 3600);

  return g_date_time_get_week_of_year (time);
}

char *
cal_format_week_number (int week)
{
  return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "Week %d", "Week %d", week), week);
}
