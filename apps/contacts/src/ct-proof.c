/* The human proof of an action on a contact (ct-proof.h). */
#include "ct-proof.h"

#include <json-glib/json-glib.h>

static const CtProofProvider *provider;

char *
ct_proof_action (CtProofVerb verb, const char *first, const char *second)
{
  switch (verb)
    {
    case CT_PROOF_MERGE:
      return g_strdup_printf ("merge contact %s into %s", first, second ? second : "");
    case CT_PROOF_DELETE:
    default:
      return g_strdup_printf ("delete contact %s", first);
    }
}

char *
ct_proof_json (const char *approver, const char *action)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autofree char *at = g_date_time_format (now, "%Y-%m-%dT%H:%M:%SZ");
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "approver");
  json_builder_add_string_value (builder, approver ? approver : "");
  json_builder_set_member_name (builder, "at");
  json_builder_add_string_value (builder, at);
  json_builder_set_member_name (builder, "action");
  json_builder_add_string_value (builder, action);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  return json_generator_to_data (generator, NULL);
}

/* ---- the shell's sheets: org.minerva.Pin1 on the session bus ---- */

typedef struct {
  CtProofDone done;
  gpointer data;
  GObject *owner;
} Ask;

static void
shell_answered (GObject *bus, GAsyncResult *res, gpointer data)
{
  Ask *ask = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (bus), res, &error);
  gboolean ok = FALSE;
  const char *why = NULL;

  if (reply)
    g_variant_get (reply, "(b&s)", &ok, &why);
  ask->done (ok, ok ? NULL : (reply ? why : error->message), ask->data);
  g_clear_object (&ask->owner);
  g_free (ask);
}

static void
shell_ask (GObject *owner, const CtProofRequest *request, CtProofDone done, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  Ask *ask;

  if (!bus)
    {
      done (FALSE, error->message, data);
      return;
    }
  ask = g_new0 (Ask, 1);
  ask->done = done;
  ask->data = data;
  ask->owner = owner ? g_object_ref (owner) : NULL;
  /* the sheet has its own deadline; the call waits a little longer than it */
  if (request->method == CT_PROOF_PIN)
    g_dbus_connection_call (bus, "org.minerva.Pin1", "/org/minerva/Pin1", "org.minerva.Pin1", "AskPin",
                            g_variant_new ("(ssii)", request->reason, request->wrong_text ? request->wrong_text : "",
                                           request->seconds, request->tries),
                            G_VARIANT_TYPE ("(bs)"), G_DBUS_CALL_FLAGS_NONE, (request->seconds + 10) * 1000, NULL,
                            shell_answered, ask);
  else
    g_dbus_connection_call (bus, "org.minerva.Pin1", "/org/minerva/Pin1", "org.minerva.Pin1", "AskFinger",
                            g_variant_new ("(si)", request->reason, request->seconds), G_VARIANT_TYPE ("(bs)"),
                            G_DBUS_CALL_FLAGS_NONE, (request->seconds + 10) * 1000, NULL, shell_answered, ask);
  g_object_unref (bus);
}

static const CtProofProvider shell_provider = { shell_ask };

const CtProofProvider *
ct_proof_shell_provider (void)
{
  return &shell_provider;
}

/* ---- development ---- */

static gboolean dev_refuse;
static CtProofRequest dev_last;
static char *dev_reason;

static void
dev_ask (GObject *owner, const CtProofRequest *request, CtProofDone done, gpointer data)
{
  g_free (dev_reason);
  dev_reason = g_strdup (request->reason);
  dev_last = *request;
  dev_last.reason = dev_reason;
  done (!dev_refuse, dev_refuse ? "refused" : NULL, data);
}

static const CtProofProvider dev_provider = { dev_ask };

const CtProofProvider *
ct_proof_dev_provider (void)
{
  return &dev_provider;
}

void
ct_proof_dev_refuse (gboolean refuse)
{
  dev_refuse = refuse;
}

const CtProofRequest *
ct_proof_dev_last (void)
{
  return dev_reason ? &dev_last : NULL;
}

void
ct_proof_set_provider (const CtProofProvider *p)
{
  provider = p;
}

void
ct_proof_ask (GObject *owner, const CtProofRequest *request, CtProofDone done, gpointer data)
{
  if (!provider)
    provider = g_getenv ("CT_RIG_DIR") && !g_strcmp0 (g_getenv ("CT_PROOF"), "dev") ? &dev_provider : &shell_provider;
  provider->ask (owner, request, done, data);
}
