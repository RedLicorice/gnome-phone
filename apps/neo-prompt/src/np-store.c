/* Neo Prompt's data: see np-store.h. */
#include "np-store.h"

#include <string.h>
#include <glib/gstdio.h>

#define CONF_DEFAULT "~/.config/minerva/neo-prompt.json"

static const char *
jstr (JsonObject *o, const char *key)
{
  return o && json_object_has_member (o, key) ? json_object_get_string_member (o, key) : NULL;
}

static int
jint (JsonObject *o, const char *key, int fallback)
{
  return o && json_object_has_member (o, key) ? (int) json_object_get_int_member (o, key) : fallback;
}

static gboolean
jbool (JsonObject *o, const char *key, gboolean fallback)
{
  return o && json_object_has_member (o, key) ? json_object_get_boolean_member (o, key) : fallback;
}

static JsonObject *
object_of (JsonObject *o, const char *key)
{
  return o && json_object_has_member (o, key) ? json_object_get_object_member (o, key) : NULL;
}

static double
jdouble (JsonObject *o, const char *key, double fallback)
{
  return o && json_object_has_member (o, key) ? json_object_get_double_member (o, key) : fallback;
}

char *
np_path_expand (const char *path)
{
  if (!path)
    return NULL;
  if (g_str_has_prefix (path, "~/"))
    return g_build_filename (g_get_home_dir (), path + 2, NULL);
  return g_strdup (path);
}

static JsonObject *
json_file_object (const char *path)
{
  g_autofree char *expanded = np_path_expand (path);
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) error = NULL;

  if (!g_file_test (expanded, G_FILE_TEST_IS_REGULAR) || !json_parser_load_from_file (parser, expanded, &error))
    return NULL;
  if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return NULL;
  return json_object_ref (json_node_get_object (json_parser_get_root (parser)));
}

const char *
np_ui (NpStore *self, const char *key)
{
  g_auto(GStrv) parts = g_strsplit (key, ".", -1);
  JsonObject *o = self->ui;

  for (guint i = 0; o && parts[i]; i++)
    {
      if (!json_object_has_member (o, parts[i]))
        return key;
      if (parts[i + 1])
        {
          if (!JSON_NODE_HOLDS_OBJECT (json_object_get_member (o, parts[i])))
            return key;
          o = json_object_get_object_member (o, parts[i]);
        }
      else
        return json_object_get_string_member (o, parts[i]);
    }
  return key;
}

/* the mode file: { "modes": { "markdown": "rendered" } }, the choices the editor has made, per kind */
static JsonObject *
state_object (NpStore *self)
{
  JsonObject *state = self->conf.state_path ? json_file_object (self->conf.state_path) : NULL;

  return state ? state : json_object_new ();
}

char *
np_mode_for (NpStore *self, const char *kind)
{
  g_autoptr(JsonObject) state = state_object (self);
  const char *mode = jstr (object_of (state, "modes"), kind);

  if (!mode && self->conf.modes)
    mode = jstr (self->conf.modes, kind);
  return g_strdup (mode && *mode ? mode : "source");
}

void
np_mode_set (NpStore *self, const char *kind, const char *mode)
{
  g_autoptr(JsonObject) state = NULL;
  g_autoptr(JsonNode) root = NULL;
  g_autoptr(JsonGenerator) generator = NULL;
  g_autofree char *dir = NULL;
  JsonObject *modes = NULL;
  char *text = NULL;

  if (!self->conf.state_path || !kind || !*kind)
    return;
  state = state_object (self);
  modes = object_of (state, "modes");
  if (!modes)
    {
      modes = json_object_new ();
      json_object_set_object_member (state, "modes", modes);
    }
  json_object_set_string_member (modes, kind, mode && *mode ? mode : "source");
  dir = g_path_get_dirname (self->conf.state_path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    return;
  root = json_node_alloc ();
  json_node_init_object (root, state);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);
  if (text && g_file_set_contents (self->conf.state_path, text, -1, NULL))
    g_chmod (self->conf.state_path, 0600);
  g_free (text);
}

static char **
argv_of (JsonArray *array)
{
  guint n = array ? json_array_get_length (array) : 0;
  char **argv = g_new0 (char *, n + 1);

  for (guint i = 0; i < n; i++)
    argv[i] = g_strdup (json_array_get_string_element (array, i));
  return argv;
}

static char **
argv_member (JsonObject *o, const char *key)
{
  if (!o || !json_object_has_member (o, key) || !JSON_NODE_HOLDS_ARRAY (json_object_get_member (o, key)))
    return NULL;
  return argv_of (json_object_get_array_member (o, key));
}

static char **
argv_substitute_person (char **argv, const char *person)
{
  guint n = 0;

  if (!argv)
    return NULL;
  while (argv[n])
    n++;
  char **copy = g_new0 (char *, n + 1);
  for (guint i = 0; i < n; i++)
    {
      const char *at = strstr (argv[i], "{person}");

      if (!at)
        copy[i] = g_strdup (argv[i]);
      else
        {
          g_autofree char *head = g_strndup (argv[i], at - argv[i]);
          copy[i] = g_strconcat (head, person, at + strlen ("{person}"), NULL);
        }
    }
  return copy;
}

void
np_argv_free (char **argv)
{
  if (!argv)
    return;
  for (guint i = 0; argv[i]; i++)
    g_free (argv[i]);
  g_free (argv);
}

void
np_doc_free (NpDoc *doc)
{
  if (!doc)
    return;
  g_free (doc->id);
  g_free (doc->group);
  g_free (doc->title);
  g_free (doc->kind);
  g_free (doc->path);
  g_free (doc->source_label);
  g_free (doc->text_key);
  np_argv_free (doc->open_argv);
  np_argv_free (doc->save_argv);
  np_argv_free (doc->validate_argv);
  np_argv_free (doc->empty_argv);
  g_free (doc->person);
  g_free (doc);
}

static NpDoc *
doc_of (JsonObject *o)
{
  NpDoc *doc = g_new0 (NpDoc, 1);
  JsonObject *source = object_of (o, "source");
  JsonObject *save = object_of (o, "save");
  const char *path = jstr (source, "file");

  doc->id = g_strdup (jstr (o, "id"));
  doc->group = g_strdup (jstr (o, "group") ? jstr (o, "group") : "");
  doc->title = g_strdup (jstr (o, "title") ? jstr (o, "title") : doc->id);
  doc->kind = g_strdup (jstr (o, "kind") ? jstr (o, "kind") : "text");
  doc->editable = jbool (o, "editable", TRUE);
  doc->history = jbool (o, "history", TRUE);
  doc->listed = jbool (o, "listed", TRUE);
  doc->needs_person = jbool (o, "needs_person", FALSE);
  if (path && *path && g_strcmp0 (path, "self"))
    doc->path = np_path_expand (path);
  doc->source_label = g_strdup (jstr (source, "label"));
  doc->text_key = g_strdup (jstr (source, "text_key") ? jstr (source, "text_key") : "text");
  doc->open_argv = argv_member (source, "argv");
  if (save)
    {
      const char *file = jstr (save, "file");

      doc->save_argv = argv_member (save, "argv");
      if (!doc->path && file && *file && g_strcmp0 (file, "self"))
        doc->path = np_path_expand (file);
    }
  doc->validate_argv = argv_member (o, "validate");
  doc->empty_argv = argv_member (o, "empty_argv");
  return doc;
}

/* --- globs: dir/<star>.txt and dir/<star>/SKILL.md, a document per match --- */

static void
add_glob (NpStore *self, JsonObject *o, GPtrArray *out)
{
  const char *pattern = jstr (o, "glob");
  const char *id_prefix = jstr (o, "id_prefix") ? jstr (o, "id_prefix") : "";
  const char *id_from = jstr (o, "id_from") ? jstr (o, "id_from") : "stem";
  g_autofree char *expanded = np_path_expand (pattern);
  char *star = strchr (expanded, '*');

  if (!star)
    return;
  if (star[1] == '/' && !strchr (star + 2, '/'))
    {
      /* dir/<star>/NAME: a document per subdirectory that holds NAME */
      g_autofree char *dir = g_path_get_dirname (expanded);
      const char *name = star + 2;
      GDir *d = g_dir_open (dir, 0, NULL);
      const char *entry;

      if (!d)
        return;
      while ((entry = g_dir_read_name (d)))
        {
          g_autofree char *sub = g_build_filename (dir, entry, NULL);
          g_autofree char *full = g_build_filename (sub, name, NULL);

          if (!g_file_test (full, G_FILE_TEST_IS_REGULAR))
            continue;
          NpDoc *doc = doc_of (o);

          g_free (doc->id);
          doc->id = g_strconcat (id_prefix, g_strcmp0 (id_from, "parent") == 0 ? entry : name, NULL);
          g_free (doc->title);
          doc->title = g_strdup (g_strcmp0 (id_from, "parent") == 0 ? entry : name);
          g_free (doc->path);
          doc->path = g_strdup (full);
          g_ptr_array_add (out, doc);
        }
      g_dir_close (d);
      return;
    }
  if (star[1] == '.')
    {
      /* dir/<star>.ext */
      g_autofree char *dir = g_path_get_dirname (expanded);
      const char *ext = star + 1;
      GDir *d = g_dir_open (dir, 0, NULL);
      const char *entry;

      if (!d)
        return;
      while ((entry = g_dir_read_name (d)))
        {
          g_autofree char *full = g_build_filename (dir, entry, NULL);
          g_autofree char *stem = g_strdup (entry);
          char *dot = NULL;

          if (!g_str_has_suffix (entry, ext) || !g_file_test (full, G_FILE_TEST_IS_REGULAR))
            continue;
          stem[strlen (stem) - strlen (ext)] = '\0';
          dot = strrchr (stem, '.');
          if (dot)
            *dot = '\0';
          NpDoc *doc = doc_of (o);

          g_free (doc->id);
          doc->id = g_strconcat (id_prefix, stem, NULL);
          g_free (doc->title);
          doc->title = g_strdup (stem);
          g_free (doc->path);
          doc->path = g_strdup (full);
          g_ptr_array_add (out, doc);
        }
      g_dir_close (d);
    }
}

NpStore *
np_store_new (const char *conf_path)
{
  NpStore *self = g_new0 (NpStore, 1);
  g_autofree char *path = np_path_expand (conf_path && *conf_path ? conf_path
                                          : g_getenv ("NEO_PROMPT_CONF") ? g_getenv ("NEO_PROMPT_CONF")
                                          : CONF_DEFAULT);
  g_autoptr(JsonObject) conf = json_file_object (path);
  JsonObject *paths = object_of (conf, "paths");
  JsonObject *limits = object_of (conf, "limits");

  self->conf.conf_path = g_strdup (path);
  self->conf.minerva_config = np_path_expand (jstr (paths, "minerva_config"));
  if (!self->conf.minerva_config)
    self->conf.minerva_config = g_strdup ("minerva-config");
  self->conf.registry = np_path_expand (jstr (paths, "registry"));
  self->conf.ui_path = np_path_expand (jstr (paths, "ui"));
  self->conf.history_dir = np_path_expand (jstr (paths, "history_dir"));
  self->conf.draft_dir = np_path_expand (jstr (paths, "draft_dir"));
  self->conf.agents_file = np_path_expand (jstr (paths, "agents_file"));
  self->conf.opencode_json = np_path_expand (jstr (paths, "opencode_json"));
  self->conf.skills_dir = np_path_expand (jstr (paths, "skills_dir"));
  self->conf.people_root = np_path_expand (jstr (paths, "people_root"));
  self->conf.event_templates = np_path_expand (jstr (paths, "event_templates"));
  self->conf.input_tags = np_path_expand (jstr (paths, "input_tags"));
  self->conf.state_path = np_path_expand (jstr (paths, "state"));
  {
    JsonObject *editor = object_of (conf, "editor");
    JsonObject *rendered = object_of (editor, "rendered");
    JsonArray *scales = rendered && json_object_has_member (rendered, "heading_scale")
                        && JSON_NODE_HOLDS_ARRAY (json_object_get_member (rendered, "heading_scale"))
                        ? json_object_get_array_member (rendered, "heading_scale") : NULL;

    self->conf.modes = object_of (editor, "modes") ? json_object_ref (object_of (editor, "modes")) : NULL;
    for (guint i = 0; i < G_N_ELEMENTS (self->conf.render.heading_scale); i++)
      self->conf.render.heading_scale[i] = scales && i < json_array_get_length (scales)
                                           ? json_array_get_double_element (scales, i) : 1.0;
    self->conf.render.heading_above = jint (rendered, "heading_above", 8);
    self->conf.render.heading_below = jint (rendered, "heading_below", 4);
    self->conf.render.list_indent = jint (rendered, "list_indent", 16);
    self->conf.render.quote_indent = jint (rendered, "quote_indent", 12);
    self->conf.render.chip_scale = jdouble (rendered, "chip_scale", 1.0);
  }
  self->conf.history_max = jint (limits, "history_max", 40);
  self->conf.chars_per_token = jint (limits, "chars_per_token", 4);
  self->conf.cache_min_tokens = jint (limits, "cache_min_tokens", 1024);
  self->conf.timeout_s = jint (limits, "timeout_s", 15);
  if (self->conf.chars_per_token < 1)
    self->conf.chars_per_token = 1;
  self->conf.block_freq = object_of (conf, "block_freq") ? json_object_ref (object_of (conf, "block_freq")) : NULL;
  self->conf.block_order = conf && json_object_has_member (conf, "block_order")
                           && JSON_NODE_HOLDS_ARRAY (json_object_get_member (conf, "block_order"))
                           ? json_array_ref (json_object_get_array_member (conf, "block_order")) : NULL;
  self->ui = self->conf.ui_path ? json_file_object (self->conf.ui_path) : NULL;
  if (!self->ui)
    self->ui = json_object_new ();

  self->docs = g_ptr_array_new_with_free_func ((GDestroyNotify) np_doc_free);
  if (self->conf.registry)
    {
      g_autoptr(JsonObject) registry = json_file_object (self->conf.registry);
      JsonArray *docs = registry ? json_object_get_array_member (registry, "docs") : NULL;

      for (guint i = 0; docs && i < json_array_get_length (docs); i++)
        {
          JsonObject *o = json_array_get_object_element (docs, i);
          const char *glob = jstr (o, "glob");

          if (glob && *glob)
            add_glob (self, o, self->docs);
          else
            g_ptr_array_add (self->docs, doc_of (o));
        }
    }
  return self;
}

void
np_store_free (NpStore *self)
{
  if (!self)
    return;
  g_free (self->conf.conf_path);
  g_free (self->conf.minerva_config);
  g_free (self->conf.registry);
  g_free (self->conf.ui_path);
  g_free (self->conf.history_dir);
  g_free (self->conf.draft_dir);
  g_free (self->conf.agents_file);
  g_free (self->conf.opencode_json);
  g_free (self->conf.skills_dir);
  g_free (self->conf.people_root);
  g_free (self->conf.event_templates);
  g_free (self->conf.state_path);
  if (self->conf.modes)
    json_object_unref (self->conf.modes);
  g_free (self->conf.input_tags);
  if (self->conf.block_freq)
    json_object_unref (self->conf.block_freq);
  if (self->conf.block_order)
    json_array_unref (self->conf.block_order);
  g_ptr_array_unref (self->docs);
  if (self->ui)
    json_object_unref (self->ui);
  g_free (self);
}

GPtrArray *
np_docs (NpStore *self)
{
  return self->docs;
}

const char *
np_doc_kind (NpDoc *doc)
{
  return doc && doc->kind ? doc->kind : "text";
}

static NpDoc *
doc_clone_for_person (NpDoc *doc, const char *person)
{
  NpDoc *copy = g_new0 (NpDoc, 1);

  copy->id = g_strdup (doc->id);
  copy->group = g_strdup (doc->group);
  copy->title = g_strdup (doc->title);
  copy->kind = g_strdup (doc->kind);
  copy->path = g_strdup (doc->path);
  copy->source_label = g_strdup (doc->source_label);
  copy->text_key = g_strdup (doc->text_key);
  copy->open_argv = argv_substitute_person (doc->open_argv, person);
  copy->save_argv = argv_substitute_person (doc->save_argv, person);
  copy->validate_argv = argv_substitute_person (doc->validate_argv, person);
  copy->empty_argv = argv_substitute_person (doc->empty_argv, person);
  copy->editable = doc->editable;
  copy->history = doc->history;
  copy->listed = doc->listed;
  copy->needs_person = doc->needs_person;
  copy->person = g_strdup (person);
  return copy;
}

NpDoc *
np_doc_by_id (NpStore *self, const char *id, const char *person)
{
  for (guint i = 0; id && i < self->docs->len; i++)
    {
      NpDoc *doc = g_ptr_array_index (self->docs, i);

      if (g_strcmp0 (doc->id, id) != 0)
        continue;
      if (doc->needs_person)
        {
          if (!person || !*person)
            return NULL;
          return doc_clone_for_person (doc, person);
        }
      return doc_clone_for_person (doc, person);
    }
  return NULL;
}

/* --- running a command --- */

static void
ans_fill (NpAns *ans, const char *out, const char *err, gboolean spawned)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GError) error = NULL;

  ans->ok = spawned;
  if (!spawned)
    {
      ans->error = g_strdup (err && *err ? err : "comando non avviato");
      return;
    }
  if (out && *out && json_parser_load_from_data (parser, out, -1, &error)
      && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      const char *refusal = jstr (root, "error");

      ans->json = json_object_ref (root);
      if (json_object_has_member (root, "ok"))
        {
          ans->ok = json_object_get_boolean_member (root, "ok");
          if (!ans->ok)
            ans->error = g_strdup (refusal ? refusal : "rifiutato");
        }
      else if (refusal)
        {
          ans->ok = FALSE;
          ans->error = g_strdup (refusal);
        }
      if (json_object_has_member (root, "text"))
        ans->text = g_strdup (json_object_get_string_member (root, "text"));
    }
  else if (err && *err)
    {
      g_autofree char *trimmed = g_strdup (err);

      ans->ok = FALSE;
      ans->error = g_strdup (g_strstrip (trimmed));
    }
}

void
np_ans_clear (NpAns *ans)
{
  if (!ans)
    return;
  g_free (ans->error);
  g_free (ans->text);
  if (ans->json)
    json_object_unref (ans->json);
  memset (ans, 0, sizeof *ans);
}

typedef struct {
  NpStore     *store;
  char        *stdin_text;
  GCancellable *cancel;
  GObject     *owner;
  NpRunDone    done;
  gpointer     data;
  guint        timeout;
} Run;

static gboolean
on_timeout (gpointer data)
{
  g_cancellable_cancel (data);
  return G_SOURCE_REMOVE;
}

static void
run_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Run *r = data;
  g_autofree char *out = NULL;
  g_autofree char *errout = NULL;
  g_autoptr(GError) error = NULL;
  NpAns ans = { 0 };

  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, &errout, &error);
  if (r->timeout)
    g_source_remove (r->timeout);
  if (error)
    ans_fill (&ans, NULL, error->message, FALSE);
  else
    ans_fill (&ans, out, errout, TRUE);
  r->done (&ans, r->data);
  np_ans_clear (&ans);
  if (r->owner)
    g_object_unref (r->owner);
  g_clear_object (&r->cancel);
  g_free (r->stdin_text);
  g_free (r);
}

void
np_run (NpStore *self, const char *const *argv, const char *stdin_text, GObject *owner, GCancellable *cancel,
        NpRunDone done, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) expanded = NULL;
  guint n = 0;
  GSubprocess *process = NULL;
  Run *r = g_new0 (Run, 1);

  while (argv && argv[n])
    n++;
  expanded = g_new0 (char *, n + 1);
  for (guint i = 0; i < n; i++)
    {
      const char *at = strstr (argv[i], "{minerva_config}");

      expanded[i] = at ? g_strdup (self->conf.minerva_config) : g_strdup (argv[i]);
    }
  process = g_subprocess_newv ((const char *const *) expanded,
                               G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                               | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);

  r->store = self;
  r->stdin_text = g_strdup (stdin_text);
  r->owner = owner ? g_object_ref (owner) : NULL;
  r->done = done;
  r->data = data;
  r->cancel = cancel ? g_object_ref (cancel) : g_cancellable_new ();
  if (!process)
    {
      NpAns ans = { 0 };

      ans.error = g_strdup (error->message);
      done (&ans, data);
      np_ans_clear (&ans);
      if (r->owner)
        g_object_unref (r->owner);
      g_clear_object (&r->cancel);
      g_free (r->stdin_text);
      g_free (r);
      return;
    }
  r->timeout = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, self->conf.timeout_s, on_timeout,
                                           g_object_ref (r->cancel), g_object_unref);
  g_subprocess_communicate_utf8_async (process, stdin_text, r->cancel, run_done, r);
  g_object_unref (process);
}

/* --- reading a document --- */

typedef struct {
  NpStore    *store;
  NpDoc      *doc;
  NpRunDone   done;
  gpointer    data;
} Load;

static void
load_answered (NpAns *ans, gpointer data)
{
  Load *l = data;

  if (ans->json && l->doc->text_key && g_strcmp0 (l->doc->text_key, "text") != 0)
    {
      const char *value = jstr (ans->json, l->doc->text_key);

      g_free (ans->text);
      ans->text = g_strdup (value ? value : "");
    }
  l->done (ans, l->data);
  g_free (l);
}

void
np_doc_load (NpStore *self, NpDoc *doc, GObject *owner, GCancellable *cancel, NpRunDone done, gpointer data)
{
  Load *l = g_new0 (Load, 1);

  l->store = self;
  l->doc = doc;
  l->done = done;
  l->data = data;
  if (!doc->path && doc->open_argv)
    {
      np_run (self, (const char *const *) doc->open_argv, NULL, owner, cancel, load_answered, l);
      return;
    }
  {
    NpAns ans = { 0 };
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;

    if (!doc->path)
      ans.error = g_strdup ("nessuna sorgente");
    else if (!g_file_get_contents (doc->path, &contents, NULL, &error))
      ans.error = g_strdup (error->message);
    else
      {
        ans.ok = TRUE;
        ans.text = g_strdup (contents);
      }
    done (&ans, data);
    np_ans_clear (&ans);
    g_free (l);
  }
}

/* --- the history --- */

static char *
id_slug (const char *id)
{
  char *slug = g_strdup (id ? id : "doc");

  for (char *p = slug; *p; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
      *p = '-';
  return slug;
}

char *
np_history_dir (NpStore *self, NpDoc *doc)
{
  g_autofree char *slug = id_slug (doc->id);

  if (doc->person && *doc->person)
    {
      g_autofree char *p = id_slug (doc->person);

      return g_build_filename (self->conf.history_dir, slug, p, NULL);
    }
  return g_build_filename (self->conf.history_dir, slug, NULL);
}

static int
name_cmp_desc (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const char **) b, *(const char **) a);
}

GPtrArray *
np_history_list (NpStore *self, NpDoc *doc)
{
  g_autofree char *dir = np_history_dir (self, doc);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GDir *d = g_dir_open (dir, 0, NULL);

  if (d)
    {
      const char *entry;

      while ((entry = g_dir_read_name (d)))
        if (g_str_has_suffix (entry, ".md"))
          g_ptr_array_add (names, g_strdup (entry));
      g_dir_close (d);
    }
  g_ptr_array_sort (names, name_cmp_desc);
  return names;
}

char *
np_history_read (NpStore *self, NpDoc *doc, const char *name)
{
  g_autofree char *dir = np_history_dir (self, doc);
  g_autofree char *path = g_build_filename (dir, name, NULL);
  char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return NULL;
  return text;
}

gboolean
np_history_push (NpStore *self, NpDoc *doc, const char *text, char **error)
{
  g_autofree char *dir = np_history_dir (self, doc);
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y%m%dT%H%M%S");
  g_autofree char *sum = np_checksum (text ? text : "");
  g_autofree char *name = g_strdup_printf ("%s-%s.md", stamp, sum);
  g_autofree char *path = g_build_filename (dir, name, NULL);

  /* the same text as the newest snapshot: nothing to keep */
  {
    GPtrArray *names = np_history_list (self, doc);

    if (names->len)
      {
        char *old = np_history_read (self, doc, g_ptr_array_index (names, 0));

        if (old && g_strcmp0 (old, text) == 0)
          {
            g_free (old);
            g_ptr_array_unref (names);
            return TRUE;
          }
        g_free (old);
      }
    g_ptr_array_unref (names);
  }
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      if (error)
        *error = g_strdup ("cartella della cronologia non creata");
      return FALSE;
    }
  if (!g_file_set_contents (path, text ? text : "", -1, NULL))
    {
      if (error)
        *error = g_strdup ("snapshot non scritto");
      return FALSE;
    }
  g_chmod (path, 0600);
  np_history_trim (self, doc);
  return TRUE;
}

void
np_history_trim (NpStore *self, NpDoc *doc)
{
  GPtrArray *names = np_history_list (self, doc);
  g_autofree char *dir = np_history_dir (self, doc);

  for (guint i = self->conf.history_max; i < names->len; i++)
    {
      g_autofree char *path = g_build_filename (dir, g_ptr_array_index (names, i), NULL);

      g_unlink (path);
    }
  g_ptr_array_unref (names);
}

/* --- the draft --- */

char *
np_draft_path (NpStore *self, NpDoc *doc)
{
  g_autofree char *slug = id_slug (doc->id);
  g_autofree char *name = doc->person && *doc->person
                          ? g_strdup_printf ("%s-%s.draft", slug, doc->person)
                          : g_strdup_printf ("%s.draft", slug);

  return g_build_filename (self->conf.draft_dir, name, NULL);
}

gboolean
np_draft_write (NpStore *self, NpDoc *doc, const char *text)
{
  g_autofree char *path = np_draft_path (self, doc);
  g_autofree char *dir = g_path_get_dirname (path);

  if (g_mkdir_with_parents (dir, 0700) != 0)
    return FALSE;
  if (!g_file_set_contents (path, text ? text : "", -1, NULL))
    return FALSE;
  g_chmod (path, 0600);
  return TRUE;
}

void
np_draft_drop (NpStore *self, NpDoc *doc)
{
  g_autofree char *path = np_draft_path (self, doc);

  g_unlink (path);
}

char *
np_checksum (const char *text)
{
  return g_compute_checksum_for_string (G_CHECKSUM_SHA256, text ? text : "", -1);
}
