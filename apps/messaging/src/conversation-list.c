/* The conversation list (conversation-list.h). */
#include "conversation-list.h"
#include "blocked-numbers.h"
#include "sms-agent.h"
#include "sms-provider.h"
#include "msg-add-contact.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-dates.h"
#include "msg-dialog.h"
#include "msg-image.h"
#include "msg-people.h"
#include "msg-swipe.h"
#include "popup-menu.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define LIST_TOP_PADDING 8
#define ROW_PADDING 16          /* conversation_list_item_view_padding */
#define AVATAR 56               /* conversation_list_contact_icon_size */
#define TEXT_PADDING 16         /* ConversationListItemViewPaddingStyle */
#define FAILED_BADGE 24
#define SNIPPET_LINES_UNREAD 3
#define FAB_MARGIN 16
#define EMPTY_TEXT_TOP 20
#define EMPTY_TEXT_SIDE 60
#define EMPTY_TEXT_PAD 4         /* the TextView's 7.9 dp more than its lines, over and under them (still:sms-archived:
                                 * 73 px for one 14 sp line) */
#define REFRESH_MS (60 * 1000)  /* "Just now" becomes "1 min" */
/* ConversationListSwipeHelper */

typedef struct {
  gint64 id;
  gboolean archived;
  char *destination;            /* the one other participant as stored, for Block; NULL in a group */
  MsgPerson *person;            /* that participant, for Add contact and their avatar; NULL in a group */
  GtkWidget *row, *swipe, *content, *avatar;
} Row;

struct _ConversationList {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  GSettings *settings;          /* de.yesman.Messaging: Swipe to delete */
  gboolean archived;
  gboolean forward;             /* ForwardMessageActivity's list: no bars, no FAB, no swipe, no select */

  GtkWidget *box;               /* the page: bar over the content */
  GtkWidget *bar, *select_bar;
  GtkWidget *archive_action, *unarchive_action, *add_contact_action, *block_action;
  GtkWidget *scroll, *list, *empty, *fab;
  GtkWidget *strip, *chips;     /* the groups (SMS, and each provider of messaging.conf), main list only */
  char *chips_key;              /* the groups the chips were made for: they are made again when the file changes */
  char *group;                  /* the group shown ("" is SMS); NULL: all of them (archived, forward) */
  NeoSnackbar *snackbar;
  GHashTable *rows;             /* id -> Row */
  GHashTable *selected;         /* ids */
  guint refresh_id;
  gboolean at_top;
};

G_DEFINE_FINAL_TYPE (ConversationList, conversation_list, GTK_TYPE_WIDGET)

enum { OPEN_CONVERSATION, START_CHAT, SHOW_ARCHIVED, SHOW_BLOCKED, SHOW_SETTINGS, CLOSED, AT_TOP_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void reload (ConversationList *self);

static const char *
country (ConversationList *self)
{
  return sms_client_get_country (self->client);
}

/* the one conversation selected, when it is with one person who is not blocked yet: what Block acts on */
static Row *
blockable (ConversationList *self)
{
  GHashTableIter iter;
  gpointer key;
  Row *row;

  if (g_hash_table_size (self->selected) != 1)
    return NULL;
  g_hash_table_iter_init (&iter, self->selected);
  g_hash_table_iter_next (&iter, &key, NULL);
  row = g_hash_table_lookup (self->rows, key);
  if (!row || !row->destination || sms_agent_name (row->destination)
      || blocked_numbers_contains (blocked_numbers_get_default (), row->destination, country (self)))
    return NULL;
  return row;
}

/* the one conversation selected, when it is with one person who is not a contact: what Add contact acts on
 * (MultiSelectActionModeCallback: one selected, not a group, no lookup key) */
static Row *
addable (ConversationList *self)
{
  GHashTableIter iter;
  gpointer key;
  Row *row;

  if (g_hash_table_size (self->selected) != 1)
    return NULL;
  g_hash_table_iter_init (&iter, self->selected);
  g_hash_table_iter_next (&iter, &key, NULL);
  row = g_hash_table_lookup (self->rows, key);
  return row && msg_contact_can_add (row->person) ? row : NULL;
}

/* --- select mode (§1.11) */

static gboolean
selecting (ConversationList *self)
{
  return g_hash_table_size (self->selected) > 0;
}

static void
sync_select (ConversationList *self)
{
  gboolean any_archived = FALSE, any_unarchived = FALSE;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      Row *row = value;
      gboolean on = g_hash_table_contains (self->selected, key);

      msg_avatar_set_checked (MSG_AVATAR (row->avatar), on);
      /* no swiping while selecting, nor in the archived list or the forward one */
      msg_swipe_set_enabled (MSG_SWIPE (row->swipe), !self->archived && !self->forward && !selecting (self));
      if (on)
        {
          any_archived |= row->archived;
          any_unarchived |= !row->archived;
        }
    }
  gtk_widget_set_visible (self->select_bar, selecting (self));
  gtk_widget_set_visible (self->bar, !self->forward && !selecting (self));
  gtk_widget_set_visible (self->archive_action, any_unarchived);
  gtk_widget_set_visible (self->unarchive_action, any_archived);
  gtk_widget_set_visible (self->add_contact_action, addable (self) != NULL);
  gtk_widget_set_visible (self->block_action, blockable (self) != NULL);
  if (self->fab)
    /* the FAB slides away to the right while selecting (300 ms) */
    gtk_widget_set_visible (self->fab, !selecting (self));
  g_signal_emit (self, signals[AT_TOP_CHANGED], 0);
}

static void
toggle_selected (ConversationList *self, gint64 id)
{
  if (!g_hash_table_remove (self->selected, &id))
    g_hash_table_add (self->selected, g_memdup2 (&id, sizeof id));
  sync_select (self);
}

gboolean
conversation_list_back (ConversationList *self)
{
  if (!selecting (self))
    return FALSE;
  g_hash_table_remove_all (self->selected);
  sync_select (self);
  return TRUE;
}

/* the selected ids, and select mode left */
static GArray *
take_selected (ConversationList *self)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, self->selected);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_array_append_val (ids, *(gint64 *) key);
  g_hash_table_remove_all (self->selected);
  sync_select (self);
  return ids;
}

typedef struct {
  ConversationList *self;
  GArray *ids;
  gboolean archived;
} Undo;

static void
undo_free (gpointer data)
{
  Undo *undo = data;

  g_array_unref (undo->ids);
  g_object_unref (undo->self);
  g_free (undo);
}

static void
on_undo (gpointer data)
{
  Undo *undo = data;

  for (guint i = 0; i < undo->ids->len; i++)
    sms_client_set_archived (undo->self->client, g_array_index (undo->ids, gint64, i), !undo->archived);
}

/* archives or unarchives them, with the snackbar's Undo: "2 archived" */
static void
set_archived (ConversationList *self, GArray *ids, gboolean archived)
{
  Undo *undo = g_new0 (Undo, 1);
  g_autofree char *text = NULL;

  for (guint i = 0; i < ids->len; i++)
    sms_client_set_archived (self->client, g_array_index (ids, gint64, i), archived);
  text = g_strdup_printf (archived ? ngettext ("%d archived", "%d archived", ids->len)
                                   : ngettext ("%d unarchived", "%d unarchived", ids->len), ids->len);
  undo->self = g_object_ref (self);
  undo->ids = g_array_ref (ids);
  undo->archived = archived;
  /* the undo lives as long as the snackbar might call it */
  g_object_set_data_full (G_OBJECT (self->snackbar), "msg-undo", undo, undo_free);
  msg_snackbar_show (self->snackbar, text, _("Undo"), on_undo, undo);
}

static void
on_archive (ConversationList *self)
{
  g_autoptr(GArray) ids = take_selected (self);

  set_archived (self, ids, TRUE);
}

static void
on_unarchive (ConversationList *self)
{
  g_autoptr(GArray) ids = take_selected (self);

  set_archived (self, ids, FALSE);
}

static void
on_delete_response (NeoDialog *dialog, int response, gpointer data)
{
  ConversationList *self = data;
  GArray *ids = g_object_get_data (G_OBJECT (dialog), "ids");

  if (response == NEO_RESPONSE_OK)
    for (guint i = 0; i < ids->len; i++)
      sms_client_delete_conversation (self->client, g_array_index (ids, gint64, i));
}

static void
on_delete (ConversationList *self)
{
  GArray *ids = take_selected (self);
  NeoDialog *dialog = neo_dialog_new (ngettext ("Delete this conversation?", "Delete these conversations?",
                                                ids->len));

  g_object_set_data_full (G_OBJECT (dialog), "ids", ids, (GDestroyNotify) g_array_unref);
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), self);
  neo_dialog_present (dialog, GTK_WIDGET (self));
}

/* Block: the number goes on the blocked list (the Phone's too) and the conversation is archived; the service then
 * keeps what it sends without a notification. Undo takes both back. */

typedef struct {
  ConversationList *self;
  gint64 id;
  char *destination;
} Block;

static void
block_free (gpointer data)
{
  Block *block = data;

  g_object_unref (block->self);
  g_free (block->destination);
  g_free (block);
}

static void
on_block_undo (gpointer data)
{
  Block *block = data;
  g_autoptr(GError) error = NULL;

  if (!blocked_numbers_remove (blocked_numbers_get_default (), block->destination, country (block->self), &error))
    {
      g_warning ("messaging: %s", error->message);
      return;
    }
  sms_client_set_archived (block->self->client, block->id, FALSE);
  msg_snackbar_show (block->self->snackbar, _("Contact unblocked & unarchived"), NULL, NULL, NULL);
}

static void
on_block_response (NeoDialog *dialog, int response, gpointer data)
{
  ConversationList *self = data;
  Block *block = g_object_steal_data (G_OBJECT (dialog), "block");
  g_autoptr(GError) error = NULL;

  if (response != NEO_RESPONSE_OK)
    {
      block_free (block);
      return;
    }
  if (!blocked_numbers_add (blocked_numbers_get_default (), block->destination, country (self), &error))
    {
      g_warning ("messaging: %s", error->message);
      block_free (block);
      return;
    }
  sms_client_set_archived (self->client, block->id, TRUE);
  /* the undo lives as long as the snackbar might call it */
  g_object_set_data_full (G_OBJECT (self->snackbar), "msg-undo", block, block_free);
  msg_snackbar_show (self->snackbar, _("Contact blocked & archived"), _("Undo"), on_block_undo, block);
}

static void
on_block (ConversationList *self)
{
  Row *row = blockable (self);
  Block *block;
  g_autofree char *title = NULL;
  NeoDialog *dialog;

  if (!row)
    return;
  block = g_new0 (Block, 1);
  block->self = g_object_ref (self);
  block->id = row->id;
  block->destination = g_strdup (row->destination);
  g_array_unref (take_selected (self));
  /* Bugle names the number as stored, not the contact */
  title = g_strdup_printf (_("Block %s?"), block->destination);
  dialog = neo_dialog_new (title);
  neo_dialog_set_message (dialog, _("You'll continue to receive messages from this number but won't be notified "
                                    "anymore. This conversation will be archived."));
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "block", block);
  g_signal_connect (dialog, "response", G_CALLBACK (on_block_response), self);
  neo_dialog_present (dialog, GTK_WIDGET (self));
}

/* Add contact: "Add to Contacts?" for the person, and select mode left
 * (AbstractConversationListActivity.onActionBarAddContact) */
static void
on_add_contact (ConversationList *self)
{
  Row *row = addable (self);

  if (!row)
    return;
  msg_contact_confirm_add (GTK_WIDGET (self), row->person);
  g_array_unref (take_selected (self));
}

/* --- rows (§1.4, §1.5) */

static void
on_row_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");

  if (selecting (self))
    toggle_selected (self, id);
  else
    g_signal_emit (self, signals[OPEN_CONVERSATION], 0, id);
}

static void
on_row_long_pressed (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");

  /* ForwardMessageActivity.onConversationClick takes a long click as a tap */
  if (self->forward)
    g_signal_emit (self, signals[OPEN_CONVERSATION], 0, id);
  else
    toggle_selected (self, id);
}

/* the avatar (ContactIconView): while selecting, a tap on it is the row's; else the person's card, or "Add to
 * Contacts?"; a group's takes the tap and does nothing. A long press is the row's. */
static void
on_avatar_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");
  Row *row = g_hash_table_lookup (self->rows, &id);

  if (selecting (self))
    toggle_selected (self, id);
  else if (row && row->person)
    msg_contact_show_or_add (pressable, row->person, self->snackbar);
}

static GtkWidget *
text_label (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (label, css);
  return label;
}

/* the status words where the time goes: "Draft", "Sending…", "Not sent. Tap to try again." */
static char *
row_time (const SmsConversation *conversation, gboolean *failed, gint64 now)
{
  *failed = FALSE;
  if (conversation->show_draft)
    return g_strdup (_("Draft"));
  switch (conversation->latest_status)
    {
    case MESSAGE_STATUS_OUTGOING_YET_TO_SEND:
    case MESSAGE_STATUS_OUTGOING_SENDING:
    case MESSAGE_STATUS_OUTGOING_RESENDING:
    case MESSAGE_STATUS_OUTGOING_AWAITING_RETRY:
      return g_strdup (_("Sending…"));
    case MESSAGE_STATUS_OUTGOING_FAILED:
    case MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER:
      *failed = TRUE;
      return g_strdup (_("Not sent. Tap to try again."));
    default:
      return msg_dates_conversation (conversation->sort_timestamp, now);
    }
}

/* swiped away: archived, with Undo ("1 archived") */
/* archived, with Undo; or with "Swipe to delete" on, a swipe to the right deletes it, with no Undo and no question
 * (ConversationListItemView.onSwipeComplete) */
static void
on_swiped (GtkWidget *swipe, gboolean right, gpointer data)
{
  ConversationList *self = data;
  g_autoptr(GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (swipe), "id");

  if (right && g_settings_get_boolean (self->settings, "swipe-right-deletes"))
    {
      sms_client_delete_conversation (self->client, id);
      msg_snackbar_show (self->snackbar, _("Conversation deleted"), NULL, NULL, NULL);
      return;
    }
  g_array_append_val (ids, id);
  set_archived (self, ids, TRUE);
}

static void row_free (gpointer data);

static GtkWidget *
make_row (ConversationList *self, const SmsConversation *conversation, gint64 now)
{
  g_autoptr(GPtrArray) people = msg_people_new (self->contacts, (const char * const *) conversation->destinations,
                                                sms_client_get_country (self->client));
  g_autofree char *name = msg_people_name (people);
  g_autofree char *time = NULL;
  gboolean failed, unread = !conversation->read;
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar_box = gtk_overlay_new ();
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name_label, *snippet, *time_label, *pressable, *avatar_pressable;
  Row *row = g_new0 (Row, 1);

  /* the avatar: the person's, or the group's (up to four) */
  if (people->len == 1)
    {
      MsgPerson *person = people->pdata[0];

      msg_avatar_set_person (MSG_AVATAR (avatar), person->name, person->identifier, person->photo);
    }
  else
    {
      g_autoptr(GPtrArray) names = g_ptr_array_new (), ids = g_ptr_array_new (), photos = g_ptr_array_new ();

      for (guint i = 0; i < people->len; i++)
        {
          MsgPerson *person = people->pdata[i];

          g_ptr_array_add (names, person->name);
          g_ptr_array_add (ids, person->identifier);
          g_ptr_array_add (photos, person->photo);
        }
      g_ptr_array_add (names, NULL);
      msg_avatar_set_group (MSG_AVATAR (avatar), (const char * const *) names->pdata,
                            (const char * const *) ids->pdata, (GBytes * const *) photos->pdata);
    }
  gtk_overlay_set_child (GTK_OVERLAY (avatar_box), avatar);
  time = row_time (conversation, &failed, now);
  if (failed && people->len == 1)
    {
      /* ic_failed_status_red at the avatar's bottom end, 1:1 only */
      GtkWidget *badge = msg_image_new ("/de/yesman/Messaging/images/ic_failed_status_red.png", FAILED_BADGE,
                                        FAILED_BADGE);

      gtk_widget_set_halign (badge, GTK_ALIGN_END);
      gtk_widget_set_valign (badge, GTK_ALIGN_END);
      gtk_overlay_add_overlay (GTK_OVERLAY (avatar_box), badge);
    }
  avatar_pressable = neo_pressable_new (avatar_box);
  neo_pressable_set_ripple (NEO_PRESSABLE (avatar_pressable), NEO_RIPPLE_NONE, 0);
  gtk_widget_set_valign (avatar_pressable, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (content), avatar_pressable);

  name_label = text_label (name, "msg-row-name");
  snippet = text_label (conversation->show_draft ? conversation->draft : conversation->snippet, "msg-row-snippet");
  time_label = text_label (time, "msg-row-time");
  /* unread: bold and light, the snippet up to 3 lines; read: 1 line */
  if (unread)
    {
      gtk_widget_add_css_class (name_label, "unread");
      gtk_widget_add_css_class (snippet, "unread");
      gtk_label_set_wrap (GTK_LABEL (snippet), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (snippet), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_lines (GTK_LABEL (snippet), SNIPPET_LINES_UNREAD);
    }
  else
    gtk_label_set_single_line_mode (GTK_LABEL (snippet), TRUE);
  if (conversation->show_draft)
    {
      gtk_widget_add_css_class (snippet, "draft");
      gtk_widget_add_css_class (time_label, "draft");
    }
  if (failed)
    gtk_widget_add_css_class (time_label, "failed");
  gtk_box_append (GTK_BOX (column), name_label);
  gtk_box_append (GTK_BOX (column), snippet);
  gtk_box_append (GTK_BOX (column), time_label);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_margin_start (column, TEXT_PADDING);
  gtk_widget_set_margin_end (column, TEXT_PADDING);
  gtk_box_append (GTK_BOX (content), column);
  gtk_widget_add_css_class (content, "msg-row-content");
  gtk_widget_set_margin_start (content, ROW_PADDING);
  gtk_widget_set_margin_end (content, ROW_PADDING);
  gtk_widget_set_margin_top (content, ROW_PADDING);
  gtk_widget_set_margin_bottom (content, ROW_PADDING);

  pressable = neo_pressable_new (content);
  gtk_widget_add_css_class (pressable, "msg-row");
  row->id = conversation->id;
  row->archived = conversation->archived;
  if (g_strv_length (conversation->destinations) == 1)
    row->destination = g_strdup (conversation->destinations[0]);
  if (people->len == 1)
    row->person = msg_person_copy (people->pdata[0]);
  row->row = pressable;
  row->content = content;
  row->avatar = avatar;
  g_object_set_data (G_OBJECT (pressable), "list", self);
  g_object_set_data_full (G_OBJECT (pressable), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_row_clicked), NULL);
  g_signal_connect (pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), NULL);
  g_object_set_data (G_OBJECT (avatar_pressable), "list", self);
  g_object_set_data_full (G_OBJECT (avatar_pressable), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (avatar_pressable, "clicked", G_CALLBACK (on_avatar_clicked), NULL);
  g_signal_connect (avatar_pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), NULL);
  g_hash_table_insert (self->rows, g_memdup2 (&row->id, sizeof row->id), row);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  row->swipe = msg_swipe_new (pressable,
                              g_settings_get_boolean (self->settings, "swipe-right-deletes")
                              ? "messaging-delete-symbolic" : "messaging-archive-symbolic",
                              "messaging-archive-symbolic");
  g_object_set_data_full (G_OBJECT (row->swipe), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (row->swipe, "swiped", G_CALLBACK (on_swiped), self);
  return row->swipe;
}

static void
row_free (gpointer data)
{
  Row *row = data;

  g_free (row->destination);
  msg_person_free (row->person);
  g_free (row);
}

/* --- groups: the conversations by who they are with, taken from messaging.conf (sms-provider.h) */

static char *
group_of (const SmsConversation *conversation)
{
  char *group = sms_provider_group ((const char * const *) conversation->destinations);

  return group ? group : g_strdup ("");
}

static void
on_chip_toggled (GtkToggleButton *chip, gpointer data)
{
  ConversationList *self = data;
  const char *group = g_object_get_data (G_OBJECT (chip), "group");

  if (!gtk_toggle_button_get_active (chip) || !g_strcmp0 (group, self->group))
    return;
  g_free (self->group);
  self->group = g_strdup (group);
  g_settings_set_string (self->settings, "last-group", group);
  reload (self);
}

static GtkWidget *
make_chip (ConversationList *self, const char *group, const char *label, GtkWidget *first)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *chip = gtk_toggle_button_new ();

  gtk_widget_add_css_class (dot, "msg-chip-dot");
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (dot, FALSE);
  gtk_box_append (GTK_BOX (box), gtk_label_new (label));
  gtk_box_append (GTK_BOX (box), dot);
  gtk_button_set_child (GTK_BUTTON (chip), box);
  gtk_widget_add_css_class (chip, "msg-chip");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (chip), first ? GTK_TOGGLE_BUTTON (first) : NULL);
  g_object_set_data_full (G_OBJECT (chip), "group", g_strdup (group), g_free);
  g_object_set_data (G_OBJECT (chip), "dot", dot);
  return chip;
}

/* the chips of the groups, made again only when the file's providers are not the ones they were made for; the group
 * shown is the last one used, or the first when it is gone */
static void
sync_chips (ConversationList *self)
{
  g_auto(GStrv) ids = sms_provider_ids ();
  g_autofree char *key = g_strjoinv (",", ids);
  GtkWidget *first = NULL, *child, *active = NULL;

  if (!g_strcmp0 (key, self->chips_key))
    return;
  g_free (self->chips_key);
  self->chips_key = g_steal_pointer (&key);
  while ((child = gtk_widget_get_first_child (self->chips)))
    gtk_box_remove (GTK_BOX (self->chips), child);
  if (!self->group)
    self->group = g_settings_get_string (self->settings, "last-group");
  for (guint i = 0; i <= g_strv_length (ids); i++)
    {
      /* SMS first, then the providers in the file's order */
      const char *group = i ? ids[i - 1] : "";
      g_autofree char *provider_label = i ? sms_provider_label (group) : NULL;
      GtkWidget *chip = make_chip (self, group, i ? (provider_label ? provider_label : group) : _("SMS"), first);

      gtk_box_append (GTK_BOX (self->chips), chip);
      first = first ? first : chip;
      if (!g_strcmp0 (group, self->group))
        active = chip;
    }
  if (!active)
    {
      active = first;
      g_free (self->group);
      self->group = g_strdup ("");
    }
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (active), TRUE);
  /* listening only now: making the group is not choosing one */
  for (child = gtk_widget_get_first_child (self->chips); child; child = gtk_widget_get_next_sibling (child))
    g_signal_connect (child, "toggled", G_CALLBACK (on_chip_toggled), self);
  /* no provider is no choice: no chips */
  gtk_widget_set_visible (self->strip, g_strv_length (ids) > 0);
}

/* the dot of each chip: some conversation of its group is unread */
static void
sync_dots (ConversationList *self, GHashTable *unread)
{
  for (GtkWidget *chip = gtk_widget_get_first_child (self->chips); chip; chip = gtk_widget_get_next_sibling (chip))
    gtk_widget_set_visible (g_object_get_data (G_OBJECT (chip), "dot"),
                            g_hash_table_contains (unread, g_object_get_data (G_OBJECT (chip), "group")));
}

void
conversation_list_show_group_of (ConversationList *self, gint64 id)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(SmsConversation) conversation = store && self->chips ? sms_store_get (store, id) : NULL;
  g_autofree char *group = conversation ? group_of (conversation) : NULL;

  for (GtkWidget *chip = gtk_widget_get_first_child (self->chips); group && chip;
       chip = gtk_widget_get_next_sibling (chip))
    if (!g_strcmp0 (group, g_object_get_data (G_OBJECT (chip), "group")))
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chip), TRUE);
}

static void
reload (ConversationList *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(GPtrArray) conversations = store ? sms_store_list_conversations (store, self->archived) : NULL;
  gint64 now = g_get_real_time () / 1000;
  GtkWidget *child;
  GHashTableIter iter;
  gpointer key;
  g_autoptr(GHashTable) unread = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (self->chips)
    sync_chips (self);
  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  g_hash_table_remove_all (self->rows);
  for (guint i = 0; conversations && i < conversations->len; i++)
    {
      SmsConversation *conversation = conversations->pdata[i];
      g_autofree char *group = self->chips ? group_of (conversation) : NULL;

      if (group && !conversation->read)
        g_hash_table_add (unread, g_strdup (group));
      if (!group || !g_strcmp0 (group, self->group))
        gtk_box_append (GTK_BOX (self->list), make_row (self, conversation, now));
    }
  if (self->chips)
    sync_dots (self, unread);
  /* what was selected and is gone stays unselected */
  g_hash_table_iter_init (&iter, self->selected);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (self->rows, key))
      g_hash_table_iter_remove (&iter);
  gtk_widget_set_visible (self->empty, g_hash_table_size (self->rows) == 0);
  gtk_widget_set_visible (self->scroll, g_hash_table_size (self->rows) > 0);
  sync_select (self);
}

static void
on_changed (SmsClient *client, gint64 conversation, gpointer data)
{
  reload (data);
}

static gboolean
on_refresh (gpointer data)
{
  reload (data);
  return G_SOURCE_CONTINUE;
}

/* --- the page */

gboolean
conversation_list_is_at_top (ConversationList *self)
{
  return !selecting (self) && self->at_top;
}

static void
on_scrolled (GObject *scroll, GParamSpec *pspec, gpointer data)
{
  ConversationList *self = data;
  gboolean at_top = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (scroll)) <= 0;

  if (at_top != self->at_top)
    {
      self->at_top = at_top;
      g_signal_emit (self, signals[AT_TOP_CHANGED], 0);
    }
}

static void
show_archived (gpointer data)
{
  g_signal_emit (data, signals[SHOW_ARCHIVED], 0);
}

static void
show_blocked (gpointer data)
{
  g_signal_emit (data, signals[SHOW_BLOCKED], 0);
}

static void
show_settings (gpointer data)
{
  g_signal_emit (data, signals[SHOW_SETTINGS], 0);
}

static void
on_overflow (GtkWidget *button, gpointer data)
{
  ConversationList *self = data;
  DialerPopupMenu *menu = dialer_popup_menu_new ();
  g_auto(GStrv) blocked = blocked_numbers_list (blocked_numbers_get_default ());

  dialer_popup_menu_append (menu, _("Archived"), show_archived, self);
  /* only with a blocked contact */
  if (blocked[0])
    dialer_popup_menu_append (menu, _("Blocked contacts"), show_blocked, self);
  dialer_popup_menu_append (menu, _("Settings"), show_settings, self);
  dialer_popup_menu_set_corner (menu, 4);
  dialer_popup_menu_present (menu, button, POPUP_MENU_OVERFLOW);
}

/* the FAB rides up over the snackbar while it shows */
static void
on_snackbar_lift (GObject *snackbar, GParamSpec *pspec, gpointer data)
{
  ConversationList *self = data;

  gtk_widget_set_margin_bottom (self->fab, FAB_MARGIN + (int) round (neo_snackbar_get_lift (NEO_SNACKBAR (snackbar))));
}

static void
on_fab (ConversationList *self)
{
  g_signal_emit (self, signals[START_CHAT], 0);
}

static void
on_up (ConversationList *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static GtkWidget *
make_fab (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name ("messaging-message-symbolic");
  g_autofree char *caps = g_utf8_strup (_("Start chat"), -1);
  GtkWidget *label = gtk_label_new (caps);
  GtkWidget *fab;
  GdkRGBA fill;

  /* ExtendedFloatingActionButton: ic_message and "START CHAT" (textAllCaps) in black on #b8ebf3, corners 16 dp;
   * padding 12 dp, 20 at the end, 12 between the icon and the text; at least 120 × 48 */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 20);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (label, "msg-fab-label");
  gtk_widget_add_css_class (box, "msg-fab-content");
  fab = neo_pressable_new (box);
  gdk_rgba_parse (&fill, "#b8ebf3");
  neo_pressable_set_fill (NEO_PRESSABLE (fab), &fill, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (fab), NEO_RIPPLE_BOUNDED, 16);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (fab), &(GdkRGBA) { 0, 0, 0, 0.24f });
  gtk_widget_add_css_class (fab, "msg-fab");
  gtk_widget_set_size_request (fab, 120, 48);
  gtk_widget_set_halign (fab, GTK_ALIGN_END);
  gtk_widget_set_valign (fab, GTK_ALIGN_END);
  gtk_widget_set_margin_end (fab, FAB_MARGIN);
  gtk_widget_set_margin_bottom (fab, FAB_MARGIN);
  gtk_accessible_update_property (GTK_ACCESSIBLE (fab), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Start new conversation"),
                                  -1);
  return fab;
}

static GtkWidget *
make_empty (gboolean archived)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image = msg_image_new ("/de/yesman/Messaging/images/ic_oobe_conv_list.png", 198, 103);
  GtkWidget *text = gtk_label_new (archived ? _("Archived conversations appear here")
                                            : _("Once you start a new conversation, you’ll see it listed here"));

  /* ic_oobe_conv_list, 198 × 103 dp, with the text 20 dp under it */
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), image);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (text, EMPTY_TEXT_TOP + EMPTY_TEXT_PAD);
  gtk_widget_set_margin_bottom (text, EMPTY_TEXT_PAD);
  gtk_widget_set_margin_start (text, EMPTY_TEXT_SIDE);
  gtk_widget_set_margin_end (text, EMPTY_TEXT_SIDE);
  gtk_widget_add_css_class (text, "msg-empty-text");
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (box, TRUE);
  return box;
}

static void
conversation_list_dispose (GObject *object)
{
  ConversationList *self = MSG_CONVERSATION_LIST (object);

  if (self->refresh_id)
    g_source_remove (self->refresh_id);
  self->refresh_id = 0;
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_signal_handlers_disconnect_by_data (blocked_numbers_get_default (), self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->chips_key, g_free);
  g_clear_pointer (&self->group, g_free);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_clear_pointer (&self->selected, g_hash_table_unref);
  G_OBJECT_CLASS (conversation_list_parent_class)->dispose (object);
}

static void
conversation_list_class_init (ConversationListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = conversation_list_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-conversation-list");
  signals[OPEN_CONVERSATION] = g_signal_new ("open-conversation", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[START_CHAT] = g_signal_new ("start-chat", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                      NULL, G_TYPE_NONE, 0);
  signals[SHOW_ARCHIVED] = g_signal_new ("show-archived", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[SHOW_BLOCKED] = g_signal_new ("show-blocked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                        NULL, NULL, G_TYPE_NONE, 0);
  signals[SHOW_SETTINGS] = g_signal_new ("show-settings", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 0);
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[AT_TOP_CHANGED] = g_signal_new ("at-top-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
}

static void
conversation_list_init (ConversationList *self)
{
  self->rows = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, row_free);
  self->selected = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  self->at_top = TRUE;
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

static GtkWidget *
build (SmsClient *client, Contacts *contacts, gboolean archived, gboolean forward)
{
  ConversationList *self = g_object_new (MSG_TYPE_CONVERSATION_LIST, NULL);
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *cancel;

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->settings = g_settings_new ("de.yesman.Messaging");
  self->archived = archived;
  self->forward = forward;
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");

  /* "Messaging", with its overflow; the archived list: "Archived" with the up arrow and no menu */
  self->bar = msg_action_bar_new (archived, archived ? _("Archived") : _("Messaging"));
  if (archived)
    g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  else
    g_signal_connect (msg_action_bar_add_overflow (self->bar), "clicked", G_CALLBACK (on_overflow), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);
  /* select mode: the X, no title, Archive or Unarchive, Delete, and Block for one person */
  self->select_bar = msg_action_bar_new (FALSE, "");
  cancel = msg_action_bar_add_close (self->select_bar, _("Done"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (conversation_list_back), self);
  self->archive_action = msg_action_bar_add_action (self->select_bar, "messaging-archive-symbolic", _("Archive"));
  self->unarchive_action = msg_action_bar_add_action (self->select_bar, "messaging-unarchive-symbolic",
                                                      _("Unarchive"));
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "messaging-delete-symbolic", _("Delete")),
                            "clicked", G_CALLBACK (on_delete), self);
  self->add_contact_action = msg_action_bar_add_action (self->select_bar, "messaging-people-add-symbolic",
                                                        _("Add contact"));
  g_signal_connect_swapped (self->add_contact_action, "clicked", G_CALLBACK (on_add_contact), self);
  self->block_action = msg_action_bar_add_action (self->select_bar, "messaging-block-symbolic", _("Block"));
  g_signal_connect_swapped (self->archive_action, "clicked", G_CALLBACK (on_archive), self);
  g_signal_connect_swapped (self->unarchive_action, "clicked", G_CALLBACK (on_unarchive), self);
  g_signal_connect_swapped (self->block_action, "clicked", G_CALLBACK (on_block), self);
  /* blocked here or in the Phone: Block goes away for that person */
  g_signal_connect_swapped (blocked_numbers_get_default (), "changed", G_CALLBACK (sync_select), self);
  gtk_widget_set_visible (self->select_bar, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->select_bar);

  if (!archived && !forward)
    {
      /* the groups, as chips that scroll sideways when there are more than fit */
      GtkWidget *strip = self->strip = gtk_scrolled_window_new ();

      self->chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_add_css_class (self->chips, "msg-chips");
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (strip), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
      gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (strip), TRUE);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (strip), self->chips);
      gtk_widget_set_visible (strip, FALSE);
      gtk_box_append (GTK_BOX (self->box), strip);
    }
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->list, LIST_TOP_PADDING);
  gtk_box_append (GTK_BOX (inner), self->list);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), inner);
  gtk_widget_set_vexpand (self->scroll, TRUE);
  g_signal_connect (self->scroll, "notify::offset", G_CALLBACK (on_scrolled), self);
  self->empty = make_empty (archived);
  gtk_overlay_set_child (GTK_OVERLAY (content), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append (GTK_BOX (gtk_overlay_get_child (GTK_OVERLAY (content))), self->scroll);
  gtk_box_append (GTK_BOX (gtk_overlay_get_child (GTK_OVERLAY (content))), self->empty);
  if (!archived && !forward)
    {
      self->fab = make_fab ();
      g_signal_connect_swapped (self->fab, "clicked", G_CALLBACK (on_fab), self);
      gtk_overlay_add_overlay (GTK_OVERLAY (content), self->fab);
    }
  self->snackbar = msg_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (content), GTK_WIDGET (self->snackbar));
  if (self->fab)
    g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_snackbar_lift), self);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (self->box), content);

  g_signal_connect (client, "changed", G_CALLBACK (on_changed), self);
  /* the network's country and the address book come in after the window opens: numbers and names as they then read
   * (before the country, a number is formatted for the locale's, and its avatar colour changes with it) */
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  g_signal_connect_swapped (self->contacts, "changed", G_CALLBACK (reload), self);
  self->refresh_id = g_timeout_add (REFRESH_MS, on_refresh, self);
  reload (self);
  return GTK_WIDGET (self);
}

GtkWidget *
conversation_list_new (SmsClient *client, Contacts *contacts, gboolean archived)
{
  return build (client, contacts, archived, FALSE);
}

GtkWidget *
conversation_list_new_forward (SmsClient *client, Contacts *contacts)
{
  return build (client, contacts, FALSE, TRUE);
}
