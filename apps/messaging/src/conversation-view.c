/* A conversation (conversation-view.h). */
#include "conversation-view.h"
#include "app-action.h"
#include "msg-action-bar.h"
#include "msg-add-contact.h"
#include "msg-avatar.h"
#include "msg-bubble.h"
#include "msg-dates.h"
#include "msg-dialog.h"
#include "msg-image.h"
#include "msg-markdown.h"
#include "msg-media.h"
#include "msg-people.h"
#include "msg-directory.h"
#include "msg-sound.h"
#include "phone-number.h"
#include "popup-menu.h"
#include "sms-agent.h"
#include "sms-provider.h"
#include "sms-segments.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <neo.h>
#include <spongy-gtk.h>

/* conversation_message_view.xml, dimens.xml */
#define ROW_SIDE_PADDING 12
#define AVATAR 48
#define ARROW_GAP 9
#define TEXT_PAD_SIDE 14
#define TEXT_PAD_TOP 10
#define TEXT_PAD_BOTTOM 12
#define GAP 18                  /* before a message */
#define GAP_CLUSTERED 2         /* before one clustered with the one above */
#define META_TOP 4
#define DELIVERED_START 4       /* SmsDeliverdBadge's margin; ic_sms_delivery_ok is 36 × 26 px, 12 × 8.7 dp */
#define DELIVERED_WIDTH 12
#define DELIVERED_HEIGHT 9
#define CLUSTER_MS (60 * 1000)  /* ConversationMessageData.canClusterWithMessage */
/* compose_message_view.xml */
#define COMPOSE_TOP 16
#define COMPOSE_BUTTON 48
#define FIELD_MARGIN_END 8
#define SEND_PAD_END 12
#define FIELD_MAX_LINES 4
#define COUNTER_THRESHOLD 10    /* CODEPOINTS_REMAINING_BEFORE_COUNTER_SHOWN */
#define REVEAL_MS 300           /* UiUtils.revealOrHideViewWithAnimation */
#define REFRESH_MS (60 * 1000)

struct _ConversationView {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  gint64 id;
  SmsConversation *conversation;
  GPtrArray *people;            /* MsgPerson */

  GtkWidget *box, *bar, *select_bar, *call_action;
  GtkWidget *forward_action, *copy_action, *resend_action, *withdraw_action;
  GtkWidget *scroll, *messages, *shadow;
  GtkWidget *field, *hint, *send, *self_avatar, *counter;
  NeoAnim *reveal;
  NeoSnackbar *snackbar;
  gint64 selected;              /* the message selected, or 0 */
  gint64 newest;                /* the newest message shown, for "New message" */
  gboolean at_bottom;
  gboolean loading;
  guint refresh_id;
  GSettings *settings;
  guint shown;                  /* how many of the newest messages are shown */
  gboolean has_older;           /* the conversation has messages above the ones shown */
  guint older_id;               /* the idle that shows more of them */
  double anchor_upper;          /* > 0: the list's height before older ones came in, to keep what was in sight */
};

G_DEFINE_FINAL_TYPE (ConversationView, conversation_view, GTK_TYPE_WIDGET)

enum { CLOSED, SHOW_OPTIONS, FORWARD, N_SIGNALS };
static guint signals[N_SIGNALS];

static void reload (ConversationView *self);

gint64
conversation_view_get_id (ConversationView *self)
{
  return self->id;
}

static const char *
country (ConversationView *self)
{
  return sms_client_get_country (self->client);
}

static MsgPerson *
person_for (ConversationView *self, const char *destination)
{
  for (guint i = 0; destination && i < self->people->len; i++)
    if (g_strcmp0 (((MsgPerson *) self->people->pdata[i])->destination, destination) == 0)
      return self->people->pdata[i];
  return NULL;
}

/* --- the messages (§2.3, §2.4) */

static gboolean
clusters (const SmsMessage *a, const SmsMessage *b)
{
  gint64 delta;

  if (!a || !b || MESSAGE_STATUS_IS_INCOMING (a->status) != MESSAGE_STATUS_IS_INCOMING (b->status)
      || g_strcmp0 (a->sender, b->sender) != 0)
    return FALSE;
  delta = a->received - b->received;
  return ABS (delta) <= CLUSTER_MS;
}

/* the status line: what is happening to it, else its time (only under a cluster's last) */
static char *
status_text (ConversationView *self, const SmsMessage *message, gboolean last_of_cluster, gboolean *failed)
{
  *failed = FALSE;
  switch (message->status)
    {
    case MESSAGE_STATUS_OUTGOING_YET_TO_SEND:
    case MESSAGE_STATUS_OUTGOING_SENDING:
    case MESSAGE_STATUS_OUTGOING_RESENDING:
      return g_strdup (_("Sending…"));
    case MESSAGE_STATUS_OUTGOING_AWAITING_RETRY:
      return g_strdup (_("Not sent. Trying again…"));
    case MESSAGE_STATUS_OUTGOING_FAILED:
      *failed = TRUE;
      return g_strdup (message->id == self->selected ? _("Resend or delete") : _("Not sent. Tap to try again."));
    case MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER:
      *failed = TRUE;
      return g_strdup (_("Please make a voice call to emergency services. Your text message could not be "
                         "delivered at this time."));
    default:
      break;
    }
  if (!last_of_cluster)
    return NULL;
  else
    {
      g_autofree char *time = msg_dates_message (MESSAGE_STATUS_IS_INCOMING (message->status) ? message->received
                                                                                             : message->sent,
                                                 g_get_real_time () / 1000);
      MsgPerson *sender = MESSAGE_STATUS_IS_INCOMING (message->status) && self->people->len > 1
                          ? person_for (self, message->sender) : NULL;

      /* a group's incoming ones say who */
      return sender ? g_strdup_printf ("%s  %s", sender->name, time) : g_steal_pointer (&time);
    }
}

static void select_message (ConversationView *self, gint64 id);

/* a provider's message tapped: its media opened in the app that shows its type, a place on the map; media that is
 * not on the phone is asked for, and the conversation changes when it is */
static void
open_media (ConversationView *self, const SmsMessage *message)
{
  GtkWindow *window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
  const SmsPart *part = message->part;

  if (msg_media_is_here (part))
    {
      g_autoptr(GFile) file = g_file_new_for_path (part->uri);
      g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new (file);

      gtk_file_launcher_launch (launcher, window, NULL, NULL, NULL);
    }
  else if (part->located || part->url)
    {
      char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
      g_autofree char *geo = part->located ? g_strdup_printf ("geo:%s,%s",
                                                              g_ascii_dtostr (lat, sizeof lat, part->latitude),
                                                              g_ascii_dtostr (lon, sizeof lon, part->longitude)) : NULL;
      g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new (part->url ? part->url : geo);

      gtk_uri_launcher_launch (launcher, window, NULL, NULL, NULL);
    }
  /* only media has a type: a contact's or a poll's message has nothing to download */
  else if (message->sms_uri && part->mime)
    sms_client_fetch_media (self->client, message->id);
}

static void
on_bubble_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationView *self = g_object_get_data (G_OBJECT (pressable), "view");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");
  g_autoptr(SmsMessage) message = NULL;
  SmsStore *store = sms_client_get_store (self->client);

  if (self->selected)
    {
      select_message (self, self->selected == id ? 0 : id);
      return;
    }
  /* "Not sent. Tap to try again." */
  message = store ? sms_store_get_message (store, id) : NULL;
  if (message && message->status == MESSAGE_STATUS_OUTGOING_FAILED)
    sms_client_resend (self->client, id);
  else if (message && message->part)
    open_media (self, message);
}

static void
on_bubble_long_pressed (GtkWidget *pressable, gpointer data)
{
  ConversationView *self = g_object_get_data (G_OBJECT (pressable), "view");

  select_message (self, *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id"));
}

/* the sender's avatar (ContactIconView): their card, or "Add to Contacts?"; a long press is the message's */
static void
on_avatar_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationView *self = g_object_get_data (G_OBJECT (pressable), "view");
  const char *sender = g_object_get_data (G_OBJECT (pressable), "sender");
  g_autoptr(MsgPerson) person = msg_person_new (self->contacts, sender, sms_client_get_country (self->client));

  msg_contact_show_or_add (pressable, person, self->snackbar);
}

/* --- an external agent's message (docs/external-agents-channels.md): its origin is messaging.conf [agents] origin,
 * then the agent's identity; the thread says who sent it, and the owner may withdraw it within the network's window */

/* the agent's identity of an outgoing message, or NULL for the owner's and Minerva's */
static char *
agent_of (const SmsMessage *message)
{
  g_autofree char *prefix = sms_provider_text ("agents", "origin");

  if (!message->origin || !prefix || !*prefix || !g_str_has_prefix (message->origin, prefix))
    return NULL;
  return g_strdup (message->origin + strlen (prefix));
}

/* may the owner still withdraw it: an agent's, sent, and within the provider's revoke_window_s */
static gboolean
withdrawable (ConversationView *self, const SmsMessage *message)
{
  g_autofree char *agent = agent_of (message);
  g_autofree char *provider = NULL, *window = NULL;
  gint64 seconds;

  if (!agent || !self->conversation || !self->conversation->destinations || !self->conversation->destinations[0]
      || (message->status != MESSAGE_STATUS_OUTGOING_COMPLETE && message->status != MESSAGE_STATUS_OUTGOING_DELIVERED))
    return FALSE;
  provider = sms_provider_owner (self->conversation->destinations[0]);
  window = provider ? sms_provider_value (provider, "revoke_window_s") : NULL;
  seconds = window ? g_ascii_strtoll (window, NULL, 10) : 0;
  return seconds > 0 && g_get_real_time () / 1000 - message->sent <= seconds * 1000;
}

static GtkWidget *
make_message (ConversationView *self, const SmsMessage *message, gboolean clustered_above,
              gboolean clustered_below)
{
  gboolean incoming = MESSAGE_STATUS_IS_INCOMING (message->status);
  gboolean selected = message->id == self->selected, failed;
  /* the delivered badge after the time, or alone where a cluster hides the time (ConversationMessageView) */
  gboolean delivered = message->status == MESSAGE_STATUS_OUTGOING_DELIVERED;
  g_autofree char *status = status_text (self, message, !clustered_below, &failed);
  MsgPerson *sender = incoming ? person_for (self, message->sender) : NULL;
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *text = gtk_label_new (NULL);
  g_autofree char *weight = g_settings_get_string (self->settings, "markdown-strong-weight");
  g_autofree char *markup = message->text && g_settings_get_boolean (self->settings, "markdown") ?
                            msg_markdown_to_pango (message->text, weight) : NULL;
  g_auto(GStrv) unlabelled = g_settings_get_strv (self->settings, "media-unlabelled");
  GtkWidget *bubble, *pressable, *preview = NULL;
  GdkRGBA color;

  /* the sender's, or for the ones sent from here "Me": hidden but keeping its place within a cluster */
  if (sender)
    msg_avatar_set_person (MSG_AVATAR (avatar), sender->name, sender->identifier, sender->photo);
  else
    msg_avatar_set_person (MSG_AVATAR (avatar), NULL, incoming ? message->sender : _("Me"), NULL);
  gtk_widget_set_opacity (avatar, clustered_above ? 0 : 1);
  gtk_widget_set_valign (avatar, GTK_ALIGN_START);
  /* an incoming message's, when it shows */
  if (incoming && !clustered_above && message->sender && *message->sender)
    {
      avatar = neo_pressable_new (avatar);
      neo_pressable_set_ripple (NEO_PRESSABLE (avatar), NEO_RIPPLE_NONE, 0);
      gtk_widget_set_valign (avatar, GTK_ALIGN_START);
      g_object_set_data (G_OBJECT (avatar), "view", self);
      g_object_set_data_full (G_OBJECT (avatar), "sender", g_strdup (message->sender), g_free);
      g_object_set_data_full (G_OBJECT (avatar), "id", g_memdup2 (&message->id, sizeof message->id), g_free);
      g_signal_connect (avatar, "clicked", G_CALLBACK (on_avatar_clicked), NULL);
      g_signal_connect (avatar, "long-pressed", G_CALLBACK (on_bubble_long_pressed), NULL);
    }

  if (message->part)
    preview = msg_media_new (message->part, g_settings_get_uint (self->settings, "media-preview-size"));
  /* a picture that says what it is: only its caption */
  if (preview && g_strv_contains ((const char * const *) unlabelled, message->part->kind))
    {
      g_clear_pointer (&markup, g_free);
      gtk_label_set_text (GTK_LABEL (text), message->part->caption ? message->part->caption : "");
      gtk_widget_set_visible (text, message->part->caption != NULL);
    }
  else if (markup)
    gtk_label_set_markup (GTK_LABEL (text), markup);
  else if (message->text)
    gtk_label_set_text (GTK_LABEL (text), message->text);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (text), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_selectable (GTK_LABEL (text), FALSE);
  gtk_widget_add_css_class (text, "msg-message-text");
  if (preview)
    gtk_box_append (GTK_BOX (content), preview);
  gtk_box_append (GTK_BOX (content), text);
  {
    g_autofree char *agent = agent_of (message);
    g_autofree char *template = agent ? sms_provider_text ("agents", "bubble_label") : NULL;

    if (template)
      {
        g_autoptr(GString) said = g_string_new (template);
        GtkWidget *who;

        g_string_replace (said, "{agent}", agent, 0);
        who = gtk_label_new (said->str);
        gtk_label_set_wrap (GTK_LABEL (who), TRUE);
        gtk_label_set_xalign (GTK_LABEL (who), 0);
        gtk_widget_add_css_class (who, "msg-message-status");
        gtk_widget_add_css_class (who, "msg-message-agent");
        gtk_box_append (GTK_BOX (content), who);
      }
  }
  if (status || delivered)
    {
      GtkWidget *meta = NULL;

      if (status)
        {
          meta = gtk_label_new (status);
          gtk_label_set_wrap (GTK_LABEL (meta), TRUE);
          gtk_label_set_xalign (GTK_LABEL (meta), 0);
          gtk_widget_add_css_class (meta, "msg-message-status");
          if (failed)
            gtk_widget_add_css_class (meta, "failed");
        }
      if (delivered)
        {
          /* message_metadata: the status (the time), then the badge centred on its line */
          GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
          GtkWidget *badge = msg_image_new ("/de/yesman/Messaging/images/ic_sms_delivery_ok.png", DELIVERED_WIDTH,
                                            DELIVERED_HEIGHT);

          gtk_widget_set_margin_start (badge, DELIVERED_START);
          gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
          gtk_widget_set_halign (line, GTK_ALIGN_START);
          if (meta)
            gtk_box_append (GTK_BOX (line), meta);
          gtk_box_append (GTK_BOX (line), badge);
          meta = line;
        }
      gtk_widget_set_margin_top (meta, META_TOP);
      gtk_box_append (GTK_BOX (content), meta);
    }
  gtk_widget_set_margin_start (content, TEXT_PAD_SIDE);
  gtk_widget_set_margin_end (content, TEXT_PAD_SIDE);
  gtk_widget_set_margin_top (content, TEXT_PAD_TOP);
  gtk_widget_set_margin_bottom (content, TEXT_PAD_BOTTOM);
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (content, incoming ? "incoming" : "outgoing");
  if (selected)
    gtk_widget_add_css_class (content, "selected");

  bubble = msg_bubble_new (content);
  /* white for the ones sent from here; an incoming one in its contact's colour, else primary (§2.3) */
  if (selected)
    gdk_rgba_parse (&color, "#8dafd1");
  else if (!incoming)
    gdk_rgba_parse (&color, "#ffffff");
  else if (sender && sender->is_contact)
    color = msg_avatar_color (sender->identifier);
  else
    gdk_rgba_parse (&color, "#33666d");
  /* a sticker shown on its own is drawn without a bubble, but a selected one still shows it is selected */
  if (preview && !selected && g_strcmp0 (message->part->kind, "sticker") == 0 && !message->part->caption)
    color.alpha = 0;
  msg_bubble_set_color (MSG_BUBBLE (bubble), &color);
  pressable = neo_pressable_new (bubble);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_NONE, 0);
  g_object_set_data (G_OBJECT (pressable), "view", self);
  /* read out as the text and what its line under it says */
  {
    g_autofree char *said = status ? g_strdup_printf ("%s, %s", message->text ? message->text : "", status)
                                   : g_strdup (message->text ? message->text : "");
    g_autofree char *spoken = delivered ? g_strdup_printf ("%s, %s", said, _("Delivered")) : g_steal_pointer (&said);

    gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, spoken, -1);
  }
  g_object_set_data_full (G_OBJECT (pressable), "id", g_memdup2 (&message->id, sizeof message->id), g_free);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_bubble_clicked), NULL);
  g_signal_connect (pressable, "long-pressed", G_CALLBACK (on_bubble_long_pressed), NULL);
  gtk_widget_set_hexpand (pressable, TRUE);
  gtk_widget_set_halign (pressable, incoming ? GTK_ALIGN_START : GTK_ALIGN_END);

  /* incoming: [avatar] gap [bubble]; outgoing: [bubble] gap [avatar], the other side's avatar width kept free */
  if (incoming)
    {
      gtk_box_append (GTK_BOX (row), avatar);
      gtk_widget_set_margin_start (pressable, ARROW_GAP);
      gtk_widget_set_margin_end (pressable, AVATAR);
      gtk_box_append (GTK_BOX (row), pressable);
    }
  else
    {
      gtk_widget_set_margin_start (pressable, AVATAR);
      gtk_widget_set_margin_end (pressable, ARROW_GAP);
      gtk_box_append (GTK_BOX (row), pressable);
      gtk_box_append (GTK_BOX (row), avatar);
    }
  gtk_widget_set_margin_start (row, ROW_SIDE_PADDING);
  gtk_widget_set_margin_end (row, ROW_SIDE_PADDING);
  gtk_widget_set_margin_top (row, clustered_above ? GAP_CLUSTERED : GAP);
  return row;
}

/* --- select mode (§2.5) */

static SmsMessage *
selected_message (ConversationView *self)
{
  SmsStore *store = sms_client_get_store (self->client);

  return self->selected && store ? sms_store_get_message (store, self->selected) : NULL;
}

static void
select_message (ConversationView *self, gint64 id)
{
  g_autoptr(SmsMessage) message = NULL;

  self->selected = id;
  message = selected_message (self);
  if (!message)
    self->selected = 0;
  gtk_widget_set_visible (self->select_bar, self->selected != 0);
  gtk_widget_set_visible (self->bar, self->selected == 0);
  if (message)
    {
      /* forward: sent, delivered or come whole (getCanForwardMessage), with text to pass on (an SMS is all text);
       * copy: text, and sent or come whole; send: one that failed */
      gtk_widget_set_visible (self->forward_action, message->text && *message->text
                                                    && (message->status == MESSAGE_STATUS_OUTGOING_COMPLETE
                                                        || message->status == MESSAGE_STATUS_OUTGOING_DELIVERED
                                                        || message->status == MESSAGE_STATUS_INCOMING_COMPLETE));
      gtk_widget_set_visible (self->copy_action, message->text && *message->text
                                                 && (!MESSAGE_STATUS_IS_INCOMING (message->status)
                                                     || message->status == MESSAGE_STATUS_INCOMING_COMPLETE));
      gtk_widget_set_visible (self->resend_action, message->status == MESSAGE_STATUS_OUTGOING_FAILED);
      gtk_widget_set_visible (self->withdraw_action, withdrawable (self, message));
    }
  reload (self);
}

gboolean
conversation_view_back (ConversationView *self)
{
  if (!self->selected)
    return FALSE;
  select_message (self, 0);
  return TRUE;
}

static void
on_copy (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);

  if (message)
    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), message->text ? message->text : "");
  select_message (self, 0);
}

/* the text goes on as a draft to the conversation picked (createForwardedMessage: a text part for a text part) */
static void
on_forward (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);
  g_autofree char *text = message && message->text ? g_strdup (message->text) : NULL;

  select_message (self, 0);
  if (text)
    g_signal_emit (self, signals[FORWARD], 0, text);
}

static void
on_resend (ConversationView *self)
{
  gint64 id = self->selected;

  select_message (self, 0);
  sms_client_resend (self->client, id);
}

static void
on_withdrawn (GObject *source, GAsyncResult *res, gpointer data)
{
  ConversationView *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  gint64 id = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (self), "withdrawing"));
  const char *json = NULL;
  gboolean ok = FALSE;

  if (reply)
    {
      g_autoptr(JsonParser) parser = json_parser_new ();
      JsonNode *root;

      g_variant_get (reply, "(&s)", &json);
      if (json && json_parser_load_from_data (parser, json, -1, NULL) && (root = json_parser_get_root (parser))
          && JSON_NODE_HOLDS_OBJECT (root))
        ok = json_object_get_boolean_member_with_default (json_node_get_object (root), "ok", FALSE);
    }
  if (ok)
    sms_client_delete_message (self->client, id);
  else
    msg_snackbar_show (self->snackbar, _("Could not withdraw the message"), NULL, NULL, NULL);
  g_object_unref (self);
}

/* «Ritira»: the provider deletes it on the network (Revoke), then it goes from the thread */
static void
on_withdraw (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);
  g_autofree char *provider = NULL, *name = NULL, *path = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  select_message (self, 0);
  if (!message || !bus || !self->conversation || !(provider = sms_provider_owner (self->conversation->destinations[0])))
    return;
  name = g_strconcat (MSG_PROVIDER_NAME_PREFIX, provider, NULL);
  path = g_strconcat (MSG_PROVIDER_PATH_PREFIX, provider, NULL);
  g_object_set_data (G_OBJECT (self), "withdrawing", GSIZE_TO_POINTER (message->id));
  g_dbus_connection_call (bus, name, path, name, "Revoke", g_variant_new ("(x)", message->id), G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_withdrawn, g_object_ref (self));
}

static void
on_details (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);
  g_autoptr(GString) body = g_string_new (NULL);
  NeoDialog *dialog;

  if (!message)
    return;
  /* MessageDetailsDialog: an SMS's type, who, and when */
  g_string_append_printf (body, "%s%s", _("Type: "), _("Text message"));
  if (MESSAGE_STATUS_IS_INCOMING (message->status))
    {
      MsgPerson *sender = person_for (self, message->sender);
      g_autofree char *sent = msg_dates_details (message->sent);
      g_autofree char *received = msg_dates_details (message->received);

      g_string_append_printf (body, "\n%s%s", _("From: "), sender ? sender->name : message->sender);
      g_string_append_printf (body, "\n%s%s", _("Sent: "), sent);
      g_string_append_printf (body, "\n%s%s", _("Received: "), received);
    }
  else
    {
      g_autofree char *to = msg_people_name (self->people);
      g_autofree char *sent = msg_dates_details (message->sent);

      g_string_append_printf (body, "\n%s%s", _("To: "), to);
      g_string_append_printf (body, "\n%s%s", _("Sent: "), sent);
    }
  dialog = neo_dialog_new (_("Message details"));
  neo_dialog_set_message (dialog, body->str);
  msg_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  neo_dialog_present (dialog, GTK_WIDGET (self));
  select_message (self, 0);
}

static void
on_delete_message_response (NeoDialog *dialog, int response, gpointer data)
{
  ConversationView *self = data;
  gint64 id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "message"));

  if (response == NEO_RESPONSE_OK)
    sms_client_delete_message (self->client, id);
}

static void
on_delete_message (ConversationView *self)
{
  NeoDialog *dialog = neo_dialog_new (_("Delete this message?"));

  neo_dialog_set_message (dialog, _("This action cannot be undone."));
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), NEO_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "message", GINT_TO_POINTER (self->selected));
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_message_response), self);
  select_message (self, 0);
  neo_dialog_present (dialog, GTK_WIDGET (self));
}

/* --- the list */

static gboolean
is_at_bottom (ConversationView *self)
{
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));

  return gtk_adjustment_get_value (adj) >= gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj) - 1;
}

static void
scroll_to_bottom (ConversationView *self)
{
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));

  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

static gboolean
show_older (gpointer data)
{
  ConversationView *self = data;
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));

  self->older_id = 0;
  /* asked for in a layout pass that was not the last: the list is no longer at its top */
  if (gtk_adjustment_get_value (adj) > 0)
    return G_SOURCE_REMOVE;
  self->anchor_upper = gtk_adjustment_get_upper (adj);
  self->shown += g_settings_get_uint (self->settings, "conversation-page");
  g_debug ("conversation %" G_GINT64_FORMAT ": showing its newest %u messages", self->id, self->shown);
  reload (self);
  return G_SOURCE_REMOVE;
}

/* at the top, or all of it in sight: the older ones, after this layout */
static void
maybe_show_older (ConversationView *self, GtkAdjustment *adj)
{
  if (self->has_older && !self->older_id && gtk_adjustment_get_value (adj) <= 0)
    self->older_id = g_idle_add (show_older, self);
}

/* a new size (a message came, the keyboard opened): one at the bottom stays there */
static void
on_adjustment_changed (GtkAdjustment *adj, gpointer data)
{
  ConversationView *self = data;

  if (self->anchor_upper > 0 && gtk_adjustment_get_upper (adj) != self->anchor_upper)
    {
      /* older messages came in above: the ones in sight stay where they were */
      double grown = gtk_adjustment_get_upper (adj) - self->anchor_upper;

      self->anchor_upper = 0;
      if (!self->at_bottom)
        gtk_adjustment_set_value (adj, gtk_adjustment_get_value (adj) + grown);
    }
  if (self->at_bottom)
    scroll_to_bottom (self);
  maybe_show_older (self, adj);
}


static void
on_adjustment_value (GtkAdjustment *adj, gpointer data)
{
  ConversationView *self = data;

  self->at_bottom = is_at_bottom (self);
  maybe_show_older (self, adj);
  /* the shadow above the compose bar shows while the newest message is out of sight */
  gtk_widget_set_opacity (self->shadow, self->at_bottom ? 0 : 1);
}

static void
on_view_new (gpointer data)
{
  ConversationView *self = data;

  self->at_bottom = TRUE;
  scroll_to_bottom (self);
}

static void
update_title (ConversationView *self)
{
  g_autofree char *name = msg_people_name (self->people);
  MsgPerson *only = self->people->len == 1 ? self->people->pdata[0] : NULL;
  gboolean callable = only && only->destination[0] && strspn (only->destination, "+0123456789")
                                                      == strlen (only->destination);

  msg_action_bar_set_title (self->bar, *name ? name : _("Messaging"));
  /* "Make a call": one other person with a phone number (a sender's name or a short code cannot be called) */
  gtk_widget_set_visible (self->call_action, callable && strlen (only->destination) > 6);
}

static void
reload (ConversationView *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(GPtrArray) messages = NULL;
  gint64 newest = self->newest;
  GtkWidget *child;

  g_clear_pointer (&self->conversation, sms_conversation_free);
  self->conversation = store ? sms_store_get (store, self->id) : NULL;
  if (!self->conversation)
    {
      /* deleted (here or elsewhere): the conversation closes */
      if (!self->loading)
        g_signal_emit (self, signals[CLOSED], 0);
      return;
    }
  g_clear_pointer (&self->people, g_ptr_array_unref);
  self->people = msg_people_new (self->contacts, (const char * const *) self->conversation->destinations,
                                 country (self));
  update_title (self);

  /* the newest ones shown, and one more: whether there are older ones, and whether the first shown clusters with it */
  messages = sms_store_list_last_messages (store, self->id, self->shown + 1);
  self->has_older = messages->len > self->shown;
  while ((child = gtk_widget_get_first_child (self->messages)))
    gtk_box_remove (GTK_BOX (self->messages), child);
  for (guint i = self->has_older ? 1 : 0; i < messages->len; i++)
    {
      SmsMessage *message = messages->pdata[i];

      gtk_box_append (GTK_BOX (self->messages),
                      make_message (self, message, i > 0 && clusters (message, messages->pdata[i - 1]),
                                    i + 1 < messages->len && clusters (message, messages->pdata[i + 1])));
    }
  if (messages->len)
    {
      SmsMessage *last = messages->pdata[messages->len - 1];

      self->newest = last->id;
      /* scrolled up when a message came: "New message", VIEW scrolls down to it */
      if (!self->loading && newest && last->id != newest && MESSAGE_STATUS_IS_INCOMING (last->status)
          && !self->at_bottom)
        msg_snackbar_show (self->snackbar, _("New message"), _("View"), on_view_new, self);
    }
}

static void
on_changed (SmsClient *client, gint64 conversation, gpointer data)
{
  ConversationView *self = data;

  if (conversation == 0 || conversation == self->id)
    reload (self);
}

static gboolean
on_refresh (gpointer data)
{
  reload (data);
  return G_SOURCE_CONTINUE;
}

/* --- the compose bar (§2.6) */

static char *
field_text (ConversationView *self)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

static void
on_reveal (NeoAnim *anim, double value, gpointer data)
{
  ConversationView *self = data;

  /* the send button grows from its centre over the "Me" avatar, which shrinks away the same way */
  neo_pressable_set_scale (NEO_PRESSABLE (self->send), value);
  gtk_widget_set_opacity (self->self_avatar, value < 1 ? 1 - value : 0);
  /* shrunk away, it is not there to tap */
  gtk_widget_set_can_target (self->send, value > 0.5);
}

static void
on_text_changed (GtkTextBuffer *buffer, gpointer data)
{
  ConversationView *self = data;
  g_autofree char *text = field_text (self);
  gboolean has_text = *text != '\0';
  SmsLength length = sms_segments_count (text);
  g_autofree char *counter = NULL;

  gtk_widget_set_visible (self->hint, !has_text);
  if (has_text != (neo_anim_get_to (self->reveal) > 0.5))
    neo_anim_start (self->reveal, neo_anim_get_value (self->reveal), has_text ? 1 : 0, REVEAL_MS, 0,
                    SPONGY_FAST_OUT_SLOW_IN);
  /* "62 / 2": shown once it takes more than one message, or the last 10 are left; its line is kept either way. An
   * agent's messages are no SMS: never */
  counter = g_strdup_printf ("%u / %u", length.remaining, length.segments);
  gtk_label_set_text (GTK_LABEL (self->counter), counter);
  gtk_widget_set_opacity (self->counter, has_text && (length.segments > 1 || length.remaining <= COUNTER_THRESHOLD)
                                         && !(self->conversation && sms_agent_is_conversation (
                                              (const char * const *) self->conversation->destinations))
                                         ? 1 : 0);
}

static void
on_send (ConversationView *self)
{
  g_autofree char *text = field_text (self);
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  if (!*stripped || !self->conversation)
    return;
  msg_sound_play_sent ();
  sms_client_send (self->client, self->id, text);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), "", -1);
  self->at_bottom = TRUE;
}

void
conversation_view_set_text (ConversationView *self, const char *text)
{
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), text ? text : "", -1);
}

void
conversation_view_save_draft (ConversationView *self)
{
  g_autofree char *text = field_text (self);
  const char *draft = self->conversation && self->conversation->show_draft ? self->conversation->draft : NULL;

  if (!self->conversation || g_strcmp0 (*text ? text : NULL, draft) == 0)
    return;
  sms_client_set_draft (self->client, self->id, text);
}

static GtkWidget *
make_compose (ConversationView *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *field_box = gtk_overlay_new ();
  GtkWidget *field_scroll = gtk_scrolled_window_new ();
  GtkWidget *right = gtk_overlay_new ();
  GtkWidget *send_icon = gtk_image_new_from_icon_name ("messaging-send-symbolic");
  GdkRGBA send_color;

  /* no attach button: there is no MMS here yet, so nothing it could attach could be sent */
  self->field = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->field), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (self->field), FALSE);
  gtk_text_view_set_input_hints (GTK_TEXT_VIEW (self->field), GTK_INPUT_HINT_UPPERCASE_SENTENCES
                                                              | GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_EMOJI);
  gtk_widget_add_css_class (self->field, "msg-compose-text");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->field), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Send message"),
                                  -1);
  g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), "changed",
                    G_CALLBACK (on_text_changed), self);
  /* at most 4 lines, then it scrolls */
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (field_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (field_scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (field_scroll), FIELD_MAX_LINES * 19 + 26);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (field_scroll), self->field);
  gtk_overlay_set_child (GTK_OVERLAY (field_box), field_scroll);
  self->hint = gtk_label_new (_("Send message"));
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0);
  gtk_widget_set_can_target (self->hint, FALSE);
  gtk_widget_add_css_class (self->hint, "msg-compose-hint");
  gtk_overlay_add_overlay (GTK_OVERLAY (field_box), self->hint);
  gtk_widget_add_css_class (field_box, "msg-compose-field");
  gtk_widget_set_hexpand (field_box, TRUE);
  gtk_widget_set_valign (field_box, GTK_ALIGN_END);
  gtk_widget_set_margin_start (field_box, ROW_SIDE_PADDING);
  gtk_widget_set_margin_end (field_box, FIELD_MARGIN_END);
  gtk_box_append (GTK_BOX (row), field_box);

  /* the right column: the "Me" avatar while the field is empty, else the send button */
  self->self_avatar = msg_avatar_new (COMPOSE_BUTTON);
  msg_avatar_set_person (MSG_AVATAR (self->self_avatar), NULL, _("Me"), NULL);
  gtk_overlay_set_child (GTK_OVERLAY (right), self->self_avatar);
  gtk_image_set_pixel_size (GTK_IMAGE (send_icon), 24);
  self->send = neo_pressable_new (send_icon);
  gtk_widget_set_size_request (self->send, COMPOSE_BUTTON, COMPOSE_BUTTON);
  gdk_rgba_parse (&send_color, "#00383f");
  neo_pressable_set_fill (NEO_PRESSABLE (self->send), &send_color, FALSE);
  neo_pressable_set_ripple (NEO_PRESSABLE (self->send), NEO_RIPPLE_CIRCLE, COMPOSE_BUTTON / 2.0);
  neo_pressable_set_scale (NEO_PRESSABLE (self->send), 0);
  gtk_widget_set_can_target (self->send, FALSE);
  gtk_widget_add_css_class (self->send, "msg-send");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->send), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Send"), -1);
  g_signal_connect_swapped (self->send, "clicked", G_CALLBACK (on_send), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (right), self->send);
  gtk_widget_set_valign (right, GTK_ALIGN_END);
  gtk_widget_set_margin_end (right, SEND_PAD_END);
  gtk_box_append (GTK_BOX (row), right);
  self->reveal = neo_anim_new (self->send, on_reveal, self);

  gtk_widget_set_margin_top (row, COMPOSE_TOP);
  gtk_box_append (GTK_BOX (bar), row);
  /* the counter's line, under the field's end, kept when it shows nothing */
  self->counter = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->counter), 1);
  gtk_widget_set_margin_end (self->counter, COMPOSE_BUTTON + SEND_PAD_END + FIELD_MARGIN_END + 16);
  gtk_widget_add_css_class (self->counter, "msg-counter");
  gtk_widget_set_opacity (self->counter, 0);
  gtk_box_append (GTK_BOX (bar), self->counter);
  return bar;
}

/* --- the action bar (§2.2) */

static void
on_up (ConversationView *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

/* Make a call (launchPhoneCallActivity, Intent.ACTION_CALL): the Phone app calls the number at once (its "call"
 * action); another phone's app gets the tel: link. An emergency number is not handed over (ConversationFragment
 * says so instead). */
static void
on_call (ConversationView *self)
{
  MsgPerson *only = self->people && self->people->len == 1 ? self->people->pdata[0] : NULL;
  g_autofree char *uri = NULL;
  g_autoptr(GError) error = NULL;

  if (!only)
    return;
  if (phone_number_is_emergency (only->destination))
    {
      msg_snackbar_show (self->snackbar, _("Can't make a voice call to emergency services here."), NULL, NULL, NULL);
      return;
    }
  if (app_action_activate (GTK_WIDGET (self), "de.yesman.Dialer", "call", g_variant_new_string (only->destination)))
    return;
  uri = g_strconcat ("tel:", only->destination, NULL);
  if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
    g_warning ("cannot call: %s", error->message);
}

static void
archive (gpointer data)
{
  ConversationView *self = data;

  sms_client_set_archived (self->client, self->id, !self->conversation->archived);
  /* ConversationFragment: archiving leaves the conversation */
  if (!self->conversation->archived)
    g_signal_emit (self, signals[CLOSED], 0);
}

static void
on_delete_response (NeoDialog *dialog, int response, gpointer data)
{
  ConversationView *self = data;

  if (response == NEO_RESPONSE_OK)
    sms_client_delete_conversation (self->client, self->id);
}

static void
delete_conversation (gpointer data)
{
  ConversationView *self = data;
  NeoDialog *dialog = neo_dialog_new (ngettext ("Delete this conversation?", "Delete these conversations?", 1));

  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), self);
  neo_dialog_present (dialog, GTK_WIDGET (self));
}

static MsgPerson *
only_person (ConversationView *self)
{
  return self->people && self->people->len == 1 ? self->people->pdata[0] : NULL;
}

static void
add_contact (gpointer data)
{
  ConversationView *self = data;
  MsgPerson *only = only_person (self);

  if (only)
    msg_contact_confirm_add (GTK_WIDGET (self), only);
}

static void
show_options (gpointer data)
{
  g_signal_emit (data, signals[SHOW_OPTIONS], 0);
}

static void
on_overflow (GtkWidget *button, gpointer data)
{
  ConversationView *self = data;
  DialerPopupMenu *menu = dialer_popup_menu_new ();

  if (!self->conversation)
    return;
  /* the other one of a 1:1, when they are not a contact (ConversationFragment.onCreateOptionsMenu) */
  if (msg_contact_can_add (only_person (self)))
    dialer_popup_menu_append (menu, _("Add contact"), add_contact, self);
  dialer_popup_menu_append (menu, _("People & options"), show_options, self);
  dialer_popup_menu_append (menu, self->conversation->archived ? _("Unarchive") : _("Archive"), archive, self);
  dialer_popup_menu_append (menu, _("Delete"), delete_conversation, self);
  dialer_popup_menu_set_corner (menu, 4);
  dialer_popup_menu_present (menu, button, POPUP_MENU_OVERFLOW);
}

/* --- the page */

static void
conversation_view_dispose (GObject *object)
{
  ConversationView *self = MSG_CONVERSATION_VIEW (object);

  if (self->refresh_id)
    g_source_remove (self->refresh_id);
  self->refresh_id = 0;
  /* taking the list down moves its adjustment: nothing may ask for older messages from here on */
  if (self->scroll)
    g_signal_handlers_disconnect_by_data (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll)), self);
  self->scroll = NULL;
  g_clear_handle_id (&self->older_id, g_source_remove);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_pointer (&self->reveal, neo_anim_free);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->conversation, sms_conversation_free);
  g_clear_pointer (&self->people, g_ptr_array_unref);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (conversation_view_parent_class)->dispose (object);
}

static void
conversation_view_class_init (ConversationViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = conversation_view_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-conversation");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SHOW_OPTIONS] = g_signal_new ("show-options", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
  signals[FORWARD] = g_signal_new ("forward", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
conversation_view_init (ConversationView *self)
{
  self->at_bottom = TRUE;
  /* it opens with the keyboard down (stateHidden): the page has the focus, not the field */
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

GtkWidget *
conversation_view_new (SmsClient *client, Contacts *contacts, gint64 conversation)
{
  ConversationView *self = g_object_new (MSG_TYPE_CONVERSATION_VIEW, NULL);
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *cancel, *title;
  GtkAdjustment *adj;

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->id = conversation;
  self->people = g_ptr_array_new ();
  self->settings = g_settings_new ("de.yesman.Messaging");
  self->shown = g_settings_get_uint (self->settings, "conversation-page");
  g_signal_connect_swapped (self->settings, "changed::markdown", G_CALLBACK (reload), self);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");

  self->bar = msg_action_bar_new (TRUE, _("Messaging"));
  g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  /* the title is a button too, the same as the up arrow */
  title = msg_action_bar_get_title (self->bar);
  {
    GtkGesture *click = gtk_gesture_click_new ();

    g_signal_connect_swapped (click, "released", G_CALLBACK (on_up), self);
    gtk_widget_add_controller (title, GTK_EVENT_CONTROLLER (click));
  }
  self->call_action = msg_action_bar_add_action (self->bar, "messaging-phone-symbolic", _("Make a call"));
  g_signal_connect_swapped (self->call_action, "clicked", G_CALLBACK (on_call), self);
  g_signal_connect (msg_action_bar_add_overflow (self->bar), "clicked", G_CALLBACK (on_overflow), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);

  self->select_bar = msg_action_bar_new (FALSE, "");
  cancel = msg_action_bar_add_close (self->select_bar, _("Done"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (conversation_view_back), self);
  /* conversation_fragment_select_menu's order, less Share (no share sheet here) and Save/Download (no MMS) */
  self->forward_action = msg_action_bar_add_action (self->select_bar, "messaging-forward-symbolic", _("Forward"));
  g_signal_connect_swapped (self->forward_action, "clicked", G_CALLBACK (on_forward), self);
  self->copy_action = msg_action_bar_add_action (self->select_bar, "messaging-copy-symbolic", _("Copy text"));
  g_signal_connect_swapped (self->copy_action, "clicked", G_CALLBACK (on_copy), self);
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "messaging-info-symbolic",
                                                       _("View details")), "clicked", G_CALLBACK (on_details), self);
  {
    g_autofree char *withdraw = sms_provider_text ("agents", "withdraw_label");

    self->withdraw_action = msg_action_bar_add_action (self->select_bar, "messaging-cancel-symbolic",
                                                       withdraw ? withdraw : _("Withdraw"));
    g_signal_connect_swapped (self->withdraw_action, "clicked", G_CALLBACK (on_withdraw), self);
    gtk_widget_set_visible (self->withdraw_action, FALSE);
  }
  self->resend_action = msg_action_bar_add_action (self->select_bar, "messaging-send-symbolic", _("Send"));
  g_signal_connect_swapped (self->resend_action, "clicked", G_CALLBACK (on_resend), self);
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "messaging-delete-symbolic", _("Delete")),
                            "clicked", G_CALLBACK (on_delete_message), self);
  gtk_widget_set_visible (self->select_bar, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->select_bar);

  /* the messages, at the bottom while they are fewer than a screen (stackFromEnd) */
  self->messages = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->messages, GTK_ALIGN_END);
  gtk_widget_set_vexpand (self->messages, TRUE);
  gtk_box_append (GTK_BOX (inner), self->messages);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), inner);
  gtk_widget_set_vexpand (self->scroll, TRUE);
  adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));
  g_signal_connect (adj, "changed", G_CALLBACK (on_adjustment_changed), self);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adjustment_value), self);
  /* the snackbar over the list's foot, above the compose bar (its anchor) */
  gtk_overlay_set_child (GTK_OVERLAY (content), self->scroll);
  self->snackbar = msg_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (content), GTK_WIDGET (self->snackbar));
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (column), content);
  /* the 4 dp shadow over the list's foot */
  self->shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->shadow, "msg-compose-shadow");
  gtk_widget_set_opacity (self->shadow, 0);
  gtk_box_append (GTK_BOX (column), self->shadow);
  gtk_box_append (GTK_BOX (column), make_compose (self));
  gtk_widget_set_vexpand (column, TRUE);
  gtk_box_append (GTK_BOX (self->box), column);

  g_signal_connect (client, "changed", G_CALLBACK (on_changed), self);
  /* the network's country and the address book can come in after the conversation opens (a notification's tap) */
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  g_signal_connect_swapped (self->contacts, "changed", G_CALLBACK (reload), self);
  /* a provider names a chat later, or renames a group: the titles read again */
  g_signal_connect_object (msg_directory_get_default (), "changed", G_CALLBACK (reload), self, G_CONNECT_SWAPPED);
  self->refresh_id = g_timeout_add (REFRESH_MS, on_refresh, self);
  self->loading = TRUE;
  reload (self);
  self->loading = FALSE;
  /* the draft comes back into the field */
  if (self->conversation && self->conversation->show_draft && self->conversation->draft)
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), self->conversation->draft,
                              -1);
  return GTK_WIDGET (self);
}
