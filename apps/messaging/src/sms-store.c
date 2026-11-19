/* The messages (sms-store.h). The schema is DatabaseHelper's, less the columns for MMS, SIMs and contacts: the app
 * looks a participant up in the address books as it shows it, so a renamed contact never shows its old name. */
#include "sms-store.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#define SCHEMA_VERSION 1
#define OTHER_SUB_ID   -2       /* ParticipantData.OTHER_THAN_SELF_SUB_ID */

struct _SmsStore {
  sqlite3 *db;
};

static const char schema[] =
  "CREATE TABLE IF NOT EXISTS conversations ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  name TEXT,"
  "  latest_message_id INT,"
  "  snippet_text TEXT,"
  "  show_draft INT DEFAULT 0,"
  "  draft_snippet_text TEXT,"
  "  archive_status INT DEFAULT 0,"
  "  sort_timestamp INT DEFAULT 0,"
  "  last_read_timestamp INT DEFAULT 0,"
  "  participant_normalized_destination TEXT,"
  "  participant_count INT DEFAULT 0"
  ");"
  "CREATE INDEX IF NOT EXISTS index_conversation_sort ON conversations (archive_status, sort_timestamp);"
  "CREATE TABLE IF NOT EXISTS participants ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  sub_id INT DEFAULT -2,"
  "  normalized_destination TEXT,"
  "  send_destination TEXT,"
  "  display_destination TEXT,"
  "  blocked INT DEFAULT 0,"
  "  UNIQUE (normalized_destination, sub_id) ON CONFLICT FAIL"
  ");"
  "CREATE TABLE IF NOT EXISTS conversation_participants ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  conversation_id INT REFERENCES conversations(_id) ON DELETE CASCADE,"
  "  participant_id INT REFERENCES participants(_id) ON DELETE CASCADE,"
  "  UNIQUE (conversation_id, participant_id) ON CONFLICT FAIL"
  ");"
  "CREATE TABLE IF NOT EXISTS messages ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  conversation_id INT REFERENCES conversations(_id) ON DELETE CASCADE,"
  "  sender_id INT REFERENCES participants(_id) ON DELETE SET NULL,"
  "  sent_timestamp INT DEFAULT 0,"
  "  received_timestamp INT DEFAULT 0,"
  "  message_protocol INT DEFAULT 0,"
  "  message_status INT DEFAULT 0,"
  "  seen INT DEFAULT 0,"
  "  read INT DEFAULT 0,"
  "  sms_message_uri TEXT,"
  "  raw_status INT DEFAULT 0,"
  "  retry_start_timestamp INT DEFAULT 0,"
  "  origin TEXT"
  ");"
  "CREATE INDEX IF NOT EXISTS index_messages_sort ON messages (conversation_id, received_timestamp);"
  "CREATE INDEX IF NOT EXISTS index_messages_status_seen ON messages (message_status, seen);"
  "CREATE TABLE IF NOT EXISTS parts ("
  "  _id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  message_id INT REFERENCES messages(_id) ON DELETE CASCADE,"
  "  text TEXT,"
  "  uri TEXT,"
  "  content_type TEXT,"
  "  timestamp INT,"
  "  conversation_id INT NOT NULL REFERENCES conversations(_id) ON DELETE CASCADE"
  ");";

void
sms_conversation_free (SmsConversation *conversation)
{
  if (!conversation)
    return;
  g_free (conversation->name);
  g_free (conversation->snippet);
  g_free (conversation->draft);
  g_strfreev (conversation->destinations);
  g_free (conversation);
}

void
sms_message_free (SmsMessage *message)
{
  if (!message)
    return;
  g_free (message->sender);
  g_free (message->text);
  g_free (message->sms_uri);
  g_free (message->origin);
  g_free (message);
}

static gboolean
exec (SmsStore *self, const char *sql, GError **error)
{
  char *message = NULL;

  if (sqlite3_exec (self->db, sql, NULL, NULL, &message) == SQLITE_OK)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "messages: %s", message);
  sqlite3_free (message);
  return FALSE;
}

static sqlite3_stmt *
prepare (SmsStore *self, const char *sql)
{
  sqlite3_stmt *statement = NULL;

  if (sqlite3_prepare_v2 (self->db, sql, -1, &statement, NULL) != SQLITE_OK)
    {
      g_warning ("messages: %s: %s", sqlite3_errmsg (self->db), sql);
      return NULL;
    }
  return statement;
}

/* a statement that returns one number, or @fallback when it returns none */
static gint64
query_int (SmsStore *self, sqlite3_stmt *statement, gint64 fallback)
{
  gint64 value = fallback;

  if (statement && sqlite3_step (statement) == SQLITE_ROW && sqlite3_column_type (statement, 0) != SQLITE_NULL)
    value = sqlite3_column_int64 (statement, 0);
  sqlite3_finalize (statement);
  return value;
}

static gboolean
run (SmsStore *self, sqlite3_stmt *statement)
{
  gboolean done = statement && sqlite3_step (statement) == SQLITE_DONE;

  if (statement && !done)
    g_warning ("messages: %s", sqlite3_errmsg (self->db));
  sqlite3_finalize (statement);
  return done;
}

static char *
column_text (sqlite3_stmt *row, int column)
{
  return g_strdup ((const char *) sqlite3_column_text (row, column));
}

SmsStore *
sms_store_open (const char *path, gboolean read_only, GError **error)
{
  SmsStore *self = g_new0 (SmsStore, 1);
  g_autofree char *dir = g_path_get_dirname (path);
  int flags = read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  sqlite3_stmt *version;
  gint64 user_version;

  if (!read_only)
    g_mkdir_with_parents (dir, 0700);
  if (sqlite3_open_v2 (path, &self->db, flags, NULL) != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "messages: %s", sqlite3_errmsg (self->db));
      sms_store_free (self);
      return NULL;
    }
  /* the service writes as the app reads: a reader waits out a write rather than failing */
  sqlite3_busy_timeout (self->db, 2000);
  if (read_only)
    return self;
  /* a message that comes as the phone loses power should still be there: WAL, synced at each commit */
  exec (self, "PRAGMA journal_mode=WAL", NULL);
  exec (self, "PRAGMA foreign_keys=ON", NULL);
  version = prepare (self, "PRAGMA user_version");
  user_version = query_int (self, version, 0);
  if (!exec (self, schema, error))
    {
      sms_store_free (self);
      return NULL;
    }
  /* a store made before the origin column: the column added, every older row the owner's */
  if (query_int (self, prepare (self, "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name = 'origin'"), 1) == 0)
    exec (self, "ALTER TABLE messages ADD COLUMN origin TEXT", NULL);
  if (user_version == 0)
    {
      g_autofree char *set = g_strdup_printf ("PRAGMA user_version=%d", SCHEMA_VERSION);

      exec (self, set, NULL);
    }
  return self;
}

void
sms_store_free (SmsStore *self)
{
  if (!self)
    return;
  sqlite3_close (self->db);
  g_free (self);
}

/* ---- participants and conversations ----------------------------------------------------------------------- */

/* the participant for @destination, made if new: its id, or 0 */
static gint64
participant_id (SmsStore *self, const char *destination)
{
  sqlite3_stmt *find = prepare (self, "SELECT _id FROM participants WHERE normalized_destination = ? AND sub_id = ?");
  sqlite3_stmt *insert;
  gint64 id;

  if (!find)
    return 0;
  sqlite3_bind_text (find, 1, destination, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (find, 2, OTHER_SUB_ID);
  if ((id = query_int (self, find, 0)))
    return id;
  insert = prepare (self, "INSERT INTO participants (sub_id, normalized_destination, send_destination,"
                          " display_destination) VALUES (?, ?, ?, ?)");
  if (!insert)
    return 0;
  sqlite3_bind_int (insert, 1, OTHER_SUB_ID);
  for (int i = 2; i <= 4; i++)
    sqlite3_bind_text (insert, i, destination, -1, SQLITE_TRANSIENT);
  return run (self, insert) ? sqlite3_last_insert_rowid (self->db) : 0;
}

static int
by_id (gconstpointer a, gconstpointer b)
{
  gint64 x = *(const gint64 *) a, y = *(const gint64 *) b;

  return x < y ? -1 : x > y;
}

/* the conversation of exactly these participants (BugleDatabaseOperations.getExistingConversation), or 0 */
static gint64
find_conversation (SmsStore *self, GArray *ids)
{
  g_autoptr(GString) sql = g_string_new ("SELECT conversation_id FROM conversation_participants"
                                         " WHERE participant_id IN (");
  sqlite3_stmt *find;

  for (guint i = 0; i < ids->len; i++)
    g_string_append (sql, i ? ", ?" : "?");
  g_string_append (sql, ") AND conversation_id IN (SELECT _id FROM conversations WHERE participant_count = ?)"
                        " GROUP BY conversation_id HAVING COUNT(*) = ? ORDER BY conversation_id LIMIT 1");
  if (!(find = prepare (self, sql->str)))
    return 0;
  for (guint i = 0; i < ids->len; i++)
    sqlite3_bind_int64 (find, i + 1, g_array_index (ids, gint64, i));
  sqlite3_bind_int (find, ids->len + 1, ids->len);
  sqlite3_bind_int (find, ids->len + 2, ids->len);
  return query_int (self, find, 0);
}

static gint64
get_conversation (SmsStore *self, const char * const *destinations)
{
  g_autoptr(GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autofree char *name = NULL;
  sqlite3_stmt *insert;
  gint64 id;

  for (guint i = 0; destinations && destinations[i]; i++)
    {
      gint64 participant = participant_id (self, destinations[i]);
      gboolean known = FALSE;

      if (!participant)
        return 0;
      for (guint j = 0; j < ids->len; j++)
        known |= g_array_index (ids, gint64, j) == participant;
      if (!known)
        g_array_append_val (ids, participant);
    }
  if (!ids->len)
    return 0;
  g_array_sort (ids, by_id);
  if ((id = find_conversation (self, ids)))
    return id;

  name = g_strjoinv (", ", (GStrv) destinations);
  insert = prepare (self, "INSERT INTO conversations (name, participant_normalized_destination, participant_count)"
                          " VALUES (?, ?, ?)");
  if (!insert)
    return 0;
  sqlite3_bind_text (insert, 1, name, -1, SQLITE_TRANSIENT);
  /* a 1:1 conversation's other participant (participant_normalized_destination) */
  if (ids->len == 1)
    sqlite3_bind_text (insert, 2, destinations[0], -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (insert, 3, ids->len);
  if (!run (self, insert))
    return 0;
  id = sqlite3_last_insert_rowid (self->db);
  for (guint i = 0; i < ids->len; i++)
    {
      sqlite3_stmt *member = prepare (self, "INSERT INTO conversation_participants (conversation_id, participant_id)"
                                            " VALUES (?, ?)");

      if (!member)
        return 0;
      sqlite3_bind_int64 (member, 1, id);
      sqlite3_bind_int64 (member, 2, g_array_index (ids, gint64, i));
      if (!run (self, member))
        return 0;
    }
  return id;
}

gint64
sms_store_get_conversation (SmsStore *self, const char * const *destinations)
{
  gint64 id;

  exec (self, "BEGIN", NULL);
  id = get_conversation (self, destinations);
  exec (self, id ? "COMMIT" : "ROLLBACK", NULL);
  return id;
}

/* the conversation's newest message is its latest_message_id, snippet and sort time
 * (BugleDatabaseOperations.refreshConversationMetadataInTransaction); a draft shown sorts by when it was typed */
static void
refresh_conversation (SmsStore *self, gint64 conversation_id)
{
  sqlite3_stmt *update = prepare (self,
    "UPDATE conversations SET"
    "  latest_message_id = (SELECT _id FROM messages WHERE conversation_id = ?1"
    "                       ORDER BY received_timestamp DESC, _id DESC LIMIT 1),"
    "  snippet_text = (SELECT group_concat(text, '') FROM parts WHERE message_id = (SELECT _id FROM messages"
    "                  WHERE conversation_id = ?1 ORDER BY received_timestamp DESC, _id DESC LIMIT 1)),"
    "  sort_timestamp = MAX(CASE WHEN show_draft THEN sort_timestamp ELSE 0 END,"
    "                       IFNULL((SELECT MAX(received_timestamp) FROM messages WHERE conversation_id = ?1), 0))"
    " WHERE _id = ?1");

  if (!update)
    return;
  sqlite3_bind_int64 (update, 1, conversation_id);
  run (self, update);
}

static gint64
add_message (SmsStore *self, gint64 conversation_id, gint64 sender_id, const char *text, gint64 sent,
             gint64 received, MessageStatus status, gboolean read, const char *sms_uri, const char *origin)
{
  sqlite3_stmt *insert = prepare (self, "INSERT INTO messages (conversation_id, sender_id, sent_timestamp,"
                                        " received_timestamp, message_protocol, message_status, seen, read,"
                                        " sms_message_uri, origin) VALUES (?, ?, ?, ?, 0, ?, ?, ?, ?, ?)");
  sqlite3_stmt *part;
  gint64 id;

  if (!insert)
    return 0;
  sqlite3_bind_int64 (insert, 1, conversation_id);
  if (sender_id)
    sqlite3_bind_int64 (insert, 2, sender_id);
  else
    sqlite3_bind_null (insert, 2);
  sqlite3_bind_int64 (insert, 3, sent);
  sqlite3_bind_int64 (insert, 4, received);
  sqlite3_bind_int (insert, 5, status);
  sqlite3_bind_int (insert, 6, read);
  sqlite3_bind_int (insert, 7, read);
  if (sms_uri)
    sqlite3_bind_text (insert, 8, sms_uri, -1, SQLITE_TRANSIENT);
  if (origin)
    sqlite3_bind_text (insert, 9, origin, -1, SQLITE_TRANSIENT);
  if (!run (self, insert))
    return 0;
  id = sqlite3_last_insert_rowid (self->db);
  part = prepare (self, "INSERT INTO parts (message_id, text, content_type, timestamp, conversation_id)"
                        " VALUES (?, ?, 'text/plain', ?, ?)");
  if (!part)
    return 0;
  sqlite3_bind_int64 (part, 1, id);
  sqlite3_bind_text (part, 2, text ? text : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (part, 3, received);
  sqlite3_bind_int64 (part, 4, conversation_id);
  if (!run (self, part))
    return 0;
  refresh_conversation (self, conversation_id);
  return id;
}

gint64
sms_store_add_incoming (SmsStore *self, const char *sender, const char *text, gint64 sent, gint64 received,
                        const char *sms_uri)
{
  const char *destinations[] = { sender, NULL };
  gint64 conversation_id, sender_id, id = 0;
  sqlite3_stmt *unarchive;

  exec (self, "BEGIN", NULL);
  if ((conversation_id = get_conversation (self, destinations)) && (sender_id = participant_id (self, sender)))
    id = add_message (self, conversation_id, sender_id, text, sent, received, MESSAGE_STATUS_INCOMING_COMPLETE,
                      FALSE, sms_uri, NULL);
  /* ReceiveSmsMessageAction: a new message brings an archived conversation back */
  if (id && (unarchive = prepare (self, "UPDATE conversations SET archive_status = 0 WHERE _id = ?")))
    {
      sqlite3_bind_int64 (unarchive, 1, conversation_id);
      run (self, unarchive);
    }
  exec (self, id ? "COMMIT" : "ROLLBACK", NULL);
  return id;
}

gint64
sms_store_add_outgoing (SmsStore *self, gint64 conversation_id, const char *text, gint64 now, const char *origin)
{
  sqlite3_stmt *draft;
  gint64 id;

  exec (self, "BEGIN", NULL);
  /* the draft is what was sent: it goes (InsertNewMessageAction) */
  if ((draft = prepare (self, "UPDATE conversations SET show_draft = 0, draft_snippet_text = NULL WHERE _id = ?")))
    {
      sqlite3_bind_int64 (draft, 1, conversation_id);
      run (self, draft);
    }
  id = add_message (self, conversation_id, 0, text, now, now, MESSAGE_STATUS_OUTGOING_YET_TO_SEND, TRUE, NULL, origin);
  exec (self, id ? "COMMIT" : "ROLLBACK", NULL);
  return id;
}

void
sms_store_set_status (SmsStore *self, gint64 message_id, MessageStatus status, int raw_status, gint64 sent)
{
  sqlite3_stmt *update = prepare (self, "UPDATE messages SET message_status = ?, raw_status = ?,"
                                        " sent_timestamp = CASE WHEN ? > 0 THEN ? ELSE sent_timestamp END"
                                        " WHERE _id = ?");

  if (!update)
    return;
  sqlite3_bind_int (update, 1, status);
  sqlite3_bind_int (update, 2, raw_status);
  sqlite3_bind_int64 (update, 3, sent);
  sqlite3_bind_int64 (update, 4, sent);
  sqlite3_bind_int64 (update, 5, message_id);
  run (self, update);
}

gint64
sms_store_find_by_uri (SmsStore *self, const char *sms_uri)
{
  sqlite3_stmt *find = sms_uri && *sms_uri ? prepare (self, "SELECT _id FROM messages WHERE sms_message_uri = ?") : NULL;
  gint64 id = 0;

  if (!find)
    return 0;
  sqlite3_bind_text (find, 1, sms_uri, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (find) == SQLITE_ROW)
    id = sqlite3_column_int64 (find, 0);
  sqlite3_reset (find);
  return id;
}

void
sms_store_set_sms_uri (SmsStore *self, gint64 message_id, const char *sms_uri)
{
  sqlite3_stmt *update = prepare (self, "UPDATE messages SET sms_message_uri = ? WHERE _id = ?");

  if (!update)
    return;
  if (sms_uri)
    sqlite3_bind_text (update, 1, sms_uri, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (update, 2, message_id);
  run (self, update);
}

static guint
mark (SmsStore *self, const char *sql, gint64 conversation_id)
{
  sqlite3_stmt *update = prepare (self, sql);

  if (!update)
    return 0;
  sqlite3_bind_int64 (update, 1, conversation_id);
  return run (self, update) ? sqlite3_changes (self->db) : 0;
}

guint
sms_store_mark_read (SmsStore *self, gint64 conversation_id)
{
  /* MarkAsReadAction: read and seen both */
  return mark (self, "UPDATE messages SET read = 1, seen = 1 WHERE conversation_id = ? AND (read = 0 OR seen = 0)",
               conversation_id);
}

guint
sms_store_mark_seen (SmsStore *self, gint64 conversation_id)
{
  /* MarkAsSeenAction: 0 for every conversation (the notification went) */
  if (!conversation_id)
    return mark (self, "UPDATE messages SET seen = 1 WHERE seen = 0 AND ? = 0", 0);
  return mark (self, "UPDATE messages SET seen = 1 WHERE conversation_id = ? AND seen = 0", conversation_id);
}

void
sms_store_set_archived (SmsStore *self, gint64 conversation_id, gboolean archived)
{
  sqlite3_stmt *update = prepare (self, "UPDATE conversations SET archive_status = ? WHERE _id = ?");

  if (!update)
    return;
  sqlite3_bind_int (update, 1, archived);
  sqlite3_bind_int64 (update, 2, conversation_id);
  run (self, update);
}

void
sms_store_set_draft (SmsStore *self, gint64 conversation_id, const char *text)
{
  gboolean show = text && *text;
  sqlite3_stmt *update = prepare (self, "UPDATE conversations SET show_draft = ?, draft_snippet_text = ?,"
                                        " sort_timestamp = CASE WHEN ? THEN ? ELSE sort_timestamp END WHERE _id = ?");

  if (!update)
    return;
  sqlite3_bind_int (update, 1, show);
  if (show)
    sqlite3_bind_text (update, 2, text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (update, 3, show);
  sqlite3_bind_int64 (update, 4, g_get_real_time () / 1000);
  sqlite3_bind_int64 (update, 5, conversation_id);
  exec (self, "BEGIN", NULL);
  run (self, update);
  refresh_conversation (self, conversation_id);
  exec (self, "COMMIT", NULL);
}

gboolean
sms_store_delete_conversation (SmsStore *self, gint64 conversation_id)
{
  return mark (self, "DELETE FROM conversations WHERE _id = ?", conversation_id) > 0;
}

gint64
sms_store_delete_message (SmsStore *self, gint64 message_id)
{
  sqlite3_stmt *find = prepare (self, "SELECT conversation_id FROM messages WHERE _id = ?");
  sqlite3_stmt *left;
  gint64 conversation_id;

  if (!find)
    return 0;
  sqlite3_bind_int64 (find, 1, message_id);
  if (!(conversation_id = query_int (self, find, 0)))
    return 0;
  exec (self, "BEGIN", NULL);
  mark (self, "DELETE FROM messages WHERE _id = ?", message_id);
  /* DeleteMessageAction: the conversation goes with its last message, unless a draft keeps it */
  if ((left = prepare (self, "SELECT COUNT(*) FROM messages WHERE conversation_id = ?")))
    {
      sqlite3_bind_int64 (left, 1, conversation_id);
      if (query_int (self, left, 0) == 0)
        mark (self, "DELETE FROM conversations WHERE _id = ? AND show_draft = 0", conversation_id);
    }
  refresh_conversation (self, conversation_id);
  exec (self, "COMMIT", NULL);
  return conversation_id;
}

/* ---- reading ------------------------------------------------------------------------------------------------ */

static GStrv
destinations_of (SmsStore *self, gint64 conversation_id)
{
  sqlite3_stmt *select = prepare (self, "SELECT p.normalized_destination FROM conversation_participants cp"
                                        " JOIN participants p ON p._id = cp.participant_id"
                                        " WHERE cp.conversation_id = ? ORDER BY cp._id");
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

  if (select)
    {
      sqlite3_bind_int64 (select, 1, conversation_id);
      while (sqlite3_step (select) == SQLITE_ROW)
        g_strv_builder_add (builder, (const char *) sqlite3_column_text (select, 0));
      sqlite3_finalize (select);
    }
  return g_strv_builder_end (builder);
}

#define CONVERSATION_COLUMNS \
  "SELECT c._id, c.name, c.snippet_text, c.sort_timestamp, c.latest_message_id," \
  " (SELECT message_status FROM messages WHERE _id = c.latest_message_id)," \
  " NOT EXISTS (SELECT 1 FROM messages WHERE conversation_id = c._id AND read = 0)," \
  " c.archive_status, c.show_draft, c.draft_snippet_text FROM conversations c"

static SmsConversation *
conversation_from_row (SmsStore *self, sqlite3_stmt *row)
{
  SmsConversation *conversation = g_new0 (SmsConversation, 1);

  conversation->id = sqlite3_column_int64 (row, 0);
  conversation->name = column_text (row, 1);
  conversation->snippet = column_text (row, 2);
  conversation->sort_timestamp = sqlite3_column_int64 (row, 3);
  conversation->latest_message_id = sqlite3_column_int64 (row, 4);
  conversation->latest_status = sqlite3_column_int (row, 5);
  conversation->read = sqlite3_column_int (row, 6);
  conversation->archived = sqlite3_column_int (row, 7);
  conversation->show_draft = sqlite3_column_int (row, 8);
  conversation->draft = column_text (row, 9);
  conversation->destinations = destinations_of (self, conversation->id);
  return conversation;
}

GPtrArray *
sms_store_list_conversations (SmsStore *self, gboolean archived)
{
  GPtrArray *conversations = g_ptr_array_new_with_free_func ((GDestroyNotify) sms_conversation_free);
  sqlite3_stmt *select = prepare (self, CONVERSATION_COLUMNS
                                        " WHERE c.archive_status = ? AND (c.latest_message_id IS NOT NULL"
                                        " OR c.show_draft) ORDER BY c.sort_timestamp DESC, c._id DESC");

  if (!select)
    return conversations;
  sqlite3_bind_int (select, 1, archived);
  while (sqlite3_step (select) == SQLITE_ROW)
    g_ptr_array_add (conversations, conversation_from_row (self, select));
  sqlite3_finalize (select);
  return conversations;
}

SmsConversation *
sms_store_get (SmsStore *self, gint64 conversation_id)
{
  sqlite3_stmt *select = prepare (self, CONVERSATION_COLUMNS " WHERE c._id = ?");
  SmsConversation *conversation = NULL;

  if (!select)
    return NULL;
  sqlite3_bind_int64 (select, 1, conversation_id);
  if (sqlite3_step (select) == SQLITE_ROW)
    conversation = conversation_from_row (self, select);
  sqlite3_finalize (select);
  return conversation;
}

#define MESSAGE_COLUMNS \
  "SELECT m._id, m.conversation_id, p.normalized_destination," \
  " (SELECT group_concat(text, '') FROM parts WHERE message_id = m._id)," \
  " m.sent_timestamp, m.received_timestamp, m.message_status, m.read, m.seen, m.sms_message_uri, m.raw_status," \
  " m.origin" \
  " FROM messages m LEFT JOIN participants p ON p._id = m.sender_id"

/* oldest first; with a limit, only the newest that many (columns 6 and 1: received_timestamp and _id) */
static GPtrArray *
list_messages (SmsStore *self, const char *where, gint64 value, guint limit)
{
  GPtrArray *messages = g_ptr_array_new_with_free_func ((GDestroyNotify) sms_message_free);
  g_autofree char *sql = limit
    ? g_strconcat ("SELECT * FROM (" MESSAGE_COLUMNS " WHERE ", where,
                   " ORDER BY m.received_timestamp DESC, m._id DESC LIMIT ?2) ORDER BY 6, 1", NULL)
    : g_strconcat (MESSAGE_COLUMNS " WHERE ", where, " ORDER BY m.received_timestamp, m._id", NULL);
  sqlite3_stmt *select = prepare (self, sql);

  if (!select)
    return messages;
  if (sqlite3_bind_parameter_count (select))
    sqlite3_bind_int64 (select, 1, value);
  if (limit)
    sqlite3_bind_int64 (select, 2, limit);
  while (sqlite3_step (select) == SQLITE_ROW)
    {
      SmsMessage *message = g_new0 (SmsMessage, 1);

      message->id = sqlite3_column_int64 (select, 0);
      message->conversation_id = sqlite3_column_int64 (select, 1);
      message->status = sqlite3_column_int (select, 6);
      /* an outgoing message's sender is this phone */
      message->sender = MESSAGE_STATUS_IS_INCOMING (message->status) ? column_text (select, 2) : NULL;
      message->text = column_text (select, 3);
      message->sent = sqlite3_column_int64 (select, 4);
      message->received = sqlite3_column_int64 (select, 5);
      message->read = sqlite3_column_int (select, 7);
      message->seen = sqlite3_column_int (select, 8);
      message->sms_uri = column_text (select, 9);
      message->raw_status = sqlite3_column_int (select, 10);
      message->origin = column_text (select, 11);
      g_ptr_array_add (messages, message);
    }
  sqlite3_finalize (select);
  return messages;
}

GPtrArray *
sms_store_list_messages (SmsStore *self, gint64 conversation_id)
{
  return list_messages (self, "m.conversation_id = ?", conversation_id, 0);
}

GPtrArray *
sms_store_list_last_messages (SmsStore *self, gint64 conversation_id, guint limit)
{
  return list_messages (self, "m.conversation_id = ?", conversation_id, MAX (limit, 1));
}

SmsMessage *
sms_store_get_message (SmsStore *self, gint64 message_id)
{
  g_autoptr(GPtrArray) messages = list_messages (self, "m._id = ?", message_id, 0);

  return messages->len ? g_ptr_array_steal_index (messages, 0) : NULL;
}

GPtrArray *
sms_store_list_unseen (SmsStore *self)
{
  return list_messages (self, "m.message_status >= 100 AND m.seen = 0", 0, 0);
}

GPtrArray *
sms_store_list_unsent (SmsStore *self)
{
  return list_messages (self, "m.message_status IN (4, 5, 6, 7)", 0, 0);
}

GHashTable *
sms_store_unread_counts (SmsStore *self)
{
  GHashTable *counts = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  sqlite3_stmt *select = prepare (self, "SELECT conversation_id, COUNT(*) FROM messages WHERE read = 0"
                                        " GROUP BY conversation_id");

  if (select)
    {
      while (sqlite3_step (select) == SQLITE_ROW)
        g_hash_table_insert (counts, g_memdup2 (&(gint64) { sqlite3_column_int64 (select, 0) }, sizeof (gint64)),
                             GINT_TO_POINTER (sqlite3_column_int (select, 1)));
      sqlite3_finalize (select);
    }
  return counts;
}

GStrv
sms_store_list_frequent (SmsStore *self, guint limit)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  sqlite3_stmt *select = prepare (self, "SELECT p.normalized_destination FROM conversation_participants cp"
                                        " JOIN participants p ON p._id = cp.participant_id"
                                        " JOIN messages m ON m.conversation_id = cp.conversation_id"
                                        " WHERE NOT p.blocked GROUP BY p._id"
                                        " ORDER BY COUNT(m._id) DESC, MAX(m.received_timestamp) DESC LIMIT ?");

  if (select)
    {
      sqlite3_bind_int (select, 1, limit);
      while (sqlite3_step (select) == SQLITE_ROW)
        g_strv_builder_add (builder, (const char *) sqlite3_column_text (select, 0));
      sqlite3_finalize (select);
    }
  return g_strv_builder_end (builder);
}
