/* The messaging providers' registry (sms-provider.h): an INI file of addresses, read when it changes. */

#include "sms-provider.h"

#include <string.h>
#include <sys/stat.h>

#include "sms-agent.h"

/* the sections of the file that are not providers */
static const char *NOT_A_PROVIDER[] = { "providers", "scopes", "limits", "defaults", "events", NULL };

const char *
sms_provider_conf_path (void)
{
  const char *path = g_getenv ("MINERVA_MESSAGING_CONF");

  return path && *path ? path : NULL;
}

static char *
user_path (void)
{
  const char *set = sms_provider_conf_path ();

  return set ? g_strdup (set) : g_build_filename (g_get_user_config_dir (), "minerva", "messaging.conf", NULL);
}

/* the parsed file, kept until its mtime or size changes */
static GKeyFile *registry = NULL;
static gint64 registry_stamp = 0;

void
sms_provider_reload (void)
{
  g_clear_pointer (&registry, g_key_file_free);
  registry_stamp = 0;
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

static GKeyFile *
conf (void)
{
  g_autofree char *path = user_path ();
  struct stat info;

  if (!stat (path, &info))
    {
      gint64 stamp = (gint64) info.st_mtime * G_GINT64_CONSTANT (1000000000) + (gint64) info.st_size;

      if (!registry || stamp != registry_stamp)
        {
          g_autoptr(GKeyFile) file = g_key_file_new ();
          g_autofree char *raw = NULL;
          g_autofree char *text = NULL;

          sms_provider_reload ();
          /* the file is the phone's INI (``key = value  ; comment``): GKeyFile knows only '#' comments */
          if (g_file_get_contents (path, &raw, NULL, NULL) && (text = strip_comments (raw))
              && g_key_file_load_from_data (file, text, -1, G_KEY_FILE_NONE, NULL))
            {
              registry = g_steal_pointer (&file);
              registry_stamp = stamp;
            }
        }
      return registry;
    }
  /* no file: the registry is empty, and the agent's prefix stays the only one known */
  return NULL;
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
