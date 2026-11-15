/* The context view: see np-context.h.
 *
 * The blocks are built as data first, ordered the way the configuration says (the cache prefix of a provider runs
 * from the first block to the last), then drawn: a block whose text cannot be read says so and carries no token
 * figure, and the total is a lower bound (>=). */
#include "np-context.h"
#include "np-ui.h"

#include <neo.h>
#include <string.h>

typedef struct {
  char    *id;
  char    *title;
  char    *source;
  int      tokens;          /* -1: not reconstructable */
  char    *freq;            /* static, session, turn */
  char    *open_id;         /* a document of the registry, or NULL */
  char    *note;
} Block;

struct _NpContext {
  NpStore   *store;
  GtkWindow *window;
  GtkWidget *snackbar;
  GtkWidget *root, *list, *scroll;
  guint      token;
  void     (*open_cb) (gpointer, const char *id, const char *person);
  gpointer   open_data;
};

static void
block_free (Block *block)
{
  g_free (block->id);
  g_free (block->title);
  g_free (block->source);
  g_free (block->freq);
  g_free (block->open_id);
  g_free (block->note);
  g_free (block);
}

static int
tokens_of (NpStore *store, const char *text)
{
  if (!text)
    return 0;
  return (int) ((g_utf8_strlen (text, -1) + store->conf.chars_per_token - 1) / store->conf.chars_per_token);
}

static const char *
freq_key (NpContext *self, const char *block_id)
{
  JsonObject *freq = self->store->conf.block_freq;
  const char *value = NULL;

  if (freq && json_object_has_member (freq, block_id))
    value = json_object_get_string_member (freq, block_id);
  return value ? value : "static";
}

static Block *
block_new (const char *id, const char *title, const char *source, int tokens, const char *freq,
           const char *open_id, const char *note)
{
  Block *block = g_new0 (Block, 1);

  block->id = g_strdup (id);
  block->title = g_strdup (title);
  block->source = g_strdup (source);
  block->tokens = tokens;
  block->freq = g_strdup (freq);
  block->open_id = g_strdup (open_id);
  block->note = g_strdup (note);
  return block;
}

static char *
frontmatter_value (const char *text, const char *key)
{
  g_auto(GStrv) lines = g_strsplit (text, "\n", -1);
  guint n = g_strv_length (lines);

  if (n < 2 || g_strcmp0 (lines[0], "---") != 0)
    return NULL;
  for (guint i = 1; i < n && g_strcmp0 (lines[i], "---") != 0; i++)
    {
      g_autofree char *prefix = g_strdup_printf ("%s:", key);

      if (g_str_has_prefix (lines[i], prefix))
        {
          char *value = g_strdup (lines[i] + strlen (prefix));

          return g_strstrip (value);
        }
    }
  return NULL;
}

static char *
read_file (const char *path)
{
  char *text = NULL;

  if (path && g_file_get_contents (path, &text, NULL, NULL))
    return text;
  return NULL;
}

static const char *
var_value (JsonObject *vars, const char *name)
{
  JsonArray *array = vars && json_object_has_member (vars, "variables")
                     ? json_object_get_array_member (vars, "variables") : NULL;

  for (guint i = 0; array && i < json_array_get_length (array); i++)
    {
      JsonObject *v = json_array_get_object_element (array, i);
      const char *key = json_object_has_member (v, "name") ? json_object_get_string_member (v, "name") : NULL;

      if (g_strcmp0 (key, name) == 0 && json_object_has_member (v, "value"))
        return json_object_get_string_member (v, "value");
    }
  return NULL;
}

/* --- the blocks, in no particular order --- */

static void
add_tools (NpContext *self, GPtrArray *blocks)
{
  g_autofree char *text = read_file (self->store->conf.opencode_json);
  g_autoptr(JsonParser) parser = json_parser_new ();
  GString *names = g_string_new ("");
  const char *note = NULL;

  if (text && json_parser_load_from_data (parser, text, -1, NULL)
      && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      JsonObject *config = json_node_get_object (json_parser_get_root (parser));
      JsonObject *mcp = json_object_has_member (config, "mcp") ? json_object_get_object_member (config, "mcp") : NULL;

      if (mcp)
        {
          GList *members = json_object_get_members (mcp);

          for (GList *l = members; l; l = l->next)
            {
              JsonNode *node = json_object_get_member (mcp, l->data);

              if (node && JSON_NODE_HOLDS_OBJECT (node))
                {
                  /* the servers of the "servers" section, not the section itself */
                  GList *inner = json_object_get_members (json_node_get_object (node));

                  for (GList *i = inner; i; i = i->next)
                    {
                      if (names->len)
                        g_string_append (names, ", ");
                      g_string_append (names, i->data);
                    }
                  g_list_free (inner);
                  continue;
                }
              if (names->len)
                g_string_append (names, ", ");
              g_string_append (names, l->data);
            }
          g_list_free (members);
        }
    }
  note = *names->str ? names->str : np_ui (self->store, "context.tools_none");
  g_ptr_array_add (blocks, block_new ("tools", np_ui (self->store, "context.tools"),
                                      np_ui (self->store, "context.tools_source"), -1,
                                      freq_key (self, "tools"), "opencode-config", note));
  g_string_free (names, TRUE);
}

static void
add_skills (NpContext *self, GPtrArray *blocks)
{
  GDir *d = NULL;
  const char *entry;

  if (!self->store->conf.skills_dir)
    return;
  d = g_dir_open (self->store->conf.skills_dir, 0, NULL);
  while (d && (entry = g_dir_read_name (d)))
    {
      g_autofree char *path = g_build_filename (self->store->conf.skills_dir, entry, "SKILL.md", NULL);
      g_autofree char *text = read_file (path);
      g_autofree char *name = NULL;
      g_autofree char *id = NULL;
      g_autofree char *open_id = NULL;
      g_autofree char *title = NULL;

      if (!text)
        continue;
      name = frontmatter_value (text, "name");
      id = g_strdup_printf ("skill:%s", entry);
      open_id = g_strdup (id);
      title = g_strdup_printf ("%s %s", np_ui (self->store, "context.skill"), name ? name : entry);
      g_ptr_array_add (blocks, block_new ("skills", title, path, tokens_of (self->store, text),
                                          freq_key (self, "skills"), open_id, NULL));
    }
  if (d)
    g_dir_close (d);
}

static void
build_blocks (NpContext *self, GPtrArray *blocks, const char *state_text, JsonObject *state_vars)
{
  const char *person = state_vars ? var_value (state_vars, "person") : NULL;

  add_tools (self, blocks);
  g_ptr_array_add (blocks, block_new ("opencode", np_ui (self->store, "context.opencode"),
                                      np_ui (self->store, "context.opencode_source"), -1,
                                      freq_key (self, "opencode"), NULL,
                                      np_ui (self->store, "context.opencode_note")));
  add_skills (self, blocks);

  {
    g_autofree char *text = read_file (self->store->conf.agents_file);
    NpDoc *doc = np_doc_by_id (self->store, "system-prompt", NULL);
    g_autofree char *source = doc && doc->source_label ? g_strdup (doc->source_label)
                              : doc && doc->path ? g_strdup (np_short_path (doc->path))
                              : g_strdup (np_short_path (self->store->conf.agents_file));

    g_ptr_array_add (blocks, block_new ("system", np_ui (self->store, "context.system"), source,
                                        tokens_of (self->store, text), freq_key (self, "system"),
                                        "system-prompt", np_ui (self->store, "context.system_note")));
    np_doc_free (doc);
  }
  {
    NpDoc *doc = np_doc_by_id (self->store, "state-template", NULL);
    g_autofree char *source = doc && doc->source_label ? g_strdup (doc->source_label)
                              : doc && doc->path ? g_strdup (np_short_path (doc->path))
                              : g_strdup ("state-template.j2");

    g_ptr_array_add (blocks, block_new ("state", np_ui (self->store, "context.state"), source,
                                        tokens_of (self->store, state_text), freq_key (self, "state"),
                                        "state-template", state_text));
    np_doc_free (doc);
  }
  if (person && *person)
    g_ptr_array_add (blocks, block_new ("notebook", np_ui (self->store, "context.notebook"),
                                        np_ui (self->store, "context.notebook_source"),
                                        tokens_of (self->store, var_value (state_vars, "notebook")),
                                        freq_key (self, "notebook"), NULL,
                                        var_value (state_vars, "notebook")));
  {
    g_autofree char *text = read_file (self->store->conf.input_tags);
    g_autofree char *sample = g_strdup (np_ui (self->store, "context.input_sample"));

    g_ptr_array_add (blocks, block_new ("input", np_ui (self->store, "context.input"),
                                        self->store->conf.input_tags
                                            ? np_short_path (self->store->conf.input_tags) : "input-tags.json",
                                        tokens_of (self->store, text), freq_key (self, "input"),
                                        "input-tags", sample));
  }
  g_ptr_array_add (blocks, block_new ("message", np_ui (self->store, "context.message"),
                                      np_ui (self->store, "context.message_source"), -1,
                                      freq_key (self, "message"), NULL,
                                      np_ui (self->store, "context.message_note")));
  {
    g_autoptr(GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *stamp = g_date_time_format (now, "%Y-%m-%d %H:%M");
    g_autofree char *text = g_strdup_printf ("%s · %s", stamp, g_get_host_name ());

    g_ptr_array_add (blocks, block_new ("env", np_ui (self->store, "context.env"),
                                        np_ui (self->store, "context.env_source"),
                                        tokens_of (self->store, text), freq_key (self, "env"), NULL, NULL));
  }
}

/* the order of the configuration: the ids not named there come after, in the order built */
static gint
order_cmp (gconstpointer a, gconstpointer b, gpointer data)
{
  JsonArray *order = data;
  Block *one = *(Block **) a, *two = *(Block **) b;
  gint io = G_MAXINT, it = G_MAXINT;

  for (guint i = 0; order && i < json_array_get_length (order); i++)
    {
      const char *id = json_array_get_string_element (order, i);

      if (!g_strcmp0 (id, one->id) && io == G_MAXINT)
        io = (gint) i;
      if (!g_strcmp0 (id, two->id) && it == G_MAXINT)
        it = (gint) i;
    }
  return io - it;
}

/* --- drawing --- */

static char *
freq_label (NpContext *self, const char *freq)
{
  if (!g_strcmp0 (freq, "turn"))
    return g_strdup (np_ui (self->store, "context.freq_turn"));
  if (!g_strcmp0 (freq, "session"))
    return g_strdup (np_ui (self->store, "context.freq_session"));
  if (!g_strcmp0 (freq, "day"))
    return g_strdup (np_ui (self->store, "context.freq_day"));
  return g_strdup (np_ui (self->store, "context.freq_static"));
}

static void
row_clicked (GtkWidget *row, gpointer data)
{
  NpContext *self = data;
  const char *id = g_object_get_data (G_OBJECT (row), "np-open");

  if (self->open_cb && id)
    self->open_cb (self->open_data, id, NULL);
}

static void
draw_block (NpContext *self, Block *block)
{
  g_autofree char *freq = freq_label (self, block->freq);
  g_autofree char *tokens = block->tokens < 0 ? g_strdup (np_ui (self->store, "context.unknown_tokens"))
                                               : g_strdup_printf ("≈ %d token", block->tokens);
  g_autofree char *sub = g_strdup_printf ("%s · %s · %s", block->source ? block->source : "", tokens, freq);
  GtkWidget *row = np_row (block->title, sub, "np-block");

  if (block->open_id)
    {
      g_object_set_data_full (G_OBJECT (row), "np-open", g_strdup (block->open_id), g_free);
      g_signal_connect (row, "clicked", G_CALLBACK (row_clicked), self);
    }
  else
    gtk_widget_add_css_class (row, "np-block-flat");
  gtk_box_append (GTK_BOX (self->list), row);
  if (block->note && *block->note)
    {
      GtkWidget *note = np_label (block->note, "np-block-note");

      gtk_box_append (GTK_BOX (self->list), note);
    }
}

static void
cache_line (NpContext *self, GPtrArray *blocks)
{
  int volatile_at = -1, stable_after = 0, sessions = 0;

  for (guint i = 0; i < blocks->len; i++)
    {
      Block *block = g_ptr_array_index (blocks, i);

      if (!g_strcmp0 (block->freq, "turn") && volatile_at < 0)
        volatile_at = (int) i;
      if (!g_strcmp0 (block->freq, "session"))
        sessions++;
    }
  if (volatile_at >= 0)
    for (guint i = volatile_at + 1; i < blocks->len; i++)
      {
        Block *block = g_ptr_array_index (blocks, i);

        if (!g_strcmp0 (block->freq, "static") && block->tokens > 0)
          stable_after += block->tokens;
      }
  if (stable_after >= self->store->conf.cache_min_tokens)
    {
      g_autofree char *text = g_strdup_printf (np_ui (self->store, "context.cache_warning"), stable_after);
      GtkWidget *banner = np_label (text, "np-cache-warning");

      gtk_box_insert_child_after (GTK_BOX (self->list), banner, gtk_widget_get_first_child (self->list));
    }
  if (sessions)
    {
      g_autofree char *quiet = g_strdup_printf (np_ui (self->store, "context.cache_session"), sessions);
      GtkWidget *note = np_label (quiet, "np-block-note");

      gtk_box_insert_child_after (GTK_BOX (self->list), note, gtk_widget_get_first_child (self->list));
    }
}

static void
rebuild (NpContext *self, const char *state_text, JsonObject *state_vars)
{
  g_autoptr(GPtrArray) blocks = g_ptr_array_new_with_free_func ((GDestroyNotify) block_free);
  g_autofree char *total = NULL;
  int sum = 0;
  JsonArray *order = self->store->conf.block_order;

  if (self->list)
    gtk_box_remove (GTK_BOX (self->root), self->list);
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->list, "np-context");
  gtk_widget_set_margin_top (self->list, 8);
  gtk_widget_set_margin_bottom (self->list, 24);
  gtk_box_append (GTK_BOX (self->root), self->list);

  gtk_box_append (GTK_BOX (self->list),
                  np_label (np_ui (self->store, "context.reconstructed"), "np-reconstructed"));

  build_blocks (self, blocks, state_text, state_vars);

  /* the order lives in the configuration: the provider's cache prefix runs first block to last */
  if (order && json_array_get_length (order))
    g_ptr_array_sort_with_data (blocks, order_cmp, order);

  for (guint i = 0; i < blocks->len; i++)
    {
      Block *block = g_ptr_array_index (blocks, i);

      draw_block (self, block);
      if (block->tokens > 0)
        sum += block->tokens;
    }
  cache_line (self, blocks);

  total = g_strdup_printf (np_ui (self->store, "context.total"), sum, self->store->conf.chars_per_token);
  gtk_box_append (GTK_BOX (self->list), np_label (np_ui (self->store, "context.reconstructed_short"), "np-badge"));
  gtk_box_append (GTK_BOX (self->list), np_label (total, "np-total"));
}

typedef struct {
  NpContext *self;
  guint      token;
} Refresh;

static void
state_answered (NpAns *ans, gpointer data)
{
  Refresh *refresh = data;
  NpContext *self = refresh->self;

  if (refresh->token != self->token)
    {
      g_free (refresh);
      return;
    }
  if (!ans->ok || !ans->json)
    rebuild (self, ans->error ? ans->error : np_ui (self->store, "context.state_error"), NULL);
  else
    rebuild (self, ans->text, ans->json);
  g_free (refresh);
}

void
np_context_refresh (NpContext *self)
{
  Refresh *refresh = NULL;
  const char *argv[] = { "{minerva_config}", "state", "show", "--json", NULL };

  self->token++;
  refresh = g_new0 (Refresh, 1);
  refresh->self = self;
  refresh->token = self->token;
  np_run (self->store, argv, NULL, G_OBJECT (self->window), NULL, state_answered, refresh);
}

NpContext *
np_context_new (NpStore *store, GtkWindow *window, GtkWidget *snackbar)
{
  NpContext *self = g_new0 (NpContext, 1);

  self->store = store;
  self->window = window;
  self->snackbar = snackbar;
  self->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (self->root), self->list);
  self->scroll = np_scroller (self->root);
  return self;
}

GtkWidget *
np_context_root (NpContext *self)
{
  return self->scroll;
}

void
np_context_set_open_cb (NpContext *self, void (*cb) (gpointer, const char *id, const char *person), gpointer data)
{
  self->open_cb = cb;
  self->open_data = data;
}
