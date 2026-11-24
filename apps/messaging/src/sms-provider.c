/* The messaging providers' registry (sms-provider.h): the channels' sections from the channel registry, under an INI
 * file of addresses read when it changes. */

#include "sms-provider.h"

#include <string.h>
#include <sys/stat.h>

#include "sms-agent.h"
#include "minerva-layers.h"
#include "ns-mc.h"

/* the sections of the file that are not providers */
static const char *NOT_A_PROVIDER[] = { "providers", "scopes", "limits", "defaults", "events", "agents", "agent-refusals",
                                        "agent-levels", NULL };

const char *
sms_provider_conf_path (void)
{
  const char *path = g_getenv ("MINERVA_MESSAGING_CONF");

  return path && *path ? path : NULL;
}

#define MESSAGING_CONF "messaging.conf"

/* the files read, lowest first: MINERVA_MESSAGING_CONF alone (a test's file), else every configuration layer of
 * messaging.conf (Minerva's docs/packaging.md, "Configuration": the package's defaults, the device's, /etc, the
 * owner's changed keys), their folders read from the layers' file once; on a phone without that file (no minerva-base
 * yet) the owner's file alone, as before the layers. with_missing: also the paths that do not exist (to notice one
 * that comes) */
static GStrv
conf_files (gboolean with_missing)
{
  static gsize once = 0;
  static GStrv layers = NULL;
  const char *set = sms_provider_conf_path ();
  g_autoptr(GStrvBuilder) out = g_strv_builder_new ();

  if (g_once_init_enter (&once))
    {
      g_autofree char *owner = minerva_layers_user_file (MESSAGING_CONF);

      layers = owner ? minerva_layers_paths (MESSAGING_CONF, TRUE) : NULL;
      if (!layers)
        {
          const char *one[] = { NULL, NULL };
          g_autofree char *path = g_build_filename (g_get_user_config_dir (), "minerva", MESSAGING_CONF, NULL);

          one[0] = path;
          layers = g_strdupv ((char **) one);
        }
      g_once_init_leave (&once, 1);
    }
  if (set)
    {
      g_strv_builder_add (out, set);
      return g_strv_builder_end (out);
    }
  for (guint i = 0; layers[i]; i++)
    if (with_missing || g_file_test (layers[i], G_FILE_TEST_EXISTS))
      g_strv_builder_add (out, layers[i]);
  return g_strv_builder_end (out);
}

/* the parsed layers, kept until one of them changes (its mtime or size, or a layer that comes or goes) */
static GKeyFile *registry = NULL;
static char *registry_stamp = NULL;

static char *
conf_stamp (void)
{
  g_auto(GStrv) paths = conf_files (TRUE);
  GString *stamp = g_string_new (NULL);
  struct stat info;

  for (guint i = 0; paths[i]; i++)
    if (!stat (paths[i], &info))
      g_string_append_printf (stamp, "%u:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ";", i, (gint64) info.st_mtime,
                              (gint64) info.st_size);
  return g_string_free (stamp, FALSE);
}

/* the channels' sections: each channel's "messaging" object of `minerva-config channels --json`, asked once per process
 * (a reload asks again); empty when minerva-config is not installed or fails */
static GKeyFile *channels = NULL;

/* a scalar member as the INI would hold it; NULL for an object, an array or null, which a section cannot hold */
static char *
scalar_text (JsonNode *node)
{
  GType type;

  if (!JSON_NODE_HOLDS_VALUE (node))
    return NULL;
  type = json_node_get_value_type (node);
  if (type == G_TYPE_BOOLEAN)
    return g_strdup (json_node_get_boolean (node) ? "true" : "false");
  if (type == G_TYPE_INT64)
    return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (node));
  if (type == G_TYPE_DOUBLE)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      return g_strdup (g_ascii_dtostr (buf, sizeof buf, json_node_get_double (node)));
    }
  return g_strdup (json_node_get_string (node));
}

static GKeyFile *
channels_conf (void)
{
  const char *args[] = { "channels", "--json", NULL };
  g_autofree char *err = NULL;
  g_autoptr(JsonNode) root = NULL;
  JsonObject *all;
  GList *names;

  if (channels)
    return channels;
  channels = g_key_file_new ();
  if (!ns_minerva_available ())
    return channels;
  root = ns_mc_run_sync ("minerva-config", args, &err);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_message ("the channel registry: %s", err ? err : "not an object");
      return channels;
    }
  all = json_node_get_object (root);
  names = json_object_get_members (all);
  for (GList *l = names; l; l = l->next)
    {
      JsonObject *section = ns_json_obj (ns_json_obj (all, l->data), "messaging");
      GList *keys = section ? json_object_get_members (section) : NULL;

      for (GList *k = keys; k; k = k->next)
        {
          g_autofree char *value = scalar_text (json_object_get_member (section, k->data));

          if (value)
            g_key_file_set_value (channels, l->data, k->data, value);
        }
      g_list_free (keys);
    }
  g_list_free (names);
  return channels;
}

/* the merged sections forgotten: rebuilt at the next call, the channels' kept */
static void
registry_forget (void)
{
  g_clear_pointer (&registry, g_key_file_free);
  g_clear_pointer (&registry_stamp, g_free);
}

void
sms_provider_reload (void)
{
  g_clear_pointer (&channels, g_key_file_free);
  registry_forget ();
}

static gboolean
is_provider_section (const char *name)
{
  for (guint i = 0; NOT_A_PROVIDER[i]; i++)
    if (g_str_equal (NOT_A_PROVIDER[i], name))
      return FALSE;
  return TRUE;
}

/* the INI as GKeyFile reads it: a line that starts with ';' is a comment, and so is what follows " ;" in a value */
static char *
strip_comments (const char *raw)
{
  g_auto(GStrv) lines = g_strsplit (raw, "\n", -1);
  GString *out = g_string_new (NULL);

  for (guint i = 0; lines[i]; i++)
    {
      const char *line = g_strstrip (lines[i]);
      const char *cut;

      if (*line == ';' || *line == '#')
        {
          g_string_append_c (out, '\n');
          continue;
        }
      if ((cut = strstr (line, " ;")) || (cut = strstr (line, "\t;")))
        {
          g_autofree char *value = g_strndup (line, cut - line);

          g_string_append (out, g_strchomp (value));
        }
      else
        g_string_append (out, line);
      g_string_append_c (out, '\n');
    }
  return g_string_free (out, FALSE);
}

/* the file is the phone's INI (``key = value  ; comment``): GKeyFile knows only '#' comments */
GKeyFile *
sms_provider_load_ini (const char *path)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *raw = NULL;
  g_autofree char *text = NULL;

  if (!path || !g_file_get_contents (path, &raw, NULL, NULL) || !(text = strip_comments (raw))
      || !g_key_file_load_from_data (file, text, -1, G_KEY_FILE_NONE, NULL))
    return NULL;
  return g_steal_pointer (&file);
}

/* @file's keys over the ones already in @into */
static gboolean
keys_over (GKeyFile *into, GKeyFile *file)
{
  g_auto(GStrv) groups = NULL;
  gboolean any = FALSE;

  groups = g_key_file_get_groups (file, NULL);
  for (guint g = 0; groups[g]; g++)
    {
      g_auto(GStrv) keys = g_key_file_get_keys (file, groups[g], NULL, NULL);

      for (guint k = 0; keys && keys[k]; k++)
        {
          g_autofree char *value = g_key_file_get_value (file, groups[g], keys[k], NULL);

          g_key_file_set_value (into, groups[g], keys[k], value);
          any = TRUE;
        }
    }
  return any;
}

/* one layer's keys over the ones already in into */
static gboolean
layer_over (GKeyFile *into, const char *path)
{
  g_autoptr(GKeyFile) file = sms_provider_load_ini (path);

  if (!file)
    return FALSE;
  keys_over (into, file);
  return TRUE;
}

static GKeyFile *
conf (void)
{
  g_autofree char *stamp = conf_stamp ();

  if (!registry || g_strcmp0 (stamp, registry_stamp) != 0)
    {
      g_auto(GStrv) files = conf_files (FALSE);
      g_autoptr(GKeyFile) merged = g_key_file_new ();
      gboolean any = FALSE;

      registry_forget ();
      /* the channels' sections first, as the packages ship them; the layers' keys over them (an owner's change) */
      any = keys_over (merged, channels_conf ());
      for (guint i = 0; files[i]; i++)
        any |= layer_over (merged, files[i]);
      /* no channel and no layer: the registry is empty, and the agent's prefix stays the only one known */
      if (!any)
        return NULL;
      registry = g_steal_pointer (&merged);
      registry_stamp = g_steal_pointer (&stamp);
    }
  return registry;
}

static char *
address_of (GKeyFile *file, const char *provider_id)
{
  return g_key_file_get_string (file, provider_id, "address", NULL);
}

/* the provider whose address fits @destination, as its id, or NULL */
static char *
owner_or_null (const char *destination)
{
  GKeyFile *file = conf ();
  g_autofree char *best_id = NULL;
  gsize best = 0;
  gsize count = 0;
  g_auto(GStrv) groups = NULL;

  if (!file || !destination || !*destination)
    return NULL;
  groups = g_key_file_get_groups (file, &count);
  for (guint g = 0; groups && groups[g]; g++)
    {
      g_autofree char *address = NULL;
      gsize len;

      if (!is_provider_section (groups[g]) || !(address = address_of (file, groups[g])))
        continue;
      len = strlen (address);
      if (len && len > best && g_str_has_prefix (destination, address) && destination[len] != '\0')
        {
          best = len;
          g_free (best_id);
          best_id = g_strdup (groups[g]);
        }
    }
  return g_steal_pointer (&best_id);
}

char *
sms_provider_owner (const char *destination)
{
  return owner_or_null (destination);
}

char *
sms_provider_chat (const char *destination)
{
  g_autofree char *id = owner_or_null (destination);
  g_autofree char *address = NULL;
  GKeyFile *file = conf ();

  if (!id || !file)
    return NULL;
  address = address_of (file, id);
  return address ? g_strdup (destination + strlen (address)) : NULL;
}

gboolean
sms_provider_is (const char *destination)
{
  g_autofree char *id = owner_or_null (destination);

  return id != NULL;
}

char *
sms_provider_group (const char * const *destinations)
{
  return destinations && destinations[0] && !destinations[1] ? owner_or_null (destinations[0]) : NULL;
}

char *
sms_provider_label (const char *provider_id)
{
  GKeyFile *file = conf ();

  if (!file || !provider_id)
    return NULL;
  return g_key_file_get_string (file, provider_id, "label", NULL);
}

char *
sms_provider_value (const char *provider_id, const char *key)
{
  GKeyFile *file = conf ();
  g_autofree char *value = NULL;

  if (!file || !provider_id || !key || !(value = g_key_file_get_string (file, provider_id, key, NULL)))
    return NULL;
  return g_strdup (g_strstrip (value));
}

char *
sms_provider_text (const char *section, const char *key)
{
  g_autofree char *value = sms_provider_value (section, key);
  gsize len = value ? strlen (value) : 0;

  /* a value written in quotes keeps its spaces; the quotes are not part of it (the Go and Python readers agree) */
  if (len >= 2 && value[0] == value[len - 1] && (value[0] == '"' || value[0] == '\''))
    return g_strndup (value + 1, len - 2);
  return g_steal_pointer (&value);
}

char *
sms_provider_address (const char *provider_id)
{
  return sms_provider_value (provider_id, "address");
}

gboolean
sms_provider_flag (const char *provider_id, const char *key)
{
  GKeyFile *file = conf ();
  g_autofree char *value = NULL;

  if (!file || !provider_id || !key)
    return FALSE;
  value = g_key_file_get_string (file, provider_id, key, NULL);
  return value && (g_ascii_strcasecmp (value, "true") == 0 || g_str_equal (value, "1")
                   || g_ascii_strcasecmp (value, "yes") == 0 || g_ascii_strcasecmp (value, "on") == 0);
}

GStrv
sms_provider_ids (void)
{
  GKeyFile *file = conf ();
  gsize count = 0;
  g_auto(GStrv) groups = NULL;
  GPtrArray *ids = g_ptr_array_new_null_terminated (0, g_free, TRUE);

  if (!file)
    return (GStrv) g_ptr_array_free (ids, FALSE);
  groups = g_key_file_get_groups (file, &count);
  for (guint i = 0; groups && groups[i]; i++)
    {
      g_autofree char *address = NULL;

      if (!is_provider_section (groups[i]) || !(address = address_of (file, groups[i])))
        continue;
      if (*address)
        g_ptr_array_add (ids, g_strdup (groups[i]));
    }
  return (GStrv) g_ptr_array_free (ids, FALSE);
}
