/* The Minerva screens of Contacts (ct-minerva-pages.h). */
#include "ct-minerva-pages.h"
#include "ct-contact.h"
#include "ct-dialogs.h"
#include "ct-mnv-core.h"
#include "ct-mnv.h"
#include "ct-page.h"
#include "ct-settings.h"
#include "ct-store.h"
#include "ct-ui.h"
#include "ct-window.h"
#include "ns-mc.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* ===== a page: the toolbar, the busy line and a scrolling column =============================================== */

typedef struct {
  GtkWidget *page, *window, *busy, *rows;
  JsonObject *known;            /* the last `people known` (a ref) */
  int ops;
  /* Sconosciuti */
  GtkWidget *note;
  /* Minerva */
  GtkWidget *rules_note;
} P;

static void
p_free (gpointer data)
{
  P *p = data;

  g_clear_pointer (&p->known, json_object_unref);
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

static void strangers_load (P *p);

static void
stranger_finished (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;

  busy_off (op->p);
  toast (op->p, err ? err : ct_mnv_text (op->done));
  strangers_load (op->p);
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
stranger_action (int response, gpointer data)
{
  Op *op = data;
  const char *const pairs[] = { "name", op->b, NULL };

  if (response == 0)
    {
      ct_text_dialog (op->p->window, ct_mnv_text ("add_heading"), op->b, ct_mnv_text ("add_hint"),
                      ct_mnv_int ("name_max_chars"), NULL, add_entered, op);
      return;
    }
  if (response == 1)
    {
      g_autofree char *heading = ct_mnv_textf ("forget_heading", pairs);
      g_autofree char *body = ct_mnv_textf ("forget_body", pairs);

      ct_confirm_dialog (op->p->window, heading, body, ct_mnv_text ("forget_ok"), forget_confirmed, op);
      return;
    }
  op_free (op);
}

static void
on_stranger (NeoPressable *row, gpointer data)
{
  P *p = data;
  const char *id = g_object_get_data (G_OBJECT (row), "ct-id"), *name = g_object_get_data (G_OBJECT (row), "ct-name");
  const char *const labels[] = { ct_mnv_text ("add"), ct_mnv_text ("forget") };

  ct_list_dialog (p->window, CT_DIALOG_APPCOMPAT, name, labels, 2, TRUE, stranger_action, op_new (p, id, name));
}

/* an entry's line in the list: kind • samples • sightings • last time */
static char *
stranger_sub (JsonObject *o)
{
  const char *kind = ct_mnv_map ("kind_one", ns_json_str (o, "kind"));
  gint64 samples = json_object_get_int_member_with_default (o, "samples", 0);
  gint64 sightings = json_object_get_int_member_with_default (o, "sightings", 0);
  g_autofree char *ns = g_strdup_printf ("%" G_GINT64_FORMAT, samples), *nv = g_strdup_printf ("%" G_GINT64_FORMAT, sightings);
  g_autofree char *date = ct_mnv_date (o, "last_seen", ct_mnv_text ("date_format"));
  g_autofree char *row = ct_mnv_textf ("stranger_row", (const char *const[]) { "samples", ns, "sightings", nv, NULL });
  g_autofree char *seen = date ? ct_mnv_textf ("last_seen", (const char *const[]) { "date", date, NULL })
                               : g_strdup (ct_mnv_text ("never_seen"));

  return g_strdup_printf ("%s%s%s • %s", kind, *kind ? " • " : "", row, seen);
}

static void
strangers_loaded (JsonObject *root, const char *err, gpointer data)
{
  P *p = data;
  g_autoptr (GPtrArray) loose = NULL;
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
  text = ct_mnv_textf ("stranger_note", (const char *const[]) { "days", days, NULL });
  note (p, text);
  loose = ct_mnv_loose (root);
  if (!loose->len)
    note (p, ct_mnv_text ("strangers_empty"));
  for (guint i = 0; i < loose->len; i++)
    {
      JsonObject *o = loose->pdata[i];
      g_autofree char *sub = stranger_sub (o);
      GtkWidget *row = ct_pref_row (ns_json_str (o, "name"), sub, NULL, G_CALLBACK (on_stranger), p);

      g_object_set_data_full (G_OBJECT (row), "ct-id", g_strdup (ns_json_str (o, "id")), g_free);
      g_object_set_data_full (G_OBJECT (row), "ct-name", g_strdup (ns_json_str (o, "name")), g_free);
      gtk_box_append (GTK_BOX (p->rows), row);
    }
}

static void
strangers_load (P *p)
{
  ct_mnv_known (G_OBJECT (p->page), strangers_loaded, p);
}

static void
on_strangers_map (GtkWidget *page, gpointer data)
{
  strangers_load (data);        /* back on the page (from the contact it opened, another app): as it is now */
}

GtkWidget *
ct_strangers_new (GtkWidget *window)
{
  P *p = page_new (window, ct_mnv_text ("strangers"), "ct-strangers");

  g_signal_connect (p->page, "map", G_CALLBACK (on_strangers_map), p);
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

  ct_window_push (p->window, ct_strangers_new (p->window));
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
  loose = ct_mnv_loose (root);
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
