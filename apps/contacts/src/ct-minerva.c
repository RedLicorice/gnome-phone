/* QuickContact's Minerva card (ct-minerva.h). */
#include "ct-minerva.h"
#include "ct-dialogs.h"
#include "ct-page.h"
#include "ct-ui.h"
#include "ct-window.h"

#include "ns-mc.h"

#include <glib/gi18n.h>
#include <neo.h>

typedef struct {
  GtkWidget *card, *window;
  char *uid, *name;             /* the contact */
  guint32 tint;                 /* the card title's colour */
  JsonObject *ui;               /* the "contacts" part of `people ui` (a ref), NULL until it answers */
  char *pid, *pname, *text;     /* the linked person and her notebook; pid NULL when nobody is linked */
  char *shown;                  /* what the body shows now, not to build the same again (and lose the scroll) */
  gboolean busy, again;         /* a load running, and another wanted after it */
  guint poll;
  GtkWidget *title, *body;
  GPtrArray *pick;              /* the people's ids the picker lists, while it is open */
} M;

static JsonObject *ui_cache;    /* `people ui` once per process: the texts do not change under a running app */

static void load (M *m);

/* ---- the data ---- */

static const char *
s (M *m, const char *key)
{
  return ns_json_str (m->ui, key);
}

static int
n (M *m, const char *key)
{
  return m->ui ? (int) json_object_get_int_member_with_default (m->ui, key, 0) : 0;
}

static char *
fmt (const char *pattern, const char *name)
{
  GString *out = g_string_new (pattern);

  g_string_replace (out, "{name}", name ? name : "", 0);
  return g_string_free (out, FALSE);
}

/* why an answer is no answer: the program failed, or minerva-config refused ({"ok": false, "error": ...}) */
static const char *
refused (M *m, JsonObject *root, const char *err)
{
  const char *why;

  if (err)
    return err;
  if (root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    return NULL;
  why = ns_json_str (root, "error");
  return *why ? why : s (m, "error");
}

static void
toast (M *m, const char *text)
{
  if (text && *text)
    ct_window_toast (m->window, text);
}

/* ---- the card ---- */

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

/* ---- Edit: the notebook in Neo Prompt, on this person's document ---- */

static void
on_edit (GtkWidget *button, gpointer data)
{
  M *m = data;

  if (m->pid)
    ns_prompt_open ("notebook", m->pid);
}

/* ---- Apri in Persone: Neo Settings' launch-panel action, as gnome-phone's panels ask for a page ---- */

static void
settings_opened (GObject *bus, GAsyncResult *res, gpointer data)
{
  GObject *card = data;  /* the reference on_open_people took */
  M *m = g_object_get_data (card, "ct-minerva");
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) error = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (bus), res, &error);
  if (!reply && m)
    {
      g_message ("Settings' %s: %s", s (m, "settings_action"), error->message);
      toast (m, s (m, "open_failed"));
    }
  g_object_unref (card);
}

static void
on_open_people (GtkWidget *button, gpointer data)
{
  M *m = data;
  const char *app = s (m, "settings_app");
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());
  g_autofree char *path = NULL;
  GVariantBuilder parameters;

  if (!bus || !g_application_id_is_valid (app))
    {
      toast (m, s (m, "open_failed"));
      return;
    }
  /* an application's object path is its id with the dots as slashes (GApplication's convention) */
  path = g_strconcat ("/", app, NULL);
  g_strdelimit (path, ".", '/');
  g_strdelimit (path, "-", '_');
  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&parameters, "v", g_variant_new ("(s@av)", s (m, "settings_page"),
                                                          g_variant_new_array (G_VARIANT_TYPE_VARIANT, NULL, 0)));
  g_dbus_connection_call (bus, app, path, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", s (m, "settings_action"), &parameters, NULL), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, settings_opened, g_object_ref (m->card));
}

/* ---- Collega a una persona: the people, and Nuova persona ---- */

static void
linked (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  const char *why = refused (m, root, err);
  g_autofree char *text = NULL;

  if (why)
    {
      toast (m, why);
      return;
    }
  text = fmt (s (m, "linked"), ns_json_str (root, "name"));
  toast (m, text);
  load (m);
}

static void
link_to (M *m, const char *pid)
{
  const char *const args[] = { "people", "link", pid, m->uid, "--json", NULL };

  ns_mc_run (G_OBJECT (m->card), args, NULL, linked, m);
}

static void
created (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  const char *why = refused (m, root, err);

  if (why)
    toast (m, why);
  else
    link_to (m, ns_json_str (root, "id"));
}

static void
picked (int response, gpointer data)
{
  M *m = data;

  if (m->pick && response >= 0 && (guint) response < m->pick->len)
    link_to (m, m->pick->pdata[response]);
  else if (m->pick && response >= 0 && (guint) response == m->pick->len && m->name && *m->name)
    {
      const char *const args[] = { "people", "create", m->name, "--json", NULL };

      ns_mc_run (G_OBJECT (m->card), args, NULL, created, m);
    }
  g_clear_pointer (&m->pick, g_ptr_array_unref);
  g_object_unref (m->card);     /* the dialog's: last, it may be the card's end */
}

static void
people_listed (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  const char *why = refused (m, root, err);
  JsonArray *people;
  guint count;
  g_autoptr (GPtrArray) labels = NULL;

  if (why)
    {
      toast (m, why);
      return;
    }
  people = ns_json_arr (root, "people");
  count = people ? json_array_get_length (people) : 0;
  labels = g_ptr_array_new ();
  g_clear_pointer (&m->pick, g_ptr_array_unref);
  m->pick = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < count; i++)
    {
      JsonObject *o = json_array_get_object_element (people, i);

      if (!o || !*ns_json_str (o, "id"))
        continue;
      g_ptr_array_add (m->pick, g_strdup (ns_json_str (o, "id")));
      g_ptr_array_add (labels, (gpointer) ns_json_str (o, "name"));
    }
  g_ptr_array_add (labels, (gpointer) s (m, "new_person"));
  g_object_ref (m->card);       /* until the dialog answers */
  ct_list_dialog (m->window, CT_DIALOG_APPCOMPAT, s (m, "pick_title"), (const char *const *) labels->pdata,
                  (int) labels->len, TRUE, picked, m);
}

static void
on_link (GtkWidget *row, gpointer data)
{
  M *m = data;
  static const char *const args[] = { "people", "list", "--json", NULL };

  ns_mc_run (G_OBJECT (m->card), args, NULL, people_listed, m);
}

/* ---- the body: the notebook, or the row to link ---- */

static GtkWidget *
action_button (const char *label, GCallback clicked, M *m)
{
  GtkWidget *button = ct_text_button (label, "ct-qc-minerva-action");

  g_signal_connect (button, "clicked", clicked, m);
  return button;
}

static void
linked_body (M *m)
{
  GtkWidget *scroll, *label, *buttons;
  g_autofree char *who = fmt (s (m, "person"), m->pname);
  g_autofree char *stripped = g_strstrip (g_strdup (m->text ? m->text : ""));
  gboolean empty = !*stripped;  /* the text itself shows as it is */

  label = text_label (who, "ct-qc-entry-sub", CT_QC_SUB_SP);
  pad (label);
  gtk_widget_set_margin_top (label, (int) CT_QC_ENTRY_PAD_TOP_DP);
  gtk_box_append (GTK_BOX (m->body), label);

  label = text_label (empty ? s (m, "notebook_empty") : m->text, "ct-qc-entry-text", CT_QC_SUB_SP);
  pad (label);
  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), n (m, "max_height_dp"));
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), label);
  gtk_widget_set_margin_top (scroll, (int) CT_QC_ENTRY_PAD_TOP_DP);
  gtk_box_append (GTK_BOX (m->body), scroll);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  pad (buttons);
  gtk_widget_set_margin_bottom (buttons, (int) CT_QC_ENTRY_PAD_END_DP);
  gtk_box_append (GTK_BOX (buttons), action_button (s (m, "open_people"), G_CALLBACK (on_open_people), m));
  gtk_box_append (GTK_BOX (buttons), action_button (s (m, "edit"), G_CALLBACK (on_edit), m));
  gtk_box_append (GTK_BOX (m->body), buttons);
}

static void
unlinked_body (M *m)
{
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = neo_pressable_new (lines);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_FRAMEWORK);

  gtk_box_append (GTK_BOX (lines), text_label (s (m, "link"), "ct-qc-entry-header", CT_QC_HEADER_SP));
  if (*s (m, "link_note"))
    gtk_box_append (GTK_BOX (lines), text_label (s (m, "link_note"), "ct-qc-entry-sub", CT_QC_SUB_SP));
  pad (lines);
  gtk_widget_set_margin_top (lines, (int) CT_QC_ENTRY_PAD_TOP_DP);
  gtk_widget_set_margin_bottom (lines, (int) CT_QC_ENTRY_PAD_BOTTOM_DP);
  gtk_widget_add_css_class (row, "ct-qc-entry");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, s (m, "link"), -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_link), m);
  gtk_box_append (GTK_BOX (m->body), row);
}

static void
render (M *m)
{
  g_autofree char *now = m->pid ? g_strconcat (m->pid, "\n", m->pname, "\n", m->text ? m->text : "", NULL)
                                : g_strdup ("");
  GtkWidget *child;

  if (m->shown && g_str_equal (now, m->shown))
    return;
  g_free (m->shown);
  m->shown = g_steal_pointer (&now);
  while ((child = gtk_widget_get_first_child (m->body)))
    gtk_box_remove (GTK_BOX (m->body), child);
  set_title (m, m->pid ? s (m, "notebook") : s (m, "title"));
  if (m->pid)
    linked_body (m);
  else
    unlinked_body (m);
  gtk_widget_set_visible (m->card, TRUE);
}

/* ---- loading: `people by-contact`, then `people notebook get` ---- */

static void
load_done (M *m)
{
  m->busy = FALSE;
  if (m->again)
    {
      m->again = FALSE;
      load (m);
    }
}

static void
notebook_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;

  if (!refused (m, root, err))
    {
      g_free (m->text);
      m->text = g_strdup (ns_json_str (root, "text"));
      render (m);
    }
  load_done (m);
}

static void
person_found (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  JsonObject *who;

  if (refused (m, root, err))
    {
      load_done (m);            /* keeps what it shows (hidden before the first answer) */
      return;
    }
  who = ns_json_obj (root, "person");
  g_clear_pointer (&m->pid, g_free);
  g_clear_pointer (&m->pname, g_free);
  if (!who || !*ns_json_str (who, "id"))
    {
      g_clear_pointer (&m->text, g_free);
      render (m);
      load_done (m);
      return;
    }
  m->pid = g_strdup (ns_json_str (who, "id"));
  m->pname = g_strdup (ns_json_str (who, "name"));
  {
    const char *const args[] = { "people", "notebook", m->pid, "get", "--json", NULL };

    ns_mc_run (G_OBJECT (m->card), args, NULL, notebook_loaded, m);
  }
}

static void
load (M *m)
{
  if (!m->ui)
    return;
  if (m->busy)
    {
      m->again = TRUE;
      return;
    }
  m->busy = TRUE;
  {
    const char *const args[] = { "people", "by-contact", m->uid, "--json", NULL };

    ns_mc_run (G_OBJECT (m->card), args, NULL, person_found, m);
  }
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
  if (!m->poll && m->ui && n (m, "poll_s") > 0)
    m->poll = g_timeout_add_seconds ((guint) n (m, "poll_s"), poll_tick, m);
}

static void
on_map (GtkWidget *card, gpointer data)
{
  M *m = data;

  poll_start (m);
  if (!m->busy)                 /* back on the card (from the editor, another app): as it is now */
    load (m);
}

static void
on_unmap (GtkWidget *card, gpointer data)
{
  M *m = data;

  g_clear_handle_id (&m->poll, g_source_remove);
}

static void
set_ui (M *m, JsonObject *ui)
{
  m->ui = json_object_ref (ui);
  if (gtk_widget_get_mapped (m->card))
    poll_start (m);
  load (m);
}

static void
ui_loaded (JsonObject *root, const char *err, gpointer data)
{
  M *m = data;
  JsonObject *contacts = refused (m, root, err) ? NULL : ns_json_obj (root, "contacts");

  if (!contacts)
    return;                     /* no Minerva people here: no card */
  if (!ui_cache)
    ui_cache = json_object_ref (contacts);
  set_ui (m, ui_cache);
}

static void
m_free (gpointer data)
{
  M *m = data;

  g_clear_handle_id (&m->poll, g_source_remove);
  g_clear_pointer (&m->ui, json_object_unref);
  g_clear_pointer (&m->pick, g_ptr_array_unref);
  g_free (m->uid);
  g_free (m->name);
  g_free (m->pid);
  g_free (m->pname);
  g_free (m->text);
  g_free (m->shown);
  g_free (m);
}

GtkWidget *
ct_minerva_card_new (GtkWidget *window, const char *uid)
{
  M *m;

  if (!uid || !*uid || !ns_minerva_available ())
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
  m->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (m->card), m->body);
  g_object_set_data_full (G_OBJECT (m->card), "ct-minerva", m, m_free);
  g_signal_connect (m->card, "map", G_CALLBACK (on_map), m);
  g_signal_connect (m->card, "unmap", G_CALLBACK (on_unmap), m);
  if (ui_cache)
    set_ui (m, ui_cache);
  else
    {
      static const char *const args[] = { "people", "ui", "--json", NULL };

      ns_mc_run (G_OBJECT (m->card), args, NULL, ui_loaded, m);
    }
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
  if (m->ui && m->shown)
    set_title (m, m->pid ? s (m, "notebook") : s (m, "title"));
}
