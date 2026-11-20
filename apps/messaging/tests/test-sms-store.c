/* sms-store against bugle_db's rules: a conversation per set of participants, the newest message as its snippet and
 * order, read and seen, drafts, archiving, and a conversation going with its last message */
#include "../src/sms-store.h"

static char *
temp_path (const char *name)
{
  g_autofree char *dir = g_dir_make_tmp ("test-sms-store-XXXXXX", NULL);

  return g_build_filename (dir, name, NULL);
}

/* how many, the list freed */
static guint
count (GPtrArray *list)
{
  guint n = list->len;

  g_ptr_array_unref (list);
  return n;
}

static SmsStore *
open_store (char **path)
{
  g_autoptr(GError) error = NULL;
  SmsStore *store;

  *path = temp_path ("bugle_db");
  store = sms_store_open (*path, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  return store;
}

static void
test_incoming (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  g_autoptr(GPtrArray) conversations = NULL;
  g_autoptr(GPtrArray) messages = NULL;
  SmsConversation *first;
  SmsMessage *message;
  gint64 anna, bob;

  anna = sms_store_add_incoming (store, "+491701234567", "Hi", 900, 1000, "/mm/Sms/1", NULL);
  bob = sms_store_add_incoming (store, "+491709999999", "Hello", 1900, 2000, "/mm/Sms/2", NULL);
  g_assert_cmpint (anna, >, 0);
  g_assert_cmpint (bob, >, anna);
  /* the same sender: the same conversation */
  g_assert_cmpint (sms_store_add_incoming (store, "+491701234567", "Are you there?", 2900, 3000, NULL, NULL), >, 0);

  conversations = sms_store_list_conversations (store, FALSE);
  g_assert_cmpuint (conversations->len, ==, 2);
  first = conversations->pdata[0];
  /* newest first, the newest message its snippet */
  g_assert_cmpstr (first->snippet, ==, "Are you there?");
  g_assert_cmpint (first->sort_timestamp, ==, 3000);
  g_assert_cmpstr (first->destinations[0], ==, "+491701234567");
  g_assert_null (first->destinations[1]);
  g_assert_false (first->read);
  g_assert_cmpint (first->latest_status, ==, MESSAGE_STATUS_INCOMING_COMPLETE);

  messages = sms_store_list_messages (store, first->id);
  g_assert_cmpuint (messages->len, ==, 2);
  message = messages->pdata[0];
  g_assert_cmpstr (message->text, ==, "Hi");
  g_assert_cmpstr (message->sender, ==, "+491701234567");
  g_assert_cmpint (message->sent, ==, 900);
  g_assert_cmpstr (message->sms_uri, ==, "/mm/Sms/1");
  g_assert_false (message->seen);
  /* only the newest one, still oldest first */
  g_clear_pointer (&messages, g_ptr_array_unref);
  messages = sms_store_list_last_messages (store, first->id, 1);
  g_assert_cmpuint (messages->len, ==, 1);
  g_assert_cmpstr (((SmsMessage *) messages->pdata[0])->text, ==, "Are you there?");

  /* unseen: the notification's three; seen for one conversation, then read for the other */
  g_assert_cmpuint (count (sms_store_list_unseen (store)), ==, 3);
  g_assert_cmpuint (sms_store_mark_seen (store, first->id), ==, 2);
  g_assert_cmpuint (sms_store_mark_read (store, ((SmsConversation *) conversations->pdata[1])->id), ==, 1);
  g_assert_cmpuint (count (sms_store_list_unseen (store)), ==, 0);
  sms_store_free (store);
}

static void
test_outgoing (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  const char *group[] = { "+491709999999", "+491701234567", NULL };
  const char *same_group[] = { "+491701234567", "+491709999999", NULL };
  const char *alone[] = { "+491701234567", NULL };
  g_autoptr(SmsConversation) conversation = NULL;
  g_autoptr(SmsMessage) message = NULL;
  gint64 id, one, sent;

  id = sms_store_get_conversation (store, group);
  g_assert_cmpint (id, >, 0);
  /* the same people in another order are the same conversation; one of them alone is another */
  g_assert_cmpint (sms_store_get_conversation (store, same_group), ==, id);
  one = sms_store_get_conversation (store, alone);
  g_assert_cmpint (one, !=, id);
  /* an empty conversation is not listed */
  g_assert_cmpuint (count (sms_store_list_conversations (store, FALSE)), ==, 0);

  /* a draft lists it, then sending takes the draft away */
  sms_store_set_draft (store, id, "Meet at 8?");
  conversation = sms_store_get (store, id);
  g_assert_true (conversation->show_draft);
  g_assert_cmpstr (conversation->draft, ==, "Meet at 8?");
  g_assert_cmpuint (count (sms_store_list_conversations (store, FALSE)), ==, 1);
  g_clear_pointer (&conversation, sms_conversation_free);

  sent = sms_store_add_outgoing (store, id, "Meet at 8?", 5000, NULL);
  g_assert_cmpint (sent, >, 0);
  conversation = sms_store_get (store, id);
  g_assert_false (conversation->show_draft);
  g_assert_null (conversation->draft);
  g_assert_true (conversation->read);
  g_assert_cmpstr (conversation->snippet, ==, "Meet at 8?");
  g_assert_cmpint (conversation->latest_status, ==, MESSAGE_STATUS_OUTGOING_YET_TO_SEND);
  g_assert_cmpuint (count (sms_store_list_unsent (store)), ==, 1);

  sms_store_set_status (store, sent, MESSAGE_STATUS_OUTGOING_FAILED, 42, 0);
  message = sms_store_get_message (store, sent);
  g_assert_null (message->sender);
  g_assert_null (message->origin);  /* the owner's own message */
  g_assert_cmpint (message->status, ==, MESSAGE_STATUS_OUTGOING_FAILED);
  g_assert_cmpint (message->raw_status, ==, 42);
  g_assert_cmpint (message->sent, ==, 5000);
  g_assert_cmpuint (count (sms_store_list_unsent (store)), ==, 0);
  sms_store_set_status (store, sent, MESSAGE_STATUS_OUTGOING_COMPLETE, 0, 5100);
  g_clear_pointer (&message, sms_message_free);
  message = sms_store_get_message (store, sent);
  g_assert_cmpint (message->sent, ==, 5100);

  /* the conversation goes with its last message */
  g_assert_cmpint (sms_store_delete_message (store, sent), ==, id);
  g_assert_null (sms_store_get (store, id));
  sms_store_free (store);
}

static void
test_archive_and_delete (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  g_autoptr(GPtrArray) archived = NULL;
  g_autoptr(SmsMessage) message = NULL;
  gint64 id;

  id = sms_store_add_incoming (store, "22000", "Your code is 1234", 0, 1000, NULL, NULL);
  message = sms_store_get_message (store, id);
  sms_store_set_archived (store, message->conversation_id, TRUE);
  g_assert_cmpuint (count (sms_store_list_conversations (store, FALSE)), ==, 0);
  archived = sms_store_list_conversations (store, TRUE);
  g_assert_cmpuint (archived->len, ==, 1);
  g_assert_true (((SmsConversation *) archived->pdata[0])->archived);

  /* a new message brings it back */
  sms_store_add_incoming (store, "22000", "Your code is 5678", 0, 2000, NULL, NULL);
  g_assert_cmpuint (count (sms_store_list_conversations (store, FALSE)), ==, 1);

  /* deleting it takes its messages along */
  g_assert_true (sms_store_delete_conversation (store, message->conversation_id));
  g_assert_cmpuint (count (sms_store_list_conversations (store, FALSE)), ==, 0);
  g_assert_cmpuint (count (sms_store_list_messages (store, message->conversation_id)), ==, 0);
  g_assert_cmpuint (count (sms_store_list_unseen (store)), ==, 0);
  sms_store_free (store);
}

/* the app's read-only view sees what the service wrote */
static void
test_read_only (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  g_autoptr(GError) error = NULL;
  SmsStore *reader;
  g_autoptr(GPtrArray) conversations = NULL;

  sms_store_add_incoming (store, "+491701234567", "Hi", 0, 1000, NULL, NULL);
  reader = sms_store_open (path, TRUE, &error);
  g_assert_no_error (error);
  conversations = sms_store_list_conversations (reader, FALSE);
  g_assert_cmpuint (conversations->len, ==, 1);
  sms_store_add_incoming (store, "+491701234567", "Still there?", 0, 2000, NULL, NULL);
  g_assert_cmpuint (count (sms_store_list_messages (reader, ((SmsConversation *) conversations->pdata[0])->id)),
                    ==, 2);
  sms_store_free (reader);
  sms_store_free (store);
}

/* an agent's message keeps who wrote it (SendAs's origin), also in a store made before the origin column */
static void
test_origin (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  const char *to[] = { "whatsapp:+390001", NULL };
  gint64 conversation = sms_store_get_conversation (store, to);
  gint64 id = sms_store_add_outgoing (store, conversation, "Arrivo alle 8", 1000, "agent:agent/pc-test");
  g_autoptr(GError) error = NULL;
  SmsMessage *message = sms_store_get_message (store, id);

  g_assert_cmpstr (message->origin, ==, "agent:agent/pc-test");
  sms_message_free (message);
  sms_store_free (store);
  store = sms_store_open (path, FALSE, &error);  /* opened again: the migration leaves a store that has the column */
  g_assert_no_error (error);
  message = sms_store_get_message (store, id);
  g_assert_cmpstr (message->origin, ==, "agent:agent/pc-test");
  sms_message_free (message);
  sms_store_free (store);
}

/* a provider's message id is kept in the message's uri: the same id is found again, another is not */
static void
test_find_by_uri (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  gint64 id = sms_store_add_incoming (store, "whatsapp:+491701111111", "Ciao", 1000, 2000, "WA-1", NULL);

  g_assert_cmpint (id, >, 0);
  g_assert_cmpint (sms_store_find_by_uri (store, "WA-1"), ==, id);
  g_assert_cmpint (sms_store_find_by_uri (store, "WA-2"), ==, 0);
  g_assert_cmpint (sms_store_find_by_uri (store, ""), ==, 0);
  g_assert_cmpint (sms_store_find_by_uri (store, NULL), ==, 0);
  sms_store_free (store);
}

/* a provider's voice note keeps its typed part (kind, duration, thumbnail) next to its label; a text message has none */
static void
test_typed_part (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  static const guint8 jpeg[] = { 0xff, 0xd8, 0xff };
  g_autoptr(GVariantDict) dict = g_variant_dict_new (NULL);
  g_autoptr(GVariant) part_dict = NULL;
  g_autoptr(SmsPart) part = NULL;
  g_autoptr(GPtrArray) messages = NULL;
  const SmsMessage *voice;
  gint64 id;

  g_variant_dict_insert (dict, "kind", "s", "voice");
  g_variant_dict_insert (dict, "mime", "s", "audio/ogg");
  g_variant_dict_insert (dict, "duration", "u", 12000);
  g_variant_dict_insert_value (dict, "thumbnail", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, jpeg, sizeof jpeg, 1));
  part_dict = g_variant_ref_sink (g_variant_dict_end (dict));
  part = sms_part_new_from_dict (part_dict);
  id = sms_store_add_incoming (store, "whatsapp:+491701111111", "vocale 0:12", 1000, 2000, "WA-1", part);
  sms_store_add_incoming (store, "whatsapp:+491701111111", "Ciao", 1000, 3000, "WA-2", NULL);
  g_autoptr(SmsMessage) first = sms_store_get_message (store, id);
  messages = sms_store_list_last_messages (store, first->conversation_id, 2);
  voice = messages->pdata[0];
  g_assert_cmpstr (voice->text, ==, "vocale 0:12");
  g_assert_nonnull (voice->part);
  g_assert_cmpstr (voice->part->kind, ==, "voice");
  g_assert_cmpstr (voice->part->mime, ==, "audio/ogg");
  g_assert_cmpuint (voice->part->duration, ==, 12000);
  g_assert_cmpuint (g_bytes_get_size (voice->part->thumbnail), ==, sizeof jpeg);
  g_assert_null (((SmsMessage *) messages->pdata[1])->part);
  /* transcribed: the transcript kept, and the text (what every reader of the message gets) with it */
  sms_store_set_transcript (store, id, "ci vediamo", "vocale 0:12: «ci vediamo»");
  {
    g_autoptr(SmsMessage) again = sms_store_get_message (store, id);

    g_assert_cmpstr (again->text, ==, "vocale 0:12: «ci vediamo»");
    g_assert_cmpstr (again->part->transcript, ==, "ci vediamo");
  }
  sms_store_free (store);
}

/* a provider's reaction, edit and deletion change the message they point at */
static void
test_changes (void)
{
  g_autofree char *path = NULL;
  SmsStore *store = open_store (&path);
  gint64 id = sms_store_add_incoming (store, "whatsapp:+491701111111", "Ciao", 1000, 2000, "WA-1", NULL);
  g_autoptr(SmsMessage) changed = NULL, deleted = NULL;

  g_assert_cmpint (sms_store_change (store, "reaction", "WA-1", "anna", "👍"), >, 0);
  sms_store_change (store, "reaction", "WA-1", "bob", "❤️");
  sms_store_change (store, "reaction", "WA-1", "anna", "😂");      /* anna's reaction replaced */
  sms_store_change (store, "reaction", "WA-1", "bob", "");         /* bob's taken away */
  sms_store_change (store, "edit", "WA-1", "", "Ciao!");
  g_assert_cmpint (sms_store_change (store, "edit", "WA-404", "", "x"), ==, 0);
  changed = sms_store_get_message (store, id);
  g_assert_cmpstr (changed->reactions, ==, "😂");
  g_assert_cmpstr (changed->text, ==, "Ciao!");
  g_assert_true (changed->edited);
  sms_store_change (store, "revoke", "WA-1", "", "messaggio eliminato");
  deleted = sms_store_get_message (store, id);
  g_assert_cmpstr (deleted->text, ==, "messaggio eliminato");
  g_assert_null (deleted->reactions);
  sms_store_free (store);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sms-store/find-by-uri", test_find_by_uri);
  g_test_add_func ("/sms-store/incoming", test_incoming);
  g_test_add_func ("/sms-store/outgoing", test_outgoing);
  g_test_add_func ("/sms-store/archive-and-delete", test_archive_and_delete);
  g_test_add_func ("/sms-store/read-only", test_read_only);
  g_test_add_func ("/sms-store/origin", test_origin);
  g_test_add_func ("/sms-store/typed-part", test_typed_part);
  g_test_add_func ("/sms-store/changes", test_changes);
  return g_test_run ();
}
