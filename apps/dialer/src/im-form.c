/* How a chat channel id is written and read on a vCard (im-form.h). */
#include "im-form.h"

#include <string.h>

static gboolean
is_prefix (const char *field)
{
  return g_str_has_suffix (field, "-");
}

static char *
first_value (EVCardAttribute *attr)
{
  GList *values = e_vcard_attribute_get_values (attr);

  return g_strstrip (g_strdup (values && values->data ? values->data : ""));
}

gboolean
im_form_usable (const ImForm *form)
{
  return form && form->write_field && *form->write_field && form->read_fields && form->read_fields[0];
}

char *
im_form_property (const char *field, const char *channel)
{
  g_autofree char *whole = NULL;

  if (!is_prefix (field))
    return g_strdup (field);
  whole = g_strconcat (field, channel, NULL);
  return g_ascii_strup (whole, -1);
}

gboolean
im_form_holds (EVCardAttribute *attr, const char *field, const char *channel, char **id)
{
  g_autofree char *property = im_form_property (field, channel);
  g_autofree char *value = NULL;
  const char *colon;

  if (g_ascii_strcasecmp (e_vcard_attribute_get_name (attr), property) != 0)
    return FALSE;
  value = first_value (attr);
  if (is_prefix (field))
    {
      if (!*value)
        return FALSE;
      if (id)
        *id = g_steal_pointer (&value);
      return TRUE;
    }
  colon = strchr (value, ':');
  if (!colon || g_ascii_strncasecmp (value, channel, colon - value) != 0 || channel[colon - value])
    return FALSE;
  if (id)
    *id = g_strstrip (g_strdup (colon + 1));
  return TRUE;
}

static void
add_once (GPtrArray *list, char *handle)
{
  for (guint i = 0; i < list->len; i++)
    if (g_strcmp0 (list->pdata[i], handle) == 0)
      {
        g_free (handle);
        return;
      }
  g_ptr_array_add (list, handle);
}

GStrv
im_form_handles (EVCard *vcard, const char *const *read_fields, const char *const *channels)
{
  GPtrArray *handles = g_ptr_array_new ();

  for (guint f = 0; read_fields && read_fields[f]; f++)
    for (GList *l = e_vcard_get_attributes (vcard); l; l = l->next)
      {
        if (!is_prefix (read_fields[f]))
          {
            g_autofree char *value = NULL;

            if (g_ascii_strcasecmp (e_vcard_attribute_get_name (l->data), read_fields[f]) != 0)
              continue;
            value = first_value (l->data);
            if (strchr (value, ':'))
              add_once (handles, g_steal_pointer (&value));
            continue;
          }
        for (guint c = 0; channels && channels[c]; c++)
          {
            g_autofree char *id = NULL;

            if (im_form_holds (l->data, read_fields[f], channels[c], &id))
              add_once (handles, g_strconcat (channels[c], ":", id, NULL));
          }
      }
  g_ptr_array_add (handles, NULL);
  return (GStrv) g_ptr_array_free (handles, FALSE);
}

GStrv
im_form_fields (const char *const *read_fields, const char *const *channels)
{
  GPtrArray *fields = g_ptr_array_new ();

  for (guint f = 0; read_fields && read_fields[f]; f++)
    {
      if (!is_prefix (read_fields[f]))
        add_once (fields, g_strdup (read_fields[f]));
      for (guint c = 0; is_prefix (read_fields[f]) && channels && channels[c]; c++)
        add_once (fields, im_form_property (read_fields[f], channels[c]));
    }
  g_ptr_array_add (fields, NULL);
  return (GStrv) g_ptr_array_free (fields, FALSE);
}
