/* The earpiece by proximity for a voice note (msg-earpiece.h).
 *
 * The earpiece has no route of its own outside the phone's call profile (the card's UCM "Voice Call" verb): the note
 * uses the dialer's own way there, callaudiod's call mode with the speaker off (apps/dialer/src/calls.c), and only
 * when callaudiod is in its default mode and ModemManager has no call, so a call is never touched. */
#include "msg-earpiece.h"

#include <gio/gio.h>

#include "sms-provider.h"

#define CALLAUDIO_NAME   "org.mobian_project.CallAudio"
#define CALLAUDIO_PATH   "/org/mobian_project/CallAudio"
#define SENSOR_NAME      "net.hadess.SensorProxy"
#define SENSOR_PATH      "/net/hadess/SensorProxy"
#define MM_NAME          "org.freedesktop.ModemManager1"
#define MM_PATH          "/org/freedesktop/ModemManager1"
#define MM_CALL          "org.freedesktop.ModemManager1.Call"
#define OBJECT_MANAGER   "org.freedesktop.DBus.ObjectManager"
/* callaudiod's CallAudioMode */
#define CALL_AUDIO_MODE_DEFAULT 0
#define CALL_AUDIO_MODE_CALL    1
/* the switch: messaging.conf [media] */
#define SWITCH_SECTION "media"
#define SWITCH_KEY     "earpiece_by_proximity"

static struct
{
  GDBusConnection *system, *session;
  GDBusProxy *sensor;
  gulong changed;
  guint calls;
  gboolean held;                /* the sound is on the earpiece because of a note */
  GCallback stop;
  gpointer data;
} state;

/* whether the a{s...} @dict has @key */
static gboolean
has (GVariant *dict, const char *key)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (dict, key, NULL);

  return value != NULL;
}

static gboolean
enabled (void)
{
  g_autofree char *value = sms_provider_value (SWITCH_SECTION, SWITCH_KEY);

  return value && (g_ascii_strcasecmp (value, "true") == 0 || g_strcmp0 (value, "1") == 0);
}

/* the system bus, or for a try on the phone one of its own with a sensor of its own (tests/try-earpiece.py) */
static GDBusConnection *
system_bus (void)
{
  const char *address = g_getenv ("NEO_MESSAGING_SYSTEM_BUS");

  if (address && *address)
    return g_dbus_connection_new_for_address_sync (address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                            | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                   NULL, NULL, NULL);
  return g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
}

static void
call_audio (const char *method, GVariant *parameters)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync (state.session, CALLAUDIO_NAME, CALLAUDIO_PATH,
                                                           CALLAUDIO_NAME, method, parameters, G_VARIANT_TYPE ("(b)"),
                                                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

  if (!reply)
    g_debug ("callaudiod %s: no answer", method);
}

/* callaudiod's mode now: not the default one means a call (or someone else) has the sound */
static guint
audio_mode (void)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync (state.session, CALLAUDIO_NAME, CALLAUDIO_PATH,
                                                           "org.freedesktop.DBus.Properties", "Get",
                                                           g_variant_new ("(ss)", CALLAUDIO_NAME, "AudioMode"),
                                                           G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                           NULL);
  g_autoptr(GVariant) value = NULL;

  if (!reply)
    return G_MAXUINT;
  g_variant_get (reply, "(v)", &value);
  return g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32 (value) : G_MAXUINT;
}

/* whether ModemManager has a call, ringing or not; no ModemManager: none */
static gboolean
in_call (void)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync (state.system, MM_NAME, MM_PATH, OBJECT_MANAGER,
                                                           "GetManagedObjects", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                                                           -1, NULL, NULL);
  g_autoptr(GVariantIter) objects = NULL;
  GVariant *interfaces;
  gboolean call = FALSE;

  if (!reply)
    return FALSE;
  g_variant_get (reply, "(a{oa{sa{sv}}})", &objects);
  while (!call && g_variant_iter_next (objects, "{&o@a{sa{sv}}}", NULL, &interfaces))
    {
      g_autoptr(GVariant) owned = interfaces;

      call = has (owned, MM_CALL);
    }
  return call;
}

static void
release (void)
{
  if (!state.held)
    return;
  state.held = FALSE;
  /* a call that came meanwhile keeps its call mode: the dialer leaves it when the call is over */
  if (!in_call ())
    call_audio ("SelectMode", g_variant_new ("(u)", CALL_AUDIO_MODE_DEFAULT));
}

static void
follow (gboolean near)
{
  if (near && !state.held && audio_mode () == CALL_AUDIO_MODE_DEFAULT && !in_call ())
    {
      state.held = TRUE;
      call_audio ("SelectMode", g_variant_new ("(u)", CALL_AUDIO_MODE_CALL));
      call_audio ("EnableSpeaker", g_variant_new ("(b)", FALSE));
    }
  else if (!near)
    release ();
}

static void
sensor_call (const char *method)
{
  g_autoptr(GVariant) reply = g_dbus_proxy_call_sync (state.sensor, method, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                      NULL);

  if (!reply)
    g_debug ("the proximity sensor: %s did not answer", method);
}

static gboolean
is_near (void)
{
  g_autoptr(GVariant) near = state.sensor ? g_dbus_proxy_get_cached_property (state.sensor, "ProximityNear") : NULL;

  return near && g_variant_get_boolean (near);
}

static void
on_sensor (GDBusProxy *sensor, GVariant *changed, GStrv invalidated, gpointer data)
{
  if (has (changed, "ProximityNear"))
    follow (is_near ());
}

/* a call came while the note was at the ear: the ringtone (and the call) have the sound, the note stops */
static void
on_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
         GVariant *parameters, gpointer data)
{
  g_autoptr(GVariant) interfaces = g_variant_get_child_value (parameters, 1);
  void (*stop) (gpointer) = (void (*) (gpointer)) state.stop;

  if (!has (interfaces, MM_CALL))
    return;
  if (state.held)
    {
      state.held = FALSE;
      call_audio ("SelectMode", g_variant_new ("(u)", CALL_AUDIO_MODE_DEFAULT));
    }
  if (stop)
    stop (state.data);
}

void
msg_earpiece_start (GCallback stop, gpointer data)
{
  g_autoptr(GVariant) proximity = NULL;

  state.stop = stop;
  state.data = data;
  if (state.sensor || !enabled ())
    return;
  if (!state.session)
    state.session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!state.system)
    state.system = system_bus ();
  if (!state.system || !state.session)
    return;
  state.sensor = g_dbus_proxy_new_sync (state.system, G_DBUS_PROXY_FLAGS_NONE, NULL, SENSOR_NAME, SENSOR_PATH,
                                        SENSOR_NAME, NULL, NULL);
  proximity = state.sensor ? g_dbus_proxy_get_cached_property (state.sensor, "HasProximity") : NULL;
  if (!proximity || !g_variant_get_boolean (proximity))
    {
      g_clear_object (&state.sensor);
      return;
    }
  sensor_call ("ClaimProximity");
  state.changed = g_signal_connect (state.sensor, "g-properties-changed", G_CALLBACK (on_sensor), NULL);
  state.calls = g_dbus_connection_signal_subscribe (state.system, MM_NAME, OBJECT_MANAGER, "InterfacesAdded", NULL,
                                                    NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_call, NULL, NULL);
  follow (is_near ());
}

void
msg_earpiece_stop (void)
{
  state.stop = NULL;
  release ();
  if (state.calls)
    g_dbus_connection_signal_unsubscribe (state.system, state.calls);
  state.calls = 0;
  if (!state.sensor)
    return;
  g_clear_signal_handler (&state.changed, state.sensor);
  sensor_call ("ReleaseProximity");
  g_clear_object (&state.sensor);
}
