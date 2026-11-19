/* Who a conversation is with (msg-people.h). */
#include "msg-people.h"
#include "msg-directory.h"
#include "phone-number.h"
#include "sms-agent.h"
#include "sms-provider.h"

/* @chat starts with the provider's @key prefix (``lid-prefix``, ``group-prefix``) */
static gboolean
has_prefix_of (const char *owner, const char *chat, const char *key)
{
  g_autofree char *prefix = sms_provider_value (owner, key);

  return prefix && *prefix && g_str_has_prefix (chat, prefix);
}

/* the provider's own name for a chat with no contact: the account's (an address book name, a group's subject), else
 * the one the sender chose, as ``push-name-format`` marks it (no format: never shown); NULL when it has none */
static char *
directory_name (const char *owner, const char *destination)
{
  gboolean push = FALSE;
  const char *name = msg_directory_name (msg_directory_get_default (), destination, &push);
  g_autofree char *format = NULL;
  g_auto(GStrv) parts = NULL;

  if (!name || !*name)
    return NULL;
  if (!push)
    return g_strdup (name);
  format = sms_provider_value (owner, "push-name-format");
  if (!format || !*format)
    return NULL;
  parts = g_strsplit (format, "%s", 2);   /* the sender's text is never a format string */
  return parts[1] ? g_strconcat (parts[0], name, parts[1], NULL) : g_strconcat (parts[0], name, NULL);
}

MsgPerson *
msg_person_new (Contacts *contacts, const char *destination, const char *country)
{
  MsgPerson *person = g_new0 (MsgPerson, 1);
  /* an agent is named by its name, and is no contact */
  /* a provider's chat is the contact whose IMPP is its address, or that lists the number after the address
   * (whatsapp:+39...); a LID or a group has no number. Two contacts are not guessed between: the number shows.
   * TODO(X-MINERVA-CHAT-<provider>): the owner's choice on an ambiguous chat, read from the chosen contact's vCard */
  g_autofree char *owner = sms_provider_owner (destination);
  g_autofree char *chat = owner ? sms_provider_chat (destination) : NULL;
  g_autofree char *label = owner ? sms_provider_label (owner) : NULL;
  const char *number = chat && !has_prefix_of (owner, chat, "lid-prefix") && !has_prefix_of (owner, chat, "group-prefix")
                       ? chat : (chat ? NULL : destination);
  const Contact *contact = NULL;
  guint matches = contacts && !sms_agent_name (destination)
                  ? contacts_match (contacts, owner ? destination : NULL, number, country, &contact) : 0;

  person->destination = g_strdup (destination);
  if (sms_agent_name (destination))
    person->name = g_strdup (sms_agent_name (destination));
  else if (contact)
    {
      person->is_contact = TRUE;
      person->name = g_strdup (contact->name_primary ? contact->name_primary : contact->name);
      if (label && sms_provider_flag (owner, "show-label"))
        {
          char *shown = g_strdup_printf ("%s \xc2\xb7 %s", person->name, label);

          g_free (person->name);
          person->name = shown;
        }
      person->identifier = g_strdup (contact->uid);
      person->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
    }
  if ((!person->name || !*person->name) && chat && matches == 0 && sms_provider_flag (owner, "directory-names"))
    {
      g_free (person->name);
      person->name = directory_name (owner, destination);
    }
  if (!person->name || !*person->name)
    {
      g_free (person->name);
      person->name = phone_number_format (chat ? chat : destination, country);
    }
  if (!person->is_contact && chat && label && sms_provider_flag (owner, "show-label"))
    {
      char *shown = g_strdup_printf ("%s \xc2\xb7 %s", person->name, label);

      g_free (person->name);
      person->name = shown;
    }
  if (!person->identifier)
    person->identifier = g_strdup (person->name);
  return person;
}

MsgPerson *
msg_person_copy (const MsgPerson *person)
{
  MsgPerson *copy = g_new0 (MsgPerson, 1);

  copy->destination = g_strdup (person->destination);
  copy->name = g_strdup (person->name);
  copy->identifier = g_strdup (person->identifier);
  copy->photo = person->photo ? g_bytes_ref (person->photo) : NULL;
  copy->is_contact = person->is_contact;
  return copy;
}

void
msg_person_free (MsgPerson *person)
{
  if (!person)
    return;
  g_free (person->destination);
  g_free (person->name);
  g_free (person->identifier);
  g_clear_pointer (&person->photo, g_bytes_unref);
  g_free (person);
}

GPtrArray *
msg_people_new (Contacts *contacts, const char * const *destinations, const char *country)
{
  GPtrArray *people = g_ptr_array_new_with_free_func ((GDestroyNotify) msg_person_free);

  for (guint i = 0; destinations && destinations[i]; i++)
    g_ptr_array_add (people, msg_person_new (contacts, destinations[i], country));
  return people;
}

char *
msg_people_name (GPtrArray *people)
{
  GString *name = g_string_new (NULL);

  for (guint i = 0; i < people->len; i++)
    {
      if (name->len)
        g_string_append (name, ", ");
      g_string_append (name, ((MsgPerson *) people->pdata[i])->name);
    }
  return g_string_free (name, FALSE);
}
