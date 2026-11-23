/* neo-messaging-sms: the Messaging's service, for everything about text messages that must go on with the app
 * closed, as Android's Messaging does from its receivers and actions (spec §10.2). It runs in the session from login
 * (neo-messaging-sms.service): a message that comes is kept in bugle_db, taken off the modem and notified; one
 * written is sent, tried again while there is no network, and notified when it is given up.
 *
 * This file is its face on D-Bus (de.yesman.Messaging.Sms) and its notifications. It is bugle_db's only writer: the
 * app reads the store itself, read-only, and is told by Changed what to read again.
 *
 * It stands aside while gsm2sip is in gateway mode, as the Phone's service does: then gsm2sipd takes the modem's
 * messages to SIP, and a second taker would delete them off the modem before it got them. */
#include "messaging-types.h"
#include "sms-agent.h"
#include "sms-modem.h"
#include "sms-provider.h"
#include "sms-store.h"
#include "blocked-numbers.h"
#include "contacts.h"
#include "msg-callers.h"
#include "msg-markdown.h"
#include "notifier.h"
#include "phone.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>
#include <string.h>

#define GATEWAY_NAME "com.callagent.Gsm2sipd"
#define GATEWAY_PATH "/com/callagent/Gsm2sipd"
#define GATEWAY_INTERFACE "com.callagent.Gsm2sipd1"

/* BugleGservicesKeys: a message that cannot go is tried again 5 s on, then twice as long each time up to 2 h, and
 * given up 20 min after it was first tried */
#define RETRY_INITIAL_MS 5000
#define RETRY_MAX_MS (2 * 60 * 60 * 1000)
#define RETRY_TIMEOUT_MS (20 * 60 * 1000)
/* MessageNotificationState: a group's name, in the notification for several conversations, its first 30 characters
 * (how many messages and how much of each the notification shows: "notification-lines", "notification-chars") */
#define GROUP_NAME_CHARS 30
/* a message to an agent that the bridge has not taken within 60 s fails (NEO_MESSAGING_AGENT_TIMEOUT_MS for the
 * tests) */
#define AGENT_TIMEOUT_MS (60 * 1000)

static const char introspection[] =
  "<node>"
  "  <interface name='" SMS_INTERFACE "'>"
  /* a modem that can send: FALSE without one, and in gateway mode */
  "    <property name='Available' type='b' access='read'/>"
  /* the network's country (ISO 3166), for formatting numbers; '' when not registered */
  "    <property name='Country' type='s' access='read'/>"
  /* the conversation with exactly these people (numbers as typed or stored), made if there is none */
  "    <method name='GetConversation'><arg name='destinations' type='as' direction='in'/>"
  "      <arg name='conversation' type='x' direction='out'/></method>"
  /* a message written into a conversation: kept, then sent. The message's id */
  "    <method name='Send'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  /* a failed message tried again (its "Tap to try again") */
  "    <method name='Resend'><arg name='message' type='x' direction='in'/></method>"
  "    <method name='MarkRead'><arg name='conversation' type='x' direction='in'/></method>"
  "    <method name='SetArchived'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='archived' type='b' direction='in'/></method>"
  /* the text typed and not sent; '' for none */
  "    <method name='SetDraft'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/></method>"
  "    <method name='DeleteConversation'><arg name='conversation' type='x' direction='in'/></method>"
  "    <method name='DeleteMessage'><arg name='message' type='x' direction='in'/></method>"
  /* what the app shows in front: -1 nothing, 0 the conversation list scrolled to its top, else that conversation.
   * Then what comes there is not notified (it is read, or seen, at once); an app that quits counts as -1 */
  "    <method name='SetVisible'><arg name='conversation' type='x' direction='in'/></method>"
  /* bugle_db changed: this conversation, or 0 for more than one */
  "    <signal name='Changed'><arg name='conversation' type='x'/></signal>"
  /* a conversation's unread messages are now this many (0: all read); GetUnread for the count before any change */
  "    <signal name='UnreadChanged'><arg name='conversation' type='x'/><arg name='count' type='u'/></signal>"
  /* the owner tapped a provider's message whose media is not on the phone: the provider of @destination downloads
   * the media of its message @ext (of type @mime) and answers with SetMedia */
  "    <signal name='MediaWanted'><arg name='destination' type='s'/><arg name='ext' type='s'/>"
  "      <arg name='mime' type='s'/></signal>"
  "    <method name='FetchMedia'><arg name='message' type='x' direction='in'/></method>"
  /* the media of the provider's message @ext is now at @path, on this phone */
  "    <method name='SetMedia'><arg name='ext' type='s' direction='in'/><arg name='path' type='s' direction='in'/></method>"
  /* a voice note of the provider's message @ext, transcribed on the phone: its @transcript, and the message's new
   * @text (its label with the transcript) */
  /* a provider's change to its message @ext: @kind "reaction" (@sender's emoji @text, '' takes it away), "edit" (its
   * new @text), "revoke" (deleted for everyone: @text what it says now) */
  "    <method name='Change'><arg name='kind' type='s' direction='in'/><arg name='ext' type='s' direction='in'/>"
  "      <arg name='sender' type='s' direction='in'/><arg name='text' type='s' direction='in'/></method>"
  "    <method name='SetTranscript'><arg name='ext' type='s' direction='in'/>"
  "      <arg name='transcript' type='s' direction='in'/><arg name='text' type='s' direction='in'/></method>"
  /* a conversation's newest @limit messages, oldest first: (id, written by the owner, text, Unix ms when it came or
   * was written), for a view that opens on it (the chat overlay); only the programs of the callers file's group
   * de.yesman.Messaging.Sms.GetMessages may ask (msg-callers.h) */
  "    <method name='GetMessages'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='limit' type='u' direction='in'/><arg name='messages' type='a(xbsx)' direction='out'/></method>"
  "    <method name='GetUnread'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='count' type='u' direction='out'/></method>"
  /* a message written into an agent's conversation (Send, Resend), for the bridge to take; kept as sending until
   * Delivered says it took it (ok) or could not */
  "    <signal name='Outgoing'><arg name='message' type='x'/><arg name='conversation' type='x'/>"
  "      <arg name='destination' type='s'/><arg name='text' type='s'/><arg name='origin' type='s'/></signal>"
  "    <method name='Delivered'><arg name='message' type='x' direction='in'/>"
  "      <arg name='ok' type='b' direction='in'/></method>"
  /* the agent has it (its turn started): a message Delivered took shows as delivered */
  "    <method name='Reached'><arg name='message' type='x' direction='in'/></method>"
  /* Send, naming who wrote it: 'owner' (Giuseppe, in Messages) or 'minerva' (the agent, through a tool). Only a
   * provider's daemon reads it, to apply its write scope to Minerva alone */
  "    <method name='SendAs'><arg name='conversation' type='x' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='origin' type='s' direction='in'/>"
  "      <arg name='message' type='x' direction='out'/></method>"
  /* a message from a provider ('whatsapp:<number>', ...): kept as one that came into its conversation (made if new).
   * @sent the network's time in Unix ms (0: now), @ext the provider's message id (a repeat is kept once), @name its
   * name for the chat (not stored) */
  /* what the owner wrote to a provider's chat elsewhere (its own app on his phone), as the provider saw it: kept as sent
   * by @origin ('phone-wa'), read, no Outgoing and no notification; @sent the network's time in Unix ms (0: now),
   * @ext its id (a repeat is kept once) */
  "    <method name='Sent'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='sent' type='x' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='origin' type='s' direction='in'/>"
  "      <arg name='message' type='x' direction='out'/></method>"
  "    <method name='Receive'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='sent' type='x' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='name' type='s' direction='in'/>"
  "      <arg name='message' type='x' direction='out'/></method>"
  /* Sent and Receive of a message that is not only text: @text is its readable label ("vocale 0:12"), @part what it
   * carries (sms_part_new_from_dict: kind, mime, caption, name, size, duration, thumbnail...), never its media */
  "    <method name='SentMedia'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='sent' type='x' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='origin' type='s' direction='in'/>"
  "      <arg name='part' type='a{sv}' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  /* Receive(Media) of a provider's backlog (a message handed over long after it was sent: a phone that was off, a
   * federation follower): kept as read and seen, no notification; @part may be empty for a text */
  "    <method name='ReceiveOld'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='sent' type='x' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='name' type='s' direction='in'/>"
  "      <arg name='part' type='a{sv}' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  "    <method name='ReceiveMedia'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='sent' type='x' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='name' type='s' direction='in'/>"
  "      <arg name='part' type='a{sv}' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  /* what the owner said to an agent ('agent:<name>') in another way (by voice), kept as sent in its conversation
   * (made if new) without Outgoing: it reached the agent already, nothing is delivered */
  "    <method name='Record'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  /* what the owner is saying to an agent ('agent:<name>') by voice, shown live in its conversation (made if new) as
   * one message per @ext (the voice's id for it; a second call updates it): its @state (dictating, confirming,
   * sending, sent, delivered, failed, unheard, cancelled, command), its @text so far ('' keeps it), the state's
   * deadline @until (Unix ms, 0 none) and who wrote it, @origin ('' the owner). No Outgoing, no notification */
  "    <method name='Live'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='ext' type='s' direction='in'/><arg name='state' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='until' type='x' direction='in'/>"
  "      <arg name='origin' type='s' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  /* a message from an agent ('agent:<name>'), kept as one that came into its conversation (made if new) */
  "    <method name='Inject'><arg name='destination' type='s' direction='in'/>"
  "      <arg name='text' type='s' direction='in'/><arg name='message' type='x' direction='out'/></method>"
  "  </interface>"
  "</node>";

/* a message sent with a delivery report asked for, until its report comes: what the reference (TP-MR) the report
 * will carry stands for. Held in memory, as Android's SMSDispatcher holds its delivery-pending trackers: a report
 * that comes after the service restarted is let go */
typedef struct {
  gint64 message;
  char *destination;            /* whom this one went to (a group's message goes once to each) */
} Report;

typedef struct {
  gint64 start;                 /* when it was first tried, monotonic µs */
  guint delay;                  /* the next wait, ms */
  guint timer;
  guint sent_to;                /* a group's: the destinations it went to already */
} Retry;

typedef struct {
  GMainLoop *loop;
  GDBusConnection *session, *system;
  guint registration;
  SmsStore *store;
  SmsModem *modem;              /* NULL in gateway mode */
  GDBusProxy *gateway;
  gboolean available;
  Contacts *contacts;
  BlockedNumbers *blocked;      /* the numbers whose texts are kept archived and not notified (blocked-numbers.h) */
  Notifier *notifier;

  guint32 notification;         /* the new messages' */
  gint64 notification_conversation;  /* what tapping it opens: a conversation, or 0 for the list */
  guint32 failure_notification;
  gint64 failure_conversation;
  GHashTable *failed;           /* the messages given up since that was looked at: id -> conversation id */
  GHashTable *unread;           /* the unread count of each conversation as last signalled (UnreadChanged) */

  GQueue outbox;                /* message ids to send now, in order */
  gint64 sending;               /* the one going, or 0 */
  GHashTable *retries;          /* message id -> Retry, while it is still to go */
  GHashTable *reports;          /* reference -> Report: the delivery reports waited for (256 at most: a TP-MR is a
                                 * byte) */
  GSettings *settings;          /* the app's (de.yesman.Messaging), for "delivery-reports"; NULL without its schema */
  GHashTable *agent_waits;      /* message id -> its timeout: the messages to agents the bridge has not taken yet */
  guint agent_timeout;          /* ms */

  gint64 visible;               /* SetVisible's */
  char *visible_sender;
  guint visible_watch;
} Service;

static void send_next (Service *self);

static const char *
country (Service *self)
{
  return self->modem ? sms_modem_get_country (self->modem) : NULL;
}

/* UnreadChanged for each conversation whose unread count is not the one signalled last (a message came, one was
 * read, one deleted): the chat overlay's badge follows Messages' read state instead of keeping a count of its own */
static void
emit_unread (Service *self)
{
  g_autoptr(GHashTable) now = sms_store_unread_counts (self->store);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, now);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (g_hash_table_lookup (self->unread, key) != value)
      g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, SMS_INTERFACE, "UnreadChanged",
                                     g_variant_new ("(xu)", *(gint64 *) key, GPOINTER_TO_UINT (value)), NULL);
  g_hash_table_iter_init (&iter, self->unread);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (now, key))
      g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, SMS_INTERFACE, "UnreadChanged",
                                     g_variant_new ("(xu)", *(gint64 *) key, 0u), NULL);
  g_hash_table_unref (self->unread);
  self->unread = g_steal_pointer (&now);
}

static void
emit_changed (Service *self, gint64 conversation)
{
  g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, SMS_INTERFACE, "Changed",
                                 g_variant_new ("(x)", conversation), NULL);
  emit_unread (self);
}

static void
emit_properties (Service *self)
{
  GVariantBuilder changed;

  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&changed, "{sv}", "Available", g_variant_new_boolean (self->available));
  g_variant_builder_add (&changed, "{sv}", "Country", g_variant_new_string (country (self) ? country (self) : ""));
  g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged", g_variant_new ("(sa{sv}as)", SMS_INTERFACE, &changed, NULL),
                                 NULL);
}

/* --- names */

/* a participant as the notifications name them: the contact's name, else the number formatted; a provider's
 * chat is looked up by the part after its address, and says which provider it is when the file asks for it */
static char *
participant_name (Service *self, const char *destination)
{
  const Contact *contact;
  g_autofree char *owner = sms_provider_owner (destination);
  g_autofree char *chat = owner ? sms_provider_chat (destination) : NULL;
  g_autofree char *label = owner ? sms_provider_label (owner) : NULL;
  const char *shown = chat ? chat : destination;

  if (sms_agent_name (destination))
    return g_strdup (sms_agent_name (destination));
  contact = contacts_lookup (self->contacts, shown, country (self));
  if (contact && contact->name)
    return label && sms_provider_flag (owner, "show-label") ? g_strdup_printf ("%s \xc2\xb7 %s", contact->name, label)
                                                           : g_strdup (contact->name);
  if (chat)
    {
      g_autofree char *formatted = phone_number_format (chat, country (self));

      return label && sms_provider_flag (owner, "show-label") ? g_strdup_printf ("%s \xc2\xb7 %s", formatted, label)
                                                             : g_steal_pointer (&formatted);
    }
  return phone_number_format (destination, country (self));
}

/* a conversation's: its people's names, joined (a group has no name of its own here) */
static char *
conversation_name (Service *self, const SmsConversation *conversation)
{
  GString *name = g_string_new (NULL);

  for (guint i = 0; conversation->destinations && conversation->destinations[i]; i++)
    {
      g_autofree char *one = participant_name (self, conversation->destinations[i]);

      if (name->len)
        g_string_append (name, ", ");
      g_string_append (name, one);
    }
  return g_string_free (name, FALSE);
}

static char *
first_chars (const char *text, glong chars)
{
  return g_utf8_strlen (text, -1) > chars ? g_utf8_substring (text, 0, chars) : g_strdup (text);
}

/* --- the notifications (spec §5) */

/* a string key of the app's settings; @fallback without them or without the key (an older schema) */
static char *
setting_string (Service *self, const char *key, const char *fallback)
{
  g_autoptr(GSettingsSchema) schema = NULL;

  if (!self->settings)
    return g_strdup (fallback);
  g_object_get (self->settings, "settings-schema", &schema, NULL);
  return g_settings_schema_has_key (schema, key) ? g_settings_get_string (self->settings, key) : g_strdup (fallback);
}

/* an unsigned key of the app's settings; 0 (no limit) without them or without the key (an older schema) */
static guint
setting_uint (Service *self, const char *key)
{
  g_autoptr(GSettingsSchema) schema = NULL;

  if (!self->settings)
    return 0;
  g_object_get (self->settings, "settings-schema", &schema, NULL);
  return g_settings_schema_has_key (schema, key) ? g_settings_get_uint (self->settings, key) : 0;
}

/* the new messages' notification, from the messages not yet seen: one conversation's (its last messages), or one
 * for several (a line for each, newest first). @alert: a message came; otherwise it is updated without a heads-up */
static void
update_notification (Service *self, gboolean alert)
{
  static const char *actions[] = { "default", "Open", NULL };
  g_autoptr(GPtrArray) unseen = sms_store_list_unseen (self->store);
  g_autoptr(GArray) conversations = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autoptr(GString) body = g_string_new (NULL);
  g_autofree char *title = NULL;
  /* a message is one line of its text as plain words, cut to the characters a line may have: a long agent reply
   * does not fill the screen; the conversation shows it whole */
  guint lines = setting_uint (self, "notification-lines");
  guint chars = setting_uint (self, "notification-chars");
  g_autofree char *separator = setting_string (self, "notification-separator", "\n");

  /* the conversations, newest first */
  for (guint i = unseen->len; i > 0; i--)
    {
      gint64 id = ((SmsMessage *) unseen->pdata[i - 1])->conversation_id;
      gboolean listed = FALSE;

      for (guint j = 0; j < conversations->len && !listed; j++)
        listed = g_array_index (conversations, gint64, j) == id;
      if (!listed)
        g_array_append_val (conversations, id);
    }
  if (conversations->len == 0)
    {
      if (self->notification)
        notifier_close (self->notifier, self->notification);
      self->notification = 0;
      return;
    }

  if (conversations->len == 1)
    {
      gint64 id = g_array_index (conversations, gint64, 0);
      g_autoptr(SmsConversation) conversation = sms_store_get (self->store, id);
      gboolean group = conversation && conversation->destinations && g_strv_length (conversation->destinations) > 1;
      guint first = lines && unseen->len > lines ? unseen->len - lines : 0;

      title = conversation ? conversation_name (self, conversation) : g_strdup ("");
      /* oldest first; in a group each line says who: "Anna  See you" */
      for (guint i = first; i < unseen->len; i++)
        {
          SmsMessage *message = unseen->pdata[i];
          g_autofree char *preview = msg_markdown_preview (message->text ? message->text : "", chars);

          if (body->len)
            g_string_append (body, separator);
          if (group)
            {
              g_autofree char *sender = participant_name (self, message->sender);

              g_string_append_printf (body, "%s  ", sender);
            }
          g_string_append (body, preview);
        }
      self->notification_conversation = id;
    }
  else
    {
      title = g_strdup_printf (ngettext ("New message", "%d new messages", unseen->len), unseen->len);
      for (guint i = 0; i < conversations->len && (!lines || i < lines); i++)
        {
          gint64 id = g_array_index (conversations, gint64, i);
          g_autoptr(SmsConversation) conversation = sms_store_get (self->store, id);
          g_autofree char *name = conversation ? conversation_name (self, conversation) : g_strdup ("");
          g_autofree char *shown = first_chars (name, GROUP_NAME_CHARS);
          g_autofree char *preview = NULL;
          const char *text = "";

          for (guint j = unseen->len; j > 0 && !*text; j--)
            if (((SmsMessage *) unseen->pdata[j - 1])->conversation_id == id)
              text = ((SmsMessage *) unseen->pdata[j - 1])->text ? ((SmsMessage *) unseen->pdata[j - 1])->text : "";
          if (body->len)
            g_string_append (body, separator);
          preview = msg_markdown_preview (text, chars);
          g_string_append_printf (body, "%s  %s", shown, preview);
        }
      self->notification_conversation = 0;
    }
  self->notification = notifier_notify (self->notifier, self->notification, title, body->str, actions,
                                        NOTIFIER_URGENCY_NORMAL,
                                        alert ? NULL : notifier_header_hints (0, NULL, TRUE));
}

/* the messages given up: "Message not sent", the conversation it was in; for more, how many in how many */
static void
update_failures (Service *self, gboolean alert)
{
  static const char *actions[] = { "default", "Open", NULL };
  guint messages = g_hash_table_size (self->failed);
  g_autoptr(GHashTable) conversations = g_hash_table_new (g_int64_hash, g_int64_equal);
  g_autofree char *title = NULL, *body = NULL;
  GHashTableIter iter;
  gpointer conversation;

  if (messages == 0)
    {
      if (self->failure_notification)
        notifier_close (self->notifier, self->failure_notification);
      self->failure_notification = 0;
      return;
    }
  g_hash_table_iter_init (&iter, self->failed);
  while (g_hash_table_iter_next (&iter, NULL, &conversation))
    g_hash_table_add (conversations, conversation);
  self->failure_conversation = 0;
  if (g_hash_table_size (conversations) == 1)
    {
      title = g_strdup (_("Message not sent"));
      g_hash_table_iter_init (&iter, conversations);
      g_hash_table_iter_next (&iter, &conversation, NULL);
      self->failure_conversation = *(gint64 *) conversation;
      if (messages == 1)
        {
          g_autoptr(SmsConversation) one = sms_store_get (self->store, self->failure_conversation);

          body = one ? conversation_name (self, one) : g_strdup ("");
        }
      else
        body = g_strdup_printf (_("%d messages in one conversation"), messages);
    }
  else
    {
      title = g_strdup (_("Some messages not sent in Messaging"));
      body = g_strdup_printf (_("%d messages in %d conversations"), messages, g_hash_table_size (conversations));
    }
  self->failure_notification = notifier_notify (self->notifier, self->failure_notification, title, body, actions,
                                                NOTIFIER_URGENCY_NORMAL,
                                                alert ? NULL : notifier_header_hints (0, NULL, TRUE));
}

/* the failures of @conversation (0: all) were looked at */
static void
failures_seen (Service *self, gint64 conversation)
{
  GHashTableIter iter;
  gpointer value;
  guint before = g_hash_table_size (self->failed);

  g_hash_table_iter_init (&iter, self->failed);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (conversation == 0 || *(gint64 *) value == conversation)
      g_hash_table_iter_remove (&iter);
  if (g_hash_table_size (self->failed) != before)
    update_failures (self, FALSE);
}

static void
open_app (Service *self, gint64 conversation)
{
  if (conversation)
    phone_open_app (self->session, MESSAGING_APP_ID, "open-conversation", g_variant_new_int64 (conversation));
  else
    phone_open_app (self->session, MESSAGING_APP_ID, "show-conversations", NULL);
}

static void
on_notification_action (guint32 id, const char *action, gpointer data)
{
  Service *self = data;

  if (id == self->notification)
    {
      if (g_str_equal (action, "default"))
        {
          /* it goes when tapped (FLAG_AUTO_CANCEL); the app marks what it shows read */
          notifier_close (self->notifier, id);
          self->notification = 0;
          open_app (self, self->notification_conversation);
        }
      else if (g_str_equal (action, "closed"))
        {
          /* swiped away: its messages are seen (the delete intent's MarkAsSeenAction) */
          self->notification = 0;
          sms_store_mark_seen (self->store, 0);
        }
      else if (g_str_equal (action, "gone"))
        self->notification = 0;
    }
  else if (id == self->failure_notification)
    {
      gint64 conversation = self->failure_conversation;

      self->failure_notification = 0;
      if (g_str_equal (action, "gone"))
        return;
      if (g_str_equal (action, "default"))
        {
          notifier_close (self->notifier, id);
          open_app (self, conversation);
        }
      g_hash_table_remove_all (self->failed);
    }
}

static void
on_notifications_restart (gpointer data)
{
  Service *self = data;

  /* a new server has none of ours: shown again, without a heads-up */
  self->notification = 0;
  self->failure_notification = 0;
  update_notification (self, FALSE);
  update_failures (self, FALSE);
}

static void
on_contacts_changed (Contacts *contacts, gpointer data)
{
  Service *self = data;

  /* the names in them */
  if (self->notification)
    update_notification (self, FALSE);
  if (self->failure_notification)
    update_failures (self, FALSE);
}

/* --- sending */

static void
retry_free (gpointer data)
{
  Retry *retry = data;

  if (retry->timer)
    g_source_remove (retry->timer);
  g_free (retry);
}

static void
queue_message (Service *self, gint64 id)
{
  if (id != self->sending && !g_queue_find (&self->outbox, GINT_TO_POINTER (id)))
    g_queue_push_tail (&self->outbox, GINT_TO_POINTER (id));
}

static gint64 *
int64_dup (gint64 value)
{
  return g_memdup2 (&value, sizeof value);
}

/* (gint64 ids in GINT_TO_POINTER: bugle_db's ids are row ids, far below 2^31 on a phone) */
static gboolean
on_retry (gpointer data)
{
  Retry *retry;
  Service *self = g_object_get_data (G_OBJECT (data), "service");
  gint64 id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (data), "message"));

  retry = g_hash_table_lookup (self->retries, &id);
  if (retry)
    retry->timer = 0;
  queue_message (self, id);
  send_next (self);
  return G_SOURCE_REMOVE;
}

/* it is still to go: when and how many times it was tried */
static Retry *
retry_of (Service *self, gint64 id)
{
  Retry *retry = g_hash_table_lookup (self->retries, &id);

  if (!retry)
    {
      retry = g_new0 (Retry, 1);
      retry->start = g_get_monotonic_time ();
      retry->delay = RETRY_INITIAL_MS;
      g_hash_table_insert (self->retries, int64_dup (id), retry);
    }
  return retry;
}

/* the network could not be reached (no modem, the radio off, out of coverage): worth trying again. Anything else
 * the network or the modem refused, and trying again would not change */
static gboolean
is_transient (const GError *error)
{
  g_autofree char *name = g_dbus_error_get_remote_error (error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return TRUE;
  return name && (g_str_has_suffix (name, ".Core.WrongState") || g_str_has_suffix (name, ".NoNetwork")
                  || g_str_has_suffix (name, ".NetworkTimeout"));
}

static void
set_status (Service *self, const SmsMessage *message, MessageStatus status, int raw_status, gint64 sent)
{
  sms_store_set_status (self->store, message->id, status, raw_status, sent);
  emit_changed (self, message->conversation_id);
}

static void send_to_next_destination (Service *self, SmsMessage *message, Retry *retry);

typedef struct {
  Service *self;
  SmsMessage *message;
  char *destination;
} Sending;

static void
report_free (gpointer data)
{
  Report *report = data;

  g_free (report->destination);
  g_free (report);
}

/* Messaging's "SMS delivery reports" (MmsUtils.isDeliveryReportRequired): off unless set */
static gboolean
delivery_reports (Service *self)
{
  return self->settings && g_settings_get_boolean (self->settings, "delivery-reports");
}

static void
on_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  Sending *sending = data;
  Service *self = sending->self;
  g_autoptr(SmsMessage) message = sending->message;
  g_autofree char *destination = sending->destination;
  g_autoptr(GError) error = NULL;
  Retry *retry = g_hash_table_lookup (self->retries, &message->id);
  int reference;

  g_free (sending);
  if (!retry)
    {
      /* deleted while it went */
      self->sending = 0;
      send_next (self);
      return;
    }
  if (sms_modem_send_finish (NULL, result, &reference, &error))
    {
      if (reference >= 0)
        {
          Report *report = g_new0 (Report, 1);

          report->message = message->id;
          report->destination = g_steal_pointer (&destination);
          /* a reference used again (256 sent since) stands for the newer message */
          g_hash_table_replace (self->reports, GUINT_TO_POINTER (reference), report);
        }
      retry->sent_to++;
      send_to_next_destination (self, g_steal_pointer (&message), retry);
      return;
    }

  self->sending = 0;
  if (is_transient (error)
      && (g_get_monotonic_time () - retry->start) / 1000 + retry->delay < RETRY_TIMEOUT_MS)
    {
      GObject *timer = g_object_new (G_TYPE_OBJECT, NULL);

      g_message ("sms: message %" G_GINT64_FORMAT " waits %u s: %s", message->id, retry->delay / 1000,
                 error->message);
      set_status (self, message, MESSAGE_STATUS_OUTGOING_AWAITING_RETRY, 0, 0);
      g_object_set_data (timer, "service", self);
      g_object_set_data (timer, "message", GINT_TO_POINTER (message->id));
      retry->timer = g_timeout_add_full (G_PRIORITY_DEFAULT, retry->delay, on_retry, timer, g_object_unref);
      retry->delay = MIN (retry->delay * 2, RETRY_MAX_MS);
    }
  else
    {
      g_dbus_error_strip_remote_error (error);
      g_message ("sms: message %" G_GINT64_FORMAT " not sent: %s", message->id, error->message);
      g_hash_table_remove (self->retries, &message->id);
      set_status (self, message, MESSAGE_STATUS_OUTGOING_FAILED, error->code, 0);
      g_hash_table_insert (self->failed, int64_dup (message->id), int64_dup (message->conversation_id));
      update_failures (self, TRUE);
    }
  send_next (self);
}

/* a group's message goes to each of them in turn, as Android sends a group SMS: one message each */
static void
send_to_next_destination (Service *self, SmsMessage *message, Retry *retry)
{
  g_autoptr(SmsConversation) conversation = sms_store_get (self->store, message->conversation_id);
  Sending *sending;

  if (!conversation || !conversation->destinations || retry->sent_to >= g_strv_length (conversation->destinations))
    {
      if (conversation)
        {
          g_message ("sms: message %" G_GINT64_FORMAT " sent", message->id);
          set_status (self, message, MESSAGE_STATUS_OUTGOING_COMPLETE, 0, g_get_real_time () / 1000);
        }
      g_hash_table_remove (self->retries, &message->id);
      sms_message_free (message);
      self->sending = 0;
      send_next (self);
      return;
    }
  sending = g_new0 (Sending, 1);
  sending->self = self;
  sending->message = message;
  sending->destination = g_strdup (conversation->destinations[retry->sent_to]);
  if (self->modem)
    sms_modem_send (self->modem, sending->destination, message->text ? message->text : "", delivery_reports (self),
                    NULL, on_sent, sending);
  else
    {
      /* gateway mode: no modem of ours, as if it had none */
      GTask *task = g_task_new (NULL, NULL, on_sent, sending);

      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No modem");
      g_object_unref (task);
    }
}

/* the next message in the outbox goes, one at a time */
static void
send_next (Service *self)
{
  while (!self->sending && !g_queue_is_empty (&self->outbox))
    {
      gint64 id = GPOINTER_TO_INT (g_queue_pop_head (&self->outbox));
      SmsMessage *message = sms_store_get_message (self->store, id);

      if (!message || MESSAGE_STATUS_IS_INCOMING (message->status)
          || message->status == MESSAGE_STATUS_OUTGOING_COMPLETE || message->status == MESSAGE_STATUS_OUTGOING_FAILED)
        {
          g_hash_table_remove (self->retries, &id);
          g_clear_pointer (&message, sms_message_free);
          continue;
        }
      self->sending = id;
      if (message->status != MESSAGE_STATUS_OUTGOING_RESENDING)
        set_status (self, message, MESSAGE_STATUS_OUTGOING_SENDING, 0, 0);
      send_to_next_destination (self, message, retry_of (self, id));
    }
}

/* the network may be back: every message waiting goes now (PendingMessagesAction on a service state change) */
static void
send_waiting (Service *self)
{
  GHashTableIter iter;
  gpointer id, value;

  g_hash_table_iter_init (&iter, self->retries);
  while (g_hash_table_iter_next (&iter, &id, &value))
    {
      Retry *retry = value;

      if (retry->timer)
        {
          g_source_remove (retry->timer);
          retry->timer = 0;
          queue_message (self, *(gint64 *) id);
        }
    }
  send_next (self);
}

/* --- delivery reports (ProcessDeliveryReportAction) */

/* what a report's delivery state makes of the message, as SendStatusReceiver and SyncMessageBatch
 * .bugleStatusForSms read a report's TP-Status: 0 (received) delivered; 0x40 and up failed (the service centre gave
 * up; the CDMA permanent errors 0x2xx too); anything else (forwarded unconfirmed, replaced, the centre still trying)
 * sent, no more. @final: no other report is to come for it (Android keeps a tracker only while the centre is still
 * trying, 0x20-0x3f). UNKNOWN for no news (ModemManager's 0x100, no state at all) */
static MessageStatus
report_status (guint state, gboolean *final)
{
  *final = TRUE;
  if (state == 0x00)
    return MESSAGE_STATUS_OUTGOING_DELIVERED;
  if ((state >= 0x40 && state < 0x100) || (state >= 0x200 && state < 0x300))
    return MESSAGE_STATUS_OUTGOING_FAILED;
  if (state < 0x20)
    return MESSAGE_STATUS_OUTGOING_COMPLETE;
  *final = FALSE;
  return state < 0x40 || state >= 0x300 ? MESSAGE_STATUS_OUTGOING_COMPLETE : MESSAGE_STATUS_UNKNOWN;
}

static void
on_status_report (const char *number, guint reference, guint state, gpointer data)
{
  Service *self = data;
  Report *report = g_hash_table_lookup (self->reports, GUINT_TO_POINTER (reference));
  g_autoptr(SmsMessage) message = NULL;
  MessageStatus status;
  gboolean final;

  /* one of ours, to the number it went to (a report carries the recipient; a reference alone is a byte, used again
   * after 256 messages) */
  if (!report || (*number && !phone_number_equal (number, report->destination, country (self))))
    {
      g_message ("sms: a delivery report (reference %u, to %s) for no message waiting for one", reference,
                 *number ? number : "no number");
      return;
    }
  status = report_status (state, &final);
  message = sms_store_get_message (self->store, report->message);
  if (final)
    g_hash_table_remove (self->reports, GUINT_TO_POINTER (reference));
  if (status == MESSAGE_STATUS_UNKNOWN || !message)
    return;
  /* only a message that went: not one still going to the rest of a group (its last one's report will tell), not
   * one tried again since, nor one that could not go to all of them */
  if (g_hash_table_contains (self->retries, &message->id)
      || (message->status != MESSAGE_STATUS_OUTGOING_COMPLETE && message->status != MESSAGE_STATUS_OUTGOING_DELIVERED))
    {
      g_message ("sms: message %" G_GINT64_FORMAT "'s delivery report (0x%x) left: it is %d now", message->id, state,
                 message->status);
      return;
    }
  g_message ("sms: message %" G_GINT64_FORMAT " %s (delivery report 0x%x)", message->id,
             status == MESSAGE_STATUS_OUTGOING_DELIVERED ? "delivered"
             : status == MESSAGE_STATUS_OUTGOING_FAILED ? "not delivered" : "not delivered yet", state);
  /* the newest report says, as in Android (a group's: each member's in turn), but a failure stays: the message did
   * not reach one of them. It keeps the report's state as its raw telephony status, and shows "Not sent. Tap to try
   * again.", as in Messaging, without a notification */
  if (status != message->status)
    set_status (self, message, status, status == MESSAGE_STATUS_OUTGOING_FAILED ? (int) state : 0, 0);
}

/* the app's settings, when its schema with the key is installed: a service started without it (an old schema, a
 * test bus) asks for no reports rather than aborting in g_settings_new */
static GSettings *
app_settings (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, MESSAGING_APP_ID, TRUE) : NULL;

  if (!schema || !g_settings_schema_has_key (schema, "delivery-reports"))
    {
      g_warning ("no %s schema with \"delivery-reports\": no delivery reports are asked for", MESSAGING_APP_ID);
      return NULL;
    }
  return g_settings_new_full (schema, NULL, NULL);
}

/* --- messages coming */

/* a message came into @conversation: where the app shows it, it is read (the conversation open) or seen (the list at
 * its top) at once */
static void
came_where_shown (Service *self, gint64 conversation)
{
  if (self->visible > 0 && conversation == self->visible)
    sms_store_mark_read (self->store, self->visible);
  else if (self->visible == 0)
    sms_store_mark_seen (self->store, 0);
}

static void
on_received (const char *path, const char *number, const char *text, gint64 timestamp, gpointer data)
{
  Service *self = data;
  g_autofree char *sender = phone_number_canonical (number, country (self));
  gint64 now = g_get_real_time () / 1000;
  g_autoptr(SmsMessage) message = NULL;
  gint64 id;

  id = sms_store_add_incoming (self->store, sender, text, timestamp ? timestamp : now, now, path, NULL);
  if (!id)
    return;                     /* left on the modem: the next start hands it over again */
  sms_modem_delete (self->modem, path);
  message = sms_store_get_message (self->store, id);
  /* from a blocked number: kept, but seen (never notified) and its conversation archived, as Messaging's block
   * dialog says ("You'll continue to receive messages from this number but won't be notified anymore. This
   * conversation will be archived."; ReceiveSmsMessageAction) */
  if (blocked_numbers_contains (self->blocked, sender, country (self)))
    {
      sms_store_mark_seen (self->store, message->conversation_id);
      sms_store_set_archived (self->store, message->conversation_id, TRUE);
      g_message ("sms: %s is from a blocked number: archived, not notified", path);
    }
  else
    came_where_shown (self, message->conversation_id);
  emit_changed (self, message->conversation_id);
  update_notification (self, TRUE);
}

static void
on_available_changed (gboolean available, gpointer data)
{
  Service *self = data;

  self->available = available;
  emit_properties (self);
  if (available)
    send_waiting (self);
}

static void
start_modem (Service *self)
{
  static const SmsModemCallbacks callbacks = { .available_changed = on_available_changed,
                                               .received = on_received, .status_report = on_status_report };

  if (!self->modem)
    self->modem = sms_modem_new (self->system, &callbacks, self);
}

static void
stop_modem (Service *self)
{
  if (!self->modem)
    return;
  g_clear_pointer (&self->modem, sms_modem_free);
  self->available = FALSE;
  emit_properties (self);
}

/* --- gsm2sip's gateway mode */

static void
sync_gateway (Service *self)
{
  g_autofree char *owner = self->gateway ? g_dbus_proxy_get_name_owner (self->gateway) : NULL;
  g_autoptr(GVariant) enabled = owner ? g_dbus_proxy_get_cached_property (self->gateway, "Enabled") : NULL;

  if (enabled && g_variant_get_boolean (enabled))
    {
      if (self->modem)
        g_message ("sms: gateway mode, the messages are gsm2sipd's");
      stop_modem (self);
    }
  else
    start_modem (self);
}

/* Two handlers, because the signals differ: g-properties-changed passes (changed, invalidated), notify:: passes
 * (pspec). One handler for both read the user data from the wrong argument when the gateway came or went. */
static void
on_gateway_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  sync_gateway (data);
}

static void
on_gateway_owner (GObject *proxy, GParamSpec *pspec, gpointer data)
{
  sync_gateway (data);
}

static void
on_gateway_proxy (GObject *source, GAsyncResult *result, gpointer data)
{
  Service *self = data;
  g_autoptr(GError) error = NULL;

  self->gateway = g_dbus_proxy_new_finish (result, &error);
  if (!self->gateway)
    g_warning ("no gsm2sip proxy: %s", error->message);
  else
    {
      g_signal_connect (self->gateway, "g-properties-changed", G_CALLBACK (on_gateway_properties), self);
      g_signal_connect (self->gateway, "notify::g-name-owner", G_CALLBACK (on_gateway_owner), self);
    }
  /* the modem is started only now: in gateway mode it is never ours, not even for a moment */
  sync_gateway (self);
}

/* --- agents (sms-agent.h): their messages go to minerva-chat-bridge and come from it, never through the modem */

typedef struct {
  Service *self;
  gint64 message;
} AgentWait;

static void
remove_source (gpointer source)
{
  g_source_remove (GPOINTER_TO_UINT (source));
}

static guint
agent_timeout (void)
{
  const char *ms = g_getenv ("NEO_MESSAGING_AGENT_TIMEOUT_MS");

  return ms && *ms ? (guint) g_ascii_strtoull (ms, NULL, 10) : AGENT_TIMEOUT_MS;
}

static gboolean
is_agent_conversation (Service *self, gint64 id)
{
  g_autoptr(SmsConversation) conversation = sms_store_get (self->store, id);

  return conversation && sms_agent_is_conversation ((const char * const *) conversation->destinations);
}

/* one destination, and it belongs to a provider (an agent's included): it never reaches the modem */
static gboolean
is_provider_conversation (Service *self, gint64 id)
{
  g_autoptr(SmsConversation) conversation = sms_store_get (self->store, id);

  return conversation && conversation->destinations && conversation->destinations[0]
         && !conversation->destinations[1] && sms_provider_is (conversation->destinations[0]);
}

static gboolean
is_delivery_conversation (Service *self, gint64 id)
{
  return is_provider_conversation (self, id) || is_agent_conversation (self, id);
}

/* the bridge took it (sent) or could not (failed, notified as an SMS that could not go) */
static void
agent_delivered (Service *self, const SmsMessage *message, gboolean ok)
{
  g_hash_table_remove (self->agent_waits, &message->id);
  if (ok)
    {
      g_message ("sms: message %" G_GINT64_FORMAT " taken by the bridge", message->id);
      if (g_hash_table_remove (self->failed, &message->id))
        update_failures (self, FALSE);
      set_status (self, message, MESSAGE_STATUS_OUTGOING_COMPLETE, 0, g_get_real_time () / 1000);
    }
  else
    {
      g_message ("sms: message %" G_GINT64_FORMAT " not taken by the bridge", message->id);
      set_status (self, message, MESSAGE_STATUS_OUTGOING_FAILED, 0, 0);
      g_hash_table_insert (self->failed, int64_dup (message->id), int64_dup (message->conversation_id));
      update_failures (self, TRUE);
    }
}

static gboolean
on_agent_timeout (gpointer data)
{
  AgentWait *wait = data;
  Service *self = wait->self;
  g_autoptr(SmsMessage) message = sms_store_get_message (self->store, wait->message);
  gpointer key;

  /* its source goes as this returns: out of the table without removing it */
  if (g_hash_table_steal_extended (self->agent_waits, &wait->message, &key, NULL))
    g_free (key);
  if (message && message->status == MESSAGE_STATUS_OUTGOING_SENDING)
    {
      g_message ("sms: message %" G_GINT64_FORMAT ": no Delivered in %u ms", message->id, self->agent_timeout);
      agent_delivered (self, message, FALSE);
    }
  return G_SOURCE_REMOVE;
}

/* the message is the bridge's: Outgoing, whether a bridge runs or not, and it fails unless Delivered comes in time */
static void
agent_send (Service *self, gint64 id, const char *origin)
{
  g_autoptr(SmsMessage) message = sms_store_get_message (self->store, id);
  g_autoptr(SmsConversation) conversation = message ? sms_store_get (self->store, message->conversation_id) : NULL;
  AgentWait *wait;
  guint source;

  if (!conversation)
    return;
  set_status (self, message, MESSAGE_STATUS_OUTGOING_SENDING, 0, 0);
  wait = g_new0 (AgentWait, 1);
  wait->self = self;
  wait->message = id;
  source = g_timeout_add_full (G_PRIORITY_DEFAULT, self->agent_timeout, on_agent_timeout, wait, g_free);
  g_hash_table_replace (self->agent_waits, int64_dup (id), GUINT_TO_POINTER (source));
  g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, SMS_INTERFACE, "Outgoing",
                                 g_variant_new ("(xxsss)", id, message->conversation_id,
                                                conversation->destinations[0], message->text ? message->text : "",
                                                origin && *origin ? origin : "owner"), NULL);
}

/* a message from a provider at @destination: as one that came by SMS (on_received), without a modem. @sent is the
 * network's time (0: now), @ext its id (the same id twice is kept once). @name, the provider's own name for the
 * chat, is not stored: the chat is named from the contacts by its number (msg-people.c). Its id, or 0 */
static gint64
provider_receive (Service *self, const char *destination, const char *text, gint64 sent, const char *ext,
                  const SmsPart *part, gboolean old)
{
  gint64 now = g_get_real_time () / 1000;
  g_autoptr(SmsMessage) message = NULL;
  gint64 id;

  if (ext && *ext && (id = sms_store_find_by_uri (self->store, ext)))
    return id;  /* the provider sent it again: it is already in the thread */
  id = old ? sms_store_add_incoming_read (self->store, destination, text, sent > 0 ? sent : now, now, ext, part)
           : sms_store_add_incoming (self->store, destination, text, sent > 0 ? sent : now, now, ext, part);
  if (!id || !(message = sms_store_get_message (self->store, id)))
    return 0;
  /* a backlog is history: kept, read, nobody told */
  if (old)
    {
      emit_changed (self, message->conversation_id);
      return id;
    }
  came_where_shown (self, message->conversation_id);
  emit_changed (self, message->conversation_id);
  update_notification (self, TRUE);
  return id;
}

/* a message from the agent at @destination: as one that came by SMS (on_received), without a modem. Its id, or 0 */
static gint64
agent_inject (Service *self, const char *destination, const char *text)
{
  return provider_receive (self, destination, text, 0, NULL, NULL, FALSE);
}

/* --- what the app shows */

static void set_visible (Service *self, const char *sender, gint64 conversation);

static void
on_visible_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  set_visible (data, NULL, -1);
}

static void
set_visible (Service *self, const char *sender, gint64 conversation)
{
  if (g_strcmp0 (sender, self->visible_sender) != 0)
    {
      if (self->visible_watch)
        g_bus_unwatch_name (self->visible_watch);
      self->visible_watch = 0;
      g_clear_pointer (&self->visible_sender, g_free);
      if (sender)
        {
          self->visible_sender = g_strdup (sender);
          self->visible_watch = g_bus_watch_name_on_connection (self->session, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                                NULL, on_visible_vanished, self, NULL);
        }
    }
  self->visible = sender ? conversation : -1;
  if (self->visible > 0)
    {
      if (sms_store_mark_read (self->store, self->visible))
        {
          emit_changed (self, self->visible);
          update_notification (self, FALSE);
        }
      failures_seen (self, self->visible);
    }
  else if (self->visible == 0)
    {
      /* ConversationListData: scrolled to the newest conversation, everything is seen */
      if (sms_store_mark_seen (self->store, 0))
        update_notification (self, FALSE);
      failures_seen (self, 0);
    }
}

/* --- the service */

static gboolean
check_conversation (Service *self, gint64 id, GError **error)
{
  g_autoptr(SmsConversation) conversation = id > 0 ? sms_store_get (self->store, id) : NULL;

  if (!conversation)
    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No conversation %" G_GINT64_FORMAT, id);
  return conversation != NULL;
}

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Service *self = data;
  g_autoptr(GError) error = NULL;
  gint64 id = 0;
  gboolean flag;
  const char *text;

  if (g_str_equal (method, "GetMessages"))
    {
      g_autoptr(GError) denied = NULL;
      g_autoptr(SmsConversation) conversation = NULL;
      g_autoptr(GPtrArray) messages = NULL;
      GVariantBuilder out;
      guint limit;

      g_variant_get (parameters, "(xu)", &id, &limit);
      if (!msg_callers_check (bus, sender, CALLERS_FILE, SMS_INTERFACE ".GetMessages", &denied))
        {
          g_message ("sms: GetMessages refused: %s", denied->message);
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                 "Not a caller of GetMessages");
          return;
        }
      if (!(conversation = sms_store_get (self->store, id)))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "No conversation %" G_GINT64_FORMAT, id);
          return;
        }
      messages = limit ? sms_store_list_last_messages (self->store, id, limit) : NULL;
      g_variant_builder_init (&out, G_VARIANT_TYPE ("a(xbsx)"));
      for (guint i = 0; messages && i < messages->len; i++)
        {
          SmsMessage *m = messages->pdata[i];

          g_variant_builder_add (&out, "(xbsx)", m->id, sms_message_is_owners (m),
                                 m->text ? m->text : "", m->received);
        }
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(xbsx))", &out));
      return;
    }

  if (g_str_equal (method, "GetUnread"))
    {
      g_variant_get (parameters, "(x)", &id);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)",
                                             GPOINTER_TO_UINT (g_hash_table_lookup (self->unread, &id))));
      return;
    }

  if (g_str_equal (method, "GetConversation"))
    {
      g_autofree const char **numbers = NULL;
      g_autoptr(GPtrArray) destinations = g_ptr_array_new_with_free_func (g_free);

      g_variant_get (parameters, "(^a&s)", &numbers);
      for (guint i = 0; numbers[i]; i++)
        {
          /* a provider's address is not a number: it is kept verbatim */
          char *destination = sms_provider_is (numbers[i]) ? g_strdup (numbers[i])
                                                          : phone_number_canonical (numbers[i], country (self));

          /* the same person twice is once */
          if (*destination && !g_ptr_array_find_with_equal_func (destinations, destination, g_str_equal, NULL))
            g_ptr_array_add (destinations, destination);
          else
            g_free (destination);
        }
      g_ptr_array_add (destinations, NULL);
      id = destinations->len > 1
           ? sms_store_get_conversation (self->store, (const char * const *) destinations->pdata) : 0;
      if (!id)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "No conversation for these numbers");
      else
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", id));
      return;
    }
  if (g_str_equal (method, "Send") || g_str_equal (method, "SendAs"))
    {
      const char *origin = "owner";
      gint64 message;

      if (g_str_equal (method, "SendAs"))
        g_variant_get (parameters, "(x&s&s)", &id, &text, &origin);
      else
        g_variant_get (parameters, "(x&s)", &id, &text);
      if (!check_conversation (self, id, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return;
        }
      /* the origin is kept on the row (an agent's message is marked as such in the thread); the owner's is NULL */
      if (!*text || !(message = sms_store_add_outgoing (self->store, id, text, g_get_real_time () / 1000,
                                                        g_strcmp0 (origin, "owner") ? origin : NULL)))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
          return;
        }
      /* written in it: the conversation is read, and back from the archive (InsertNewMessageAction) */
      sms_store_mark_read (self->store, id);
      sms_store_set_archived (self->store, id, FALSE);
      emit_changed (self, id);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
      if (is_delivery_conversation (self, id))
        agent_send (self, message, origin);
      else
        {
          queue_message (self, message);
          send_next (self);
        }
      return;
    }
  if (g_str_equal (method, "Inject"))
    {
      const char *destination;
      gint64 message;

      g_variant_get (parameters, "(&s&s)", &destination, &text);
      if (!sms_agent_name (destination) || !*text)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "Not an agent's destination, or no text");
      else if (!(message = agent_inject (self, destination, text)))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
      else
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
      return;
    }
  if (g_str_equal (method, "Record"))
    {
      const char *destination;
      gint64 now = g_get_real_time () / 1000, message = 0;
      g_autoptr(SmsMessage) row = NULL;

      g_variant_get (parameters, "(&s&s)", &destination, &text);
      const char *const one[] = { destination, NULL };
      if (!sms_agent_name (destination) || !*text)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "Not an agent's destination, or no text");
      else if (!(id = sms_store_get_conversation (self->store, one))
               || !(message = sms_store_add_outgoing (self->store, id, text, now, NULL))
               || !(row = sms_store_get_message (self->store, message)))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
      else
        {
          set_status (self, row, MESSAGE_STATUS_OUTGOING_COMPLETE, 0, now);
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
        }
      return;
    }
  if (g_str_equal (method, "Live"))
    {
      const char *destination, *ext, *state, *origin;
      gint64 until, now = g_get_real_time () / 1000, message = 0;
      g_autoptr(SmsMessage) row = NULL;
      MessageStatus status;

      g_variant_get (parameters, "(&s&s&s&sx&s)", &destination, &ext, &state, &text, &until, &origin);
      const char *const one[] = { destination, NULL };
      status = sms_live_status (state);
      if (!sms_agent_name (destination) || !*ext || status == MESSAGE_STATUS_UNKNOWN)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Not an agent's destination, no ext or no such state");
          return;
        }
      /* the first word of it: kept as the owner's (or @origin's), read and seen, the conversation's draft left alone */
      if (!(id = sms_store_get_conversation (self->store, one))
          || (!(message = sms_store_find_by_uri (self->store, ext))
              && !(message = sms_store_add_sent (self->store, destination, text, now, ext, *origin ? origin : NULL,
                                                 NULL)))
          || !(row = sms_store_get_message (self->store, message)))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
          return;
        }
      /* an id another conversation's message has (a provider's) is not this one's */
      if (row->conversation_id != id)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Message %s is not in the agent's conversation", ext);
          return;
        }
      sms_store_set_live (self->store, message, state, text, now, until);
      /* the status last, through set_status: one Changed shows the state and the words together; sent is its time */
      set_status (self, row, status, 0, g_str_equal (state, "sent") ? now : 0);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
      return;
    }
  if (g_str_equal (method, "Sent") || g_str_equal (method, "SentMedia"))
    {
      const char *destination, *ext, *origin;
      gint64 sent, now = g_get_real_time () / 1000, message = 0;
      g_autoptr(SmsMessage) row = NULL;
      g_autoptr(GVariant) dict = NULL;
      g_autoptr(SmsPart) part = NULL;

      if (g_str_equal (method, "SentMedia"))
        {
          g_variant_get (parameters, "(&s&sx&s&s@a{sv})", &destination, &text, &sent, &ext, &origin, &dict);
          part = sms_part_new_from_dict (dict);
        }
      else
        g_variant_get (parameters, "(&s&sx&s&s)", &destination, &text, &sent, &ext, &origin);
      if (!sms_provider_is (destination) || !*text || !*origin || !g_strcmp0 (origin, "owner"))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "Not a provider\'s destination, no text or no origin");
      else if (*ext && (message = sms_store_find_by_uri (self->store, ext)))
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
      else if (!(message = sms_store_add_sent (self->store, destination, text, sent > 0 ? sent : now,
                                               *ext ? ext : NULL, origin, part))
               || !(row = sms_store_get_message (self->store, message)))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
      else
        {
          emit_changed (self, row->conversation_id);
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
        }
      return;
    }

  if (g_str_equal (method, "Receive") || g_str_equal (method, "ReceiveMedia") || g_str_equal (method, "ReceiveOld"))
    {
      const char *destination, *ext, *name;
      gint64 sent, message;
      g_autoptr(GVariant) dict = NULL;
      g_autoptr(SmsPart) part = NULL;

      if (!g_str_equal (method, "Receive"))
        {
          g_variant_get (parameters, "(&s&sx&s&s@a{sv})", &destination, &text, &sent, &ext, &name, &dict);
          part = sms_part_new_from_dict (dict);
        }
      else
        g_variant_get (parameters, "(&s&sx&s&s)", &destination, &text, &sent, &ext, &name);
      if (!sms_provider_is (destination) || !*text)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "Not a provider\'s destination, or no text");
      else if (!(message = provider_receive (self, destination, text, sent, ext, part, g_str_equal (method, "ReceiveOld"))))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Not kept");
      else
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(x)", message));
      return;
    }

  if (g_str_equal (method, "Resend"))
    {
      g_autoptr(SmsMessage) message = NULL;

      g_variant_get (parameters, "(x)", &id);
      message = sms_store_get_message (self->store, id);
      if (!message || message->status != MESSAGE_STATUS_OUTGOING_FAILED)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "No failed message %" G_GINT64_FORMAT, id);
          return;
        }
      g_hash_table_remove (self->failed, &id);
      update_failures (self, FALSE);
      g_hash_table_remove (self->retries, &id);
      if (is_delivery_conversation (self, message->conversation_id))
        agent_send (self, id, "owner");
      else
        {
          set_status (self, message, MESSAGE_STATUS_OUTGOING_RESENDING, 0, 0);
          queue_message (self, id);
          send_next (self);
        }
    }
  else if (g_str_equal (method, "Delivered"))
    {
      g_autoptr(SmsMessage) message = NULL;

      g_variant_get (parameters, "(xb)", &id, &flag);
      message = sms_store_get_message (self->store, id);
      if (!message || MESSAGE_STATUS_IS_INCOMING (message->status)
          || !is_delivery_conversation (self, message->conversation_id))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No message to an agent %" G_GINT64_FORMAT,
                     id);
      else
        agent_delivered (self, message, flag);
    }
  else if (g_str_equal (method, "Reached"))
    {
      g_autoptr(SmsMessage) message = NULL;

      g_variant_get (parameters, "(x)", &id);
      message = sms_store_get_message (self->store, id);
      if (!message || message->status != MESSAGE_STATUS_OUTGOING_COMPLETE
          || !is_delivery_conversation (self, message->conversation_id))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No sent message to an agent %" G_GINT64_FORMAT,
                     id);
      else
        set_status (self, message, MESSAGE_STATUS_OUTGOING_DELIVERED, 0, 0);
    }
  else if (g_str_equal (method, "MarkRead"))
    {
      g_variant_get (parameters, "(x)", &id);
      if (sms_store_mark_read (self->store, id))
        {
          emit_changed (self, id);
          update_notification (self, FALSE);
        }
      failures_seen (self, id);
    }
  else if (g_str_equal (method, "SetArchived"))
    {
      g_variant_get (parameters, "(xb)", &id, &flag);
      if (check_conversation (self, id, &error))
        {
          sms_store_set_archived (self->store, id, flag);
          emit_changed (self, id);
        }
    }
  else if (g_str_equal (method, "SetDraft"))
    {
      g_variant_get (parameters, "(x&s)", &id, &text);
      if (check_conversation (self, id, &error))
        {
          sms_store_set_draft (self->store, id, *text ? text : NULL);
          emit_changed (self, id);
        }
    }
  else if (g_str_equal (method, "DeleteConversation"))
    {
      g_variant_get (parameters, "(x)", &id);
      if (sms_store_delete_conversation (self->store, id))
        {
          emit_changed (self, id);
          update_notification (self, FALSE);
          failures_seen (self, id);
        }
    }
  else if (g_str_equal (method, "FetchMedia"))
    {
      g_autoptr(SmsMessage) message = NULL;
      g_autoptr(SmsConversation) conversation = NULL;

      g_variant_get (parameters, "(x)", &id);
      message = sms_store_get_message (self->store, id);
      conversation = message ? sms_store_get (self->store, message->conversation_id) : NULL;
      if (!message || !message->part || !message->sms_uri || !conversation || !conversation->destinations[0]
          || !sms_provider_is (conversation->destinations[0]))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No provider's media message %" G_GINT64_FORMAT,
                     id);
      else
        g_dbus_connection_emit_signal (self->session, NULL, SMS_PATH, SMS_INTERFACE, "MediaWanted",
                                       g_variant_new ("(sss)", conversation->destinations[0], message->sms_uri,
                                                      message->part->mime ? message->part->mime : ""), NULL);
    }
  else if (g_str_equal (method, "Change"))
    {
      const char *kind, *ext, *who;
      gint64 conversation;

      g_variant_get (parameters, "(&s&s&s&s)", &kind, &ext, &who, &text);
      if (!*ext || !(conversation = sms_store_change (self->store, kind, ext, who, text)))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No message %s, or no such change", ext);
      else
        {
          emit_changed (self, conversation);
          update_notification (self, FALSE);
        }
    }
  else if (g_str_equal (method, "SetTranscript"))
    {
      const char *ext, *transcript;
      gint64 message;

      g_variant_get (parameters, "(&s&s&s)", &ext, &transcript, &text);
      if (!*ext || !*text || !(message = sms_store_find_by_uri (self->store, ext)))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No message %s, or no text", ext);
      else
        {
          g_autoptr(SmsMessage) row = NULL;

          sms_store_set_transcript (self->store, message, transcript, text);
          if ((row = sms_store_get_message (self->store, message)))
            {
              emit_changed (self, row->conversation_id);
              update_notification (self, FALSE);
            }
        }
    }
  else if (g_str_equal (method, "SetMedia"))
    {
      const char *ext, *path;
      gint64 message;

      g_variant_get (parameters, "(&s&s)", &ext, &path);
      if (!*ext || !(message = sms_store_find_by_uri (self->store, ext)) || !g_path_is_absolute (path)
          || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
        g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No message %s, or no file at the path", ext);
      else
        {
          g_autoptr(SmsMessage) row = NULL;

          sms_store_set_media (self->store, message, path);
          if ((row = sms_store_get_message (self->store, message)))
            emit_changed (self, row->conversation_id);
        }
    }
  else if (g_str_equal (method, "DeleteMessage"))
    {
      gint64 conversation;

      g_variant_get (parameters, "(x)", &id);
      /* one still going is not stopped on the modem, but nothing more of it goes */
      g_hash_table_remove (self->retries, &id);
      g_hash_table_remove (self->agent_waits, &id);
      if (g_hash_table_remove (self->failed, &id))
        update_failures (self, FALSE);
      conversation = sms_store_delete_message (self->store, id);
      if (conversation)
        {
          emit_changed (self, conversation);
          update_notification (self, FALSE);
        }
    }
  else if (g_str_equal (method, "SetVisible"))
    {
      g_variant_get (parameters, "(x)", &id);
      if (id >= 0)
        set_visible (self, sender, id);
      else if (g_strcmp0 (sender, self->visible_sender) == 0)
        set_visible (self, NULL, -1);
    }

  if (error)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation, NULL);
}

static GVariant *
on_get_property (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                 const char *property, GError **error, gpointer data)
{
  Service *self = data;

  if (g_str_equal (property, "Available"))
    return g_variant_new_boolean (self->available);
  return g_variant_new_string (country (self) ? country (self) : "");
}

static const GDBusInterfaceVTable vtable = { on_method_call, on_get_property, NULL };

static void
on_name_acquired (GDBusConnection *bus, const char *name, gpointer data)
{
  g_message ("serving %s", name);
}

static void
on_name_lost (GDBusConnection *bus, const char *name, gpointer data)
{
  Service *self = data;

  /* another one has it: two would take the same messages off the modem */
  g_warning ("lost the bus name %s", name);
  g_main_loop_quit (self->loop);
}

static gboolean
on_signal (gpointer data)
{
  g_main_loop_quit (((Service *) data)->loop);
  return G_SOURCE_REMOVE;
}

/* the system bus, or for the tests a bus of their own with a ModemManager of their own (tests/mock-modem.py) */
static GDBusConnection *
system_bus (GError **error)
{
  const char *address = g_getenv ("NEO_MESSAGING_SYSTEM_BUS");

  if (address && *address)
    return g_dbus_connection_new_for_address_sync (address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                            | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                   NULL, NULL, error);
  return g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
}

int
main (int argc, char **argv)
{
  Service service = { .visible = -1 };
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusNodeInfo) node = NULL;
  g_autoptr(GPtrArray) unsent = NULL;
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "neo-messaging", NULL);
  g_autofree char *store_path = g_build_filename (dir, "bugle_db", NULL);
  guint owner;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  service.loop = g_main_loop_new (NULL, FALSE);
  service.session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!service.session)
    g_error ("no session bus: %s", error->message);
  service.system = system_bus (&error);
  if (!service.system)
    g_error ("no system bus: %s", error->message);
  g_mkdir_with_parents (dir, 0700);
  service.store = sms_store_open (store_path, FALSE, &error);
  if (!service.store)
    g_error ("%s", error->message);
  service.contacts = contacts_new ();
  service.blocked = blocked_numbers_new (NULL);
  g_signal_connect (service.contacts, "changed", G_CALLBACK (on_contacts_changed), &service);
  service.notifier = notifier_new (on_notification_action, on_notifications_restart, &service);
  service.failed = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_free);
  service.unread = sms_store_unread_counts (service.store);
  service.retries = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, retry_free);
  service.reports = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, report_free);
  service.settings = app_settings ();
  service.agent_waits = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, remove_source);
  service.agent_timeout = agent_timeout ();
  g_queue_init (&service.outbox);

  node = g_dbus_node_info_new_for_xml (introspection, NULL);
  service.registration = g_dbus_connection_register_object (service.session, SMS_PATH, node->interfaces[0],
                                                            &vtable, &service, NULL, &error);
  if (!service.registration)
    g_error ("%s", error->message);
  owner = g_bus_own_name_on_connection (service.session, SMS_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                        on_name_acquired, on_name_lost, &service, NULL);
  g_dbus_proxy_new (service.system, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, GATEWAY_NAME, GATEWAY_PATH,
                    GATEWAY_INTERFACE, NULL, on_gateway_proxy, &service);

  /* what was still to go when the service last stopped goes now, its 20 minutes from now; what came and was not
   * seen is notified again, quietly */
  unsent = sms_store_list_unsent (service.store);
  for (guint i = 0; i < unsent->len; i++)
    {
      SmsMessage *message = unsent->pdata[i];

      /* but one to an agent, which the bridge may have taken already, fails: given twice only when tapped again */
      if (is_delivery_conversation (&service, message->conversation_id))
        sms_store_set_status (service.store, message->id, MESSAGE_STATUS_OUTGOING_FAILED, 0, 0);
      else
        queue_message (&service, message->id);
    }
  update_notification (&service, FALSE);

  g_unix_signal_add (SIGTERM, on_signal, &service);
  g_unix_signal_add (SIGINT, on_signal, &service);
  g_main_loop_run (service.loop);

  g_bus_unown_name (owner);
  g_dbus_connection_unregister_object (service.session, service.registration);
  if (service.visible_watch)
    g_bus_unwatch_name (service.visible_watch);
  g_free (service.visible_sender);
  if (service.gateway)
    g_signal_handlers_disconnect_by_data (service.gateway, &service);
  g_clear_object (&service.gateway);
  g_clear_pointer (&service.modem, sms_modem_free);
  g_queue_clear (&service.outbox);
  g_hash_table_unref (service.retries);
  g_hash_table_unref (service.reports);
  g_hash_table_unref (service.agent_waits);
  g_clear_object (&service.settings);
  g_hash_table_unref (service.failed);
  g_hash_table_unref (service.unread);
  notifier_free (service.notifier);
  g_signal_handlers_disconnect_by_data (service.contacts, &service);
  g_object_unref (service.contacts);
  g_object_unref (service.blocked);
  sms_store_free (service.store);
  g_object_unref (service.system);
  g_object_unref (service.session);
  g_main_loop_unref (service.loop);
  return 0;
}
