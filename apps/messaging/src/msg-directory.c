/* The names the providers give their chats (msg-directory.h). */
#include "msg-directory.h"

#include <json-glib/json-glib.h>

#include "sms-provider.h"

#define CHATS_CHANGED "ChatsChanged"

typedef struct {
  char    *name;
  gboolean push;
} Entry;

struct _MsgDirectory {
  GObject          parent_instance;
  GDBusConnection *bus;
  GCancellable    *cancel;
  GHashTable      *names;     /* destination -> Entry, every provider's together */
  GArray          *watches;   /* guint: one name watch per provider */
  GArray          *signals;   /* guint: one ChatsChanged subscription per provider */
};

G_DEFINE_FINAL_TYPE (MsgDirectory, msg_directory, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
entry_free (Entry *e)
{
  g_free (e->name);
  g_free (e);
}

typedef struct {
  MsgDirectory *self;
  char         *id;
} Read;

void
msg_provider_bus (const char *id, char **name, char **path, char **iface)
{
  g_autofree char *element = g_dbus_escape_object_path (id);

  *name = g_strconcat (MSG_PROVIDER_NAME_PREFIX, id, NULL);
  *path = g_strconcat (MSG_PROVIDER_PATH_PREFIX, element, NULL);
  *iface = g_strconcat (MSG_PROVIDER_NAME_PREFIX, element, NULL);
}

static void
read_free (Read *r)
{
  g_object_unref (r->self);
  g_free (r->id);
  g_free (r);
}

/* one provider's answer replaces what was known of its chats */
static void
on_chats (GObject *source, GAsyncResult *res, gpointer data)
{
  Read *r = data;
  MsgDirectory *self = r->self;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autofree char *address = sms_provider_address (r->id);
  const char *json = NULL;
  JsonNode *root;
  JsonArray *chats;
  GHashTableIter iter;
  gpointer key;

  if (!reply || !address)
    {
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("directory of %s not read: %s", r->id, error->message);
      read_free (r);
      return;
    }
  g_variant_get (reply, "(&s)", &json);
  if (!json_parser_load_from_data (parser, json, -1, NULL) || !(root = json_parser_get_root (parser))
      || !JSON_NODE_HOLDS_OBJECT (root)
      || !json_object_has_member (json_node_get_object (root), "chats")
      || !(chats = json_object_get_array_member (json_node_get_object (root), "chats")))
    {
      read_free (r);
      return;
    }
  g_hash_table_iter_init (&iter, self->names);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (g_str_has_prefix (key, address))
      g_hash_table_iter_remove (&iter);
  for (guint i = 0; i < json_array_get_length (chats); i++)
    {
      JsonObject *o = json_array_get_object_element (chats, i);
      const char *chat = o ? json_object_get_string_member_with_default (o, "chat", "") : "";
      const char *name = o ? json_object_get_string_member_with_default (o, "name", "") : "";
      Entry *e;

      if (!*chat || !*name)
        continue;
      e = g_new0 (Entry, 1);
      e->name = g_strdup (name);
      e->push = json_object_get_boolean_member_with_default (o, "push", FALSE);
      g_hash_table_replace (self->names, g_strconcat (address, chat, NULL), e);
    }
  g_signal_emit (self, signals[CHANGED], 0);
  read_free (r);
}

static void
read_provider (MsgDirectory *self, const char *id)
{
  g_autofree char *name = NULL, *path = NULL, *iface = NULL;
  Read *r = g_new0 (Read, 1);

  msg_provider_bus (id, &name, &path, &iface);
  r->self = g_object_ref (self);
  r->id = g_strdup (id);
  g_dbus_connection_call (self->bus, name, path, iface, "Chats", NULL, G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, self->cancel, on_chats, r);
}

static void
on_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  read_provider (MSG_DIRECTORY (data), name + strlen (MSG_PROVIDER_NAME_PREFIX));
}

/* a subscription's provider: its interface carries the escaped id, so the id travels with the subscription */
static void
on_chats_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                  const char *member, GVariant *params, gpointer data)
{
  Read *sub = data;

  read_provider (sub->self, sub->id);
}

static void
sub_free (gpointer data)
{
  Read *sub = data;

  g_free (sub->id);
  g_free (sub);
}

static void
on_bus (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr(MsgDirectory) self = data;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) ids = NULL;

  self->bus = g_bus_get_finish (res, &error);
  if (!self->bus)
    {
      g_debug ("no session bus for the providers' directories: %s", error->message);
      return;
    }
  ids = sms_provider_ids ();
  for (guint i = 0; ids && ids[i]; i++)
    {
      g_autofree char *name = NULL, *path = NULL, *iface = NULL;
      Read *r;
      guint watch, sub;

      if (!sms_provider_flag (ids[i], "directory-names"))
        continue;
      msg_provider_bus (ids[i], &name, &path, &iface);
      /* self outlives the subscription: dispose unsubscribes; the struct only carries the id */
      r = g_new0 (Read, 1);
      r->self = self;
      r->id = g_strdup (ids[i]);
      sub = g_dbus_connection_signal_subscribe (self->bus, NULL, iface, CHATS_CHANGED, path, NULL,
                                                G_DBUS_SIGNAL_FLAGS_NONE, on_chats_changed, r, sub_free);
      watch = g_bus_watch_name_on_connection (self->bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE, on_appeared, NULL,
                                              self, NULL);
      g_array_append_val (self->signals, sub);
      g_array_append_val (self->watches, watch);
    }
}

static void
msg_directory_dispose (GObject *object)
{
  MsgDirectory *self = MSG_DIRECTORY (object);

  g_cancellable_cancel (self->cancel);
  for (guint i = 0; i < self->watches->len; i++)
    g_bus_unwatch_name (g_array_index (self->watches, guint, i));
  for (guint i = 0; self->bus && i < self->signals->len; i++)
    g_dbus_connection_signal_unsubscribe (self->bus, g_array_index (self->signals, guint, i));
  g_array_set_size (self->watches, 0);
  g_array_set_size (self->signals, 0);
  g_clear_object (&self->bus);
  g_clear_object (&self->cancel);
  G_OBJECT_CLASS (msg_directory_parent_class)->dispose (object);
}

static void
msg_directory_finalize (GObject *object)
{
  MsgDirectory *self = MSG_DIRECTORY (object);

  g_hash_table_unref (self->names);
  g_array_unref (self->watches);
  g_array_unref (self->signals);
  G_OBJECT_CLASS (msg_directory_parent_class)->finalize (object);
}

static void
msg_directory_class_init (MsgDirectoryClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = msg_directory_dispose;
  G_OBJECT_CLASS (klass)->finalize = msg_directory_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
msg_directory_init (MsgDirectory *self)
{
  self->cancel = g_cancellable_new ();
  self->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) entry_free);
  self->watches = g_array_new (FALSE, FALSE, sizeof (guint));
  self->signals = g_array_new (FALSE, FALSE, sizeof (guint));
  g_bus_get (G_BUS_TYPE_SESSION, self->cancel, on_bus, g_object_ref (self));
}

MsgDirectory *
msg_directory_get_default (void)
{
  static MsgDirectory *directory;

  if (!directory)
    directory = g_object_new (MSG_TYPE_DIRECTORY, NULL);
  return directory;
}

const char *
msg_directory_name (MsgDirectory *self, const char *destination, gboolean *push)
{
  Entry *e = destination ? g_hash_table_lookup (self->names, destination) : NULL;

  if (push)
    *push = e && e->push;
  return e ? e->name : NULL;
}
