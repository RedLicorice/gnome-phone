/* QuickContact's Minerva section (ct-minerva.h). */
#include "ct-minerva.h"
#include "ct-contact.h"
#include "ct-dialogs.h"
#include "ct-mnv-core.h"
#include "ct-mnv.h"
#include "ct-page.h"
#include "ct-store.h"
#include "ct-ui.h"
#include "ns-mc.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <neo.h>

typedef struct {
  GtkWidget *card, *window, *busy, *title, *body;
  char *uid, *name;             /* the contact */
  guint32 tint;                 /* the card title's colour */
  JsonObject *person;           /* `people by-contact` (a ref), NULL when nobody is linked */
  JsonObject *voice;            /* `people voice-status` (a ref), NULL until it answered */
  char *text;                   /* the notebook */
  gint64 max_samples;           /* the rule of `people known`, 0 until it answered */
  char *synced;                 /* the contact name already handed to the archive: no second try on the same one */
  char *shown;                  /* what the body shows now, not to build the same again (and lose the scroll) */
  char *op_busy;                /* the busy line of the running action (a key of the data), NULL when none */
  int ops;                      /* actions running */
  gboolean loading, again;      /* a load running, and another wanted after it */
  guint poll;
} M;

typedef struct {
  M *m;
  char *a, *b;                  /* what the dialog's answer needs: ids, names */
  GPtrArray *ids, *names;       /* the rows of the dialog: an id and a name each */
  const char *done;             /* the key of the text that says it was done */
  const char *busy;             /* the key of the busy line */
} Ask;

static void load (M *m);

/* ---- the data ---- */

static const char *
person_str (M *m, const char *member)
{
  return ns_json_str (m->person, member);
}

static gint64
person_int (M *m, const char *member)
{
  return m->person ? json_object_get_int_member_with_default (m->person, member, 0) : 0;
}

static gboolean
person_flag (M *m, const char *member)
{
  return m->person && json_object_get_boolean_member_with_default (m->person, member, FALSE);
}

static gboolean
voice_flag (M *m, const char *member)
{
  return m->voice && json_object_get_boolean_member_with_default (m->voice, member, FALSE);
}

static gint64
voice_int (JsonObject *o, const char *member)
{
  return o ? json_object_get_int_member_with_default (o, member, 0) : 0;
}

static char *
number (gint64 n)
{
  return g_strdup_printf ("%" G_GINT64_FORMAT, n);
}

static char *
object_text (JsonObject *o)
{
  g_autoptr (JsonNode) node = o ? json_node_init_object (json_node_alloc (), o) : NULL;

  return node ? json_to_string (node, FALSE) : g_strdup ("");
}

static void
toast (M *m, const char *text)
{
  if (text && *text)
    ct_window_toast (m->window, text);
}

static gboolean
learning (M *m)
{
  JsonObject *learn = m->person ? ns_json_obj (m->person, "voice_learn") : NULL;

  return learn && json_object_get_boolean_member_with_default (learn, "enabled", FALSE);
}

static gboolean
enrolling_here (M *m)
{
  return voice_flag (m, "enrolling") && !g_strcmp0 (ns_json_str (m->voice, "person"), person_str (m, "id"));
}

/* ---- dialogs' state ---- */

static Ask *
ask_new (M *m, const char *a, const char *b)
{
  Ask *ask = g_new0 (Ask, 1);

  ask->m = m;
  ask->a = g_strdup (a);
  ask->b = g_strdup (b);
  g_object_ref (m->card);       /* until the dialog answers */
  return ask;
}

static void
ask_free (Ask *ask)
{
  GtkWidget *card = ask->m->card;

  g_free (ask->a);
  g_free (ask->b);
  g_clear_pointer (&ask->ids, g_ptr_array_unref);
  g_clear_pointer (&ask->names, g_ptr_array_unref);
  g_free (ask);
  g_object_unref (card);        /* last: it may be the card's end */
}

/* ---- the busy line ---- */

static void
busy_update (M *m)
{
  const char *what = NULL;
  g_autofree char *text = NULL;

  if (m->ops > 0 && m->op_busy)
    what = ct_mnv_text (m->op_busy);
  else if (enrolling_here (m))
    {
      g_autofree char *got = number (voice_int (m->voice, "got")), *need = number (voice_int (m->voice, "need"));
      const char *const pairs[] = { "got", got, "need", need, "name", m->name ? m->name : "", NULL };

      text = ct_mnv_textf ("voice_enrolling", pairs);
      what = text;
    }
  if (what)
    {
      ct_busy_start (m->busy, what, ct_mnv_text ("busy_slow"), (guint) ct_mnv_int ("busy_slow_s"));
      /* the card is long and the line is at its top: the toast says it where the person is looking */
      if (m->ops > 0)
        ct_window_toast_hold (m->window, what);
    }
  else
    ct_busy_stop (m->busy);
}

/* an action's answer: stop its busy line, say what was done (or why not), and look again */
static void
op_done (JsonObject *root, const char *err, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  m->ops--;
  if (m->ops <= 0)
    g_clear_pointer (&m->op_busy, g_free);
  toast (m, err ? err : ct_mnv_text (ask->done));
  busy_update (m);
  g_clear_pointer (&m->shown, g_free);
  load (m);
  ask_free (ask);
}

/* the action is about to start: its busy line shows at once */
static Ask *
op_begin (M *m, const char *busy, const char *done)
{
  Ask *ask = ask_new (m, NULL, NULL);

  ask->busy = busy;
  ask->done = done;
  m->ops++;
  g_free (m->op_busy);
  m->op_busy = g_strdup (busy);
  busy_update (m);
  return ask;
}

/* ---- widgets ---- */

static GtkWidget *
text_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = ct_label (text, css_class, size);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_hexpand (label, TRUE);
  return label;
}

static void
pad (GtkWidget *widget)
{
  gtk_widget_set_margin_start (widget, (int) CT_QC_ENTRY_PAD_START_DP);
  gtk_widget_set_margin_end (widget, (int) CT_QC_ENTRY_PAD_END_DP);
}

static void
heading (M *m, const char *text)
{
  GtkWidget *label = ct_label (text, "ct-qc-minerva-heading", CT_QC_SUB_SP);

  gtk_widget_set_margin_start (label, (int) CT_QC_ENTRY_PAD_START_DP);
  gtk_box_append (GTK_BOX (m->body), label);
}

/* a row of the section: a header and a line under it, a tap on it does @clicked (a row that does nothing, or that is
 * not offered now, is dim and does not take the tap) */
static GtkWidget *
entry_row (M *m, const char *title, const char *sub, GCallback clicked, gboolean enabled)
{
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = neo_pressable_new (lines);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_FRAMEWORK);

  gtk_box_append (GTK_BOX (lines), text_label (title, "ct-qc-entry-header", CT_QC_HEADER_SP));
  if (sub && *sub)
    gtk_box_append (GTK_BOX (lines), text_label (sub, "ct-qc-entry-sub", CT_QC_SUB_SP));
  pad (lines);
  gtk_widget_set_margin_top (lines, (int) CT_QC_ENTRY_PAD_TOP_DP);
  gtk_widget_set_margin_bottom (lines, (int) CT_QC_ENTRY_PAD_BOTTOM_DP);
  gtk_widget_add_css_class (row, "ct-qc-entry");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  if (clicked && enabled)
    g_signal_connect (row, "clicked", clicked, m);
  else
    {
      gtk_widget_set_can_target (row, FALSE);
      gtk_widget_add_css_class (row, "ct-qc-entry-off");
    }
  gtk_box_append (GTK_BOX (m->body), row);
  return row;
}

static GtkWidget *
action_button (const char *label, GCallback clicked, M *m)
{
  GtkWidget *button = ct_text_button (label, "ct-qc-minerva-action");

  g_signal_connect (button, "clicked", clicked, m);
  return button;
}

/* ---- Modifica: the notebook in Neo Prompt, on this person's document ---- */

static void
on_edit (GtkWidget *button, gpointer data)
{
  M *m = data;

  if (m->person)
    ns_prompt_open ("notebook", person_str (m, "id"));
}

/* ---- Svuota ---- */

static void
clear_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_clear", "cleared");

      ct_mnv_notebook_clear (G_OBJECT (m->card), person_str (m, "id"), op_done, op);
    }
  ask_free (ask);
}

static void
on_clear (GtkWidget *button, gpointer data)
{
  M *m = data;
  const char *const pairs[] = { "name", m->name ? m->name : "", NULL };
  g_autofree char *body = ct_mnv_textf ("clear_body", pairs);

  ct_confirm_dialog (m->window, ct_mnv_text ("clear_heading"), body, ct_mnv_text ("clear"), clear_confirmed,
                     ask_new (m, NULL, NULL));
}

/* ---- Crea il notebook, Collega a una persona ---- */

static void
created (JsonObject *root, const char *err, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (err)
    {
      op_done (NULL, err, ask);
      return;
    }
  /* the second step of one action: the busy line stays */
  ct_mnv_link (G_OBJECT (m->card), ns_json_str (root, "id"), m->uid, op_done, ask);
}

static void
on_create (GtkWidget *row, gpointer data)
{
  M *m = data;
  Ask *ask;

  if (!m->name || !*m->name)
    {
      toast (m, ct_mnv_text ("name_missing"));
      return;
    }
  ask = op_begin (m, "busy_create", "created");
  ct_mnv_create (G_OBJECT (m->card), m->name, created, ask);
}

static void
picked_link (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (ask->ids && response >= 0 && (guint) response < ask->ids->len)
    {
      Ask *op = op_begin (m, "busy_link", "linked");

      ct_mnv_link (G_OBJECT (m->card), ask->ids->pdata[response], m->uid, op_done, op);
    }
  ask_free (ask);
}

/* the archive's entries that are not contacts, as rows of a dialog: "name • kind" */
static GPtrArray *
loose_labels (JsonObject *known, GPtrArray **ids, GPtrArray **names)
{
  g_autoptr (GPtrArray) loose = ct_mnv_loose (known);
  GPtrArray *labels = g_ptr_array_new_with_free_func (g_free);

  *ids = g_ptr_array_new_with_free_func (g_free);
  *names = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < loose->len; i++)
    {
      JsonObject *o = loose->pdata[i];
      const char *kind = ct_mnv_map ("kind_one", ns_json_str (o, "kind"));

      g_ptr_array_add (*ids, g_strdup (ns_json_str (o, "id")));
      g_ptr_array_add (*names, g_strdup (ns_json_str (o, "name")));
      g_ptr_array_add (labels, *kind ? g_strdup_printf ("%s • %s", ns_json_str (o, "name"), kind)
                                     : g_strdup (ns_json_str (o, "name")));
    }
  return labels;
}

static void
known_for_link (JsonObject *root, const char *err, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;
  g_autoptr (GPtrArray) labels = NULL;

  if (err)
    {
      toast (m, err);
      ask_free (ask);
      return;
    }
  labels = loose_labels (root, &ask->ids, &ask->names);
  if (!labels->len)
    {
      toast (m, ct_mnv_text ("loose_empty"));
      ask_free (ask);
      return;
    }
  ct_list_dialog (m->window, CT_DIALOG_APPCOMPAT, ct_mnv_text ("pick_title"), (const char *const *) labels->pdata,
                  (int) labels->len, TRUE, picked_link, ask);
}

static void
on_link (GtkWidget *row, gpointer data)
{
  M *m = data;

  ct_mnv_known (G_OBJECT (m->card), known_for_link, ask_new (m, NULL, NULL));
}

/* ---- Unisci: a stranger's record into this person ---- */

static void
merge_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_merge", "merged");

      ct_mnv_merge (G_OBJECT (m->card), ask->a, person_str (m, "id"), op_done, op);
    }
  ask_free (ask);
}

static void
picked_merge (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (ask->ids && response >= 0 && (guint) response < ask->ids->len)
    {
      Ask *next = ask_new (m, ask->ids->pdata[response], NULL);
      g_autofree char *heading_text = NULL, *body = NULL;
      const char *other = ask->names->pdata[response];
      const char *const pairs[] = { "other", other, "name", m->name ? m->name : "", NULL };

      heading_text = ct_mnv_textf ("merge_heading", pairs);
      body = ct_mnv_textf ("merge_body", pairs);
      ct_confirm_dialog (m->window, heading_text, body, ct_mnv_text ("merge_ok"), merge_confirmed, next);
    }
  ask_free (ask);
}

static void
known_for_merge (JsonObject *root, const char *err, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;
  g_autoptr (GPtrArray) labels = NULL;
  g_autofree char *title = NULL;

  if (err)
    {
      toast (m, err);
      ask_free (ask);
      return;
    }
  labels = loose_labels (root, &ask->ids, &ask->names);
  if (!labels->len)
    {
      toast (m, ct_mnv_text ("loose_empty"));
      ask_free (ask);
      return;
    }
  title = ct_mnv_textf ("merge_title", (const char *const[]) { "name", m->name ? m->name : "", NULL });
  ct_list_dialog (m->window, CT_DIALOG_APPCOMPAT, title, (const char *const *) labels->pdata, (int) labels->len, TRUE,
                  picked_merge, ask);
}

static void
on_merge (GtkWidget *row, gpointer data)
{
  M *m = data;

  ct_mnv_known (G_OBJECT (m->card), known_for_merge, ask_new (m, NULL, NULL));
}

/* ---- Tipo ---- */

static void
kind_picked (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (ask->ids && response >= 0 && (guint) response < ask->ids->len)
    {
      Ask *op = op_begin (m, "busy_kind", "kind_changed");

      ct_mnv_set_kind (G_OBJECT (m->card), person_str (m, "id"), ask->ids->pdata[response], op_done, op);
    }
  ask_free (ask);
}

static void
known_for_kind (JsonObject *root, const char *err, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;
  JsonArray *kinds = err ? NULL : ns_json_arr (root, "kinds");
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  int selected = -1;

  if (err || !kinds)
    {
      toast (m, err ? err : ct_mnv_text ("error"));
      ask_free (ask);
      return;
    }
  ask->ids = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < json_array_get_length (kinds); i++)
    {
      JsonObject *o = json_array_get_object_element (kinds, i);
      const char *id = ns_json_str (o, "id"), *label = ct_mnv_map ("kind_one", id);

      if (!*id)
        continue;
      if (!g_strcmp0 (id, person_str (m, "kind")))
        selected = (int) ask->ids->len;
      g_ptr_array_add (ask->ids, g_strdup (id));
      g_ptr_array_add (labels, g_strdup (*label ? label : id));
    }
  ct_radio_dialog (m->window, ct_mnv_text ("kind_row"), (const char *const *) labels->pdata, (int) labels->len,
                   selected, kind_picked, ask);
}

static void
on_kind (GtkWidget *row, gpointer data)
{
  M *m = data;

  ct_mnv_known (G_OBJECT (m->card), known_for_kind, ask_new (m, NULL, NULL));
}

/* ---- Voce ---- */

static void
enroll_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_voice", "voice_started");

      ct_mnv_voice_enroll (G_OBJECT (m->card), person_str (m, "id"), ask->a, op_done, op);
    }
  ask_free (ask);
}

static void
on_enroll (GtkWidget *row, gpointer data)
{
  M *m = data;
  const char *const pairs[] = { "name", m->name ? m->name : "", NULL };
  g_autofree char *body = ct_mnv_textf ("consent_body", pairs);

  /* the consent's own words are what the archive keeps as the consent */
  ct_confirm_dialog (m->window, ct_mnv_text ("consent_heading"), body, ct_mnv_text ("consent_ok"), enroll_confirmed,
                     ask_new (m, body, NULL));
}

static void
voice_forget_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_voice", "voice_forgotten");

      ct_mnv_voice_forget (G_OBJECT (m->card), person_str (m, "id"), op_done, op);
    }
  ask_free (ask);
}

static void
on_voice_forget (GtkWidget *row, gpointer data)
{
  M *m = data;
  const char *const pairs[] = { "name", m->name ? m->name : "", NULL };
  g_autofree char *body = ct_mnv_textf ("voice_forget_body", pairs);

  ct_confirm_dialog (m->window, ct_mnv_text ("voice_forget_heading"), body, ct_mnv_text ("voice_forget"),
                     voice_forget_confirmed, ask_new (m, NULL, NULL));
}

static void
learn_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_voice", "learn_enabled");

      ct_mnv_voice_learn (G_OBJECT (m->card), person_str (m, "id"), TRUE, ask->a, op_done, op);
    }
  ask_free (ask);
}

static void
on_learn (GtkWidget *row, gpointer data)
{
  M *m = data;
  const char *const pairs[] = { "name", m->name ? m->name : "", NULL };

  if (learning (m))
    {
      Ask *op = op_begin (m, "busy_voice", "learn_disabled");

      ct_mnv_voice_learn (G_OBJECT (m->card), person_str (m, "id"), FALSE, NULL, op_done, op);
    }
  else
    {
      g_autofree char *heading_text = ct_mnv_textf ("learn_consent_heading", pairs);
      g_autofree char *body = ct_mnv_textf ("learn_consent_body", pairs);

      ct_confirm_dialog (m->window, heading_text, body, ct_mnv_text ("learn_consent_ok"), learn_confirmed,
                         ask_new (m, body, NULL));
    }
}

static void
clips_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_voice", "clips_forgotten");

      ct_mnv_clips_forget (G_OBJECT (m->card), person_str (m, "id"), op_done, op);
    }
  ask_free (ask);
}

static void
on_clips (GtkWidget *row, gpointer data)
{
  M *m = data;
  JsonObject *learn = m->voice ? ns_json_obj (m->voice, "learn") : NULL;
  g_autofree char *clips = number (voice_int (learn, "learned"));
  const char *const pairs[] = { "name", m->name ? m->name : "", "clips", clips, NULL };
  g_autofree char *body = ct_mnv_textf ("clips_forget_body", pairs);

  ct_confirm_dialog (m->window, ct_mnv_text ("clips_forget_heading"), body, ct_mnv_text ("clips_forget"),
                     clips_confirmed, ask_new (m, NULL, NULL));
}

/* ---- Scollega, Elimina ---- */

static void
on_unlink (GtkWidget *row, gpointer data)
{
  M *m = data;
  Ask *op = op_begin (m, "busy_unlink", "unlinked");

  ct_mnv_unlink (G_OBJECT (m->card), person_str (m, "id"), op_done, op);
}

static void
forget_confirmed (int response, gpointer data)
{
  Ask *ask = data;
  M *m = ask->m;

  if (response == CT_DIALOG_OK)
    {
      Ask *op = op_begin (m, "busy_forget", "forgotten");

      ct_mnv_forget (G_OBJECT (m->card), person_str (m, "id"), op_done, op);
    }
  ask_free (ask);
}

static void
on_forget (GtkWidget *row, gpointer data)
{
  M *m = data;
  const char *const pairs[] = { "name", m->name ? m->name : "", NULL };
  g_autofree char *heading_text = ct_mnv_textf ("delete_heading", pairs);
  g_autofree char *body = ct_mnv_textf ("delete_body", pairs);

  ct_confirm_dialog (m->window, heading_text, body, ct_mnv_text ("delete_ok"), forget_confirmed,
                     ask_new (m, NULL, NULL));
}

/* ---- the channels: WhatsApp and Telegram, IM rows of the contact ---- */

static JsonObject *
channel_at (guint index)
{
  JsonArray *channels = ct_mnv_list ("channel_list");

  return channels && index < json_array_get_length (channels) ? json_array_get_object_element (channels, index) : NULL;
}

static EContact *
contact_of (M *m)
{
  CtEntry *entry = ct_store_lookup (ct_window_get_store (m->window), m->uid);

  return entry ? entry->contact : NULL;
}

static void
channel_saved (const char *uid, const GError *error, gpointer data)
{
  Ask *ask = data;

  toast (ask->m, error ? error->message : ct_mnv_text (ask->done));
  ask_free (ask);
}

/* the contact written again with the channel's property set to @value ("" removes it); the address book's own store
 * does it, as for any other edit of a contact */
static void
channel_write (M *m, guint index, const char *value, const char *done)
{
  EContact *contact = contact_of (m);
  JsonObject *channel = channel_at (index);
  g_autoptr (EContact) copy = NULL;
  g_autoptr (GPtrArray) changed = NULL;
  Ask *ask;

  if (!contact || !channel)
    return;
  copy = e_contact_duplicate (contact);
  ct_contact_set_channel (copy, ns_json_str (channel, "property"), value);
  changed = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (changed, g_object_ref (copy));
  ask = ask_new (m, NULL, NULL);
  ask->done = done;
  ct_store_modify (ct_window_get_store (m->window), changed, channel_saved, ask);
}

static void
channel_entered (const char *text, gpointer data)
{
  Ask *ask = data;

  if (text)
    channel_write (ask->m, (guint) g_ascii_strtoull (ask->a, NULL, 10), text, "channel_saved");
  ask_free (ask);
}

static void
channel_edit (M *m, guint index, const char *current)
{
  JsonObject *channel = channel_at (index);
  g_autofree char *at = g_strdup_printf ("%u", index);

  ct_text_dialog (m->window, ns_json_str (channel, "title"), current, ns_json_str (channel, "hint"),
                  ct_mnv_int ("channel_max_chars"), NULL, channel_entered, ask_new (m, at, NULL));
}

static void
channel_action (int response, gpointer data)
{
  Ask *ask = data;
  guint index = (guint) g_ascii_strtoull (ask->a, NULL, 10);

  if (response == 0)
    channel_edit (ask->m, index, ask->b);
  else if (response == 1)
    channel_write (ask->m, index, "", "channel_removed");
  ask_free (ask);
}

static void
on_channel (GtkWidget *row, gpointer data)
{
  M *m = data;
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "ct-channel"));
  JsonObject *channel = channel_at (index);
  EContact *contact = contact_of (m);
  g_autofree char *value = contact && channel ? ct_contact_get_channel (contact, ns_json_str (channel, "property"))
                                              : g_strdup ("");
  g_autofree char *at = g_strdup_printf ("%u", index);

  if (!*value)
    channel_edit (m, index, "");
  else
    {
      const char *const labels[] = { ct_mnv_text ("channel_edit"), ct_mnv_text ("channel_remove") };

      ct_list_dialog (m->window, CT_DIALOG_APPCOMPAT, ns_json_str (channel, "title"), labels, 2, TRUE, channel_action,
                      ask_new (m, at, value));
    }
}

static void
channel_rows (M *m)
{
  JsonArray *channels = ct_mnv_list ("channel_list");
  EContact *contact = contact_of (m);

  heading (m, ct_mnv_text ("channels"));
  for (guint i = 0; channels && i < json_array_get_length (channels); i++)
    {
      JsonObject *channel = json_array_get_object_element (channels, i);
      g_autofree char *value = contact ? ct_contact_get_channel (contact, ns_json_str (channel, "property"))
                                       : g_strdup ("");
      GtkWidget *row = entry_row (m, ns_json_str (channel, "title"), *value ? value : ct_mnv_text ("channel_none"),
                                  G_CALLBACK (on_channel), contact != NULL);

      g_object_set_data (G_OBJECT (row), "ct-channel", GUINT_TO_POINTER (i));
    }
  /* the invitation, which asks the person on Telegram to write to the bot, is a task of its own */
  entry_row (m, ct_mnv_text ("invite_title"), ct_mnv_text ("invite_note"), NULL, FALSE);
}

/* ---- the body ---- */

static void
notebook_rows (M *m)
{
  GtkWidget *scroll, *label, *buttons;
  g_autofree char *stripped = g_strstrip (g_strdup (m->text ? m->text : ""));
  gboolean empty = !*stripped;  /* the text itself shows as it is */

  heading (m, ct_mnv_text ("notebook"));
  if (!person_flag (m, "notebook_open"))
    {
      label = text_label (ct_mnv_text ("notebook_closed"), "ct-qc-entry-sub", CT_QC_SUB_SP);
      pad (label);
      gtk_widget_set_margin_top (label, (int) CT_QC_ENTRY_PAD_TOP_DP);
      gtk_widget_set_margin_bottom (label, (int) CT_QC_ENTRY_PAD_BOTTOM_DP);
      gtk_box_append (GTK_BOX (m->body), label);
      return;
    }
  label = text_label (empty ? ct_mnv_text ("notebook_empty") : m->text, "ct-qc-entry-text", CT_QC_SUB_SP);
  pad (label);
  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), ct_mnv_int ("max_height_dp"));
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), label);
  gtk_widget_set_margin_top (scroll, (int) CT_QC_ENTRY_PAD_TOP_DP);
  gtk_box_append (GTK_BOX (m->body), scroll);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  pad (buttons);
  gtk_widget_set_margin_bottom (buttons, (int) CT_QC_ENTRY_PAD_END_DP);
  if (!empty)
    gtk_box_append (GTK_BOX (buttons), action_button (ct_mnv_text ("clear"), G_CALLBACK (on_clear), m));
  gtk_box_append (GTK_BOX (buttons), action_button (ct_mnv_text ("edit"), G_CALLBACK (on_edit), m));
  gtk_box_append (GTK_BOX (m->body), buttons);
}

static void
recognition_rows (M *m)
{
  gboolean running = voice_flag (m, "running"), enrolled = person_flag (m, "voiceprint");
  gint64 faces = person_int (m, "samples");
  g_autofree char *faces_count = number (faces);
  g_autofree char *faces_text = faces ? ct_mnv_textf ("faces_value", (const char *const[]) { "n", faces_count, NULL })
                                      : g_strdup (ct_mnv_text ("faces_none"));
  JsonObject *learn = m->voice ? ns_json_obj (m->voice, "learn") : NULL;
  g_autofree char *clips = number (voice_int (learn, "learned")), *need = number (voice_int (learn, "needed"));
  g_autofree char *learn_text = NULL;
  const char *voice_text;

  heading (m, ct_mnv_text ("recognition"));
  entry_row (m, ct_mnv_text ("faces_row"), faces_text, NULL, FALSE);

  if (m->voice && !running)
    voice_text = ct_mnv_text ("voice_off");
  else
    voice_text = ct_mnv_text (enrolled ? "voice_enrolled" : "voice_none");
  entry_row (m, ct_mnv_text ("voice_row"), voice_text, NULL, FALSE);
  if (running && !voice_flag (m, "enrolling"))
    entry_row (m, ct_mnv_text ("voice_enroll"), NULL, G_CALLBACK (on_enroll), TRUE);
  if (running && enrolled && !enrolling_here (m))
    entry_row (m, ct_mnv_text ("voice_forget"), NULL, G_CALLBACK (on_voice_forget), TRUE);

  if (!learning (m))
    learn_text = g_strdup (ct_mnv_text ("learn_off"));
  else if (voice_int (learn, "learned") < voice_int (learn, "needed"))
    learn_text = ct_mnv_textf ("learn_progress", (const char *const[]) { "clips", clips, "need", need, NULL });
  else
    learn_text = ct_mnv_textf ("learn_progress_done", (const char *const[]) { "clips", clips, NULL });
  entry_row (m, ct_mnv_text ("learn_row"), learn_text, G_CALLBACK (on_learn), running);
  if (voice_int (learn, "learned") > 0)
    entry_row (m, ct_mnv_text ("clips_forget"), NULL, G_CALLBACK (on_clips), TRUE);
}

/* the read-only numbers of the person: one row, a line each */
static char *
stats_text (M *m)
{
  GString *out = g_string_new (NULL);
  g_autofree char *samples = number (person_int (m, "samples")), *max = number (m->max_samples);
  g_autofree char *sightings = number (person_int (m, "sightings"));
  g_autofree char *confidence = NULL, *first = NULL, *last = NULL;
  const char *date_format = ct_mnv_text ("date_format");
  g_autofree char *samples_value = m->max_samples > 0
    ? ct_mnv_textf ("stat_samples_value", (const char *const[]) { "n", samples, "max", max, NULL })
    : g_strdup (samples);
  g_autofree char *sightings_value = ct_mnv_textf ("stat_sightings_value", (const char *const[]) { "n", sightings, NULL });
  double confidence_pct = m->person ? json_object_get_double_member_with_default (m->person, "confidence", 0) * 100 + 0.5 : 0;
  g_autofree char *pct = number ((gint64) confidence_pct);

  confidence = ct_mnv_textf ("stat_confidence_value", (const char *const[]) { "n", pct, NULL });
  first = ct_mnv_date (m->person, "first_seen", date_format);
  last = ct_mnv_date (m->person, "last_seen", date_format);
  g_string_append_printf (out, "%s: %s\n%s: %s\n%s: %s\n%s: %s\n%s: %s", ct_mnv_text ("stat_samples"), samples_value,
                          ct_mnv_text ("stat_sightings"), sightings_value, ct_mnv_text ("stat_confidence"), confidence,
                          ct_mnv_text ("stat_first"), first ? first : ct_mnv_text ("stat_never"),
                          ct_mnv_text ("stat_last"), last ? last : ct_mnv_text ("stat_never"));
  return g_string_free (out, FALSE);
}

static void
person_rows (M *m)
{
  const char *kind = person_str (m, "kind");
  const char *kind_text = ct_mnv_map ("kind_one", kind);
  g_autofree char *stats = stats_text (m);

  heading (m, ct_mnv_text ("record"));
  entry_row (m, ct_mnv_text ("kind_row"), *kind_text ? kind_text : kind, G_CALLBACK (on_kind), TRUE);
  entry_row (m, ct_mnv_text ("stats"), stats, NULL, FALSE);
  entry_row (m, ct_mnv_text ("merge_row"), ct_mnv_text ("merge_sub"), G_CALLBACK (on_merge), TRUE);
  entry_row (m, ct_mnv_text ("unlink"), ct_mnv_text ("unlink_sub"), G_CALLBACK (on_unlink), TRUE);
  entry_row (m, ct_mnv_text ("delete"), ct_mnv_text ("delete_sub"), G_CALLBACK (on_forget), TRUE);
}

static void
unlinked_rows (M *m)
{
  heading (m, ct_mnv_text ("notebook"));
  entry_row (m, ct_mnv_text ("create"), ct_mnv_text ("create_note"), G_CALLBACK (on_create), TRUE);
  entry_row (m, ct_mnv_text ("link"), ct_mnv_text ("link_note"), G_CALLBACK (on_link), TRUE);
}

static void
set_title (M *m, const char *text)
{
  GdkRGBA color = ct_rgba (m->tint);
  PangoAttrList *attrs = pango_attr_list_new ();

  gtk_label_set_text (GTK_LABEL (m->title), text);
  gtk_widget_set_visible (m->title, text && *text);
  /* the theme colour, which no style sheet knows (as QuickContact's card_new) */
  pango_attr_list_insert (attrs, pango_attr_foreground_new ((guint16) (color.red * 65535),
                                                            (guint16) (color.green * 65535),
                                                            (guint16) (color.blue * 65535)));
  gtk_label_set_attributes (GTK_LABEL (m->title), attrs);
  pango_attr_list_unref (attrs);
}

static void
render (M *m)
{
  EContact *contact = contact_of (m);
  JsonArray *channels = ct_mnv_list ("channel_list");
  g_autofree char *person = object_text (m->person), *voice = object_text (m->voice);
  g_autoptr (GString) now = g_string_new (NULL);
  GtkWidget *child;

  if (!ct_mnv_ui_ready ())
    return;
  /* what the card shows depends on all of this: the same again is not built again (the scroll would be lost) */
  g_string_append_printf (now, "%s\n%s\n%s\n%s\n%" G_GINT64_FORMAT "\n%d", person, voice, m->text ? m->text : "",
                          m->name ? m->name : "", m->max_samples, contact != NULL);
  for (guint i = 0; contact && channels && i < json_array_get_length (channels); i++)
    {
      g_autofree char *value = ct_contact_get_channel (contact, ns_json_str (json_array_get_object_element (channels, i),
                                                                           "property"));

      g_string_append_printf (now, "\n%s", value);
    }
  if (m->shown && g_str_equal (now->str, m->shown))
    {
      busy_update (m);
      return;
    }
  g_free (m->shown);
  m->shown = g_strdup (now->str);
  while ((child = gtk_widget_get_first_child (m->body)))
    gtk_box_remove (GTK_BOX (m->body), child);
  set_title (m, ct_mnv_text ("title"));
  if (m->person)
    notebook_rows (m);
  else
    unlinked_rows (m);
  channel_rows (m);
  if (m->person)
    {
      recognition_rows (m);
      person_rows (m);
    }
  gtk_widget_set_visible (m->card, TRUE);
  busy_update (m);
}

/* ---- loading: `people by-contact`, `people notebook get`, `people voice-status`, the rules once ---- */

static void
load_done (M *m)
{
  m->loading = FALSE;
  if (m->again)
    {
      m->again = FALSE;
      load (m);
    }
}

static void
voice_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;

  if (!err)
    {
      g_clear_pointer (&m->voice, json_object_unref);
      m->voice = json_object_ref (root);
    }
  render (m);
  load_done (m);
}

static void
rules_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  JsonObject *rules = err ? NULL : ns_json_obj (root, "rules");

  m->max_samples = rules ? voice_int (rules, "max_samples") : 0;
  ct_mnv_voice_status (G_OBJECT (m->card), voice_loaded, m);
}

static void
notebook_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;

  if (err)
    {
      load_done (m);            /* keeps what it shows */
      return;
    }
  g_free (m->text);
  m->text = g_strdup (ns_json_str (root, "text"));
  /* the voice service every time while there is something to follow (a voice being recorded, the learning), else only
   * the first time; the rules' sample limit once */
  if (!m->max_samples)
    ct_mnv_known (G_OBJECT (m->card), rules_loaded, m);
  else if (!m->voice || enrolling_here (m) || voice_flag (m, "enrolling") || learning (m))
    ct_mnv_voice_status (G_OBJECT (m->card), voice_loaded, m);
  else
    {
      render (m);
      load_done (m);
    }
}

static void
name_synced (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;

  if (root)
    {
      g_clear_pointer (&m->person, json_object_unref);
      m->person = json_object_ref (root);
    }
  ct_mnv_notebook (G_OBJECT (m->card), person_str (m, "id"), notebook_loaded, m);
}

static void
person_found (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  JsonObject *who;

  if (err)
    {
      load_done (m);            /* keeps what it shows (hidden before the first answer) */
      return;
    }
  who = ns_json_obj (root, "person");
  g_clear_pointer (&m->person, json_object_unref);
  if (!who || !*ns_json_str (who, "id"))
    {
      g_clear_pointer (&m->text, g_free);
      g_clear_pointer (&m->voice, json_object_unref);
      render (m);
      load_done (m);
      return;
    }
  m->person = json_object_ref (who);
  /* the person's name is the contact's: the archive's copy follows it, once for each name */
  if (m->name && g_strcmp0 (m->synced, m->name) && ct_mnv_name_stale (person_str (m, "name"), m->name))
    {
      g_free (m->synced);
      m->synced = g_strdup (m->name);
      ct_mnv_sync_name (G_OBJECT (m->card), person_str (m, "id"), person_str (m, "name"), m->name, name_synced, m);
      return;
    }
  ct_mnv_notebook (G_OBJECT (m->card), person_str (m, "id"), notebook_loaded, m);
}

static void
load (M *m)
{
  if (!ct_mnv_ui_ready ())
    return;
  if (m->loading)
    {
      m->again = TRUE;
      return;
    }
  m->loading = TRUE;
  ct_mnv_by_contact (G_OBJECT (m->card), m->uid, person_found, m);
}

static gboolean
poll_tick (gpointer data)
{
  load (data);
  return G_SOURCE_CONTINUE;
}

static void
poll_start (M *m)
{
  if (!m->poll && ct_mnv_ui_ready () && ct_mnv_int ("poll_s") > 0)
    m->poll = g_timeout_add_seconds ((guint) ct_mnv_int ("poll_s"), poll_tick, m);
}

static void
on_map (GtkWidget *card, gpointer data)
{
  M *m = data;

  poll_start (m);
  if (!m->loading)              /* back on the card (from the editor, another app): as it is now */
    load (m);
}

static void
on_unmap (GtkWidget *card, gpointer data)
{
  M *m = data;

  g_clear_handle_id (&m->poll, g_source_remove);
}

static void
ui_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;

  if (err)
    return;                     /* no Minerva people here: no card */
  if (gtk_widget_get_mapped (m->card))
    poll_start (m);
  load (m);
}

static void
m_free (gpointer data)
{
  M *m = data;

  g_clear_handle_id (&m->poll, g_source_remove);
  g_clear_pointer (&m->person, json_object_unref);
  g_clear_pointer (&m->voice, json_object_unref);
  g_free (m->uid);
  g_free (m->name);
  g_free (m->text);
  g_free (m->synced);
  g_free (m->shown);
  g_free (m->op_busy);
  g_free (m);
}

GtkWidget *
ct_minerva_card_new (GtkWidget *window, const char *uid)
{
  M *m;

  if (!uid || !*uid || !ct_mnv_available ())
    return NULL;
  m = g_new0 (M, 1);
  m->window = window;
  m->uid = g_strdup (uid);
  m->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (m->card, "ct-qc-card");
  gtk_widget_set_overflow (m->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_visible (m->card, FALSE);
  m->title = ct_label ("", "ct-qc-card-title", CT_QC_CARD_TITLE_SP);
  gtk_label_set_ellipsize (GTK_LABEL (m->title), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (m->card), m->title);
  m->busy = ct_busy_new ();
  gtk_box_append (GTK_BOX (m->card), m->busy);
  m->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (m->card), m->body);
  g_object_set_data_full (G_OBJECT (m->card), "ct-minerva", m, m_free);
  g_signal_connect (m->card, "map", G_CALLBACK (on_map), m);
  g_signal_connect (m->card, "unmap", G_CALLBACK (on_unmap), m);
  ct_mnv_ui_load (G_OBJECT (m->card), ui_loaded, m);
  return m->card;
}

void
ct_minerva_card_update (GtkWidget *card, const char *name, guint32 title_argb)
{
  M *m = g_object_get_data (G_OBJECT (card), "ct-minerva");

  if (!m)
    return;
  g_free (m->name);
  m->name = g_strdup (name);
  m->tint = title_argb;
  if (ct_mnv_ui_ready () && m->shown)
    {
      set_title (m, ct_mnv_text ("title"));
      render (m);               /* the channels may have changed, and so the name the archive follows */
    }
}
