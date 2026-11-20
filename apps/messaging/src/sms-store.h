/* The messages: every text message the phone sent or took in, kept in SQLite (Messaging's bugle_db; spec §10.1). Its
 * tables and columns are bugle_db's, the ones this app uses, so what Messaging reads from it maps one to one:
 * conversations, messages, parts (a message's text), participants and conversation_participants.
 *
 * Android keeps the messages twice, in the telephony provider and in bugle_db, and syncs the two. Here the modem
 * hands each message over once (ModemManager, which then forgets it), so bugle_db is the only store.
 *
 * Only the service writes it (a message comes or is sent, the user deletes or archives); the app reads it, and is
 * told by the service when it changed. A participant is known by its destination: a number in its canonical form
 * (E.164, "+493023575124", or as it came when it is not a phone number: "Vodafone", "22000"), which the caller
 * works out. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* MessageData's statuses (spec §10.1) */
typedef enum
{
  MESSAGE_STATUS_UNKNOWN = 0,
  MESSAGE_STATUS_OUTGOING_COMPLETE = 1,
  MESSAGE_STATUS_OUTGOING_DELIVERED = 2,
  MESSAGE_STATUS_OUTGOING_DRAFT = 3,
  MESSAGE_STATUS_OUTGOING_YET_TO_SEND = 4,
  MESSAGE_STATUS_OUTGOING_SENDING = 5,
  MESSAGE_STATUS_OUTGOING_RESENDING = 6,
  MESSAGE_STATUS_OUTGOING_AWAITING_RETRY = 7,
  MESSAGE_STATUS_OUTGOING_FAILED = 8,
  MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER = 9,
  MESSAGE_STATUS_INCOMING_COMPLETE = 100,
} MessageStatus;

#define MESSAGE_STATUS_IS_INCOMING(status) ((status) >= MESSAGE_STATUS_INCOMING_COMPLETE)

typedef struct
{
  gint64 id;
  char *name;                   /* as stored: the other participants' destinations, joined; the app names them */
  char *snippet;                /* the newest message's text (or the draft's, with show_draft) */
  gint64 sort_timestamp;        /* the newest message's time, Unix ms: the list's order */
  gint64 latest_message_id;
  MessageStatus latest_status;
  gboolean read;                /* no message in it is unread */
  gboolean archived;
  gboolean show_draft;
  char *draft;                  /* the text typed and not sent, or NULL */
  GStrv destinations;           /* the other participants, as stored */
} SmsConversation;

/* what a message that is not only text carries (a provider's: a photo, a voice note, a location...): its typed part.
 * The part's text is its readable label ("vocale 0:12"), which the list, the notification and the agents show */
typedef struct
{
  char *kind;                   /* "image", "video", "gif", "sticker", "voice", "audio", "document", "location", "contact",
                                 * "poll" (the provider's word, never interpreted beyond showing it) */
  char *mime;
  char *caption;
  char *name;                   /* a document's file name, a location's, a contact's or a poll's */
  guint64 size;                 /* bytes, 0 unknown */
  guint duration;               /* ms */
  guint width, height;
  GBytes *thumbnail;            /* the small preview the message carried (JPEG, a sticker's PNG), or NULL */
  gboolean animated, view_once;
  gboolean located;             /* latitude and longitude are set */
  double latitude, longitude;
  char *url;
  char *uri;                    /* the media on this phone, once downloaded, or NULL */
} SmsPart;

typedef struct
{
  gint64 id;
  gint64 conversation_id;
  char *sender;                 /* an incoming message's sender (a destination); NULL for one sent from here */
  char *text;
  gint64 sent;                  /* Unix ms: when it was sent (an incoming one: the network's timestamp) */
  gint64 received;              /* Unix ms: when it came, or for an outgoing one when it was written */
  MessageStatus status;
  gboolean read, seen;          /* read: the conversation was looked at; seen: its notification was */
  char *sms_uri;                /* the ModemManager SMS object it was, or NULL */
  int raw_status;               /* ModemManager's error for a failed send, else 0 */
  char *origin;                 /* who wrote an outgoing one: NULL for the owner, else SendAs's origin ("minerva",
                                 * "agent:<identity>": docs/external-agents-channels.md) */
  SmsPart *part;                /* what it carries besides text, or NULL for a text message */
} SmsMessage;

typedef struct _SmsStore SmsStore;

/* opens (or makes) the store at @path; read-only for the app */
SmsStore *sms_store_open (const char *path, gboolean read_only, GError **error);
void      sms_store_free (SmsStore *store);

/* --- writing (the service) */

/* the conversation with exactly these participants, made if there is none: its id, or 0 */
gint64    sms_store_get_conversation (SmsStore *store, const char * const *destinations);
/* a message that came from @sender: into its conversation (made if new), unread and not seen. Its id, or 0 */
gint64    sms_store_add_incoming (SmsStore *store, const char *sender, const char *text, gint64 sent,
                                  gint64 received, const char *sms_uri, const SmsPart *part);
/* a message the owner wrote to @destination elsewhere (a provider's own app on his phone): kept as sent, read and seen,
   by @origin, with the network's id @sms_uri; the conversation's draft stays. Its id, or 0 */
gint64    sms_store_add_sent (SmsStore *store, const char *destination, const char *text, gint64 sent,
                              const char *sms_uri, const char *origin, const SmsPart *part);
/* the message whose provider/modem id is @sms_uri (a provider's message id is kept there): its id, or 0 */
gint64    sms_store_find_by_uri (SmsStore *store, const char *sms_uri);
/* a message to send, as written now: OUTGOING_YET_TO_SEND, read; the conversation's draft goes. Its id, or 0 */
gint64    sms_store_add_outgoing (SmsStore *store, gint64 conversation_id, const char *text, gint64 now,
                                  const char *origin);
void      sms_store_set_status (SmsStore *store, gint64 message_id, MessageStatus status, int raw_status,
                                gint64 sent);
void      sms_store_set_sms_uri (SmsStore *store, gint64 message_id, const char *sms_uri);
/* the media of the message's typed part is at @uri (a path on this phone) */
void      sms_store_set_media (SmsStore *store, gint64 message_id, const char *uri);
/* the messages are read (the conversation was shown) and seen (their notification can go); how many changed */
guint     sms_store_mark_read (SmsStore *store, gint64 conversation_id);
guint     sms_store_mark_seen (SmsStore *store, gint64 conversation_id);
void      sms_store_set_archived (SmsStore *store, gint64 conversation_id, gboolean archived);
/* the text typed and not sent (DraftMessageData), or NULL for none */
void      sms_store_set_draft (SmsStore *store, gint64 conversation_id, const char *text);
gboolean  sms_store_delete_conversation (SmsStore *store, gint64 conversation_id);
/* the message goes; its conversation goes with its last message. The conversation's id */
gint64    sms_store_delete_message (SmsStore *store, gint64 message_id);

/* --- reading */

/* the conversations with a message (or a draft), newest first: SmsConversation */
GPtrArray      *sms_store_list_conversations (SmsStore *store, gboolean archived);
SmsConversation *sms_store_get (SmsStore *store, gint64 conversation_id);
/* a conversation's messages, oldest first: SmsMessage */
GPtrArray      *sms_store_list_messages (SmsStore *store, gint64 conversation_id);
/* only its newest limit messages, still oldest first: SmsMessage */
GPtrArray      *sms_store_list_last_messages (SmsStore *store, gint64 conversation_id, guint limit);
SmsMessage     *sms_store_get_message (SmsStore *store, gint64 message_id);
/* the incoming messages not yet seen, oldest first (the notification's): SmsMessage */
GPtrArray      *sms_store_list_unseen (SmsStore *store);
/* the outgoing messages still to go (YET_TO_SEND, SENDING, RESENDING, AWAITING_RETRY), oldest first: SmsMessage */
GPtrArray      *sms_store_list_unsent (SmsStore *store);
/* the people written with most, the most messages first (the picker's Frequents): destinations */
GStrv           sms_store_list_frequent (SmsStore *store, guint limit);
/* the unread messages of every conversation that has any: conversation id (gint64 *) -> count (GINT_TO_POINTER) */
GHashTable     *sms_store_unread_counts (SmsStore *store);

void sms_conversation_free (SmsConversation *conversation);
void sms_message_free (SmsMessage *message);
/* a part from ReceiveMedia's a{sv} (kind, mime, caption, name, size t, duration u ms, width u, height u, thumbnail ay,
 * animated b, view-once b, latitude d, longitude d, url): NULL without a kind */
SmsPart *sms_part_new_from_dict (GVariant *dict);
void sms_part_free (SmsPart *part);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SmsConversation, sms_conversation_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SmsMessage, sms_message_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SmsPart, sms_part_free)

G_END_DECLS
