/* Minerva's scheduler as a calendar: one-shot, recurring and past entries of `schedule range` become the right
 * events, read-only where the app cannot change them; a repeat survives the trip RRULE -> entry. */
#include "../src/cal-minerva.h"

#include <locale.h>
#include <string.h>

static const char *range =
  "{\"ok\": true, \"history_from\": \"2026-11-20T02:23:15+01:00\", \"occurrences\": ["
  "{\"task\": \"t1\", \"title\": \"chiama Anna\", \"goal\": \"chiama Anna\\nper la cena\", \"kind\": \"wake\","
  " \"app_kind\": \"prompt\", \"start\": \"2026-11-23T18:00:00+01:00\", \"end\": \"2026-11-23T18:15:00+01:00\","
  " \"recurring\": false, \"state\": \"scheduled\", \"past\": false, \"repeat\": \"none\"},"
  "{\"task\": \"t2\", \"title\": \"il meteo\", \"kind\": \"wake\", \"app_kind\": \"prompt\","
  " \"start\": \"2026-11-24T07:30:00+01:00\", \"end\": \"2026-11-24T07:45:00+01:00\", \"recurring\": true,"
  " \"cron\": \"30 7 * * 1-5\", \"state\": \"scheduled\", \"past\": false, \"repeat\": \"weekdays\"},"
  "{\"task\": \"t3\", \"title\": \"pulizia\", \"kind\": \"session\", \"app_kind\": \"other\","
  " \"start\": \"2026-11-20T03:00:00+01:00\", \"end\": \"2026-11-20T03:04:00+01:00\", \"state\": \"failed\","
  " \"past\": true, \"run\": 2, \"error\": \"timeout\"}]}";

static void
test_events (void)
{
  g_autoptr(GError) error = NULL;
  gint64 history = 0;
  g_autoptr(GPtrArray) events = cal_minerva_events (range, 0xc9a8f0, &history, &error);
  CalEvent *once, *daily, *past;

  g_assert_no_error (error);
  g_assert_cmpuint (events->len, ==, 3);
  once = events->pdata[0], daily = events->pdata[1], past = events->pdata[2];
  g_assert_cmpstr (once->calendar, ==, CAL_MINERVA_CALENDAR);
  g_assert_cmpstr (once->title, ==, "chiama Anna");
  g_assert_cmpstr (once->description, ==, "chiama Anna\nper la cena");
  g_assert_null (once->rid);
  g_assert_false (once->readonly);
  g_assert_cmpuint (once->color, ==, 0xc9a8f0);
  g_assert_true (daily->recurring);
  g_assert_cmpstr (daily->rrule, ==, "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR");
  g_assert_false (daily->readonly);
  g_assert_true (past->minerva->past);
  g_assert_true (past->readonly);                    /* a run is history */
  g_assert_cmpstr (past->rid, ==, "run-2");
  g_assert_cmpstr (past->minerva->error, ==, "timeout");
  g_assert_cmpint (history, ==, 1795137795);   /* 01:23:15 UTC */
  g_assert_null (cal_minerva_events ("[", 0, NULL, &error));
}

static void
test_repeats (void)
{
  g_autoptr(GDateTime) monday = g_date_time_new_local (2026, 11, 23, 7, 30, 0);
  gint64 start = g_date_time_to_unix (monday);
  CalEvent *event = cal_event_new ();
  g_autofree char *entry = NULL;

  g_assert_cmpstr (cal_minerva_repeat_of_rrule (NULL, start), ==, "none");
  g_assert_cmpstr (cal_minerva_repeat_of_rrule ("FREQ=DAILY", start), ==, "daily");
  g_assert_cmpstr (cal_minerva_repeat_of_rrule ("FREQ=WEEKLY;BYDAY=FR,MO,TU,WE,TH", start), ==, "weekdays");
  g_assert_cmpstr (cal_minerva_repeat_of_rrule ("FREQ=WEEKLY;BYDAY=MO", start), ==, "weekly");
  g_assert_cmpstr (cal_minerva_repeat_of_rrule ("FREQ=MONTHLY;BYMONTHDAY=23", start), ==, "monthly");
  g_assert_null (cal_minerva_repeat_of_rrule ("FREQ=DAILY;COUNT=5", start));
  g_assert_null (cal_minerva_repeat_of_rrule ("FREQ=WEEKLY;BYDAY=MO,WE", start));
  event->title = g_strdup ("il meteo");
  event->start = start;
  event->rrule = cal_minerva_rrule_of_repeat ("weekly", start);
  entry = cal_minerva_entry (event, "prompt");
  g_assert_nonnull (strstr (entry, "\"repeat\":\"weekly\""));
  g_assert_nonnull (strstr (entry, "\"kind\":\"prompt\""));
  g_assert_nonnull (strstr (entry, "\"text\":\"il meteo\""));
  {
    g_autofree char *line = cal_minerva_fill ("Errore: {error}", "error", "timeout");

    g_assert_cmpstr (line, ==, "Errore: timeout");
  }
  g_free (event->rrule);
  event->rrule = g_strdup ("FREQ=DAILY;INTERVAL=2");
  g_assert_null (cal_minerva_entry (event, "prompt"));
  cal_event_unref (event);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_setenv ("TZ", "Europe/Rome", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/minerva/events", test_events);
  g_test_add_func ("/minerva/repeats", test_repeats);
  return g_test_run ();
}
