/* CalStore: the phone's calendars from evolution-data-server (cal-store.h). */
#include "cal-store.h"
#include "cal-minerva.h"
#include "ns-mc.h"

#include <glib/gi18n.h>
#include <libedataserver/libedataserver.h>
#include <string.h>

#define CONNECT_WAIT_SECONDS 30         /* e_cal_client_connect: how long a backend may take to come online */
#define CHANGED_DELAY_MS 250            /* a burst of view notifications (a sync, an import) is one "changed" */

/* one calendar's connection */
typedef struct {
  CalStore *store;                      /* not a reference: the store frees its connections */
  char *uid;
  ESource *source;
  ECalClient *client;                   /* NULL while connecting, or if it failed */
  ECalClientView *view;
  GCancellable *cancellable;
  gboolean done;                        /* connected or failed */
  char *email;                          /* the backend's cal-email-address, once it has answered */
} Connection;

struct _CalStore {
  GObject parent;
  gboolean hidden_too;
  GCancellable *cancellable;
  ESourceRegistry *registry;
  GPtrArray *calendars;                 /* CalCalendar, by account and name */
  GHashTable *connections;              /* uid -> Connection */
  guint rebuild_id, changed_id;
  gboolean registry_done;
  /* Minerva's scheduler as one more calendar (cal-minerva.h): on once the app asked for it and minerva-config is
   * installed; its texts and colour from `schedule ui`; TasksChanged of minerva-events says it changed */
  gboolean minerva_wanted, minerva_visible;
  JsonObject *minerva_ui;
  GDBusConnection *session;
  guint minerva_signal;
  gint64 minerva_history;
};

G_DEFINE_FINAL_TYPE (CalStore, cal_store, G_TYPE_OBJECT)

enum { CALENDARS_CHANGED, CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void rebuild (CalStore *self);

/* --- helpers */

/* "#rgb", "#rrggbb" or "rgb(r,g,b)" (what Evolution and GNOME Calendar write); FALSE for anything else */
static gboolean
parse_color (const char *text, guint32 *rgb)
{
  unsigned r, g, b;

  if (!text)
    return FALSE;
  while (g_ascii_isspace (*text))
    text++;
  if (text[0] == '#' && strlen (text) >= 7 && sscanf (text + 1, "%2x%2x%2x", &r, &g, &b) == 3)
    ;
  else if (text[0] == '#' && strlen (text) == 4 && sscanf (text + 1, "%1x%1x%1x", &r, &g, &b) == 3)
    {
      r *= 17;
      g *= 17;
      b *= 17;
    }
  else if (sscanf (text, "rgb(%u,%u,%u)", &r, &g, &b) == 3 || sscanf (text, "rgba(%u,%u,%u", &r, &g, &b) == 3)
    ;
  else
    return FALSE;
  *rgb = (MIN (r, 255) << 16) | (MIN (g, 255) << 8) | MIN (b, 255);
  return TRUE;
}

static CalCalendar *
calendar_copy (const CalCalendar *calendar)
{
  CalCalendar *copy = g_memdup2 (calendar, sizeof *calendar);

  copy->uid = g_strdup (calendar->uid);
  copy->name = g_strdup (calendar->name);
  copy->account = g_strdup (calendar->account);
  copy->email = g_strdup (calendar->email);
  return copy;
}

/* the zone the app shows times in: the home time zone when it is used (cal_set_home_zone sets TZ), else this
 * phone's. EDS's own system zone reads /etc/localtime and never TZ, so it cannot stand for it. */
static ICalTimezone *
app_zone (void)
{
  const char *tz = g_getenv ("TZ");
  ICalTimezone *zone = tz && *tz ? i_cal_timezone_get_builtin_timezone (tz[0] == ':' ? tz + 1 : tz) : NULL;

  return zone ? zone : e_cal_util_get_system_timezone ();
}

void
cal_store_zone_changed (CalStore *self)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->connections);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (((Connection *) value)->client)
      e_cal_client_set_default_timezone (((Connection *) value)->client, app_zone ());
  g_signal_emit (self, signals[CHANGED], 0);
}

gint64
cal_ical_time_to_unix (ICalTime *time)
{
  g_autoptr(ICalTimezone) zone = NULL;

  if (i_cal_time_is_date (time))
    {
      g_autoptr(GDateTime) midnight = g_date_time_new_local (i_cal_time_get_year (time), i_cal_time_get_month (time),
                                                             i_cal_time_get_day (time), 0, 0, 0);
      return g_date_time_to_unix (midnight);
    }
  zone = i_cal_time_get_timezone (time);
  if (zone || i_cal_time_is_utc (time))
    return i_cal_time_as_timet_with_zone (time, zone);
  /* floating: the wall time here */
  {
    g_autoptr(GDateTime) local = g_date_time_new_local (i_cal_time_get_year (time), i_cal_time_get_month (time),
                                                        i_cal_time_get_day (time), i_cal_time_get_hour (time),
                                                        i_cal_time_get_minute (time), i_cal_time_get_second (time));
    return g_date_time_to_unix (local);
  }
}

static char *
text_of (ECalComponentText *text)
{
  const char *value = text ? e_cal_component_text_get_value (text) : NULL;

  return value && *value ? g_strdup (value) : NULL;
}

static char *
empty_to_null (char *text)
{
  if (text && !*g_strstrip (text))
    g_clear_pointer (&text, g_free);
  return text;
}

/* the event's VALARMs as Etar's reminders: those relative to the start; an absolute or end-relative one has no
 * place in Etar's reminder rows and is left as it is in the event */
static void
read_reminders (ECalComponent *comp, GArray *reminders)
{
  GSList *alarms = e_cal_component_get_all_alarms (comp);

  for (GSList *l = alarms; l; l = l->next)
    {
      ECalComponentAlarm *alarm = l->data;
      ECalComponentAlarmTrigger *trigger = e_cal_component_alarm_get_trigger (alarm);
      CalReminder reminder;

      if (!trigger || e_cal_component_alarm_trigger_get_kind (trigger) != E_CAL_COMPONENT_ALARM_TRIGGER_RELATIVE_START)
        continue;
      switch (e_cal_component_alarm_get_action (alarm))
        {
        case E_CAL_COMPONENT_ALARM_DISPLAY:
          reminder.method = CAL_METHOD_NOTIFICATION;
          break;
        case E_CAL_COMPONENT_ALARM_EMAIL:
          reminder.method = CAL_METHOD_EMAIL;
          break;
        case E_CAL_COMPONENT_ALARM_AUDIO:
          reminder.method = CAL_METHOD_ALARM;
          break;
        default:
          continue;
        }
      /* the trigger is "-PT10M": negative before the start; Etar's minutes count before it */
      reminder.minutes = -i_cal_duration_as_int (e_cal_component_alarm_trigger_get_duration (trigger)) / 60;
      g_array_append_val (reminders, reminder);
    }
  g_slist_free_full (alarms, e_cal_component_alarm_free);
}

static CalStatus
status_of (ICalParameterPartstat partstat)
{
  switch (partstat)
    {
    case I_CAL_PARTSTAT_ACCEPTED: return CAL_STATUS_ACCEPTED;
    case I_CAL_PARTSTAT_DECLINED: return CAL_STATUS_DECLINED;
    case I_CAL_PARTSTAT_TENTATIVE: return CAL_STATUS_TENTATIVE;
    case I_CAL_PARTSTAT_NEEDSACTION: return CAL_STATUS_INVITED;
    default: return CAL_STATUS_NONE;
    }
}

/* "mailto:a@b" -> "a@b" */
static char *
address_of (const char *value)
{
  if (!value || !*value)
    return NULL;
  return g_strdup (g_ascii_strncasecmp (value, "mailto:", 7) == 0 ? value + 7 : value);
}

/* the guests (ATTENDEE, the ORGANIZER first as Etar's attendee list has it), and the owner's own status: Etar's
 * Instances.SELF_ATTENDEE_STATUS, which the provider takes from the attendee row with the calendar's owner address */
static void
read_attendees (ECalComponent *comp, const CalCalendar *calendar, CalEvent *event)
{
  GSList *attendees = e_cal_component_get_attendees (comp);
  ECalComponentOrganizer *organizer = e_cal_component_has_organizer (comp) ? e_cal_component_get_organizer (comp)
                                                                           : NULL;
  const char *me = calendar->email;

  event->self_status = CAL_STATUS_NONE;
  if (organizer)
    event->organizer = address_of (e_cal_component_organizer_get_value (organizer));
  for (GSList *l = attendees; l; l = l->next)
    {
      ECalComponentAttendee *a = l->data;
      CalAttendee *attendee = g_new0 (CalAttendee, 1);
      const char *name = e_cal_component_attendee_get_cn (a);

      attendee->email = address_of (e_cal_component_attendee_get_value (a));
      attendee->name = name && *name ? g_strdup (name) : NULL;
      attendee->status = status_of (e_cal_component_attendee_get_partstat (a));
      attendee->organizer = event->organizer && attendee->email
                            && g_ascii_strcasecmp (attendee->email, event->organizer) == 0;
      if (me && attendee->email && g_ascii_strcasecmp (attendee->email, me) == 0)
        event->self_status = attendee->status;
      if (!attendee->email)
        cal_attendee_free (attendee);
      else if (attendee->organizer)
        g_ptr_array_insert (event->attendees, 0, attendee);
      else
        g_ptr_array_add (event->attendees, attendee);
    }
  /* the organizer without an ATTENDEE of their own: the owner organizing is "accepted" to the provider */
  if (event->organizer && me && g_ascii_strcasecmp (event->organizer, me) == 0
      && event->self_status == CAL_STATUS_NONE && attendees)
    event->self_status = CAL_STATUS_ACCEPTED;
  g_slist_free_full (attendees, e_cal_component_attendee_free);
  g_clear_pointer (&organizer, e_cal_component_organizer_free);
}

CalEvent *
cal_event_from_component (ECalComponent *comp, const CalCalendar *calendar, ICalTime *start, ICalTime *end)
{
  ICalComponent *icomp = e_cal_component_get_icalcomponent (comp);
  CalEvent *event = cal_event_new ();
  ECalComponentText *summary = e_cal_component_get_summary (comp);
  GSList *descriptions = e_cal_component_get_descriptions (comp);
  ECalComponentDateTime *dtstart = e_cal_component_get_dtstart (comp);
  g_autoptr(ICalProperty) color = i_cal_component_get_first_property (icomp, I_CAL_COLOR_PROPERTY);
  g_autoptr(ICalProperty) rrule = i_cal_component_get_first_property (icomp, I_CAL_RRULE_PROPERTY);
  guint32 rgb;

  event->calendar = g_strdup (calendar->uid);
  event->uid = g_strdup (e_cal_component_get_uid (comp));
  event->title = text_of (summary);
  event->location = empty_to_null (e_cal_component_get_location (comp));
  event->description = descriptions ? text_of (descriptions->data) : NULL;
  event->url = empty_to_null (e_cal_component_get_url (comp));
  event->all_day = i_cal_time_is_date (start);
  event->start = cal_ical_time_to_unix (start);
  event->end = end ? cal_ical_time_to_unix (end) : event->start;
  /* an all-day event without an end lasts its day (RFC 5545 3.6.1) */
  if (event->all_day && event->end <= event->start)
    event->end = cal_day_start (cal_day_number (event->start) + 1);
  event->recurring = e_cal_component_has_recurrences (comp) || e_cal_component_is_instance (comp);
  if (event->recurring)
    event->rid = e_cal_component_get_recurid_as_string (comp);
  if (rrule)
    {
      g_autoptr(ICalRecurrence) recurrence = i_cal_property_get_rrule (rrule);

      event->rrule = i_cal_recurrence_to_string (recurrence);
    }
  if (dtstart && !event->all_day && e_cal_component_datetime_get_tzid (dtstart))
    event->tzid = g_strdup (e_cal_component_datetime_get_tzid (dtstart));
  event->own_color = color && parse_color (i_cal_property_get_color (color), &rgb);
  event->color = event->own_color ? rgb : calendar->color;
  event->readonly = calendar->readonly;
  read_reminders (comp, event->reminders);
  read_attendees (comp, calendar, event);
  event->availability = e_cal_component_get_transparency (comp) == E_CAL_COMPONENT_TRANSP_TRANSPARENT
                        ? CAL_AVAILABILITY_FREE : CAL_AVAILABILITY_BUSY;
  event->cancelled = e_cal_component_get_status (comp) == I_CAL_STATUS_CANCELLED;
  switch (e_cal_component_get_classification (comp))
    {
    case E_CAL_COMPONENT_CLASS_PUBLIC: event->access = CAL_ACCESS_PUBLIC; break;
    case E_CAL_COMPONENT_CLASS_PRIVATE: event->access = CAL_ACCESS_PRIVATE; break;
    case E_CAL_COMPONENT_CLASS_CONFIDENTIAL: event->access = CAL_ACCESS_CONFIDENTIAL; break;
    default: event->access = CAL_ACCESS_DEFAULT; break;
    }

  g_clear_pointer (&summary, e_cal_component_text_free);
  g_slist_free_full (descriptions, e_cal_component_text_free);
  g_clear_pointer (&dtstart, e_cal_component_datetime_free);
  return event;
}

/* --- the registry and the connections */

static void
connection_free (Connection *connection)
{
  g_cancellable_cancel (connection->cancellable);
  g_clear_object (&connection->cancellable);
  if (connection->view)
    {
      g_signal_handlers_disconnect_by_data (connection->view, connection);
      e_cal_client_view_stop (connection->view, NULL);
    }
  g_clear_object (&connection->view);
  g_clear_object (&connection->client);
  g_clear_object (&connection->source);
  g_free (connection->uid);
  g_free (connection->email);
  g_free (connection);
}

static gboolean
emit_changed (gpointer data)
{
  CalStore *self = data;

  self->changed_id = 0;
  g_signal_emit (self, signals[CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static void
schedule_changed (CalStore *self)
{
  if (!self->changed_id)
    self->changed_id = g_timeout_add (CHANGED_DELAY_MS, emit_changed, self);
}

static void
on_view_objects (ECalClientView *view, gpointer objects, Connection *connection)
{
  schedule_changed (connection->store);
}

static void
on_view (GObject *source, GAsyncResult *result, gpointer data)
{
  Connection *connection = data;
  g_autoptr(GError) error = NULL;
  ECalClientView *view = NULL;
  GSList fields = { (gpointer) "UID", NULL };

  if (!e_cal_client_get_view_finish (E_CAL_CLIENT (source), result, &view, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("calendar %s: no change notifications: %s", connection->uid, error->message);
      return;
    }
  connection->view = view;
  /* only what changes, not every event the view matches at the start; and none of their data, which the queries
   * read for themselves */
  e_cal_client_view_set_flags (view, E_CAL_CLIENT_VIEW_FLAGS_NONE, NULL);
  e_cal_client_view_set_fields_of_interest (view, &fields, NULL);
  g_signal_connect (view, "objects-added", G_CALLBACK (on_view_objects), connection);
  g_signal_connect (view, "objects-modified", G_CALLBACK (on_view_objects), connection);
  g_signal_connect (view, "objects-removed", G_CALLBACK (on_view_objects), connection);
  e_cal_client_view_start (view, &error);
  if (error)
    g_warning ("calendar %s: no change notifications: %s", connection->uid, error->message);
}

static void
on_readonly (GObject *client, GParamSpec *pspec, Connection *connection)
{
  rebuild (connection->store);
}

/* the owner's address: guests' status is read against it */
static void
on_email (GObject *client, GAsyncResult *result, gpointer data)
{
  Connection *connection = data;
  g_autofree char *email = NULL;

  if (!e_client_get_backend_property_finish (E_CLIENT (client), result, &email, NULL))
    return;
  g_free (connection->email);
  connection->email = email && *email ? g_steal_pointer (&email) : NULL;
  if (connection->email)
    {
      rebuild (connection->store);
      schedule_changed (connection->store);
    }
}

static void
on_connected (GObject *source, GAsyncResult *result, gpointer data)
{
  Connection *connection = data;
  g_autoptr(GError) error = NULL;
  EClient *client = e_cal_client_connect_finish (result, &error);

  if (!client)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
      g_warning ("calendar %s: cannot open it: %s", connection->uid, error->message);
      connection->done = TRUE;
      rebuild (connection->store);
      return;
    }
  connection->client = E_CAL_CLIENT (client);
  connection->done = TRUE;
  /* floating times and all-day dates are the app's zone's */
  e_cal_client_set_default_timezone (connection->client, app_zone ());
  g_signal_connect (client, "notify::readonly", G_CALLBACK (on_readonly), connection);
  e_cal_client_get_view (connection->client, "#t", connection->cancellable, on_view, connection);
  e_client_get_backend_property (client, E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS, connection->cancellable,
                                 on_email, connection);
  rebuild (connection->store);
  schedule_changed (connection->store);
}

static void
connect_calendar (CalStore *self, ESource *source)
{
  Connection *connection = g_new0 (Connection, 1);

  connection->store = self;
  connection->uid = g_strdup (e_source_get_uid (source));
  connection->source = g_object_ref (source);
  connection->cancellable = g_cancellable_new ();
  g_hash_table_insert (self->connections, connection->uid, connection);
  e_cal_client_connect (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, CONNECT_WAIT_SECONDS, connection->cancellable,
                        on_connected, connection);
}

static int
compare_calendars (gconstpointer a, gconstpointer b)
{
  const CalCalendar *x = *(CalCalendar * const *) a, *y = *(CalCalendar * const *) b;
  int by_account = g_utf8_collate (x->account, y->account);

  return by_account ? by_account : g_utf8_collate (x->name, y->name);
}

/* the calendars anew from the registry; connect the ones to show, let go of the others */
static gboolean
do_rebuild (gpointer data)
{
  CalStore *self = data;
  GList *sources = e_source_registry_list_sources (self->registry, E_SOURCE_EXTENSION_CALENDAR);
  g_autoptr(GHashTable) wanted = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTableIter iter;
  Connection *connection;

  self->rebuild_id = 0;
  g_ptr_array_set_size (self->calendars, 0);
  for (GList *l = sources; l; l = l->next)
    {
      ESource *source = l->data;
      ESourceSelectable *selectable = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);
      g_autoptr(ESource) parent = e_source_get_parent (source)
                                  ? e_source_registry_ref_source (self->registry, e_source_get_parent (source))
                                  : NULL;
      gboolean enabled = e_source_registry_check_enabled (self->registry, source);
      CalCalendar *calendar;

      if (!enabled)
        continue;
      calendar = g_new0 (CalCalendar, 1);
      calendar->uid = e_source_dup_uid (source);
      calendar->name = e_source_dup_display_name (source);
      calendar->account = parent ? e_source_dup_display_name (parent) : g_strdup ("");
      if (!parse_color (e_source_selectable_get_color (selectable), &calendar->color))
        calendar->color = 0x3f51b5;     /* Etar's default calendar colour */
      calendar->visible = e_source_selectable_get_selected (selectable);
      calendar->alarms = !e_source_has_extension (source, E_SOURCE_EXTENSION_ALARMS)
                         || e_source_alarms_get_include_me (e_source_get_extension (source,
                                                                                    E_SOURCE_EXTENSION_ALARMS));
      connection = g_hash_table_lookup (self->connections, calendar->uid);
      /* until it answers, nothing may be written to it */
      calendar->readonly = !connection || !connection->client || e_client_is_readonly (E_CLIENT (connection->client));
      calendar->email = connection ? g_strdup (connection->email) : NULL;
      calendar->local = e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR)
                        && g_strcmp0 (e_source_backend_get_backend_name (E_SOURCE_BACKEND (selectable)), "local") == 0;
      g_ptr_array_add (self->calendars, calendar);

      if (calendar->visible || self->hidden_too)
        {
          g_hash_table_add (wanted, calendar->uid);
          if (!connection)
            connect_calendar (self, source);
        }
    }
  g_list_free_full (sources, g_object_unref);
  if (self->minerva_ui)
    {
      CalCalendar *minerva = g_new0 (CalCalendar, 1);
      const char *color = ns_json_str (self->minerva_ui, "color");

      minerva->uid = g_strdup (CAL_MINERVA_CALENDAR);
      minerva->name = g_strdup (ns_json_str (self->minerva_ui, "calendar_name"));
      minerva->account = g_strdup (minerva->name);
      if (!parse_color (color, &minerva->color))
        minerva->color = 0x3f51b5;
      minerva->visible = self->minerva_visible;
      minerva->readonly = FALSE;   /* its writes go to the scheduler, not to EDS */
      g_ptr_array_add (self->calendars, minerva);
    }
  g_ptr_array_sort (self->calendars, compare_calendars);

  /* the hidden and the removed ones: their events go from the views */
  g_hash_table_iter_init (&iter, self->connections);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &connection))
    if (!g_hash_table_contains (wanted, connection->uid))
      {
        g_hash_table_iter_remove (&iter);
        schedule_changed (self);
      }

  g_signal_emit (self, signals[CALENDARS_CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static void
rebuild (CalStore *self)
{
  if (!self->rebuild_id && self->registry)
    self->rebuild_id = g_idle_add (do_rebuild, self);
}

static void
on_source_event (ESourceRegistry *registry, ESource *source, CalStore *self)
{
  if (e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR) || !e_source_get_parent (source))
    rebuild (self);
}

static void
on_registry (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(CalStore) self = data;
  g_autoptr(GError) error = NULL;
  ESourceRegistry *registry = e_source_registry_new_finish (result, &error);

  self->registry_done = TRUE;
  if (!registry)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("no calendars: %s", error->message);
      g_signal_emit (self, signals[CALENDARS_CHANGED], 0);
      return;
    }
  self->registry = registry;
  g_signal_connect (registry, "source-added", G_CALLBACK (on_source_event), self);
  g_signal_connect (registry, "source-removed", G_CALLBACK (on_source_event), self);
  g_signal_connect (registry, "source-changed", G_CALLBACK (on_source_event), self);
  g_signal_connect (registry, "source-enabled", G_CALLBACK (on_source_event), self);
  g_signal_connect (registry, "source-disabled", G_CALLBACK (on_source_event), self);
  do_rebuild (self);
}

/* --- GObject */

static void
cal_store_dispose (GObject *object)
{
  CalStore *self = CAL_STORE (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->rebuild_id, g_source_remove);
  g_clear_handle_id (&self->changed_id, g_source_remove);
  if (self->registry)
    g_signal_handlers_disconnect_by_data (self->registry, self);
  g_clear_pointer (&self->connections, g_hash_table_unref);
  g_clear_object (&self->registry);
  if (self->session && self->minerva_signal)
    g_dbus_connection_signal_unsubscribe (self->session, self->minerva_signal);
  self->minerva_signal = 0;
  g_clear_object (&self->session);
  g_clear_pointer (&self->minerva_ui, json_object_unref);
  G_OBJECT_CLASS (cal_store_parent_class)->dispose (object);
}

static void
cal_store_finalize (GObject *object)
{
  CalStore *self = CAL_STORE (object);

  g_ptr_array_unref (self->calendars);
  g_object_unref (self->cancellable);
  G_OBJECT_CLASS (cal_store_parent_class)->finalize (object);
}

static void
cal_store_class_init (CalStoreClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cal_store_dispose;
  G_OBJECT_CLASS (klass)->finalize = cal_store_finalize;
  signals[CALENDARS_CHANGED] = g_signal_new ("calendars-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
cal_store_init (CalStore *self)
{
  self->cancellable = g_cancellable_new ();
  self->calendars = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_calendar_free);
  self->connections = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) connection_free);
}

CalStore *
cal_store_new (gboolean hidden_too)
{
  CalStore *self = g_object_new (CAL_TYPE_STORE, NULL);

  self->hidden_too = hidden_too;
  e_source_registry_new (self->cancellable, on_registry, g_object_ref (self));
  return self;
}

/* --- Minerva's scheduler */

static void
on_tasks_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
                  GVariant *params, gpointer data)
{
  schedule_changed (CAL_STORE (data));
}

static void
on_minerva_ui (JsonObject *root, const char *err, gpointer data)
{
  CalStore *self = data;

  if (err || !root || !json_object_get_boolean_member_with_default (root, "ok", FALSE))
    {
      g_warning ("the Minerva calendar is left out: %s", err ? err : ns_json_str (root, "error"));
      return;
    }
  g_clear_pointer (&self->minerva_ui, json_object_unref);
  self->minerva_ui = json_object_ref (root);
  self->session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (self->session)
    self->minerva_signal = g_dbus_connection_signal_subscribe (self->session, NULL,
                                                               ns_json_str (root, "signal_interface"),
                                                               ns_json_str (root, "signal_name"),
                                                               ns_json_str (root, "signal_path"), NULL,
                                                               G_DBUS_SIGNAL_FLAGS_NONE, on_tasks_changed, self, NULL);
  rebuild (self);
  schedule_changed (self);
}

void
cal_store_enable_minerva (CalStore *self)
{
  static const char *const args[] = { "schedule", "ui", "--json", NULL };

  if (self->minerva_wanted || !ns_minerva_available ())
    return;
  self->minerva_wanted = TRUE;
  self->minerva_visible = TRUE;
  ns_mc_run (G_OBJECT (self), args, NULL, on_minerva_ui, self);
}

JsonObject *
cal_store_minerva_ui (CalStore *self)
{
  return self->minerva_ui;
}

gint64
cal_store_minerva_history (CalStore *self)
{
  return self->minerva_history;
}

static char *
iso_of (gint64 time)
{
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc (time);

  return g_date_time_format (utc, "%Y-%m-%dT%H:%M:%SZ");
}

/* a minerva-config answer as a task's result: TRUE, or its error */
static void
on_minerva_written (JsonObject *root, const char *err, gpointer data)
{
  GTask *task = data;

  if (!err && root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err ? err : ns_json_str (root, "error"));
  g_object_unref (task);
}

GPtrArray *
cal_store_get_calendars (CalStore *self)
{
  return self->calendars;
}

const CalCalendar *
cal_store_find_calendar (CalStore *self, const char *uid)
{
  for (guint i = 0; uid && i < self->calendars->len; i++)
    if (g_str_equal (((CalCalendar *) self->calendars->pdata[i])->uid, uid))
      return self->calendars->pdata[i];
  return NULL;
}

ECalClient *
cal_store_get_client (CalStore *self, const char *uid)
{
  Connection *connection = uid ? g_hash_table_lookup (self->connections, uid) : NULL;

  return connection ? connection->client : NULL;
}

const CalCalendar *
cal_store_default_calendar (CalStore *self, const char *last_used)
{
  const CalCalendar *last = cal_store_find_calendar (self, last_used);

  if (last && last->visible && !last->readonly)
    return last;
  /* a new event is the owner's own unless he last chose Minerva's calendar */
  for (guint i = 0; i < self->calendars->len; i++)
    {
      const CalCalendar *calendar = self->calendars->pdata[i];

      if (calendar->visible && !calendar->readonly && g_strcmp0 (calendar->uid, CAL_MINERVA_CALENDAR) != 0)
        return calendar;
    }
  return NULL;
}

static void
on_source_written (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!e_source_write_finish (E_SOURCE (source), result, &error))
    g_warning ("%s: %s", e_source_get_uid (E_SOURCE (source)), error->message);
}

void
cal_store_set_visible (CalStore *self, const char *uid, gboolean visible)
{
  if (g_strcmp0 (uid, CAL_MINERVA_CALENDAR) == 0)
    {
      self->minerva_visible = visible;
      rebuild (self);
      schedule_changed (self);
      return;
    }
  g_autoptr(ESource) source = self->registry ? e_source_registry_ref_source (self->registry, uid) : NULL;
  ESourceSelectable *selectable;

  if (!source || !e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
    return;
  selectable = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);
  if (e_source_selectable_get_selected (selectable) == visible)
    return;
  e_source_selectable_set_selected (selectable, visible);
  e_source_write (source, NULL, on_source_written, NULL);
  rebuild (self);
}

ESourceRegistry *
cal_store_get_registry (CalStore *self)
{
  return self->registry;
}

gboolean
cal_store_is_ready (CalStore *self)
{
  GHashTableIter iter;
  Connection *connection;

  if (!self->registry_done || self->rebuild_id)
    return FALSE;
  g_hash_table_iter_init (&iter, self->connections);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &connection))
    if (!connection->done)
      return FALSE;
  return TRUE;
}

/* --- reading occurrences */

typedef struct {
  ECalClient *client;
  CalCalendar *calendar;
  gint64 start, end;
  GPtrArray *events;
} Reading;

static void
reading_free (Reading *reading)
{
  g_object_unref (reading->client);
  cal_calendar_free (reading->calendar);
  g_clear_pointer (&reading->events, g_ptr_array_unref);
  g_free (reading);
}

static gboolean
on_instance (ICalComponent *icomp, ICalTime *start, ICalTime *end, gpointer data, GCancellable *cancellable,
             GError **error)
{
  Reading *reading = data;
  g_autoptr(ECalComponent) comp = e_cal_component_new_from_icalcomponent (i_cal_component_clone (icomp));

  if (comp)
    g_ptr_array_add (reading->events, cal_event_from_component (comp, reading->calendar, start, end));
  return !g_cancellable_is_cancelled (cancellable);
}

static void
read_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Reading *reading = data;

  reading->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  e_cal_client_generate_instances_sync (reading->client, reading->start, reading->end, cancellable, on_instance,
                                        reading);
  g_task_return_boolean (task, TRUE);
}

typedef struct {
  int pending;
  GPtrArray *events;
  GCancellable *cancellable;
  CalQueryDone done;
  gpointer data;
} Query;

static void
query_finish (Query *query)
{
  if (g_cancellable_is_cancelled (query->cancellable))
    query->done (NULL, query->data);
  else
    {
      g_ptr_array_sort (query->events, cal_event_compare);
      query->done (query->events, query->data);
    }
  g_ptr_array_unref (query->events);
  g_object_unref (query->cancellable);
  g_free (query);
}

static void
on_read (GObject *source, GAsyncResult *result, gpointer data)
{
  Query *query = data;
  Reading *reading = g_task_get_task_data (G_TASK (result));

  if (g_task_propagate_boolean (G_TASK (result), NULL) && reading->events)
    for (guint i = 0; i < reading->events->len; i++)
      g_ptr_array_add (query->events, cal_event_ref (reading->events->pdata[i]));
  if (--query->pending == 0)
    query_finish (query);
}

typedef struct {
  CalStore *store;
  Query *query;
  guint32 color;
} MinervaRead;

/* the Minerva calendar's occurrences: what the scheduler answered, or nothing when it did not */
static void
on_minerva_read (JsonObject *root, const char *err, gpointer data)
{
  MinervaRead *read = data;
  Query *query = read->query;

  if (!err && root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    {
      g_autoptr(GPtrArray) events = cal_minerva_events_of (root, read->color, &read->store->minerva_history);

      for (guint i = 0; i < events->len; i++)
        g_ptr_array_add (query->events, cal_event_ref (events->pdata[i]));
    }
  else
    g_warning ("the Minerva calendar: %s", err ? err : ns_json_str (root, "error"));
  if (--query->pending == 0)
    query_finish (query);
  g_free (read);
}

static gboolean
query_finish_idle (gpointer data)
{
  query_finish (data);
  return G_SOURCE_REMOVE;
}

void
cal_store_query (CalStore *self, gint64 start, gint64 end, GCancellable *cancellable, CalQueryDone done, gpointer data)
{
  Query *query = g_new0 (Query, 1);

  query->events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  query->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
  query->done = done;
  query->data = data;
  for (guint i = 0; i < self->calendars->len; i++)
    {
      const CalCalendar *calendar = self->calendars->pdata[i];
      ECalClient *client = cal_store_get_client (self, calendar->uid);
      g_autoptr(GTask) task = NULL;
      Reading *reading;

      if (calendar->visible && g_strcmp0 (calendar->uid, CAL_MINERVA_CALENDAR) == 0)
        {
          g_autofree char *from = iso_of (start), *to = iso_of (end);
          const char *args[] = { "schedule", "range", from, to, "--json", NULL };
          MinervaRead *read = g_new0 (MinervaRead, 1);

          read->store = self;
          read->query = query;
          read->color = calendar->color;
          ns_mc_run (G_OBJECT (self), args, NULL, on_minerva_read, read);
          query->pending++;
          continue;
        }
      if (!calendar->visible || !client)
        continue;
      reading = g_new0 (Reading, 1);
      reading->client = g_object_ref (client);
      reading->calendar = calendar_copy (calendar);
      reading->start = start;
      reading->end = end;
      task = g_task_new (self, query->cancellable, on_read, query);
      g_task_set_task_data (task, reading, (GDestroyNotify) reading_free);
      g_task_run_in_thread (task, read_thread);
      query->pending++;
    }
  if (!query->pending)
    g_idle_add (query_finish_idle, query);
}

/* --- one event */

typedef struct {
  ECalClient *client;
  CalCalendar *calendar;
  char *uid, *rid;
  gint64 near;
  CalEvent *found;
} Finding;

static void
finding_free (Finding *finding)
{
  g_object_unref (finding->client);
  cal_calendar_free (finding->calendar);
  g_free (finding->uid);
  g_free (finding->rid);
  g_clear_pointer (&finding->found, cal_event_unref);
  g_free (finding);
}

static gboolean
on_found_instance (ICalComponent *icomp, ICalTime *start, ICalTime *end, gpointer data, GCancellable *cancellable,
                   GError **error)
{
  Finding *finding = data;
  g_autoptr(ECalComponent) comp = e_cal_component_new_from_icalcomponent (i_cal_component_clone (icomp));
  CalEvent *event;

  if (!comp)
    return TRUE;
  event = cal_event_from_component (comp, finding->calendar, start, end);
  /* the same occurrence (its RECURRENCE-ID), else the one nearest to where it was */
  if (g_strcmp0 (event->rid, finding->rid) == 0
      || (!finding->found
          || (g_strcmp0 (finding->found->rid, finding->rid) != 0
              && ABS (event->start - finding->near) < ABS (finding->found->start - finding->near))))
    {
      g_clear_pointer (&finding->found, cal_event_unref);
      finding->found = event;
    }
  else
    cal_event_unref (event);
  return TRUE;
}

static void
find_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Finding *finding = data;

  /* a year either way: an event moved by its editor is still found */
  e_cal_client_generate_instances_for_uid_sync (finding->client, finding->uid, finding->near - 366 * 86400,
                                                finding->near + 366 * 86400, cancellable, on_found_instance,
                                                finding);
  if (finding->found)
    g_task_return_pointer (task, cal_event_ref (finding->found), (GDestroyNotify) cal_event_unref);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", _("Event not found."));
}

typedef struct {
  GTask *task;
  char *uid, *rid;
  guint32 color;
} MinervaFind;

static void
on_minerva_found (JsonObject *root, const char *err, gpointer data)
{
  MinervaFind *find = data;
  CalEvent *found = NULL;

  if (!err && root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    {
      g_autoptr(GPtrArray) events = cal_minerva_events_of (root, find->color, NULL);

      for (guint i = 0; !found && i < events->len; i++)
        {
          CalEvent *e = events->pdata[i];

          if (g_strcmp0 (e->uid, find->uid) == 0 && (!find->rid || g_strcmp0 (e->rid, find->rid) == 0))
            found = cal_event_ref (e);
        }
    }
  if (found)
    g_task_return_pointer (find->task, found, (GDestroyNotify) cal_event_unref);
  else
    g_task_return_new_error (find->task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", _("Event not found."));
  g_object_unref (find->task);
  g_free (find->uid);
  g_free (find->rid);
  g_free (find);
}

void
cal_store_find_event (CalStore *self, const char *calendar, const char *uid, const char *rid, gint64 near,
                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new (self, cancellable, callback, data);
  ECalClient *client = cal_store_get_client (self, calendar);
  const CalCalendar *info = cal_store_find_calendar (self, calendar);
  Finding *finding;

  if (g_strcmp0 (calendar, CAL_MINERVA_CALENDAR) == 0)
    {
      /* the scheduler again around @near: the same task and due time (or run) */
      g_autofree char *from = iso_of (near - 86400), *to = iso_of (near + 86400 * 2);
      const char *args[] = { "schedule", "range", from, to, "--json", NULL };
      MinervaFind *find = g_new0 (MinervaFind, 1);

      find->task = g_object_ref (task);
      find->uid = g_strdup (uid);
      find->rid = g_strdup (rid);
      find->color = info ? info->color : 0;
      ns_mc_run (G_OBJECT (self), args, NULL, on_minerva_found, find);
      return;
    }

  if (!client || !info)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", _("Event not found."));
      return;
    }
  finding = g_new0 (Finding, 1);
  finding->client = g_object_ref (client);
  finding->calendar = calendar_copy (info);
  finding->uid = g_strdup (uid);
  finding->rid = g_strdup (rid);
  finding->near = near;
  g_task_set_task_data (task, finding, (GDestroyNotify) finding_free);
  g_task_run_in_thread (task, find_thread);
}

CalEvent *
cal_store_find_event_finish (CalStore *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- writing */

typedef struct {
  ECalClient *client;
  CalEvent *original, *edited;
  CalScope scope;
} Writing;

static void
writing_free (Writing *writing)
{
  g_object_unref (writing->client);
  g_clear_pointer (&writing->original, cal_event_unref);
  g_clear_pointer (&writing->edited, cal_event_unref);
  g_free (writing);
}

static void
set_text (ECalComponent *comp, void (*setter) (ECalComponent *, const ECalComponentText *), const char *value)
{
  ECalComponentText *text = value && *value ? e_cal_component_text_new (value, NULL) : NULL;

  setter (comp, text);
  g_clear_pointer (&text, e_cal_component_text_free);
}

/* DTSTART and DTEND: an all-day event's as DATEs (the end the day after its last), a timed one's in this phone's
 * zone, which goes into the calendar too (a TZID needs its VTIMEZONE) */
static gboolean
set_times (ECalComponent *comp, ECalClient *client, gint64 start, gint64 end, gboolean all_day, const char *tzid,
           GCancellable *cancellable, GError **error)
{
  /* the editor's time zone (EditEventView's), else the app's */
  ICalTimezone *zone = tzid && *tzid ? i_cal_timezone_get_builtin_timezone (tzid) : NULL;

  if (!zone)
    zone = app_zone ();
  g_autoptr(ICalTime) a = NULL, b = NULL;
  ECalComponentDateTime *dt;

  if (all_day)
    {
      g_autoptr(GDateTime) x = g_date_time_new_from_unix_local (start);
      g_autoptr(GDateTime) y = g_date_time_new_from_unix_local (end);

      a = i_cal_time_new_null_time ();
      i_cal_time_set_date (a, g_date_time_get_year (x), g_date_time_get_month (x), g_date_time_get_day_of_month (x));
      i_cal_time_set_is_date (a, TRUE);
      b = i_cal_time_new_null_time ();
      i_cal_time_set_date (b, g_date_time_get_year (y), g_date_time_get_month (y), g_date_time_get_day_of_month (y));
      i_cal_time_set_is_date (b, TRUE);
      dt = e_cal_component_datetime_new (a, NULL);
      e_cal_component_set_dtstart (comp, dt);
      e_cal_component_datetime_free (dt);
      dt = e_cal_component_datetime_new (b, NULL);
      e_cal_component_set_dtend (comp, dt);
      e_cal_component_datetime_free (dt);
      return TRUE;
    }
  if (zone && !e_cal_client_add_timezone_sync (client, zone, cancellable, error))
    return FALSE;
  a = i_cal_time_new_from_timet_with_zone (start, FALSE, zone);
  b = i_cal_time_new_from_timet_with_zone (end, FALSE, zone);
  dt = e_cal_component_datetime_new (a, zone ? i_cal_timezone_get_tzid (zone) : NULL);
  e_cal_component_set_dtstart (comp, dt);
  e_cal_component_datetime_free (dt);
  dt = e_cal_component_datetime_new (b, zone ? i_cal_timezone_get_tzid (zone) : NULL);
  e_cal_component_set_dtend (comp, dt);
  e_cal_component_datetime_free (dt);
  return TRUE;
}

/* the reminders anew: Etar deletes an event's Reminders rows and writes the editor's (EditEventHelper.saveReminders) */
static void
set_reminders (ECalComponent *comp, CalEvent *edited)
{
  /* the alarms Etar does not show (absolute, relative to the end) stay */
  GSList *uids = e_cal_component_get_alarm_uids (comp);

  for (GSList *l = uids; l; l = l->next)
    {
      ECalComponentAlarm *alarm = e_cal_component_get_alarm (comp, l->data);
      ECalComponentAlarmTrigger *trigger = alarm ? e_cal_component_alarm_get_trigger (alarm) : NULL;
      ECalComponentAlarmAction action = alarm ? e_cal_component_alarm_get_action (alarm) : 0;

      if (trigger && e_cal_component_alarm_trigger_get_kind (trigger) == E_CAL_COMPONENT_ALARM_TRIGGER_RELATIVE_START
          && (action == E_CAL_COMPONENT_ALARM_DISPLAY || action == E_CAL_COMPONENT_ALARM_EMAIL
              || action == E_CAL_COMPONENT_ALARM_AUDIO))
        e_cal_component_remove_alarm (comp, l->data);
      g_clear_pointer (&alarm, e_cal_component_alarm_free);
    }
  g_slist_free_full (uids, g_free);

  for (guint i = 0; i < edited->reminders->len; i++)
    {
      CalReminder *reminder = &g_array_index (edited->reminders, CalReminder, i);
      ECalComponentAlarm *alarm = e_cal_component_alarm_new ();
      g_autoptr(ICalDuration) before = i_cal_duration_new_from_int (-reminder->minutes * 60);

      e_cal_component_alarm_set_action (alarm, reminder->method == CAL_METHOD_EMAIL ? E_CAL_COMPONENT_ALARM_EMAIL
                                               : reminder->method == CAL_METHOD_ALARM ? E_CAL_COMPONENT_ALARM_AUDIO
                                               : E_CAL_COMPONENT_ALARM_DISPLAY);
      e_cal_component_alarm_take_trigger (alarm, e_cal_component_alarm_trigger_new_relative (
                                                   E_CAL_COMPONENT_ALARM_TRIGGER_RELATIVE_START, before));
      /* RFC 5545 wants a DESCRIPTION in a DISPLAY alarm */
      e_cal_component_alarm_take_description (alarm, e_cal_component_text_new (edited->title ? edited->title : "",
                                                                                NULL));
      e_cal_component_add_alarm (comp, alarm);
      e_cal_component_alarm_free (alarm);
    }
}

static gboolean
same_reminders (GArray *a, GArray *b)
{
  if (!a || !b)
    return (!a || !a->len) && (!b || !b->len);
  if (a->len != b->len)
    return FALSE;
  for (guint i = 0; i < a->len; i++)
    if (g_array_index (a, CalReminder, i).minutes != g_array_index (b, CalReminder, i).minutes
        || g_array_index (a, CalReminder, i).method != g_array_index (b, CalReminder, i).method)
      return FALSE;
  return TRUE;
}

static void
set_rrule (ECalComponent *comp, const char *rrule)
{
  ICalComponent *icomp = e_cal_component_get_icalcomponent (comp);
  ICalProperty *prop;

  while ((prop = i_cal_component_get_first_property (icomp, I_CAL_RRULE_PROPERTY)))
    {
      i_cal_component_remove_property (icomp, prop);
      g_object_unref (prop);
    }
  if (rrule && *rrule)
    {
      g_autoptr(ICalRecurrence) recurrence = i_cal_recurrence_new_from_string (rrule);

      if (recurrence)
        i_cal_component_take_property (icomp, i_cal_property_new_rrule (recurrence));
    }
}

static void
set_color (ECalComponent *comp, CalEvent *original, CalEvent *edited)
{
  ICalComponent *icomp = e_cal_component_get_icalcomponent (comp);
  g_autoptr(ICalProperty) prop = i_cal_component_get_first_property (icomp, I_CAL_COLOR_PROPERTY);
  char text[8];

  /* only a colour chosen is the event's own; else it keeps following its calendar's. "Set to default calendar
   * color" (own_color off) takes the event's own away (EditEventHelper: EVENT_COLOR_KEY NO_EVENT_COLOR) */
  if (original ? edited->own_color == original->own_color && (!edited->own_color || edited->color == original->color)
               : !edited->own_color)
    return;
  if (!edited->own_color)
    {
      if (prop)
        i_cal_component_remove_property (icomp, prop);
      return;
    }
  g_snprintf (text, sizeof text, "#%06x", edited->color & 0xffffff);
  if (prop)
    i_cal_property_set_color (prop, text);
  else
    i_cal_component_take_property (icomp, i_cal_property_new_color (text));
}

static gboolean
same_guests (GPtrArray *a, GPtrArray *b)
{
  guint na = a ? a->len : 0, nb = b ? b->len : 0;

  if (na != nb)
    return FALSE;
  for (guint i = 0; i < na; i++)
    if (g_ascii_strcasecmp (((CalAttendee *) a->pdata[i])->email, ((CalAttendee *) b->pdata[i])->email) != 0)
      return FALSE;
  return TRUE;
}

/* The guests as the editor left them. Etar writes Attendees rows and its sync adapter invites them; nothing here
 * invites anyone, and nothing may: each ATTENDEE says SCHEDULE-AGENT=CLIENT (RFC 6638: a CalDAV server sends no
 * invitation for it, the client would) and no ORGANIZER is added, which is what server-side scheduling starts
 * from. Guests already there keep their properties (their status, their name). */
static void
set_attendees (ECalComponent *comp, CalEvent *edited)
{
  GSList *old = e_cal_component_get_attendees (comp), *list = NULL;

  for (guint i = 0; i < edited->attendees->len; i++)
    {
      CalAttendee *guest = edited->attendees->pdata[i];
      g_autofree char *value = g_strconcat ("mailto:", guest->email, NULL);
      ECalComponentAttendee *attendee = NULL;
      ICalParameter *agent;

      /* (an organizer in the list is one with an ATTENDEE line of its own: kept like any other) */
      for (GSList *l = old; l && !attendee; l = l->next)
        {
          const char *v = e_cal_component_attendee_get_value (l->data);

          if (v && g_ascii_strcasecmp (g_ascii_strncasecmp (v, "mailto:", 7) == 0 ? v + 7 : v, guest->email) == 0)
            attendee = e_cal_component_attendee_copy (l->data);
        }
      if (!attendee)
        {
          attendee = e_cal_component_attendee_new ();
          e_cal_component_attendee_set_value (attendee, value);
          e_cal_component_attendee_set_cutype (attendee, I_CAL_CUTYPE_INDIVIDUAL);
          e_cal_component_attendee_set_role (attendee, I_CAL_ROLE_REQPARTICIPANT);
          e_cal_component_attendee_set_partstat (attendee, I_CAL_PARTSTAT_NEEDSACTION);
          if (guest->name)
            e_cal_component_attendee_set_cn (attendee, guest->name);
        }
      e_cal_component_parameter_bag_remove_by_kind (e_cal_component_attendee_get_parameter_bag (attendee),
                                                    I_CAL_SCHEDULEAGENT_PARAMETER, TRUE);
      agent = i_cal_parameter_new_scheduleagent (I_CAL_SCHEDULEAGENT_CLIENT);
      e_cal_component_parameter_bag_take (e_cal_component_attendee_get_parameter_bag (attendee), agent);
      list = g_slist_prepend (list, attendee);
    }
  list = g_slist_reverse (list);
  e_cal_component_set_attendees (comp, list);
  g_slist_free_full (list, e_cal_component_attendee_free);
  g_slist_free_full (old, e_cal_component_attendee_free);
}

static gboolean
apply_edit (ECalComponent *comp, ECalClient *client, CalEvent *original, CalEvent *edited, gboolean with_times,
            gboolean with_rrule, GCancellable *cancellable, GError **error)
{
  GSList descriptions = { NULL, NULL };
  ECalComponentText *description = edited->description && *edited->description
                                   ? e_cal_component_text_new (edited->description, NULL) : NULL;

  set_text (comp, e_cal_component_set_summary, edited->title);
  e_cal_component_set_location (comp, edited->location && *edited->location ? edited->location : NULL);
  descriptions.data = description;
  e_cal_component_set_descriptions (comp, description ? &descriptions : NULL);
  g_clear_pointer (&description, e_cal_component_text_free);
  e_cal_component_set_url (comp, edited->url && *edited->url ? edited->url : NULL);
  /* the rule and the reminders only when they were changed: what was written elsewhere (several RRULEs, an
   * alarm's own UID and acknowledgement) stays as it was */
  if (with_rrule && (!original || g_strcmp0 (original->rrule, edited->rrule) != 0))
    set_rrule (comp, edited->rrule);
  if (!original || !same_reminders (original->reminders, edited->reminders))
    set_reminders (comp, edited);
  set_color (comp, original, edited);
  /* availability, privacy and guests, each only when the editor changed it (or on a new event) */
  if (!original || original->availability != edited->availability)
    e_cal_component_set_transparency (comp, edited->availability == CAL_AVAILABILITY_FREE
                                            ? E_CAL_COMPONENT_TRANSP_TRANSPARENT : E_CAL_COMPONENT_TRANSP_OPAQUE);
  if (!original || original->access != edited->access)
    e_cal_component_set_classification (comp, edited->access == CAL_ACCESS_PUBLIC ? E_CAL_COMPONENT_CLASS_PUBLIC
                                              : edited->access == CAL_ACCESS_PRIVATE ? E_CAL_COMPONENT_CLASS_PRIVATE
                                              : edited->access == CAL_ACCESS_CONFIDENTIAL
                                                ? E_CAL_COMPONENT_CLASS_CONFIDENTIAL
                                              : E_CAL_COMPONENT_CLASS_NONE);
  if (original ? !same_guests (original->attendees, edited->attendees) : edited->attendees->len > 0)
    set_attendees (comp, edited);
  if (with_times && !set_times (comp, client, edited->start, edited->end, edited->all_day, edited->tzid,
                                cancellable, error))
    return FALSE;
  return TRUE;
}

/* the event as a new one with its own UID: a new event, or the rest of a series cut at an occurrence */
static gboolean
create (ECalClient *client, ECalComponent *comp, CalEvent *original, CalEvent *edited, GCancellable *cancellable,
        GError **error)
{
  g_autofree char *uid = e_util_generate_uid ();
  g_autofree char *created = NULL;

  e_cal_component_set_uid (comp, uid);
  if (!apply_edit (comp, client, original, edited, TRUE, TRUE, cancellable, error))
    return FALSE;
  return e_cal_client_create_object_sync (client, e_cal_component_get_icalcomponent (comp),
                                          E_CAL_OPERATION_FLAG_NONE, &created, cancellable, error);
}

static void
save_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Writing *w = data;
  CalEvent *original = w->original, *edited = w->edited;
  g_autoptr(GError) error = NULL;
  ICalComponent *icomp = NULL;
  g_autoptr(ECalComponent) comp = NULL;
  gboolean times_changed, ok;

  if (!original)
    {
      comp = e_cal_component_new ();
      e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
      ok = create (w->client, comp, NULL, edited, cancellable, &error);
      goto done;
    }

  times_changed = original->start != edited->start || original->end != edited->end
                  || original->all_day != edited->all_day
                  || (!edited->all_day && g_strcmp0 (original->tzid, edited->tzid) != 0);

  if (!original->recurring || w->scope == CAL_SCOPE_ALL)
    {
      /* the event, or the series it repeats: its master */
      if (!e_cal_client_get_object_sync (w->client, original->uid, NULL, &icomp, cancellable, &error))
        {
          ok = FALSE;
          goto done;
        }
      comp = e_cal_component_new_from_icalcomponent (icomp);
      if (original->recurring && times_changed)
        {
          /* the series moves as the occurrence moved: by the same amount, to the new length (EditEventHelper:
           * "the user changed the time of an occurrence and chose all events") */
          ECalComponentDateTime *dtstart = e_cal_component_get_dtstart (comp);
          gint64 master = dtstart ? cal_ical_time_to_unix (e_cal_component_datetime_get_value (dtstart))
                                  : original->start;
          CalEvent shifted = *edited;

          g_clear_pointer (&dtstart, e_cal_component_datetime_free);
          shifted.start = master + (edited->start - original->start);
          shifted.end = shifted.start + (edited->end - edited->start);
          if (!set_times (comp, w->client, shifted.start, shifted.end, edited->all_day, edited->tzid, cancellable,
                          &error))
            {
              ok = FALSE;
              goto done;
            }
        }
      /* times the editor did not touch stay as they were written, in their own zone */
      ok = apply_edit (comp, w->client, original, edited, !original->recurring && times_changed, TRUE, cancellable,
                       &error);
      if (ok)
        {
          e_cal_component_commit_sequence (comp);
          ok = e_cal_client_modify_object_sync (w->client, e_cal_component_get_icalcomponent (comp),
                                                original->recurring ? E_CAL_OBJ_MOD_ALL : E_CAL_OBJ_MOD_THIS,
                                                E_CAL_OPERATION_FLAG_NONE, cancellable, &error);
        }
    }
  else if (w->scope == CAL_SCOPE_THIS)
    {
      /* one occurrence becomes an exception of its own (RECURRENCE-ID), the series stays */
      if (!e_cal_client_get_object_sync (w->client, original->uid, original->rid, &icomp, cancellable, &error))
        {
          ok = FALSE;
          goto done;
        }
      comp = e_cal_component_new_from_icalcomponent (icomp);
      /* an exception repeats nothing itself */
      set_rrule (comp, NULL);
      ok = apply_edit (comp, w->client, original, edited, times_changed, FALSE, cancellable, &error);
      if (ok)
        {
          e_cal_component_commit_sequence (comp);
          ok = e_cal_client_modify_object_sync (w->client, e_cal_component_get_icalcomponent (comp),
                                                E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE, cancellable, &error);
        }
    }
  else
    {
      /* "this and future": the series ends before this occurrence, a new one starts from it with the changes, as
       * Android's CalendarProvider splits a series */
      ICalProperty *prop;

      if (!e_cal_client_get_object_sync (w->client, original->uid, NULL, &icomp, cancellable, &error))
        {
          ok = FALSE;
          goto done;
        }
      comp = e_cal_component_new_from_icalcomponent (i_cal_component_clone (icomp));
      g_object_unref (icomp);
      /* the new series: none of the old one's exceptions */
      while ((prop = i_cal_component_get_first_property (e_cal_component_get_icalcomponent (comp),
                                                         I_CAL_EXDATE_PROPERTY)))
        {
          i_cal_component_remove_property (e_cal_component_get_icalcomponent (comp), prop);
          g_object_unref (prop);
        }
      ok = e_cal_client_remove_object_sync (w->client, original->uid, original->rid, E_CAL_OBJ_MOD_THIS_AND_FUTURE,
                                            E_CAL_OPERATION_FLAG_NONE, cancellable, &error)
           && create (w->client, comp, original, edited, cancellable, &error);
    }

done:
  if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
cal_store_save (CalStore *self, CalEvent *original, CalEvent *edited, CalScope scope, GAsyncReadyCallback callback,
                gpointer data)
{
  g_autoptr(GTask) task = g_task_new (self, NULL, callback, data);
  ECalClient *client = cal_store_get_client (self, edited->calendar);
  Writing *writing;

  if (g_strcmp0 (edited->calendar, CAL_MINERVA_CALENDAR) == 0)
    {
      /* to the scheduler: a new task, or the same task changed (its id and history stay) */
      const char *kind = edited->minerva && edited->minerva->kind ? edited->minerva->kind : "prompt";
      g_autofree char *entry = cal_minerva_entry (edited, kind);
      const char *task_id = original && original->minerva ? original->minerva->task : NULL;
      const char *args[] = { "schedule", task_id ? "update" : "create", task_id, "--json", NULL };

      if (!task_id)
        args[2] = "--json", args[3] = NULL;
      if (!entry)
        {
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s",
                                   ns_json_str (self->minerva_ui, "bad_repeat"));
          return;
        }
      ns_mc_run (G_OBJECT (self), args, entry, on_minerva_written, g_object_ref (task));
      return;
    }
  if (!client || e_client_is_readonly (E_CLIENT (client)))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "calendar %s cannot be written",
                               edited->calendar);
      return;
    }
  writing = g_new0 (Writing, 1);
  writing->client = g_object_ref (client);
  writing->original = original ? cal_event_ref (original) : NULL;
  writing->edited = cal_event_ref (edited);
  writing->scope = scope;
  g_task_set_task_data (task, writing, (GDestroyNotify) writing_free);
  g_task_run_in_thread (task, save_thread);
}

gboolean
cal_store_save_finish (CalStore *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
delete_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Writing *w = data;
  CalEvent *event = w->original;
  g_autoptr(GError) error = NULL;
  gboolean ok;

  if (!event->recurring || w->scope == CAL_SCOPE_ALL)
    ok = e_cal_client_remove_object_sync (w->client, event->uid, NULL, E_CAL_OBJ_MOD_ALL, E_CAL_OPERATION_FLAG_NONE,
                                          cancellable, &error);
  else
    ok = e_cal_client_remove_object_sync (w->client, event->uid, event->rid,
                                          w->scope == CAL_SCOPE_THIS ? E_CAL_OBJ_MOD_THIS
                                                                     : E_CAL_OBJ_MOD_THIS_AND_FUTURE,
                                          E_CAL_OPERATION_FLAG_NONE, cancellable, &error);
  if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
cal_store_delete (CalStore *self, CalEvent *event, CalScope scope, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new (self, NULL, callback, data);
  ECalClient *client = cal_store_get_client (self, event->calendar);
  Writing *writing;

  if (g_strcmp0 (event->calendar, CAL_MINERVA_CALENDAR) == 0 && event->minerva)
    {
      /* a task is cancelled whole: the scheduler has no single due time to drop */
      const char *args[] = { "schedule", "cancel", event->minerva->task, "--json", NULL };

      ns_mc_run (G_OBJECT (self), args, NULL, on_minerva_written, g_object_ref (task));
      return;
    }
  if (!client || e_client_is_readonly (E_CLIENT (client)))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "calendar %s cannot be written",
                               event->calendar);
      return;
    }
  writing = g_new0 (Writing, 1);
  writing->client = g_object_ref (client);
  writing->original = cal_event_ref (event);
  writing->scope = scope;
  g_task_set_task_data (task, writing, (GDestroyNotify) writing_free);
  g_task_run_in_thread (task, delete_thread);
}

gboolean
cal_store_delete_finish (CalStore *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

char *
cal_store_export_sync (CalStore *self, CalEvent *event, GError **error)
{
  ECalClient *client = cal_store_get_client (self, event->calendar);
  ICalComponent *icomp = NULL;
  char *text;

  if (!client)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", _("Event not found."));
      return NULL;
    }
  /* the series as written (its master), in a VCALENDAR with the time zones it names */
  if (!e_cal_client_get_object_sync (client, event->uid, NULL, &icomp, NULL, error))
    return NULL;
  text = e_cal_client_get_component_as_string (client, icomp);
  g_object_unref (icomp);
  return text;
}
