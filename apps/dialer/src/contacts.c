/* The contacts (contacts.h): an EBookClientView on every enabled address book, its contacts kept in one table. */
#include "contacts.h"
#include "im-form.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libebook/libebook.h>
#include <string.h>

struct _Contacts {
  GObject parent;
  GCancellable *cancellable;
  ESourceRegistry *registry;
  GPtrArray *views;             /* EBookClientView, one per address book */
  guint pending;                /* views not yet complete */
  GHashTable *by_uid;           /* uid -> Contact */
  GHashTable *matches;          /* "handle\nnumber\ncountry" -> Match, emptied on every change */
  GPtrArray *sorted;            /* the same contacts by name; NULL until asked for after a change */
  guint changed_id;             /* "changed" waits for a burst of changes to end */
  gboolean loaded;
  GSettings *settings;          /* the Phone's, for Display options; NULL where its schema is not installed */
  gboolean sort_alternative;    /* Sort by: last name */
  gboolean name_alternative;    /* Name format: last name first */
  GStrv read_fields;            /* where a chat handle is read (im-form.h), from card-form-command; NULL: none */
  GStrv channels;               /* the channels an X-<CHANNEL> field is read for */
  GStrv card_form_command;      /* the Phone's card-form-command; NULL where its schema has none */
};

G_DEFINE_FINAL_TYPE (Contacts, contacts, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
contact_number_free (ContactNumber *number)
{
  g_free (number->number);
  g_free (number->label);
  g_free (number);
}

static void
contact_free (Contact *contact)
{
  g_free (contact->uid);
  g_free (contact->name_primary);
  g_free (contact->name_alternative);
  g_free (contact->sort_key_primary);
  g_free (contact->sort_key_alternative);
  g_ptr_array_unref (contact->numbers);
  g_strfreev (contact->handles);
  g_clear_pointer (&contact->photo, g_bytes_unref);
  g_free (contact);
}

/* ContactsContract.CommonDataKinds.Phone.getTypeLabel, from a vCard TEL's TYPE parameters */
static char *
number_label (EVCardAttribute *tel)
{
  GList *types = e_vcard_attribute_get_param (tel, EVC_TYPE);
  gboolean fax = FALSE, home = FALSE, work = FALSE, cell = FALSE, pager = FALSE;

  for (GList *l = types; l; l = l->next)
    {
      const char *type = l->data;

      if (g_ascii_strcasecmp (type, "FAX") == 0)
        fax = TRUE;
      else if (g_ascii_strcasecmp (type, "HOME") == 0)
        home = TRUE;
      else if (g_ascii_strcasecmp (type, "WORK") == 0)
        work = TRUE;
      else if (g_ascii_strcasecmp (type, "CELL") == 0)
        cell = TRUE;
      else if (g_ascii_strcasecmp (type, "PAGER") == 0)
        pager = TRUE;
      else if (g_ascii_strcasecmp (type, "CAR") == 0)
        return g_strdup (_("Car"));
      else if (g_ascii_strcasecmp (type, "ISDN") == 0)
        return g_strdup (_("ISDN"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-ASSISTANT") == 0)
        return g_strdup (_("Assistant"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-CALLBACK") == 0)
        return g_strdup (_("Callback"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-COMPANY") == 0)
        return g_strdup (_("Company main"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-RADIO") == 0)
        return g_strdup (_("Radio"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-TELEX") == 0)
        return g_strdup (_("Telex"));
      else if (g_ascii_strcasecmp (type, "X-EVOLUTION-TTYTDD") == 0)
        return g_strdup (_("TTY/TDD"));
    }
  if (fax)
    return g_strdup (work ? _("Work fax") : home ? _("Home fax") : _("Other fax"));
  if (pager)
    return g_strdup (work ? _("Work pager") : _("Pager"));
  if (cell)
    return g_strdup (work ? _("Work mobile") : _("Mobile"));
  if (work)
    return g_strdup (_("Work"));
  if (home)
    return g_strdup (_("Home"));
  return g_strdup (_("Other"));
}

static gboolean
has_text (const char *s)
{
  return s && *s;
}

/* the two orders of the name, NameSplitter.join's for a Western name: the given names, the middle ones and the
 * family name; or the family name, a comma, then the given and middle ones */
static void
contact_set_names (Contact *contact, EContact *econtact)
{
  g_autofree char *full_name = e_contact_get (econtact, E_CONTACT_FULL_NAME);
  EContactName *name = e_contact_get (econtact, E_CONTACT_NAME);
  const char *given = name && has_text (name->given) ? name->given : NULL;
  const char *middle = name && has_text (name->additional) ? name->additional : NULL;
  const char *family = name && has_text (name->family) ? name->family : NULL;

  if (has_text (full_name))
    contact->name_primary = g_steal_pointer (&full_name);
  else
    contact->name_primary = g_strstrip (g_strjoin (" ", given ? given : "", middle ? middle : "",
                                                   family ? family : "", NULL));
  /* the joins leave a double space where a part is missing */
  for (char *gap; (gap = strstr (contact->name_primary, "  "));)
    memmove (gap, gap + 1, strlen (gap));
  if (family && (given || middle))
    contact->name_alternative = g_strdup_printf ("%s, %s%s%s", family, given ? given : "",
                                                 given && middle ? " " : "", middle ? middle : "");
  else
    contact->name_alternative = g_strdup (contact->name_primary);
  contact->sort_key_primary = g_utf8_collate_key (contact->name_primary, -1);
  contact->sort_key_alternative = g_utf8_collate_key (contact->name_alternative, -1);
  e_contact_name_free (name);
}

static void
contact_apply_order (Contact *contact, gboolean sort_alternative, gboolean name_alternative)
{
  contact->name = name_alternative ? contact->name_alternative : contact->name_primary;
  contact->sort_name = sort_alternative ? contact->name_alternative : contact->name_primary;
  contact->sort_key = sort_alternative ? contact->sort_key_alternative : contact->sort_key_primary;
}

static Contact *
contact_from_econtact (Contacts *self, EContact *econtact)
{
  Contact *contact = g_new0 (Contact, 1);
  g_autolist(EVCardAttribute) tels = e_contact_get_attributes (econtact, E_CONTACT_TEL);
  EContactPhoto *photo = e_contact_get (econtact, E_CONTACT_PHOTO);

  contact->uid = e_contact_get (econtact, E_CONTACT_UID);
  contact_set_names (contact, econtact);
  contact_apply_order (contact, self->sort_alternative, self->name_alternative);
  contact->numbers = g_ptr_array_new_with_free_func ((GDestroyNotify) contact_number_free);
  for (GList *l = tels; l; l = l->next)
    {
      g_autofree char *value = e_vcard_attribute_get_value (l->data);
      ContactNumber *number;

      if (!value || !*value)
        continue;
      number = g_new0 (ContactNumber, 1);
      number->number = g_steal_pointer (&value);
      number->label = number_label (l->data);
      g_ptr_array_add (contact->numbers, number);
    }
  /* the chat handles, "<channel>:<id>", in every form Minerva's memory service reads them */
  contact->handles = im_form_handles (E_VCARD (econtact), (const char *const *) self->read_fields,
                                      (const char *const *) self->channels);
  if (photo && photo->type == E_CONTACT_PHOTO_TYPE_INLINED && photo->data.inlined.length)
    contact->photo = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);
  e_contact_photo_free (photo);
  return contact;
}

/* --- changes */

static gboolean
emit_changed (gpointer data)
{
  Contacts *self = data;

  self->changed_id = 0;
  g_signal_emit (self, signals[CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static void
changed (Contacts *self)
{
  g_clear_pointer (&self->sorted, g_ptr_array_unref);
  g_hash_table_remove_all (self->matches);
  /* an address book comes in pieces: one "changed" for all of them */
  if (!self->changed_id)
    self->changed_id = g_timeout_add (100, emit_changed, self);
}

static void
on_objects_added (EBookClientView *view, const GSList *econtacts, gpointer data)
{
  Contacts *self = data;

  for (const GSList *l = econtacts; l; l = l->next)
    {
      Contact *contact = contact_from_econtact (self, l->data);

      if (contact->numbers->len == 0 || !contact->uid)
        {
          contact_free (contact);
          continue;
        }
      g_hash_table_replace (self->by_uid, contact->uid, contact);
    }
  changed (self);
}

static void
on_objects_removed (EBookClientView *view, const GSList *uids, gpointer data)
{
  Contacts *self = data;

  for (const GSList *l = uids; l; l = l->next)
    g_hash_table_remove (self->by_uid, l->data);
  changed (self);
}

static void
on_complete (EBookClientView *view, const GError *error, gpointer data)
{
  Contacts *self = data;

  if (self->pending && --self->pending == 0)
    {
      self->loaded = TRUE;
      changed (self);
    }
}

/* --- the address books */

static void
on_view (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  EBookClientView *view = NULL;
  Contacts *self;

  if (!e_book_client_get_view_finish (E_BOOK_CLIENT (source), result, &view, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self = data;
          g_warning ("contacts: %s", error->message);
          on_complete (NULL, NULL, self);
        }
      return;
    }
  self = data;
  g_ptr_array_add (self->views, view);
  g_signal_connect (view, "objects-added", G_CALLBACK (on_objects_added), self);
  g_signal_connect (view, "objects-modified", G_CALLBACK (on_objects_added), self);
  g_signal_connect (view, "objects-removed", G_CALLBACK (on_objects_removed), self);
  g_signal_connect (view, "complete", G_CALLBACK (on_complete), self);
  e_book_client_view_start (view, &error);
  if (error)
    g_warning ("contacts: %s", error->message);
}

static void
on_client (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(EClient) client = e_book_client_connect_finish (result, &error);
  g_autofree char *query = NULL;
  Contacts *self;

  if (!client)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          self = data;
          g_warning ("contacts: %s", error->message);
          on_complete (NULL, NULL, self);
        }
      return;
    }
  self = data;
  /* the contacts with a phone number (HAS_PHONE_NUMBER) or a chat handle (Messages' providers, im-form.h) */
  {
    g_auto(GStrv) fields = im_form_fields ((const char *const *) self->read_fields, (const char *const *) self->channels);
    g_autoptr(GPtrArray) either = g_ptr_array_new ();
    EBookQuery *q;

    g_ptr_array_add (either, e_book_query_field_exists (E_CONTACT_TEL));
    for (guint i = 0; fields[i]; i++)
      g_ptr_array_add (either, e_book_query_vcard_field_exists (fields[i]));
    q = e_book_query_or ((int) either->len, (EBookQuery **) either->pdata, TRUE);
    query = e_book_query_to_string (q);
    e_book_query_unref (q);
  }
  e_book_client_get_view (E_BOOK_CLIENT (client), query, self->cancellable, on_view, self);
}

static void
on_registry (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  ESourceRegistry *registry = e_source_registry_new_finish (result, &error);
  g_autolist(ESource) sources = NULL;
  Contacts *self;

  if (!registry)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
      /* no Evolution Data Server: no contacts, which is a full answer too (every number shows as itself) */
      g_warning ("contacts: no address books: %s", error->message);
      self = data;
      self->loaded = TRUE;
      changed (self);
      return;
    }
  self = data;
  self->registry = registry;
  sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  self->pending = g_list_length (sources);
  if (!self->pending)
    {
      self->loaded = TRUE;
      changed (self);
    }
  for (GList *l = sources; l; l = l->next)
    e_book_client_connect (l->data, 30, self->cancellable, on_client, self);
}

/* --- Display options */

static void
read_order (Contacts *self)
{
  GHashTableIter iter;
  gpointer value;

  self->sort_alternative = g_settings_get_enum (self->settings, "sort-order") == 1;
  self->name_alternative = g_settings_get_enum (self->settings, "display-order") == 1;
  g_hash_table_iter_init (&iter, self->by_uid);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    contact_apply_order (value, self->sort_alternative, self->name_alternative);
}

static void
on_order_changed (Contacts *self)
{
  read_order (self);
  changed (self);
}

/* --- the model */

static int
by_name (gconstpointer a, gconstpointer b)
{
  const Contact *ca = *(Contact **) a, *cb = *(Contact **) b;

  return strcmp (ca->sort_key, cb->sort_key);
}

GPtrArray *
contacts_get_all (Contacts *self)
{
  if (!self->sorted)
    {
      GHashTableIter iter;
      gpointer value;

      self->sorted = g_ptr_array_new ();
      g_hash_table_iter_init (&iter, self->by_uid);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        if (((Contact *) value)->numbers->len)
          g_ptr_array_add (self->sorted, value);
      g_ptr_array_sort (self->sorted, by_name);
    }
  return self->sorted;
}

const Contact *
contacts_get (Contacts *self, const char *uid)
{
  return uid ? g_hash_table_lookup (self->by_uid, uid) : NULL;
}

const Contact *
contacts_lookup (Contacts *self, const char *number, const char *country)
{
  GHashTableIter iter;
  gpointer value;

  if (!number || !*number)
    return NULL;
  g_hash_table_iter_init (&iter, self->by_uid);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (contact_number_label (value, number, country))
      return value;
  return NULL;
}

typedef struct {
  guint count;
  char *uid;                    /* the one contact, when count is 1 */
} Match;

static void
match_free (Match *m)
{
  g_free (m->uid);
  g_free (m);
}

guint
contacts_match (Contacts *self, const char *handle, const char *number, const char *country,
                const Contact **contact)
{
  g_autofree char *key = g_strjoin ("\n", handle ? handle : "", number ? number : "", country ? country : "", NULL);
  Match *m = g_hash_table_lookup (self->matches, key);

  if (!m)
    {
      GHashTableIter iter;
      gpointer value;

      m = g_new0 (Match, 1);
      g_hash_table_iter_init (&iter, self->by_uid);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          Contact *c = value;

          if ((handle && *handle && c->handles && g_strv_contains ((const char * const *) c->handles, handle))
              || (number && *number && contact_number_label (c, number, country)))
            {
              if (!m->count++)
                m->uid = g_strdup (c->uid);
            }
        }
      if (m->count != 1)
        g_clear_pointer (&m->uid, g_free);
      g_hash_table_insert (self->matches, g_steal_pointer (&key), m);
    }
  if (contact)
    *contact = m->uid ? g_hash_table_lookup (self->by_uid, m->uid) : NULL;
  return m->count;
}

const char *
contact_number_label (const Contact *contact, const char *number, const char *country)
{
  for (guint i = 0; i < contact->numbers->len; i++)
    {
      ContactNumber *n = contact->numbers->pdata[i];

      if (phone_number_equal (n->number, number, country))
        return n->label;
    }
  return NULL;
}

gboolean
contacts_is_loaded (Contacts *self)
{
  return self->loaded;
}

static void
contacts_dispose (GObject *object)
{
  Contacts *self = NEO_DIALER_CONTACTS (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->changed_id, g_source_remove);
  if (self->views)
    {
      for (guint i = 0; i < self->views->len; i++)
        g_signal_handlers_disconnect_by_data (self->views->pdata[i], self);
      g_clear_pointer (&self->views, g_ptr_array_unref);
    }
  g_clear_object (&self->registry);
  g_clear_object (&self->cancellable);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (contacts_parent_class)->dispose (object);
}

static void
contacts_finalize (GObject *object)
{
  Contacts *self = NEO_DIALER_CONTACTS (object);

  g_clear_pointer (&self->sorted, g_ptr_array_unref);
  g_hash_table_unref (self->by_uid);
  g_hash_table_unref (self->matches);
  g_strfreev (self->read_fields);
  g_strfreev (self->channels);
  g_strfreev (self->card_form_command);
  G_OBJECT_CLASS (contacts_parent_class)->finalize (object);
}

static void
contacts_class_init (ContactsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = contacts_dispose;
  object_class->finalize = contacts_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
contacts_init (Contacts *self)
{
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (schemas, "de.yesman.Dialer", TRUE)
                                              : NULL;

  self->cancellable = g_cancellable_new ();
  self->views = g_ptr_array_new_with_free_func (g_object_unref);
  self->by_uid = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) contact_free);
  self->matches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) match_free);
  /* the app and the service (a missed call's name) alike */
  if (schema)
    {
      self->settings = g_settings_new ("de.yesman.Dialer");
      g_signal_connect_swapped (self->settings, "changed::sort-order", G_CALLBACK (on_order_changed), self);
      g_signal_connect_swapped (self->settings, "changed::display-order", G_CALLBACK (on_order_changed), self);
      read_order (self);
      if (g_settings_schema_has_key (schema, "card-form-command"))
        self->card_form_command = g_settings_get_strv (self->settings, "card-form-command");
    }
}

/* --- how a chat handle is written on a card: asked of Minerva's contacts resolver (card-form-command), once */

static GStrv
strv_member (JsonObject *o, const char *member)
{
  JsonArray *list = json_object_has_member (o, member) ? json_object_get_array_member (o, member) : NULL;
  GStrvBuilder *builder = g_strv_builder_new ();
  GStrv out;

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      const char *text = json_array_get_string_element (list, i);

      if (text && *text)
        g_strv_builder_add (builder, text);
    }
  out = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  return out;
}

/* the answer, then the address books; a resolver that cannot answer leaves no chat handle (the numbers still match) */
static void
on_card_form (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *out = NULL;
  g_autoptr(JsonParser) parser = json_parser_new ();
  Contacts *self;

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result, &out, NULL, &error)
      && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  self = data;
  if (!error && g_subprocess_get_successful (G_SUBPROCESS (source)) && out
      && json_parser_load_from_data (parser, out, -1, &error) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      JsonObject *o = json_node_get_object (json_parser_get_root (parser));

      self->read_fields = strv_member (o, "im_read_fields");
      self->channels = strv_member (o, "channels");
    }
  else
    g_warning ("contacts: no chat handles, the card form is not known: %s",
               error ? error->message : "the resolver failed");
  e_source_registry_new (self->cancellable, on_registry, self);
}

Contacts *
contacts_new (void)
{
  Contacts *self = g_object_new (CONTACTS_TYPE, NULL);
  GStrv argv = self->card_form_command;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) resolver = argv && argv[0] && *argv[0]
                                    ? g_subprocess_newv ((const char *const *) argv,
                                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                         &error)
                                    : NULL;

  if (resolver)
    g_subprocess_communicate_utf8_async (resolver, NULL, self->cancellable, on_card_form, self);
  else
    {
      if (error)
        g_warning ("contacts: no chat handles: %s", error->message);
      e_source_registry_new (self->cancellable, on_registry, self);
    }
  return self;
}
