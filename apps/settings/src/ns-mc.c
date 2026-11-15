/* Running `minerva-config`; see ns-mc.h. */
#include "ns-mc.h"
#include "ns-pages.h"

static char *
mc_bin (void)
{
  const char *env = g_getenv ("MINERVA_CONFIG_BIN");
  return env && *env ? g_strdup (env)
                     : g_build_filename (g_get_home_dir (), ".local", "share", "minerva", "bin", "minerva-config", NULL);
}

gboolean
ns_minerva_available (void)
{
  g_autofree char *bin = mc_bin ();
  return g_file_test (bin, G_FILE_TEST_IS_EXECUTABLE);
}

typedef struct {
  GObject  *owner;
  NsMcDone  done;
  gpointer  data;
} Job;

static void
job_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  g_autofree char *out = NULL, *err = NULL;
  g_autoptr (GError) e = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, &err, &e);
  gboolean ok = !e && g_subprocess_get_if_exited (G_SUBPROCESS (src)) &&
                g_subprocess_get_exit_status (G_SUBPROCESS (src)) == 0;
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root = NULL;
  if (ok && out && *out && json_parser_load_from_data (parser, out, -1, NULL)) {
    JsonNode *n = json_parser_get_root (parser);
    root = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
  }
  g_autofree char *why = NULL;
  if (!ok) {
    if (err && *err)
      why = g_strdup (g_strstrip (err));
    else
      why = g_strdup (e ? e->message : "minerva-config failed");
  }
  j->done (root, why, j->data);
  g_object_unref (j->owner);
  g_free (j);
}

void
ns_mc_run (GObject *owner, const char *const *args, const char *stdin_text, NsMcDone done, gpointer data)
{
  g_autofree char *bin = mc_bin ();
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, bin);
  for (guint i = 0; args[i]; i++)
    g_ptr_array_add (argv, (char *) args[i]);
  g_ptr_array_add (argv, NULL);
  g_autoptr (GError) err = NULL;
  GSubprocess *p = g_subprocess_newv ((const char *const *) argv->pdata,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                      (stdin_text ? G_SUBPROCESS_FLAGS_STDIN_PIPE : G_SUBPROCESS_FLAGS_NONE), &err);
  if (!p) {
    done (NULL, err->message, data);
    return;
  }
  Job *j = g_new0 (Job, 1);
  j->owner = g_object_ref (owner);
  j->done = done;
  j->data = data;
  g_subprocess_communicate_utf8_async (p, stdin_text, NULL, job_done, j);
  g_object_unref (p);
}

const char *
ns_json_str (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && JSON_NODE_HOLDS_VALUE (json_object_get_member (o, member)) &&
         json_object_get_string_member (o, member)
           ? json_object_get_string_member (o, member) : "";
}

JsonObject *
ns_json_obj (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && JSON_NODE_HOLDS_OBJECT (json_object_get_member (o, member))
           ? json_object_get_object_member (o, member) : NULL;
}

JsonArray *
ns_json_arr (JsonObject *o, const char *member)
{
  return o && json_object_has_member (o, member) && JSON_NODE_HOLDS_ARRAY (json_object_get_member (o, member))
           ? json_object_get_array_member (o, member) : NULL;
}
