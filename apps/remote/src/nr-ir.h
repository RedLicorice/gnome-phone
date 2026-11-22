/* The IR remote service (minerva-ir's org.minerva.Ir1 on the session bus, D-Bus activated): every method answers
 * JSON text, a refusal is a D-Bus error with the reason. One asynchronous call at a time per caller is enough. */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* `result` is the parsed answer (NULL on error); `error` the reason, remote prefix stripped */
typedef void (*NrIrDone) (JsonNode *result, const GError *error, gpointer data);

void nr_ir_set_timeout (int timeout_ms);
/* params: a floating GVariant tuple of the method's arguments (g_variant_new ("(ss)", ...)), or NULL */
void nr_ir_call        (const char *method, GVariant *params, NrIrDone done, gpointer data);

G_END_DECLS
