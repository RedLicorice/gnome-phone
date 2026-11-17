/* The logic of the Minerva parts of Contacts (ct-mnv-core.h). */
#include "ct-mnv-core.h"

#include <math.h>
#include <string.h>

char *
ct_mnv_format (const char *tpl, const char *const *pairs)
{
  GString *out = g_string_new (tpl ? tpl : "");

  for (guint i = 0; pairs && pairs[i] && pairs[i + 1]; i += 2)
    {
      g_autofree char *mark = g_strdup_printf ("{%s}", pairs[i]);

      g_string_replace (out, mark, pairs[i + 1], 0);
    }
  return g_string_free (out, FALSE);
}

GPtrArray *
ct_mnv_loose (JsonObject *known)
{
  GPtrArray *loose = g_ptr_array_new ();
  JsonArray *people = known && json_object_has_member (known, "people") ? json_object_get_array_member (known, "people")
                                                                           : NULL;

  for (guint i = 0; people && i < json_array_get_length (people); i++)
    {
      JsonObject *person = json_array_get_object_element (people, i);

      if (person && json_object_has_member (person, "id")
          && !(json_object_has_member (person, "contact_uid")
               && *json_object_get_string_member_with_default (person, "contact_uid", "")))
        g_ptr_array_add (loose, person);
    }
  return loose;
}

gboolean
ct_mnv_name_stale (const char *person_name, const char *contact_name)
{
  g_autofree char *a = g_strstrip (g_strdup (person_name ? person_name : ""));
  g_autofree char *b = g_strstrip (g_strdup (contact_name ? contact_name : ""));

  return *b && strcmp (a, b) != 0;
}

gboolean
ct_mnv_rule_value (const char *text, double lo, double hi, gboolean integer, char **value)
{
  g_autofree char *plain = g_strstrip (g_strdup (text ? text : ""));
  char *end = NULL;
  double number;

  *value = NULL;
  g_strdelimit (plain, ",", '.');
  if (!*plain)
    return FALSE;
  number = g_ascii_strtod (plain, &end);
  if (*end || !isfinite (number) || number < lo || number > hi)
    return FALSE;
  if (integer && number != floor (number))
    return FALSE;
  if (integer)
    *value = g_strdup_printf ("%d", (int) number);
  else
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_formatd (buf, sizeof buf, "%.2f", number);
      *value = g_strdup (buf);
    }
  return TRUE;
}

char *
ct_mnv_date (JsonObject *o, const char *member, const char *format)
{
  JsonNode *node = o ? json_object_get_member (o, member) : NULL;
  g_autoptr(GDateTime) when = NULL;

  if (!node || !JSON_NODE_HOLDS_VALUE (node))
    return NULL;
  if (json_node_get_value_type (node) == G_TYPE_STRING)
    {
      const char *text = json_node_get_string (node);
      g_autoptr(GTimeZone) zone = g_time_zone_new_local ();

      if (!text || !*text)
        return NULL;
      when = g_date_time_new_from_iso8601 (text, zone);
      if (!when)
        {
          g_autofree char *full = g_strconcat (text, "T00:00:00", NULL);

          when = g_date_time_new_from_iso8601 (full, zone);
        }
      if (!when)
        return g_strdup (text);
    }
  else
    {
      double seconds = json_node_get_double (node);

      if (seconds <= 0)
        return NULL;
      if (seconds > 1e11)
        seconds /= 1000;        /* milliseconds */
      when = g_date_time_new_from_unix_local ((gint64) seconds);
    }
  return when ? g_date_time_format (when, format && *format ? format : "%x") : NULL;
}

static JsonArray *
people_of (JsonObject *known)
{
  return known && json_object_has_member (known, "people") ? json_object_get_array_member (known, "people") : NULL;
}

const char *
ct_mnv_owner_contact (JsonObject *known)
{
  JsonArray *people = people_of (known);
  const char *owner = known ? json_object_get_string_member_with_default (known, "owner", "") : "";

  for (guint i = 0; *owner && people && i < json_array_get_length (people); i++)
    {
      JsonObject *person = json_array_get_object_element (people, i);

      if (person && !g_strcmp0 (json_object_get_string_member_with_default (person, "id", ""), owner))
        return json_object_get_string_member_with_default (person, "contact_uid", "");
    }
  return "";
}

gboolean
ct_mnv_is_owner (JsonObject *known, JsonObject *person)
{
  const char *owner = known ? json_object_get_string_member_with_default (known, "owner", "") : "";

  return *owner && person && !g_strcmp0 (json_object_get_string_member_with_default (person, "id", ""), owner);
}

GPtrArray *
ct_mnv_owned_by (JsonObject *known, const char *uid)
{
  GPtrArray *owned = g_ptr_array_new ();
  JsonArray *people = people_of (known);

  for (guint i = 0; uid && *uid && people && i < json_array_get_length (people); i++)
    {
      JsonObject *person = json_array_get_object_element (people, i);

      if (person && !g_strcmp0 (json_object_get_string_member_with_default (person, "owner_contact", ""), uid))
        g_ptr_array_add (owned, person);
    }
  return owned;
}

GPtrArray *
ct_mnv_of_kind (JsonObject *known, const char *kind, gboolean loose_only)
{
  GPtrArray *found = g_ptr_array_new ();
  JsonArray *people = people_of (known);

  for (guint i = 0; people && i < json_array_get_length (people); i++)
    {
      JsonObject *person = json_array_get_object_element (people, i);

      if (!person || !json_object_has_member (person, "id")
          || g_strcmp0 (json_object_get_string_member_with_default (person, "kind", "person"), kind))
        continue;
      if (loose_only && *json_object_get_string_member_with_default (person, "contact_uid", ""))
        continue;
      g_ptr_array_add (found, person);
    }
  return found;
}
