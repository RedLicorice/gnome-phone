/* Running `minerva-config` (and the other programs beside it); see ns-mc.h. */
#include "ns-mc.h"

#include <gio/gdesktopappinfo.h>

/* the program's path: `prog`'s own env var ("minerva-perm" -> MINERVA_PERM_BIN), else the directory the Minerva
 * packages install it in (MINERVA_BIN_DIR, the meson option minerva-bin-dir) */
static char *
prog_bin (const char *prog)
{
  g_autofree char *name = g_ascii_strup (prog, -1);
  for (char *p = name; *p; p++)
    if (*p == '-')
      *p = '_';
  g_autofree char *var = g_strconcat (name, "_BIN", NULL);
  const char *env = g_getenv (var);
  if (env && *env)
    return g_strdup (env);
  return g_build_filename (MINERVA_BIN_DIR, prog, NULL);
}

gboolean
ns_mc_prog_available (const char *prog)
{
  g_autofree char *bin = prog_bin (prog);
  return g_file_test (bin, G_FILE_TEST_IS_EXECUTABLE);
}

gboolean
ns_minerva_available (void)
{
  g_autofree char *bin = prog_bin ("minerva-config");
  gboolean there = g_file_test (bin, G_FILE_TEST_IS_EXECUTABLE);

  g_debug ("minerva-config: %s (%s)", bin, there ? "available" : "missing");
  return there;
}

typedef struct {
  GObject      *owner;
  NsMcDone      done;        /* one of these two */
  NsMcListDone  list_done;
  gpointer      data;
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
  JsonArray *list = NULL;
  if (ok && out && *out && json_parser_load_from_data (parser, out, -1, NULL)) {
    JsonNode *n = json_parser_get_root (parser);
    root = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    list = n && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
  }
  g_autofree char *why = NULL;
  if (!ok) {
    if (err && *err)
      why = g_strdup (g_strstrip (err));
    else
      why = g_strdup (e ? e->message : "minerva-config failed");
  }
  if (j->done)
    j->done (root, why, j->data);
  else
    j->list_done (list, why, j->data);
  g_object_unref (j->owner);
  g_free (j);
}

static void
job_start (GObject *owner, const char *prog, const char *const *args, const char *stdin_text, Job *j)
{
  g_autofree char *bin = prog_bin (prog);
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
    if (j->done)
      j->done (NULL, err->message, j->data);
    else
      j->list_done (NULL, err->message, j->data);
    g_free (j);
    return;
  }
  j->owner = g_object_ref (owner);
  g_subprocess_communicate_utf8_async (p, stdin_text, NULL, job_done, j);
  g_object_unref (p);
}

void
ns_mc_run_bin (GObject *owner, const char *prog, const char *const *args, const char *stdin_text, NsMcDone done,
               gpointer data)
{
  Job *j = g_new0 (Job, 1);
  j->done = done;
  j->data = data;
  job_start (owner, prog, args, stdin_text, j);
}

void
ns_mc_run (GObject *owner, const char *const *args, const char *stdin_text, NsMcDone done, gpointer data)
{
  ns_mc_run_bin (owner, "minerva-config", args, stdin_text, done, data);
}

void
ns_mc_run_list (GObject *owner, const char *prog, const char *const *args, NsMcListDone done, gpointer data)
{
  Job *j = g_new0 (Job, 1);
  j->list_done = done;
  j->data = data;
  job_start (owner, prog, args, NULL, j);
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

gboolean
ns_prompt_installed (void)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new ("de.yesman.NeoPrompt.desktop");
  return info != NULL;
}

gboolean
ns_prompt_open (const char *id, const char *person)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new ("de.yesman.NeoPrompt.desktop");
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *uri = NULL;
  GList files = { NULL, NULL, NULL };

  if (!info)
    {
      g_message ("neo-prompt is not installed");
      return FALSE;
    }
  uri = person && *person ? g_strdup_printf ("neoprompt://open/%s?person=%s", id, person)
                          : g_strdup_printf ("neoprompt://open/%s", id);
  file = g_file_new_for_uri (uri);
  files.data = file;
  if (!g_app_info_launch (G_APP_INFO (info), &files, NULL, &error))
    {
      g_message ("neo-prompt: %s", error ? error->message : "not launched");
      return FALSE;
    }
  return TRUE;
}
