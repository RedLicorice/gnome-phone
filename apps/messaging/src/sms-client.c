/* The app's side of the SMS service (sms-client.h). */
#include "sms-client.h"
#include "messaging-types.h"

struct _SmsClient {
  GObject parent;
  GDBusConnection *bus;
  guint changed_signal, owner_watch;
  SmsStore *store;
  char *store_path;
  gboolean available;
  char *country;
};

G_DEFINE_FINAL_TYPE (SmsClient, sms_client, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

enum { PROP_0, PROP_AVAILABLE, PROP_COUNTRY, N_PROPS };
static GParamSpec *props[N_PROPS];

SmsStore *
sms_client_get_store (SmsClient *self)
{
  /* the service makes it on its first start: until then there is nothing to read */
  if (!self->store && g_file_test (self->store_path, G_FILE_TEST_EXISTS))
    {
      g_autoptr(GError) error = NULL;

      self->store = sms_store_open (self->store_path, TRUE, &error);
      if (!self->store)
        g_warning ("messages: %s", error->message);
    }
  return self->store;
}

gboolean
sms_client_get_available (SmsClient *self)
{
  return self->available;
}

const char *
sms_client_get_country (SmsClient *self)
{
  return self->country && *self->country ? self->country : NULL;
}

/* --- asking the service */

static void
on_called (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

  if (!reply)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("messages: %s: %s", (const char *) data, error->message);
    }
}

static void
call (SmsClient *self, const char *method, GVariant *parameters)
{
  if (!self->bus)
    return;
  g_dbus_connection_call (self->bus, SMS_BUS_NAME, SMS_PATH, SMS_INTERFACE, method, parameters, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_called, (gpointer) method);
}

gint64
sms_client_get_conversation (SmsClient *self, const char * const *numbers)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  gint64 id = 0;

  if (!self->bus)
    return 0;
  /* synchronous: the app opens the conversation it gets, and the service answers from its store at once */
  reply = g_dbus_connection_call_sync (self->bus, SMS_BUS_NAME, SMS_PATH, SMS_INTERFACE, "GetConversation",
                                       g_variant_new ("(^as)", numbers), G_VARIANT_TYPE ("(x)"),
                                       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  if (!reply)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("messages: GetConversation: %s", error->message);
      return 0;
    }
  g_variant_get (reply, "(x)", &id);
  return id;
}

void
sms_client_send (SmsClient *self, gint64 conversation, const char *text)
{
  call (self, "Send", g_variant_new ("(xs)", conversation, text));
}

void
sms_client_resend (SmsClient *self, gint64 message)
{
  call (self, "Resend", g_variant_new ("(x)", message));
}

void
sms_client_mark_read (SmsClient *self, gint64 conversation)
{
  call (self, "MarkRead", g_variant_new ("(x)", conversation));
}

void
sms_client_set_archived (SmsClient *self, gint64 conversation, gboolean archived)
{
  call (self, "SetArchived", g_variant_new ("(xb)", conversation, archived));
}

void
sms_client_set_draft (SmsClient *self, gint64 conversation, const char *text)
{
  call (self, "SetDraft", g_variant_new ("(xs)", conversation, text ? text : ""));
}

void
sms_client_delete_conversation (SmsClient *self, gint64 conversation)
{
  call (self, "DeleteConversation", g_variant_new ("(x)", conversation));
}

void
sms_client_fetch_media (SmsClient *self, gint64 message)
{
  call (self, "FetchMedia", g_variant_new ("(x)", message));
}

void
sms_client_delete_message (SmsClient *self, gint64 message)
{
  call (self, "DeleteMessage", g_variant_new ("(x)", message));
}

void
sms_client_set_visible (SmsClient *self, gint64 conversation)
{
  call (self, "SetVisible", g_variant_new ("(x)", conversation));
}

/* --- the service's state */

static void
take_properties (SmsClient *self, GVariant *properties)
{
  gboolean available;
  const char *country;

  if (g_variant_lookup (properties, "Available", "b", &available) && available != self->available)
    {
      self->available = available;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
    }
  if (g_variant_lookup (properties, "Country", "&s", &country) && g_strcmp0 (country, self->country) != 0)
    {
      g_free (self->country);
      self->country = g_strdup (country);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COUNTRY]);
    }
}

static void
on_properties (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(SmsClient) self = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autoptr(GVariant) properties = NULL;

  if (!reply)
    return;
  g_variant_get (reply, "(@a{sv})", &properties);
  take_properties (self, properties);
}

static void
on_signal (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
           GVariant *parameters, gpointer data)
{
  SmsClient *self = data;

  if (g_str_equal (signal, "Changed"))
    {
      gint64 conversation;

      g_variant_get (parameters, "(x)", &conversation);
      sms_client_get_store (self);
      g_signal_emit (self, signals[CHANGED], 0, conversation);
    }
  else if (g_str_equal (signal, "PropertiesChanged"))
    {
      const char *name;
      g_autoptr(GVariant) properties = NULL;

      g_variant_get (parameters, "(&s@a{sv}as)", &name, &properties, NULL);
      if (g_str_equal (name, SMS_INTERFACE))
        take_properties (self, properties);
    }
}

static void
on_service_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  SmsClient *self = data;

  /* it may have made the store just now; everything may have changed while it was away */
  sms_client_get_store (self);
  g_dbus_connection_call (bus, SMS_BUS_NAME, SMS_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                          g_variant_new ("(s)", SMS_INTERFACE), G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                          -1, NULL, on_properties, g_object_ref (self));
  g_signal_emit (self, signals[CHANGED], 0, (gint64) 0);
}

static void
on_service_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  SmsClient *self = data;

  if (self->available)
    {
      self->available = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
    }
}

static void
sms_client_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  SmsClient *self = SMS_CLIENT (object);

  switch (id)
    {
    case PROP_AVAILABLE:
      g_value_set_boolean (value, self->available);
      break;
    case PROP_COUNTRY:
      g_value_set_string (value, sms_client_get_country (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
sms_client_dispose (GObject *object)
{
  SmsClient *self = SMS_CLIENT (object);

  if (self->changed_signal)
    g_dbus_connection_signal_unsubscribe (self->bus, self->changed_signal);
  self->changed_signal = 0;
  if (self->owner_watch)
    g_bus_unwatch_name (self->owner_watch);
  self->owner_watch = 0;
  g_clear_object (&self->bus);
  g_clear_pointer (&self->store, sms_store_free);
  G_OBJECT_CLASS (sms_client_parent_class)->dispose (object);
}

static void
sms_client_finalize (GObject *object)
{
  SmsClient *self = SMS_CLIENT (object);

  g_free (self->store_path);
  g_free (self->country);
  G_OBJECT_CLASS (sms_client_parent_class)->finalize (object);
}

static void
sms_client_class_init (SmsClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = sms_client_get_property;
  object_class->dispose = sms_client_dispose;
  object_class->finalize = sms_client_finalize;
  props[PROP_AVAILABLE] = g_param_spec_boolean ("available", NULL, NULL, FALSE,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_COUNTRY] = g_param_spec_string ("country", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void
sms_client_init (SmsClient *self)
{
  g_autoptr(GError) error = NULL;

  self->store_path = g_build_filename (g_get_user_data_dir (), "neo-messaging", "bugle_db", NULL);
  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!self->bus)
    {
      g_warning ("messages: no session bus: %s", error->message);
      return;
    }
  /* both the service's own signal and its properties' */
  self->changed_signal = g_dbus_connection_signal_subscribe (self->bus, SMS_BUS_NAME, NULL, NULL, SMS_PATH, NULL,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, on_signal, self, NULL);
  /* starts it if it does not run (it runs from login, so this is for a crash or a first run) */
  self->owner_watch = g_bus_watch_name_on_connection (self->bus, SMS_BUS_NAME,
                                                      G_BUS_NAME_WATCHER_FLAGS_AUTO_START, on_service_appeared,
                                                      on_service_vanished, self, NULL);
}

SmsClient *
sms_client_new (void)
{
  return g_object_new (SMS_TYPE_CLIENT, NULL);
}
