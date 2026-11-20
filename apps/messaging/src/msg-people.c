/* Who a conversation is with (msg-people.h). */
#include "msg-people.h"
#include "msg-assistant.h"
#include "msg-directory.h"
#include "phone-number.h"
#include "sms-agent.h"
#include "sms-provider.h"

#include <json-glib/json-glib.h>

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

/* Minerva's own contact (docs/contacts-roles.md), the name and the photo of the conversation of the agent the
 * messaging.conf [agent] ``self`` key names: asked of the one contacts resolver (``self_command``, then
 * ``photo_command <id>``), kept for ``self_refresh_s``. A resolver that cannot answer leaves the agent's name. */
typedef struct {
  gint64 at;                    /* g_get_monotonic_time () of the last ask, 0 never */
  char *name;
  GBytes *photo;
} SelfCard;

static JsonObject *
resolver_ask (const char *key, const char *extra, JsonParser *parser)
{
  g_autofree char *line = sms_provider_value ("agent", key);
  g_auto(GStrv) argv = NULL;
  g_autoptr(GPtrArray) args = g_ptr_array_new ();
  g_autofree char *out = NULL;
  int status = 0;

  if (!line || !*line || !g_shell_parse_argv (line, NULL, &argv, NULL))
    return NULL;
  for (guint i = 0; argv[i]; i++)
    g_ptr_array_add (args, argv[i]);
  if (extra)
    g_ptr_array_add (args, (gpointer) extra);
  g_ptr_array_add (args, NULL);
  if (!g_spawn_sync (NULL, (char **) args->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, &status, NULL) || !g_spawn_check_wait_status (status, NULL))
    return NULL;
  if (!out || !json_parser_load_from_data (parser, out, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return NULL;
  return json_node_get_object (json_parser_get_root (parser));
}

static const SelfCard *
self_card (void)
{
  static SelfCard card;
  g_autofree char *refresh = sms_provider_value ("agent", "self_refresh_s");
  gint64 every = (refresh ? g_ascii_strtoll (refresh, NULL, 10) : 0) * G_USEC_PER_SEC;
  g_autoptr(JsonParser) p1 = json_parser_new (), p2 = json_parser_new ();
  JsonObject *me, *photo;

  if (card.at && g_get_monotonic_time () - card.at < every)
    return card.name ? &card : NULL;
  card.at = g_get_monotonic_time ();
  g_clear_pointer (&card.name, g_free);
  g_clear_pointer (&card.photo, g_bytes_unref);
  if (!(me = resolver_ask ("self_command", NULL, p1)) || !json_object_get_string_member_with_default (me, "id", NULL))
    return NULL;
  card.name = g_strdup (json_object_get_string_member_with_default (me, "name", ""));
  if ((photo = resolver_ask ("photo_command", json_object_get_string_member (me, "id"), p2)))
    {
      gsize len = 0;
      guchar *bytes = g_base64_decode (json_object_get_string_member_with_default (photo, "data", ""), &len);

      if (len)
        card.photo = g_bytes_new_take (bytes, len);
      else
        g_free (bytes);
    }
  return *card.name ? &card : NULL;
}

/* @destination is the conversation of the agent that shows her own contact ([agent] self; without the key the node's
 * assistant, assistant.conf; empty: none) */
static gboolean
is_self_agent (const char *destination)
{
  g_autofree char *self = sms_provider_value ("agent", "self");
  const char *agent = self ? self : msg_assistant_name ();

  return agent && *agent && g_strcmp0 (sms_agent_name (destination), agent) == 0;
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
  if (is_self_agent (destination) && self_card ())
    {
      const SelfCard *me = self_card ();

      person->name = g_strdup (me->name);
      person->photo = me->photo ? g_bytes_ref (me->photo) : NULL;
    }
  else if (sms_agent_name (destination))
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
