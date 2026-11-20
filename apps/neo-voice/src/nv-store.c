/* Neo Voice's data: see nv-store.h. */
#include "nv-store.h"

#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <string.h>

#define UI_RESOURCE "/de/yesman/NeoVoice/neo-voice-ui.json"

typedef struct {
  NvStore *store;
  NvReply  reply;
  NvClip   clip;
  gpointer data;
} Call;

static JsonObject *
member_object (JsonObject *o, const char *name)
{
  JsonNode *node = o ? json_object_get_member (o, name) : NULL;

  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

/* the first of the user's languages the file has, else conf.default_lang */
static JsonObject *
pick_language (JsonObject *root, JsonObject *conf)
{
  const char * const *langs = g_get_language_names ();

  for (guint i = 0; langs[i]; i++)
    {
      g_autofree char *two = g_strndup (langs[i], 2);
      JsonObject *strings = strlen (two) == 2 ? member_object (root, two) : NULL;

      if (strings)
        return strings;
    }
  return member_object (root, conf && json_object_has_member (conf, "default_lang")
                                ? json_object_get_string_member (conf, "default_lang") : "en");
}

NvStore *
nv_store_new (void)
{
  NvStore *self = g_new0 (NvStore, 1);
  g_autoptr(GBytes) bytes = g_resources_lookup_data (UI_RESOURCE, 0, NULL);
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) error = NULL;

  if (bytes && json_parser_load_from_data (parser, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), &error))
    {
      JsonNode *root = json_parser_get_root (parser);

      if (JSON_NODE_HOLDS_OBJECT (root))
        self->root = json_object_ref (json_node_get_object (root));
    }
  if (!self->root)
    self->root = json_object_new ();
  self->conf = member_object (self->root, "conf");
  self->strings = pick_language (self->root, self->conf);
  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!self->bus)
    g_warning ("no session bus: %s", error ? error->message : "?");
  return self;
}

void
nv_store_free (NvStore *self)
{
  g_clear_object (&self->bus);
  g_clear_pointer (&self->root, json_object_unref);
  g_free (self);
}

const char *
nv_ui (NvStore *self, const char *key)
{
  g_auto(GStrv) parts = g_strsplit (key, ".", -1);
  JsonObject *o = self->strings;

  for (guint i = 0; o && parts[i]; i++)
    {
      JsonNode *node = json_object_get_member (o, parts[i]);

      if (!node)
        return key;
      if (!parts[i + 1])
        return JSON_NODE_HOLDS_VALUE (node) ? json_node_get_string (node) : key;
      o = JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
    }
  return key;
}

gint64
nv_conf_int (NvStore *self, const char *key)
{
  return self->conf && json_object_has_member (self->conf, key) ? json_object_get_int_member (self->conf, key) : 0;
}

double
nv_conf_double (NvStore *self, const char *key)
{
  return self->conf && json_object_has_member (self->conf, key) ? json_object_get_double_member (self->conf, key) : 0;
}

const char *
nv_conf_str (NvStore *self, const char *key)
{
  return self->conf && json_object_has_member (self->conf, key) ? json_object_get_string_member (self->conf, key) : "";
}

GPtrArray *
nv_ui_list (NvStore *self, const char *key)
{
  GPtrArray *out = g_ptr_array_new ();
  JsonArray *array = json_object_has_member (self->root, key) ? json_object_get_array_member (self->root, key) : NULL;

  for (guint i = 0; array && i < json_array_get_length (array); i++)
    g_ptr_array_add (out, (gpointer) json_array_get_string_element (array, i));
  return out;
}

/* ---- the calls ---- */

static Call *
call_new (NvStore *store, NvReply reply, NvClip clip, gpointer data)
{
  Call *c = g_new0 (Call, 1);

  c->store = store;
  c->reply = reply;
  c->clip = clip;
  c->data = data;
  return c;
}

/* the D-Bus error's own text, without the GDBus prefix */
static const char *
error_text (GError *error)
{
  if (g_dbus_error_is_remote_error (error))
    g_dbus_error_strip_remote_error (error);
  return error->message;
}

static void
on_json_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  Call *c = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr(JsonNode) node = NULL;

  if (reply && g_variant_is_of_type (reply, G_VARIANT_TYPE ("(s)")))
    {
      const char *text = NULL;

      g_variant_get (reply, "(&s)", &text);
      node = json_from_string (text, &error);
    }
  else if (reply)
    node = json_node_new (JSON_NODE_NULL);   /* a method with no answer (DetectionDelete) */
  if (c->reply)
    c->reply (c->data, node, error ? error_text (error) : NULL);
  g_free (c);
}

static void
call (NvStore *self, const char *method, GVariant *args, const char *out, Call *c)
{
  if (!self->bus)
    {
      if (c->reply)
        c->reply (c->data, NULL, "no session bus");
      g_free (c);
      if (args)
        g_variant_unref (g_variant_ref_sink (args));
      return;
    }
  g_dbus_connection_call (self->bus, nv_conf_str (self, "bus_name"), nv_conf_str (self, "bus_path"),
                          nv_conf_str (self, "bus_interface"), method, args,
                          out ? G_VARIANT_TYPE (out) : NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          (int) nv_conf_int (self, "call_timeout_ms"), NULL, on_json_reply, c);
}

void
nv_list (NvStore *self, const char *filter_json, NvReply cb, gpointer data)
{
  call (self, "DetectionList", g_variant_new ("(s)", filter_json), "(s)", call_new (self, cb, NULL, data));
}

void
nv_status (NvStore *self, NvReply cb, gpointer data)
{
  call (self, "DetectionStatus", NULL, "(s)", call_new (self, cb, NULL, data));
}

void
nv_label (NvStore *self, const char *id, const char *label, const char *note, NvReply cb, gpointer data)
{
  call (self, "DetectionLabel", g_variant_new ("(sss)", id, label, note ? note : ""), "(s)",
        call_new (self, cb, NULL, data));
}

void
nv_trim (NvStore *self, const char *id, int start_ms, int end_ms, NvReply cb, gpointer data)
{
  call (self, "DetectionTrim", g_variant_new ("(sii)", id, start_ms, end_ms), "(s)", call_new (self, cb, NULL, data));
}

void
nv_delete (NvStore *self, const char *id, NvReply cb, gpointer data)
{
  call (self, "DetectionDelete", g_variant_new ("(s)", id), NULL, call_new (self, cb, NULL, data));
}

/* the clip arrives as a read-only fd (never as data on the bus): read whole, it is a few hundred kB at most */
static void
on_clip_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  Call *c = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source), &fds,
                                                                              result, &error);
  g_autoptr(GBytes) bytes = NULL;

  if (reply && fds && g_unix_fd_list_get_length (fds) > 0)
    {
      gint32 index = 0;
      int fd = -1;

      g_variant_get (reply, "(h)", &index);
      fd = g_unix_fd_list_get (fds, index, &error);
      if (fd >= 0)
        {
          g_autoptr(GInputStream) in = g_unix_input_stream_new (fd, TRUE);
          g_autoptr(GOutputStream) out = g_memory_output_stream_new_resizable ();

          if (g_output_stream_splice (out, in, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &error) >= 0)
            bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
        }
    }
  else if (reply && !error)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "no fd in the reply");
  c->clip (c->data, bytes, error ? error_text (error) : NULL);
  g_free (c);
}

void
nv_clip (NvStore *self, const char *id, NvClip cb, gpointer data)
{
  Call *c = call_new (self, NULL, cb, data);

  if (!self->bus)
    {
      cb (data, NULL, "no session bus");
      g_free (c);
      return;
    }
  g_dbus_connection_call_with_unix_fd_list (self->bus, nv_conf_str (self, "bus_name"), nv_conf_str (self, "bus_path"),
                                            nv_conf_str (self, "bus_interface"), "DetectionClip",
                                            g_variant_new ("(s)", id), G_VARIANT_TYPE ("(h)"),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, (int) nv_conf_int (self, "call_timeout_ms"),
                                            NULL, NULL, on_clip_reply, c);
}
