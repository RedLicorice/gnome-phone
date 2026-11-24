/* The Minerva screens of Contacts (ct-minerva-pages.h). */
#include "ct-minerva-pages.h"
#include "ct-contact.h"
#include "ct-dialogs.h"
#include "ct-mnv-core.h"
#include "ct-mnv.h"
#include "ct-page.h"
#include "ct-picker.h"
#include "ct-settings.h"
#include "ct-store.h"
#include "ct-ui.h"
#include "ct-window.h"
#include "ns-mc.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <string.h>

/* ===== a page: the toolbar, the busy line and a scrolling column =============================================== */

typedef struct {
  GtkWidget *page, *window, *busy, *rows;
  JsonObject *known;            /* the last `people known` (a ref) */
  int ops;
  char *list_id;                /* an entries list: its id in the data's "lists" */
  char *provider;               /* a provider's chats: its id, */
  gboolean linking;             /* for an existing contact: the chats other contacts have show too */
  CtChatPicked picked;          /* who gets the chat picked, */
  gpointer picked_data;
  GDestroyNotify picked_destroy;
  GtkWidget *search;            /* and the field that narrows them */
} P;

static void
p_free (gpointer data)
{
  P *p = data;

  g_clear_pointer (&p->known, json_object_unref);
  if (p->picked_destroy)
    p->picked_destroy (p->picked_data);
  g_free (p->list_id);
  g_free (p->provider);
  g_free (p);
}

static P *
page_new (GtkWidget *window, const char *title, const char *key)
{
  P *p = g_new0 (P, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  p->page = page;
  p->window = window;
  g_object_set_data_full (G_OBJECT (page), key, p, p_free);
  gtk_widget_add_css_class (page, "ct-settings");
  gtk_box_append (GTK_BOX (page), ct_page_toolbar (CT_PAGE (page), NULL, "ct-arrow-back-symbolic", _("Back"), title,
                                                   NULL, NULL));
  p->busy = ct_busy_new ();
  gtk_box_append (GTK_BOX (content), p->busy);
  p->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), p->rows);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), content);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), scroll);
  gtk_widget_add_css_class (shadow, "ct-toolbar-shadow");
  gtk_widget_add_css_class (shadow, "settings");
  gtk_widget_set_valign (shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), shadow);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_box_append (GTK_BOX (page), overlay);
  return p;
}

static void
clear_rows (P *p)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (p->rows)))
    gtk_box_remove (GTK_BOX (p->rows), child);
}

static void
category (P *p, const char *text)
{
  GtkWidget *label = ct_label (text, "ct-pref-category", 14);

  gtk_box_append (GTK_BOX (p->rows), label);
}

static void
note (P *p, const char *text)
{
  GtkWidget *label = ct_label (text, "ct-pref-note", 14);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_box_append (GTK_BOX (p->rows), label);
}

static void
toast (P *p, const char *text)
{
  if (text && *text)
    ct_window_toast (p->window, text);
}

/* a dialog's or an action's state: the page is kept alive until the answer comes */
typedef struct {
  P *p;
  char *a, *b, *c;
  GPtrArray *actions;           /* an entry's dialog: the action of each row */
  const char *done;             /* the key of the text that says it was done */
} Op;

static Op *
op_new (P *p, const char *a, const char *b)
{
  Op *op = g_new0 (Op, 1);

  op->p = p;
  op->a = g_strdup (a);
  op->b = g_strdup (b);
  g_object_ref (p->page);
  return op;
}

static void
op_free (Op *op)
{
  GtkWidget *page = op->p->page;

  g_free (op->a);
  g_free (op->b);
  g_free (op->c);
  g_clear_pointer (&op->actions, g_ptr_array_unref);
  g_free (op);
  g_object_unref (page);
}

static void
busy_on (P *p, const char *key)
{
  p->ops++;
  ct_busy_start (p->busy, ct_mnv_text (key), ct_mnv_text ("busy_slow"), (guint) ct_mnv_int ("busy_slow_s"));
  ct_window_toast_hold (p->window, ct_mnv_text (key));
}

static void
busy_off (P *p)
{
  if (--p->ops <= 0)
    ct_busy_stop (p->busy);
}

/* ===== Sconosciuti ============================================================================================= */

static void list_load (P *p);

static void
stranger_finished (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;

  busy_off (op->p);
  toast (op->p, err ? err : ct_mnv_text (op->done));
  list_load (op->p);
  op_free (op);
}

static void
stranger_named (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;

  if (err)
    {
      stranger_finished (NULL, err, op);
      return;
    }
  ct_mnv_link (G_OBJECT (op->p->page), op->a, op->c, stranger_finished, op);
}

static void
contact_saved (const char *uid, const GError *error, gpointer data)
{
  Op *op = data;

  if (error)
    {
      stranger_finished (NULL, error->message, op);
      return;
    }
  /* the contact's name is the person's: the archive's copy is set to it before the link */
  op->c = g_strdup (uid);
  ct_mnv_sync_name (G_OBJECT (op->p->page), op->a, NULL, op->b, stranger_named, op);
}

static void
add_entered (const char *text, gpointer data)
{
  Op *op = data;
  P *p = op->p;
  CtStore *store = ct_window_get_store (p->window);
  g_autoptr (EContact) contact = e_contact_new ();
  CtName *name = ct_name_new ();

  if (!text)
    {
      op_free (op);
      return;
    }
  g_free (op->b);
  op->b = g_strstrip (g_strdup (text));
  g_free (name->given);
  name->given = g_strdup (op->b);
  ct_contact_set_name (contact, name);
  ct_name_free (name);
  op->done = "stranger_added";
  busy_on (p, "busy_add");
  ct_store_save (store, ct_store_default_book (store), contact, TRUE, contact_saved, op);
}

static void
forget_confirmed (int response, gpointer data)
{
  Op *op = data;

  if (response == CT_DIALOG_OK)
    {
      op->done = "stranger_forgotten";
      busy_on (op->p, "busy_forget");
      ct_mnv_forget (G_OBJECT (op->p->page), op->a, stranger_finished, op);
    }
  else
    op_free (op);
}

static void
renamed_entered (const char *text, gpointer data)
{
  Op *op = data;

  if (!text)
    {
      op_free (op);
      return;
    }
  op->done = "renamed";
  busy_on (op->p, "busy_rename");
  ct_mnv_rename (G_OBJECT (op->p->page), op->a, text, stranger_finished, op);
}

static void
owner_picked (GPtrArray *uids, gpointer data)
{
  Op *op = data;

  if (uids->len)
    {
      op->done = "owner_set";
      busy_on (op->p, "busy_owner");
      ct_mnv_set_owner (G_OBJECT (op->p->page), op->a, uids->pdata[0], stranger_finished, op);
    }
  else
    op_free (op);
}

static void
entry_action (int response, gpointer data)
{
  Op *op = data;
  const char *action = op->actions && response >= 0 && (guint) response < op->actions->len
                         ? op->actions->pdata[response] : NULL;
  const char *const pairs[] = { "name", op->b, NULL };

  if (!action)
    op_free (op);
  else if (!g_strcmp0 (action, "rename"))
    ct_text_dialog (op->p->window, ct_mnv_text ("rename_heading"), op->b, ct_mnv_text ("add_hint"),
                    ct_mnv_int ("name_max_chars"), NULL, renamed_entered, op);
  else if (!g_strcmp0 (action, "add"))
    ct_text_dialog (op->p->window, ct_mnv_text ("add_heading"), op->b, ct_mnv_text ("add_hint"),
                    ct_mnv_int ("name_max_chars"), NULL, add_entered, op);
  else if (!g_strcmp0 (action, "owner"))
    {
      g_autofree char *title = ct_mnv_textf ("owner_pick_title", pairs);

      /* the picker calls back only when a contact is picked: the state of this action is kept until then */
      ct_window_push (op->p->window, ct_picker_new (op->p->window, title, NULL, FALSE, owner_picked, op));
    }
  else if (!g_strcmp0 (action, "owner_clear"))
    {
      op->done = "owner_cleared";
      busy_on (op->p, "busy_owner");
      ct_mnv_set_owner (G_OBJECT (op->p->page), op->a, "", stranger_finished, op);
    }
  else if (!g_strcmp0 (action, "forget"))
    {
      g_autofree char *heading = ct_mnv_textf ("forget_heading", pairs);
      g_autofree char *body = ct_mnv_textf ("forget_body", pairs);

      ct_confirm_dialog (op->p->window, heading, body, ct_mnv_text ("forget_ok"), forget_confirmed, op);
    }
  else
    op_free (op);
}

static JsonObject *
list_def (const char *id)
{
  JsonArray *lists = ct_mnv_list ("lists");

  for (guint i = 0; lists && i < json_array_get_length (lists); i++)
    {
      JsonObject *def = json_array_get_object_element (lists, i);

      if (!g_strcmp0 (ns_json_str (def, "id"), id))
        return def;
    }
  return NULL;
}

/* a row's data key for each action the list's definition names */
static const char *
action_label (const char *action)
{
  g_autofree char *key = g_strconcat ("action_", action, NULL);

  return ct_mnv_text (key);
}

static void
on_entry (NeoPressable *row, gpointer data)
{
  P *p = data;
  const char *id = g_object_get_data (G_OBJECT (row), "ct-id"), *name = g_object_get_data (G_OBJECT (row), "ct-name");
  gboolean protected_ = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-protected"));
  gboolean owned = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ct-owned"));
  JsonArray *actions = ns_json_arr (list_def (p->list_id), "actions");
  Op *op = op_new (p, id, name);
  g_autoptr (GPtrArray) labels = g_ptr_array_new ();

  op->actions = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; actions && i < json_array_get_length (actions); i++)
    {
      const char *action = json_array_get_string_element (actions, i);

      /* the owner of Minerva is never forgotten from a list */
      if (protected_ && !g_strcmp0 (action, "forget"))
        continue;
      g_ptr_array_add (op->actions, g_strdup (action));
      g_ptr_array_add (labels, (gpointer) action_label (action));
      if (owned && !g_strcmp0 (action, "owner"))
        {
          g_ptr_array_add (op->actions, g_strdup ("owner_clear"));
          g_ptr_array_add (labels, (gpointer) action_label ("owner_clear"));
        }
    }
  ct_list_dialog (p->window, CT_DIALOG_APPCOMPAT, name, (const char *const *) labels->pdata, (int) labels->len, TRUE,
                  entry_action, op);
}

/* an entry's line in the list: [Minerva's owner •] [owner of the thing •] samples • sightings • last time */
static char *
entry_sub (P *p, JsonObject *o, gboolean protected_)
{
  gint64 samples = json_object_get_int_member_with_default (o, "samples", 0);
  gint64 sightings = json_object_get_int_member_with_default (o, "sightings", 0);
  g_autofree char *ns = g_strdup_printf ("%" G_GINT64_FORMAT, samples), *nv = g_strdup_printf ("%" G_GINT64_FORMAT, sightings);
  g_autofree char *date = ct_mnv_date (o, "last_seen", ct_mnv_text ("date_format"));
  g_autofree char *row = ct_mnv_textf ("stranger_row", (const char *const[]) { "samples", ns, "sightings", nv, NULL });
  g_autofree char *seen = date ? ct_mnv_textf ("last_seen", (const char *const[]) { "date", date, NULL })
                               : g_strdup (ct_mnv_text ("never_seen"));
  g_autoptr (GString) out = g_string_new (NULL);

  if (protected_)
    g_string_append_printf (out, "%s • ", ct_mnv_text ("owner_label"));
  if (json_object_get_boolean_member_with_default (o, "ownable", FALSE))
    {
      const char *uid = ns_json_str (o, "owner_contact");
      CtEntry *entry = *uid ? ct_store_lookup (ct_window_get_store (p->window), uid) : NULL;
      g_autofree char *who = entry ? ct_contact_display_name (entry->contact,
                                                              ct_display_order (ct_window_get_settings (p->window)))
                                   : NULL;
      g_autofree char *text = who && *who ? ct_mnv_textf ("owner_row", (const char *const[]) { "name", who, NULL })
                                          : g_strdup (ct_mnv_text ("no_owner"));

      g_string_append_printf (out, "%s • ", text);
    }
  g_string_append_printf (out, "%s • %s", row, seen);
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
list_loaded (JsonObject *root, const char *err, gpointer data)
{
  P *p = data;
  JsonObject *def = list_def (p->list_id);
  g_autoptr (GPtrArray) found = NULL;
  JsonObject *rules;
  g_autofree char *days = NULL, *text = NULL;

  if (err)
    {
      toast (p, err);
      return;
    }
  clear_rows (p);
  rules = ns_json_obj (root, "rules");
  days = g_strdup_printf ("%" G_GINT64_FORMAT, rules ? json_object_get_int_member_with_default (rules, "forget_days", 0) : 0);
  if (*ns_json_str (def, "note"))
    {
      text = ct_mnv_format (ns_json_str (def, "note"), (const char *const[]) { "days", days, NULL });
      note (p, text);
    }
  found = ct_mnv_of_kind (root, ns_json_str (def, "kind"),
                          json_object_get_boolean_member_with_default (def, "loose", FALSE));
  if (!found->len)
    note (p, ns_json_str (def, "empty"));
  for (guint i = 0; i < found->len; i++)
    {
      JsonObject *o = found->pdata[i];
      gboolean protected_ = ct_mnv_is_owner (root, o);
      g_autofree char *sub = entry_sub (p, o, protected_);
      GtkWidget *row = ct_pref_row (ns_json_str (o, "name"), sub, NULL, G_CALLBACK (on_entry), p);

      g_object_set_data_full (G_OBJECT (row), "ct-id", g_strdup (ns_json_str (o, "id")), g_free);
      g_object_set_data_full (G_OBJECT (row), "ct-name", g_strdup (ns_json_str (o, "name")), g_free);
      g_object_set_data (G_OBJECT (row), "ct-protected", GINT_TO_POINTER (protected_));
      g_object_set_data (G_OBJECT (row), "ct-owned", GINT_TO_POINTER (*ns_json_str (o, "owner_contact") != '\0'));
      gtk_box_append (GTK_BOX (p->rows), row);
    }
}

static void
list_load (P *p)
{
  ct_mnv_known (G_OBJECT (p->page), list_loaded, p);
}

static void
on_list_map (GtkWidget *page, gpointer data)
{
  list_load (data);        /* back on the page (from the picker, another app): as it is now */
}

GtkWidget *
ct_mnv_list_new (GtkWidget *window, const char *id)
{
  JsonObject *def = list_def (id);
  P *p = page_new (window, ns_json_str (def, "title"), "ct-mnv-list");

  p->list_id = g_strdup (id);
  g_signal_connect (p->page, "map", G_CALLBACK (on_list_map), p);
  return p->page;
}

/* ===== Minerva: the rules and the counts ======================================================================= */

typedef struct {
  Op *op;                       /* the page's state, kept for the dialog */
  double lo, hi;
  gboolean integer;
  char *error;
} Rule;

static void rules_load (P *p);

static void
rule_saved (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;

  busy_off (op->p);
  toast (op->p, err ? err : ct_mnv_text ("rules_saved"));
  rules_load (op->p);
  op_free (op);
}

static const char *
rule_check (const char *text, gpointer data)
{
  Rule *rule = data;
  g_autofree char *value = NULL;
  g_autofree char *lo = NULL, *hi = NULL;
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  if (ct_mnv_rule_value (text, rule->lo, rule->hi, rule->integer, &value))
    return NULL;
  lo = g_strdup (g_ascii_formatd (buf, sizeof buf, "%g", rule->lo));
  hi = g_strdup (g_ascii_formatd (buf, sizeof buf, "%g", rule->hi));
  g_free (rule->error);
  rule->error = ct_mnv_textf ("rule_invalid", (const char *const[]) { "min", lo, "max", hi, NULL });
  return rule->error;
}

static void
rule_entered (const char *text, gpointer data)
{
  Rule *rule = data;
  Op *op = rule->op;
  g_autofree char *value = NULL;

  if (text && ct_mnv_rule_value (text, rule->lo, rule->hi, rule->integer, &value))
    {
      busy_on (op->p, "busy_rules");
      ct_mnv_rule_set (G_OBJECT (op->p->page), op->a, value, rule_saved, op);
    }
  else
    op_free (op);
  g_free (rule->error);
  g_free (rule);
}

static void
on_rule (NeoPressable *row, gpointer data)
{
  P *p = data;
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "ct-rule"));
  JsonArray *rules = ct_mnv_list ("rules");
  JsonObject *def = rules ? json_array_get_object_element (rules, index) : NULL;
  JsonObject *known = p->known ? ns_json_obj (p->known, "rules") : NULL;
  Rule *rule;
  g_autofree char *current = NULL;
  const char *key = ns_json_str (def, "key");

  if (!def)
    return;
  rule = g_new0 (Rule, 1);
  rule->op = op_new (p, key, NULL);
  rule->lo = json_object_get_double_member_with_default (def, "min", 0);
  rule->hi = json_object_get_double_member_with_default (def, "max", 0);
  rule->integer = json_object_get_boolean_member_with_default (def, "integer", FALSE);
  if (known && json_object_has_member (known, key))
    {
      double value = json_object_get_double_member_with_default (known, key, 0);
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      current = g_strdup (g_ascii_formatd (buf, sizeof buf, rule->integer ? "%.0f" : "%g", value));
    }
  ct_text_dialog (p->window, ns_json_str (def, "title"), current, NULL, ct_mnv_int ("rule_max_chars"), rule_check,
                  rule_entered, rule);
}

static void
on_open_strangers (NeoPressable *row, gpointer data)
{
  P *p = data;

  ct_window_push (p->window, ct_mnv_list_new (p->window, "strangers"));
}

static void
count_row (P *p, const char *title, gint64 n, GCallback clicked)
{
  g_autofree char *count = g_strdup_printf ("%" G_GINT64_FORMAT, n);

  gtk_box_append (GTK_BOX (p->rows), ct_pref_row (title, count, NULL, clicked, p));
}

static void
rules_loaded (JsonObject *root, const char *err, gpointer data)
{
  P *p = data;
  JsonArray *people = err ? NULL : ns_json_arr (root, "people");
  JsonArray *kinds = err ? NULL : ns_json_arr (root, "kinds");
  JsonArray *rules = ct_mnv_list ("rules");
  JsonObject *values = err ? NULL : ns_json_obj (root, "rules");
  g_autoptr (GPtrArray) loose = NULL;
  gint64 voices = 0, linked = 0;
  g_autofree char *days = NULL, *sweep = NULL, *text = NULL;

  if (err)
    {
      toast (p, err);
      return;
    }
  g_clear_pointer (&p->known, json_object_unref);
  p->known = json_object_ref (root);
  clear_rows (p);

  category (p, ct_mnv_text ("counts"));
  for (guint k = 0; kinds && k < json_array_get_length (kinds); k++)
    {
      JsonObject *kind = json_array_get_object_element (kinds, k);
      const char *id = ns_json_str (kind, "id"), *label = ct_mnv_map ("kinds", id);
      gint64 n = 0;

      for (guint i = 0; people && i < json_array_get_length (people); i++)
        n += !g_strcmp0 (ns_json_str (json_array_get_object_element (people, i), "kind"), id);
      count_row (p, *label ? label : id, n, NULL);
    }
  for (guint i = 0; people && i < json_array_get_length (people); i++)
    {
      JsonObject *o = json_array_get_object_element (people, i);

      voices += json_object_get_boolean_member_with_default (o, "voiceprint", FALSE);
      linked += *ns_json_str (o, "contact_uid") != '\0';
    }
  loose = ct_mnv_of_kind (root, "person", TRUE);
  count_row (p, ct_mnv_text ("count_linked"), linked, NULL);
  count_row (p, ct_mnv_text ("count_voices"), voices, NULL);
  count_row (p, ct_mnv_text ("strangers"), loose->len, G_CALLBACK (on_open_strangers));

  category (p, ct_mnv_text ("rules_group"));
  for (guint i = 0; rules && i < json_array_get_length (rules); i++)
    {
      JsonObject *def = json_array_get_object_element (rules, i);
      const char *key = ns_json_str (def, "key");
      gboolean integer = json_object_get_boolean_member_with_default (def, "integer", FALSE);
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_autofree char *value = values && json_object_has_member (values, key)
        ? g_strdup (g_ascii_formatd (buf, sizeof buf, integer ? "%.0f" : "%g",
                                     json_object_get_double_member_with_default (values, key, 0)))
        : g_strdup ("");
      GtkWidget *row = ct_pref_row (ns_json_str (def, "title"), value, NULL, G_CALLBACK (on_rule), p);

      g_object_set_data (G_OBJECT (row), "ct-rule", GUINT_TO_POINTER (i));
      gtk_box_append (GTK_BOX (p->rows), row);
    }
  note (p, ct_mnv_text ("rules_note"));
  days = g_strdup_printf ("%" G_GINT64_FORMAT, values ? json_object_get_int_member_with_default (values, "forget_days", 0) : 0);
  sweep = ct_mnv_textf ("rules_sweep", (const char *const[]) { "days", days, NULL });
  text = g_strdup_printf ("%s %s", sweep,
                          ct_mnv_text (values && json_object_get_boolean_member_with_default (values, "sweep_log_only", FALSE)
                                       ? "rules_sweep_log" : "rules_sweep_apply"));
  note (p, text);
}

static void
rules_load (P *p)
{
  ct_mnv_known (G_OBJECT (p->page), rules_loaded, p);
}

GtkWidget *
ct_minerva_settings_new (GtkWidget *window)
{
  P *p = page_new (window, ct_mnv_text ("settings_title"), "ct-minerva-settings");

  rules_load (p);
  return p->page;
}

/* ===== a provider's chats with one person: the one tapped is picked ============================================ */

/* what the search matches a row by: its title and its chat id, case-folded */
static void
chats_filter (GtkEditable *search, P *p)
{
  g_autofree char *fold = g_utf8_casefold (gtk_editable_get_text (search), -1);
  g_auto(GStrv) words = g_strsplit (fold, " ", -1);

  for (GtkWidget *row = gtk_widget_get_first_child (p->rows); row; row = gtk_widget_get_next_sibling (row))
    {
      const char *text = g_object_get_data (G_OBJECT (row), "ct-text");
      gboolean shown = TRUE;

      if (!text)
        continue;               /* a note */
      for (guint i = 0; shown && words[i]; i++)
        shown = !*words[i] || strstr (text, words[i]);
      gtk_widget_set_visible (row, shown);
    }
}

/* the list goes first: what the pick opens comes over the screen the list was opened from */
static void
chat_picked (P *p, const char *chat)
{
  GtkWidget *page = g_object_ref (p->page);
  g_autofree char *picked = g_strdup (chat);

  ct_window_pop (p->window, page);
  if (p->picked)
    p->picked (p->provider, picked, p->picked_data);
  g_object_unref (page);
}

static void
on_taken_confirmed (int response, gpointer data)
{
  Op *op = data;

  if (response == CT_DIALOG_OK)
    chat_picked (op->p, op->a);
  op_free (op);
}

static void
on_chat (NeoPressable *row, gpointer data)
{
  P *p = data;
  const char *chat = g_object_get_data (G_OBJECT (row), "ct-chat");
  const char *owner = g_object_get_data (G_OBJECT (row), "ct-contact-name");

  /* a chat another contact has: linking it here makes it two contacts' */
  if (owner)
    {
      Op *op = op_new (p, chat, owner);
      g_autofree char *body = ct_mnv_textf ("link_taken_body", (const char *const[]) { "name", owner, NULL });

      ct_confirm_dialog (p->window, ct_mnv_text ("link_taken_heading"), body, ct_mnv_text ("link_taken_ok"),
                         on_taken_confirmed, op);
      return;
    }
  chat_picked (p, chat);
}

static void
chats_loaded (JsonObject *root, const char *err, gpointer data)
{
  P *p = data;
  JsonArray *chats = err ? NULL : ns_json_arr (root, "chats");
  guint shown = 0;

  ct_busy_stop (p->busy);
  clear_rows (p);
  if (err)
    {
      note (p, err);
      return;
    }
  for (guint i = 0; chats && i < json_array_get_length (chats); i++)
    {
      JsonObject *o = json_array_get_object_element (chats, i);
      const char *chat = ns_json_str (o, "chat"), *label = ns_json_str (o, "label");
      gboolean taken = *ns_json_str (o, "contact") != '\0';
      GtkWidget *row;
      g_autofree char *text = NULL, *sub = NULL;

      /* a chat that is a contact already is not one to make a contact of */
      if (!*chat || (taken && !p->linking))
        continue;
      if (!*label)
        label = chat;
      /* the id under a name, which of two with one name it is; and whose it is when a contact has it (the listing
       * names such a chat after its contact) */
      sub = taken ? ct_mnv_textf ("link_taken_row", (const char *const[]) { "name", label, NULL })
                  : g_strcmp0 (label, chat) ? g_strdup (chat) : NULL;
      row = ct_pref_row (taken ? chat : label, sub, NULL, G_CALLBACK (on_chat), p);
      text = g_utf8_casefold (label, -1);
      g_object_set_data_full (G_OBJECT (row), "ct-chat", g_strdup (chat), g_free);
      if (taken)
        g_object_set_data_full (G_OBJECT (row), "ct-contact-name", g_strdup (label), g_free);
      g_object_set_data_full (G_OBJECT (row), "ct-text", g_strconcat (text, " ", chat, NULL), g_free);
      gtk_box_append (GTK_BOX (p->rows), row);
      shown++;
    }
  if (!shown)
    note (p, ct_mnv_text ("import_empty"));
  chats_filter (GTK_EDITABLE (p->search), p);
}

GtkWidget *
ct_mnv_chats_new (GtkWidget *window, const char *provider, gboolean linking, CtChatPicked picked, gpointer data,
                  GDestroyNotify destroy)
{
  JsonObject *def = ct_mnv_import_provider (provider);
  const char *label = *ns_json_str (def, "label") ? ns_json_str (def, "label") : provider;
  g_autofree char *title = ct_mnv_textf ("import_pick_title", (const char *const[]) { "provider", label, NULL });
  P *p = page_new (window, title, "ct-mnv-chats");
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *input = gtk_overlay_new ();

  p->provider = g_strdup (provider);
  p->linking = linking;
  p->picked = picked;
  p->picked_data = data;
  p->picked_destroy = destroy;
  /* the search field, in the editor's look (an EditText) */
  p->search = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (p->search), ct_mnv_text ("import_search"));
  gtk_widget_add_css_class (p->search, "ct-ed-text");
  gtk_widget_set_valign (p->search, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->search), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  ct_mnv_text ("import_search"), -1);
  gtk_widget_add_css_class (field, "ct-ed-field");
  gtk_widget_add_css_class (input, "ct-ed-input");
  gtk_overlay_set_child (GTK_OVERLAY (input), p->search);
  gtk_widget_set_vexpand (input, TRUE);
  gtk_box_append (GTK_BOX (field), input);
  gtk_box_insert_child_after (GTK_BOX (gtk_widget_get_parent (p->rows)), field, p->busy);
  g_signal_connect (p->search, "changed", G_CALLBACK (chats_filter), p);
  ct_busy_start (p->busy, ct_mnv_text ("import_loading"), ct_mnv_text ("busy_slow"), (guint) ct_mnv_int ("busy_slow_s"));
  ct_mnv_user_chats (G_OBJECT (p->page), provider, chats_loaded, p);
  return p->page;
}

/* ===== a chat becomes a new contact ============================================================================= */

typedef struct {
  GtkWidget *window;
  char *provider, *chat;
} Import;

static void
import_free (Import *import)
{
  g_object_unref (import->window);
  g_free (import->provider);
  g_free (import->chat);
  g_free (import);
}

/* the new-contact editor with what the provider knows of the chat: its name, its number, its id there (written as
 * the memory service writes it) and its picture (a local file, read by the editor) */
static void
import_answered (JsonObject *root, const char *err, gpointer data)
{
  Import *import = data;
  g_autoptr(EContact) prefill = NULL;
  const char *channel, *id, *photo;

  if (err)
    {
      ct_window_toast (import->window, err);
      import_free (import);
      return;
    }
  ct_window_toast_hide (import->window);
  prefill = ct_contact_prefill (ns_json_str (root, "name"), ns_json_str (root, "phone"), NULL);
  channel = ns_json_str (root, "channel");
  id = ns_json_str (root, "channel_id");
  photo = ns_json_str (root, "photo");
  /* the channel's name; its id goes on the card with the channel's IMPP scheme */
  if (*channel && *id && im_form_usable (ct_mnv_im_form ()))
    ct_contact_set_channel (prefill, ct_mnv_channel (channel) ? ct_mnv_channel_scheme (ct_mnv_channel (channel)) : channel,
                            NULL, ct_mnv_im_form (), id);
  if (*photo)
    {
      g_autofree char *uri = g_filename_to_uri (photo, NULL, NULL);
      EContactPhoto picture = { .type = E_CONTACT_PHOTO_TYPE_URI };

      picture.data.uri = uri;
      if (uri)
        e_contact_set (prefill, E_CONTACT_PHOTO, &picture);
    }
  ct_window_new_contact (import->window, prefill);
  import_free (import);
}

/* Minerva's texts first: they say how a channel id is written on the card */
static void
import_ui_ready (JsonObject *root, const char *err, gpointer data)
{
  Import *import = data;

  if (err)
    {
      import_answered (NULL, err, import);
      return;
    }
  ct_window_toast_hold (import->window, ct_mnv_text ("import_busy"));
  ct_mnv_chat_contact (G_OBJECT (import->window), import->provider, import->chat, import_answered, import);
}

void
ct_mnv_import_chat (GtkWidget *window, const char *provider, const char *chat)
{
  Import *import = g_new0 (Import, 1);

  import->window = g_object_ref (window);
  import->provider = g_strdup (provider);
  import->chat = g_strdup (chat);
  ct_mnv_ui_load (G_OBJECT (window), import_ui_ready, import);
}

static void
on_import_picked (const char *provider, const char *chat, gpointer data)
{
  ct_mnv_import_chat (data, provider, chat);
}

void
ct_mnv_import_pick (GtkWidget *window, const char *provider)
{
  ct_window_push (window, ct_mnv_chats_new (window, provider, FALSE, on_import_picked, window, NULL));
}
