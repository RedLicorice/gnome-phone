/* The IR remote service client: see nr-ir.h. */
#include "nr-ir.h"

#define IR_NAME  "org.minerva.Ir1"
#define IR_PATH  "/org/minerva/Ir1"
#define IR_IFACE "org.minerva.Ir1"

static int timeout_ms = -1;

typedef struct {
  NrIrDone done;
  gpointer data;
} Call;

void
nr_ir_set_timeout (int ms)
{
  timeout_ms = ms;
}

static void
on_reply (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Call *call = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  g_autoptr(JsonParser) parser = NULL;
  const char *text = NULL;

  if (reply)
    {
      g_variant_get (reply, "(&s)", &text);
      parser = json_parser_new ();
      if (!json_parser_load_from_data (parser, text, -1, &error))
        g_clear_object (&parser);
    }
  if (error)
    g_dbus_error_strip_remote_error (error);
  call->done (parser ? json_parser_get_root (parser) : NULL, error, call->data);
  g_free (call);
}

void
nr_ir_call (const char *method, GVariant *params, NrIrDone done, gpointer data)
{
  Call *call = g_new0 (Call, 1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);   /* cached by GIO */

  call->done = done;
  call->data = data;
  if (!bus)
    {
      if (params)
        g_variant_unref (g_variant_ref_sink (params));
      done (NULL, error, data);
      g_free (call);
      return;
    }
  g_dbus_connection_call (bus, IR_NAME, IR_PATH, IR_IFACE, method, params, G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, on_reply, call);
}
