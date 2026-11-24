/* cam-key-import: see cam-key-import.h. */
#include "cam-key-import.h"
#include "cam-qr-sheet.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
  CamKeyImportDone done;
  gpointer         data;
} Job;

/* how long the pairing gate may wait on `minerva-config messaging list` during a scan */
#define PAIR_GATE_SECONDS 2

static char *
mc_bin (void)
{
  const char *env = g_getenv ("MINERVA_CONFIG_BIN");

  return env && *env ? g_strdup (env) : g_build_filename (MINERVA_BIN_DIR, "minerva-config", NULL);
}

/* a JSON string literal; the output buffer holds secrets, so it is the caller's to wipe */
static void
append_json_string (GString *out, const char *text)
{
  g_string_append_c (out, '"');
  for (const char *p = text; *p; p++)
    {
      guchar c = *p;

      if (c == '"' || c == '\\')
        g_string_append_printf (out, "\\%c", c);
      else if (c < 0x20)
        g_string_append_printf (out, "\\u%04x", c);
      else
        g_string_append_c (out, c);
    }
  g_string_append_c (out, '"');
}

static const char *
member_string (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : NULL;
}

static void
finish (Job *job, CamKeyResult *result)
{
  job->done (result, job->data);
  g_free (result->provider);
  g_free (result->name);
  g_free (result->message);
  g_free (result->error);
  g_free (result);
  g_free (job);
}

static void
on_finished (GObject *source, GAsyncResult *async, gpointer data)
{
  Job *job = data;
  CamKeyResult *result = g_new0 (CamKeyResult, 1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) out_bytes = NULL, err_bytes = NULL;
  g_autofree char *out = NULL, *err = NULL;
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonObject *root = NULL;
  gboolean exited_ok;

  g_subprocess_communicate_finish (G_SUBPROCESS (source), async, &out_bytes, &err_bytes, &error);
  out = out_bytes ? g_strndup (g_bytes_get_data (out_bytes, NULL), g_bytes_get_size (out_bytes)) : NULL;
  err = err_bytes ? g_strndup (g_bytes_get_data (err_bytes, NULL), g_bytes_get_size (err_bytes)) : NULL;
  exited_ok = !error && g_subprocess_get_if_exited (G_SUBPROCESS (source))
              && g_subprocess_get_exit_status (G_SUBPROCESS (source)) == 0;
  if (out && *out && json_parser_load_from_data (parser, out, -1, NULL))
    {
      JsonNode *node = json_parser_get_root (parser);

      root = node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
    }
  if (exited_ok && root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    {
      result->ok = TRUE;
      result->provider = g_strdup (member_string (root, "provider"));
      result->name = g_strdup (member_string (root, "name") ? member_string (root, "name")
                                                              : member_string (root, "provider"));
      result->message = g_strdup (member_string (root, "message"));
    }
  else if (root)
    {
      const char *why = member_string (root, "error");

      result->need_passphrase = json_object_get_boolean_member_with_default (root, "need_passphrase", FALSE);
      result->error = g_strdup (why && *why ? why : _("The key could not be saved"));
    }
  else
    {
      /* no JSON: the program is missing or broke; its own words (stderr carries no secret: it never sees one) */
      if (err && *err)
        result->error = g_strdup (g_strstrip (err));
      else
        result->error = g_strdup (error ? error->message : _("minerva-config gave no answer"));
    }
  /* a line to show, not a page */
  if (result->error && g_utf8_strlen (result->error, -1) > 160)
    {
      char *cut = g_utf8_offset_to_pointer (result->error, 160);

      strcpy (cut, "…");
    }
  finish (job, result);
}

/* runs @argv with @json on its stdin, without blocking; @json's buffer is wiped as it is sent */
static void
run_json (const char *const *argv, GString *json, CamKeyImportDone done, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) process = NULL;
  Job *job = g_new0 (Job, 1);
  GBytes *input;
  gsize len = json->len;
  char *text = g_string_free (json, FALSE);

  job->done = done;
  job->data = data;
  process = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                      | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
  if (!process)
    {
      CamKeyResult *result = g_new0 (CamKeyResult, 1);

      explicit_bzero (text, len);
      g_free (text);
      result->error = g_strdup (error->message);
      finish (job, result);
      return;
    }
  /* a GBytes that wipes its buffer when the last reference goes: the subprocess drops its own once it has written */
  input = g_bytes_new_with_free_func (text, len, (GDestroyNotify) cam_scrub, text);
  g_subprocess_communicate_async (process, input, NULL, on_finished, job);
  g_bytes_unref (input);
}

void
cam_key_import_run (const char *payload, const char *passphrase, CamKeyImportDone done, gpointer data)
{
  g_autofree char *bin = mc_bin ();
  const char *argv[] = { bin, "key", "import", "--apply", "--json", NULL };
  GString *json = g_string_new ("{\"payload\":");

  append_json_string (json, payload);
  if (passphrase && *passphrase)
    {
      g_string_append (json, ",\"passphrase\":");
      append_json_string (json, passphrase);
    }
  g_string_append_c (json, '}');
  run_json (argv, json, done, data);
}

void
cam_pair_run (const char *provider, const char *payload, CamKeyImportDone done, gpointer data)
{
  g_autofree char *bin = mc_bin ();
  const char *argv[] = { bin, "messaging", "pair", provider, "--json", NULL };
  GString *json = g_string_new ("{\"payload\":");

  /* the scanned code names the provider: never let it be read as an option (argv, not the program's input) */
  if (!provider || !*provider || provider[0] == '-')
    {
      CamKeyResult *result = g_new0 (CamKeyResult, 1);
      Job *job = g_new0 (Job, 1);

      job->done = done;
      job->data = data;
      result->error = g_strdup (_("The code names no provider"));
      g_string_free (json, TRUE);
      finish (job, result);
      return;
    }
  append_json_string (json, payload);
  g_string_append_c (json, '}');
  run_json (argv, json, done, data);
}

/* a timeout source for the gate: cancels the call it is watching */
static gboolean
cancel_call (gpointer data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
  return G_SOURCE_REMOVE;
}

gboolean
cam_pair_allowed (const char *provider)
{
  g_autofree char *bin = mc_bin ();
  const char *argv[] = { bin, "messaging", "list", "--json", NULL };
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autofree char *out = NULL;
  GSource *deadline;
  JsonObject *root = NULL;
  JsonArray *rows;

  if (!provider || !*provider)
    return FALSE;
  process = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
  if (!process)
    return TRUE;    /* no program to ask: the pair command says so, in its own words */
  /* a scan must not wait on the program: a short deadline cancels it, and that counts as a failed call */
  deadline = g_timeout_source_new_seconds (PAIR_GATE_SECONDS);
  g_source_set_callback (deadline, cancel_call, cancel, NULL);
  g_source_attach (deadline, NULL);
  if (!g_subprocess_communicate_utf8 (process, NULL, cancel, &out, NULL, &error))
    {
      g_source_destroy (deadline);
      g_source_unref (deadline);
      /* cancelled (or broken): no command left behind for a scan that did not wait */
      if (!g_subprocess_get_if_exited (process))
        g_subprocess_force_exit (process);
      return TRUE;
    }
  g_source_destroy (deadline);
  g_source_unref (deadline);
  if (!out || !json_parser_load_from_data (parser, out, -1, NULL))
    return TRUE;
  if (JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    root = json_node_get_object (json_parser_get_root (parser));
  rows = root ? json_object_get_array_member (root, "providers") : NULL;
  for (guint i = 0; rows && i < json_array_get_length (rows); i++)
    {
      JsonObject *row = json_array_get_object_element (rows, i);
      const char *id = row ? member_string (row, "id") : NULL;
      const char *pairing = row ? member_string (row, "pairing") : NULL;

      if (g_strcmp0 (id, provider) == 0)
        return pairing && *pairing && g_ascii_strcasecmp (pairing, "none") != 0;
    }
  /* not a provider it knows: the pair command refuses it */
  return FALSE;
}
