/* Minerva's scheduler as one more calendar (cal-minerva.h). */
#include "cal-minerva.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

/* the repeats of minerva-config's [schedule] repeats */
#define REPEAT_NONE "none"
#define REPEAT_DAILY "daily"
#define REPEAT_WEEKDAYS "weekdays"
#define REPEAT_WEEKLY "weekly"
#define REPEAT_MONTHLY "monthly"
#define REPEAT_CUSTOM "custom"
/* the kinds the app makes (minerva-config [schedule] kinds); anything else is Minerva's own, read-only here */
#define KIND_PROMPT "prompt"
#define KIND_REMINDER "reminder"
#define STATE_SCHEDULED "scheduled"

static const char *const byday[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

static const char *
str (JsonObject *o, const char *member)
{
  JsonNode *node = o ? json_object_get_member (o, member) : NULL;

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : "";
}

static gint64
unix_of (const char *iso)
{
  g_autoptr(GDateTime) time = iso && *iso ? g_date_time_new_from_iso8601 (iso, NULL) : NULL;

  return time ? g_date_time_to_unix (time) : 0;
}

GPtrArray *
cal_minerva_events (const char *json, guint32 color, gint64 *history_from, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new ();

  if (history_from)
    *history_from = 0;
  if (!json_parser_load_from_data (parser, json, -1, error))
    return NULL;
  if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a schedule range answer");
      return NULL;
    }
  return cal_minerva_events_of (json_node_get_object (json_parser_get_root (parser)), color, history_from);
}

GPtrArray *
cal_minerva_events_of (JsonObject *root, guint32 color, gint64 *history_from)
{
  JsonArray *list;
  GPtrArray *out;

  if (history_from)
    *history_from = unix_of (str (root, "history_from"));
  out = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  list = json_object_has_member (root, "occurrences") ? json_object_get_array_member (root, "occurrences") : NULL;
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *o = json_array_get_object_element (list, i);
      CalEvent *event = cal_event_new ();
      CalMinervaInfo *info = g_new0 (CalMinervaInfo, 1);
      gboolean recurring = json_object_get_boolean_member_with_default (o, "recurring", FALSE);

      info->task = g_strdup (str (o, "task"));
      info->kind = g_strdup (*str (o, "app_kind") ? str (o, "app_kind") : "other");
      info->state = g_strdup (str (o, "state"));
      info->repeat = g_strdup (*str (o, "repeat") ? str (o, "repeat") : REPEAT_NONE);
      info->goal = g_strdup (str (o, "goal"));
      info->error = *str (o, "error") ? g_strdup (str (o, "error")) : NULL;
      info->past = json_object_get_boolean_member_with_default (o, "past", FALSE);
      info->approval = json_object_get_boolean_member_with_default (o, "needs_approval", FALSE);
      info->run = (int) json_object_get_int_member_with_default (o, "run", 0);
      event->minerva = info;
      event->calendar = g_strdup (CAL_MINERVA_CALENDAR);
      event->uid = g_strdup (info->task);
      /* one event per due time or run: the rid tells them apart (a run by its number) */
      event->rid = info->past ? g_strdup_printf ("run-%d", info->run) : recurring ? g_strdup (str (o, "start")) : NULL;
      event->title = g_strdup (str (o, "title"));
      event->description = *info->goal ? g_strdup (info->goal) : NULL;
      event->start = unix_of (str (o, "start"));
      event->end = MAX (unix_of (str (o, "end")), event->start);
      event->color = color;
      event->recurring = recurring && !info->past;
      event->rrule = event->recurring ? cal_minerva_rrule_of_repeat (info->repeat, event->start) : NULL;
      event->readonly = info->past || g_strcmp0 (info->state, STATE_SCHEDULED) != 0
                        || (g_strcmp0 (info->kind, KIND_PROMPT) != 0 && g_strcmp0 (info->kind, KIND_REMINDER) != 0)
                        || g_strcmp0 (info->repeat, REPEAT_CUSTOM) == 0;
      event->cancelled = info->past && g_strcmp0 (info->state, "cancelled") == 0;
      g_ptr_array_add (out, event);
    }
  return out;
}

void
cal_minerva_mark_outcomes (GPtrArray *events, JsonObject *ui)
{
  for (guint i = 0; ui && events && i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];
      g_autofree char *key = NULL;
      const char *mark;

      if (!event->minerva || !event->minerva->past || !event->minerva->state)
        continue;
      key = g_strconcat ("mark_", event->minerva->state, NULL);
      mark = json_object_get_string_member_with_default (ui, key, "");
      if (*mark)
        {
          char *title = g_strdup_printf ("%s %s", mark, event->title ? event->title : "");

          g_free (event->title);
          event->title = title;
        }
    }
}

/* the day of the week of @start (0 Sunday), in local time */
static int
weekday_of (gint64 start)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (start);

  return g_date_time_get_day_of_week (local) % 7;
}

static int
mday_of (gint64 start)
{
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (start);

  return g_date_time_get_day_of_month (local);
}

const char *
cal_minerva_repeat_of_rrule (const char *rrule, gint64 start)
{
  g_auto(GStrv) parts = NULL;
  const char *freq = NULL, *days = NULL, *mday = NULL;

  if (!rrule || !*rrule)
    return REPEAT_NONE;
  parts = g_strsplit (rrule, ";", -1);
  for (guint i = 0; parts[i]; i++)
    {
      const char *eq = strchr (parts[i], '=');

      if (!eq)
        return NULL;
      if (g_str_has_prefix (parts[i], "FREQ="))
        freq = eq + 1;
      else if (g_str_has_prefix (parts[i], "BYDAY="))
        days = eq + 1;
      else if (g_str_has_prefix (parts[i], "BYMONTHDAY="))
        mday = eq + 1;
      else if (g_str_has_prefix (parts[i], "WKST=") || g_str_equal (parts[i], "INTERVAL=1"))
        continue;
      else
        return NULL;    /* an interval, a count, an end, a month, a position: no repeat makes it */
    }
  if (g_strcmp0 (freq, "DAILY") == 0 && !days && !mday)
    return REPEAT_DAILY;
  if (g_strcmp0 (freq, "WEEKLY") == 0 && !mday)
    {
      g_auto(GStrv) list = days ? g_strsplit (days, ",", -1) : NULL;
      guint n = list ? g_strv_length (list) : 0;

      if (n == 0 || (n == 1 && g_str_equal (list[0], byday[weekday_of (start)])))
        return REPEAT_WEEKLY;
      if (n == 5)
        {
          static const char *const weekdays[] = { "MO", "TU", "WE", "TH", "FR" };

          for (guint i = 0; i < G_N_ELEMENTS (weekdays); i++)
            if (!g_strv_contains ((const char *const *) list, weekdays[i]))
              return NULL;
          return REPEAT_WEEKDAYS;
        }
      return NULL;
    }
  if (g_strcmp0 (freq, "MONTHLY") == 0 && !days && (!mday || atoi (mday) == mday_of (start)))
    return REPEAT_MONTHLY;
  return NULL;
}

char *
cal_minerva_rrule_of_repeat (const char *repeat, gint64 start)
{
  if (g_strcmp0 (repeat, REPEAT_DAILY) == 0)
    return g_strdup ("FREQ=DAILY");
  if (g_strcmp0 (repeat, REPEAT_WEEKDAYS) == 0)
    return g_strdup ("FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR");
  if (g_strcmp0 (repeat, REPEAT_WEEKLY) == 0)
    return g_strdup_printf ("FREQ=WEEKLY;BYDAY=%s", byday[weekday_of (start)]);
  if (g_strcmp0 (repeat, REPEAT_MONTHLY) == 0)
    return g_strdup_printf ("FREQ=MONTHLY;BYMONTHDAY=%d", mday_of (start));
  return NULL;
}

char *
cal_minerva_entry (const CalEvent *event, const char *kind)
{
  const char *repeat = cal_minerva_repeat_of_rrule (event->rrule, event->start);
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  g_autoptr(JsonGenerator) generator = json_generator_new ();
  g_autoptr(JsonNode) root = NULL;
  g_autoptr(GDateTime) local = g_date_time_new_from_unix_local (event->start);
  g_autofree char *start = g_date_time_format_iso8601 (local);
  const char *text = event->title && *event->title ? event->title : event->description;

  if (!repeat || g_str_equal (repeat, REPEAT_CUSTOM))
    return NULL;
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "kind");
  json_builder_add_string_value (builder, kind);
  json_builder_set_member_name (builder, "text");
  json_builder_add_string_value (builder, text ? text : "");
  json_builder_set_member_name (builder, "start");
  json_builder_add_string_value (builder, start);
  json_builder_set_member_name (builder, "repeat");
  json_builder_add_string_value (builder, repeat);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  return json_generator_to_data (generator, NULL);
}

char *
cal_minerva_fill (const char *template, const char *key, const char *value)
{
  g_autofree char *marker = g_strdup_printf ("{%s}", key);
  g_auto(GStrv) parts = g_strsplit (template ? template : "", marker, -1);

  return g_strjoinv (value ? value : "", parts);
}
