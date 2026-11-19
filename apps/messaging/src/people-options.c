/* People & options (people-options.h). */
#include "people-options.h"
#include "blocked-numbers.h"
#include "msg-add-contact.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-dialog.h"
#include "msg-people.h"
#include "msg-directory.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* people_options_item_view.xml, people_list_item_view.xml, person_item_view.xml, dimens.xml */
#define OPTION_HEIGHT 56
#define PAD 16
#define PERSON_PAD_VERTICAL 8
#define AVATAR 42
#define TEXT_PAD_START 12
#define TEXT_PAD_END 24

struct _PeopleOptions {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  gint64 id;
  gboolean had_conversation;

  GtkWidget *box, *list;
  NeoSnackbar *snackbar;
};

G_DEFINE_FINAL_TYPE (PeopleOptions, people_options, GTK_TYPE_WIDGET)

enum { CLOSED, BLOCKED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
country (PeopleOptions *self)
{
  return sms_client_get_country (self->client);
}

static GtkWidget *
text_label (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

/* a section's header: 16 sp grey with 16 dp all round; the second has a 1 dp line above it */
static GtkWidget *
header (const char *text, gboolean divider)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = text_label (text, "msg-options-header");

  if (divider)
    {
      GtkWidget *line = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

      gtk_widget_add_css_class (line, "msg-options-divider");
      gtk_box_append (GTK_BOX (box), line);
    }
  gtk_widget_set_margin_start (label, PAD);
  gtk_widget_set_margin_end (label, PAD);
  gtk_widget_set_margin_top (label, PAD);
  gtk_widget_set_margin_bottom (label, PAD);
  gtk_box_append (GTK_BOX (box), label);
  return box;
}

/* --- Block / Unblock */

static void
on_block_response (NeoDialog *dialog, int response, gpointer data)
{
  PeopleOptions *self = data;
  const char *destination = g_object_get_data (G_OBJECT (dialog), "destination");
  g_autoptr(GError) error = NULL;

  if (response != NEO_RESPONSE_OK)
    return;
  if (!blocked_numbers_add (blocked_numbers_get_default (), destination, country (self), &error))
    {
      g_warning ("messaging: %s", error->message);
      return;
    }
  sms_client_set_archived (self->client, self->id, TRUE);
  g_signal_emit (self, signals[BLOCKED], 0);
}

static void
on_block_clicked (GtkWidget *row, gpointer data)
{
  PeopleOptions *self = data;
  const char *destination = g_object_get_data (G_OBJECT (row), "destination");
  g_autoptr(GError) error = NULL;
  g_autofree char *shown = NULL, *title = NULL;
  NeoDialog *dialog;

  /* Unblock is at once, and the conversation comes back out of the archive */
  if (blocked_numbers_contains (blocked_numbers_get_default (), destination, country (self)))
    {
      if (!blocked_numbers_remove (blocked_numbers_get_default (), destination, country (self), &error))
        g_warning ("messaging: %s", error->message);
      else
        sms_client_set_archived (self->client, self->id, FALSE);
      return;
    }
  shown = phone_number_format (destination, country (self));
  title = g_strdup_printf (_("Block %s?"), shown);
  dialog = neo_dialog_new (title);
  neo_dialog_set_message (dialog, _("You'll continue to receive messages from this number but won't be notified "
                                    "anymore. This conversation will be archived."));
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "destination", g_strdup (destination), g_free);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_block_response), self, 0);
  neo_dialog_present (dialog, row);
}

static GtkWidget *
block_row (PeopleOptions *self, const char *destination)
{
  g_autofree char *shown = phone_number_format (destination, country (self));
  g_autofree char *text = g_strdup_printf (blocked_numbers_contains (blocked_numbers_get_default (), destination,
                                                                     country (self)) ? _("Unblock %s")
                                                                                     : _("Block %s"), shown);
  GtkWidget *title = text_label (text, "msg-options-title");
  GtkWidget *row;

  gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (title, PAD);
  gtk_widget_set_margin_end (title, PAD);
  row = neo_pressable_new (title);
  gtk_widget_set_size_request (row, -1, OPTION_HEIGHT);
  g_object_set_data_full (G_OBJECT (row), "destination", g_strdup (destination), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_block_clicked), self);
  return row;
}

/* --- the people */

static void
on_person_clicked (GtkWidget *row, gpointer data)
{
  PeopleOptions *self = data;

  /* PersonItemView: the contact's card, or "Add to Contacts?" (ContactUtil.showOrAddContact) */
  msg_contact_show_or_add (row, g_object_get_data (G_OBJECT (row), "person"), self->snackbar);
}

static void
on_copy_response (NeoDialog *dialog, int response, gpointer data)
{
  if (response == NEO_RESPONSE_OK)
    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (data)),
                            g_object_get_data (G_OBJECT (dialog), "number"));
}

/* "Copy to clipboard?" with the number, Cancel and Copy */
static void
on_person_long_pressed (GtkWidget *row, gpointer data)
{
  PeopleOptions *self = data;
  const char *number = g_object_get_data (G_OBJECT (row), "number");
  NeoDialog *dialog = neo_dialog_new (_("Copy to clipboard?"));

  neo_dialog_set_message (dialog, number);
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Copy"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "number", g_strdup (number), g_free);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_copy_response), self, 0);
  neo_dialog_present (dialog, row);
}

static GtkWidget *
person_row (PeopleOptions *self, MsgPerson *person)
{
  g_autofree char *number = phone_number_format (person->destination, country (self));
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row;

  msg_avatar_set_person (MSG_AVATAR (avatar), person->name, person->identifier, person->photo);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), avatar);
  gtk_box_append (GTK_BOX (column), text_label (person->name, "msg-person-name"));
  /* the number under a contact's name; someone not in the contacts is named by the number already */
  if (person->is_contact)
    {
      GtkWidget *label = text_label (number, "msg-person-number");

      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_NONE);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_lines (GTK_LABEL (label), 2);
      gtk_box_append (GTK_BOX (column), label);
    }
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_margin_start (column, TEXT_PAD_START);
  gtk_widget_set_margin_end (column, TEXT_PAD_END);
  gtk_box_append (GTK_BOX (content), column);
  gtk_widget_set_margin_start (content, PAD);
  gtk_widget_set_margin_top (content, PERSON_PAD_VERTICAL);
  gtk_widget_set_margin_bottom (content, PERSON_PAD_VERTICAL);

  row = neo_pressable_new (content);
  g_object_set_data_full (G_OBJECT (row), "number", g_strdup (number), g_free);
  g_object_set_data_full (G_OBJECT (row), "person", msg_person_copy (person), (GDestroyNotify) msg_person_free);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, person->name, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_person_clicked), self);
  g_signal_connect (row, "long-pressed", G_CALLBACK (on_person_long_pressed), self);
  return row;
}

/* --- the page */

static void
reload (PeopleOptions *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(SmsConversation) conversation = store ? sms_store_get (store, self->id) : NULL;
  g_autoptr(GPtrArray) people = NULL;
  GtkWidget *child;

  if (!conversation)
    {
      /* deleted while this was open (here or elsewhere) */
      if (self->had_conversation)
        g_signal_emit (self, signals[CLOSED], 0);
      return;
    }
  self->had_conversation = TRUE;
  people = msg_people_new (self->contacts, (const char * const *) conversation->destinations, country (self));
  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  gtk_box_append (GTK_BOX (self->list), header (_("General"), FALSE));
  if (people->len == 1)
    gtk_box_append (GTK_BOX (self->list), block_row (self, ((MsgPerson *) people->pdata[0])->destination));
  gtk_box_append (GTK_BOX (self->list), header (_("People in this conversation"), TRUE));
  for (guint i = 0; i < people->len; i++)
    gtk_box_append (GTK_BOX (self->list), person_row (self, people->pdata[i]));
}

static void
on_changed (SmsClient *client, gint64 conversation, gpointer data)
{
  PeopleOptions *self = data;

  if (conversation == 0 || conversation == self->id)
    reload (self);
}

static void
on_up (PeopleOptions *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
people_options_dispose (GObject *object)
{
  PeopleOptions *self = MSG_PEOPLE_OPTIONS (object);

  g_signal_handlers_disconnect_by_data (blocked_numbers_get_default (), self);
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (people_options_parent_class)->dispose (object);
}

static void
people_options_class_init (PeopleOptionsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = people_options_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-people-options");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[BLOCKED] = g_signal_new ("blocked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
people_options_init (PeopleOptions *self)
{
}

GtkWidget *
people_options_new (SmsClient *client, Contacts *contacts, gint64 conversation)
{
  PeopleOptions *self = g_object_new (MSG_TYPE_PEOPLE_OPTIONS, NULL);
  GtkWidget *bar = msg_action_bar_new (TRUE, _("People & options"));
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *scroll = spongy_scroll_view_new ();

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->id = conversation;
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");
  g_signal_connect_swapped (msg_action_bar_get_up (bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (self->box), bar);

  /* a ListView with no dividers but the one over the people */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), self->list);
  gtk_overlay_set_child (GTK_OVERLAY (content), scroll);
  gtk_widget_add_css_class (content, "msg-people-options");
  self->snackbar = msg_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (content), GTK_WIDGET (self->snackbar));
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (self->box), content);

  g_signal_connect (client, "changed", G_CALLBACK (on_changed), self);
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (reload), self);
  /* a provider names a chat later, or renames a group: the titles read again */
  g_signal_connect_object (msg_directory_get_default (), "changed", G_CALLBACK (reload), self, G_CONNECT_SWAPPED);
  /* blocked or unblocked here, in the list, or in the Phone: the row says which it is now */
  g_signal_connect_swapped (blocked_numbers_get_default (), "changed", G_CALLBACK (reload), self);
  reload (self);
  return GTK_WIDGET (self);
}
