/* Accounts > API keys: the API providers of the phone's agent and their keys. Which providers exist is data of
 * `minerva-config` (a shipped list plus the owner's own endpoints); this page names none of them. Everything is
 * read with `providers list --json` and written through `minerva-config` (ns-mc.c); a key travels on its standard
 * input (never in argv) and is shown only as "set, …last4" or "missing". */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <string.h>

typedef struct Ak Ak;
typedef struct Detail Detail;

struct Ak {
  AdwNavigationPage *page;
  GtkWidget  *msg;          /* the last result: saved, restarted, or the error */
  GtkWidget  *list;         /* the providers card, rebuilt on every refresh */
  JsonObject *data;         /* the last `providers list --json` */
  Detail     *detail;       /* the provider page that is open, to refresh in place */
  gboolean    busy;
  gboolean    scanning;     /* the camera was opened to scan a key and the list has not been read since */
  gboolean    watching;     /* the window's "is-active" is followed (once) */
};

/* a provider page: what it is, its key, and for the owner's own endpoints Edit and Delete */
struct Detail {
  Ak         *ak;
  char       *id;
  GtkWidget  *key_row, *remove_row, *type, *where, *does, *models;
  AdwNavigationPage *page;
};

static void ak_refresh (Ak *ak);

static void
say (Ak *ak, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (ak->msg), text);
  gtk_widget_set_visible (ak->msg, text && *text);
  if (error)
    gtk_widget_add_css_class (ak->msg, "ns-danger");
  else
    gtk_widget_remove_css_class (ak->msg, "ns-danger");
}

/* ---- the data ---- */

static JsonObject *
find_provider (Ak *ak, const char *id)
{
  JsonArray *a = ns_json_arr (ak->data, "providers");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *o = json_array_get_object_element (a, i);
    if (!g_strcmp0 (ns_json_str (o, "id"), id))
      return o;
  }
  return NULL;
}

static gboolean
is_builtin (JsonObject *p)
{
  return p && json_object_has_member (p, "builtin") && json_object_get_boolean_member (p, "builtin");
}

static gboolean
key_is_set (JsonObject *p)
{
  return g_str_has_prefix (ns_json_str (p, "key"), "set");
}

/* what the key row says: the masked key, or that there is none (for an endpoint of your own that is no fault) */
static char *
key_text (JsonObject *p)
{
  const char *k = ns_json_str (p, "key");
  if (key_is_set (p))
    return g_strdup (k);
  return g_strdup (is_builtin (p) ? "Missing" : "No key (optional)");
}

/* "language models • speech • dictation": the capability tags in words */
static char *
caps_text (JsonObject *p)
{
  JsonArray *a = ns_json_arr (p, "caps");
  g_autoptr (GString) s = g_string_new ("");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    const char *c = json_array_get_string_element (a, i);
    const char *w = !g_strcmp0 (c, "llm") ? "Language models" : !g_strcmp0 (c, "tts") ? "Speech" :
                    !g_strcmp0 (c, "stt") ? "Dictation" : c;
    g_string_append_printf (s, "%s%s", s->len ? ", " : "", w);
  }
  return g_strdup (s->len ? s->str : "None");
}

static const char *
kind_label (Ak *ak, JsonObject *p)
{
  const char *k = ns_json_str (p, "kind"), *l = ns_json_str (ns_json_obj (ak->data, "kind_labels"), k);
  return *l ? l : k;
}

/* ---- running a change ---- */

typedef struct {
  Ak       *ak;
  char     *key;      /* a key to store once the provider exists: only in memory, on its way to stdin */
  gboolean  pop;      /* go back to the list when it worked (a form) */
  GtkWidget *from;
} Op;

static void
op_free (Op *op)
{
  if (op->key) {
    memset (op->key, 0, strlen (op->key));
    g_free (op->key);
  }
  g_free (op);
}

static void
op_done (JsonObject *root, const char *err, gpointer data)
{
  Op *op = data;
  Ak *ak = op->ak;
  ak->busy = FALSE;
  if (err) {
    g_autofree char *t = g_strdup_printf ("Not saved: %s", err);
    say (ak, t, TRUE);
  } else {
    JsonObject *applied = ns_json_obj (root, "apply") ? ns_json_obj (root, "apply") : root;
    JsonArray *restarted = ns_json_arr (applied, "restarted");
    g_autoptr (GString) s = g_string_new ("Saved and applied.");
    for (guint i = 0; restarted && i < json_array_get_length (restarted); i++)
      g_string_append_printf (s, "%s%s", i ? ", " : " Restarted: ", json_array_get_string_element (restarted, i));
    say (ak, s->str, FALSE);
    if (op->pop && op->from)
      gtk_widget_activate_action (op->from, "navigation.pop", NULL);
  }
  ak_refresh (ak);
  op_free (op);
}

static void
op_run (Ak *ak, const char *const *args, const char *stdin_text, gboolean pop, GtkWidget *from)
{
  if (ak->busy) {
    say (ak, "Still applying the last change…", FALSE);
    return;
  }
  ak->busy = TRUE;
  say (ak, "Applying…", FALSE);
  Op *op = g_new0 (Op, 1);
  op->ak = ak;
  op->pop = pop;
  op->from = from;
  ns_mc_run (G_OBJECT (ak->page), args, stdin_text, op_done, op);
}

/* ---- a provider page ---- */

static void
detail_update (Detail *d)
{
  JsonObject *p = find_provider (d->ak, d->id);
  if (!p) {   /* deleted: the page is on its way out */
    return;
  }
  g_autofree char *k = key_text (p), *c = caps_text (p);
  ns_row_set_subtitle (d->key_row, k);
  gtk_widget_set_visible (d->remove_row, key_is_set (p));
  ns_row_set_subtitle (d->type, kind_label (d->ak, p));
  ns_row_set_subtitle (d->where, ns_json_str (p, "base_url"));
  ns_row_set_subtitle (d->does, c);
  if (d->models) {
    JsonArray *m = ns_json_arr (p, "models");
    g_autofree char *t = m && json_array_get_length (m) ? g_strdup_printf ("%u", json_array_get_length (m))
                                                          : g_strdup ("None listed");
    ns_row_set_subtitle (d->models, t);
  }
}

static void
key_entered (const char *text, gpointer data)
{
  Detail *d = data;
  if (!text || !*text)
    return;
  const char *const args[] = { "key", "set", d->id, "--apply", "--json", NULL };
  op_run (d->ak, args, text, FALSE, NULL);   /* the dialog's string is not kept; minerva-config strips and checks it */
}

static void
key_activated (GtkWidget *row, gpointer data)
{
  Detail *d = data;
  JsonObject *p = find_provider (d->ak, d->id);
  g_autofree char *heading = g_strdup_printf ("%s key", ns_json_str (p, "name"));
  ns_ask (row, heading, "Paste the key. It is stored on this phone, readable by you only, and never shown again.", "",
          TRUE, "Save", key_entered, d);
}

static void
key_removed (gboolean yes, gpointer data)
{
  Detail *d = data;
  if (!yes)
    return;
  const char *const args[] = { "key", "remove", d->id, "--apply", "--json", NULL };
  op_run (d->ak, args, NULL, FALSE, NULL);
}

static void
key_remove_clicked (GtkWidget *row, gpointer data)
{
  Detail *d = data;
  JsonObject *p = find_provider (d->ak, d->id);
  g_autofree char *heading = g_strdup_printf ("Remove the %s key?", ns_json_str (p, "name"));
  ns_confirm (row, heading, "The key is deleted from this phone. You can enter one again at any time.", "Remove", TRUE,
              key_removed, d);
}

static void
docs_open (GtkWidget *row, gpointer data)
{
  const char *url = data;
  const char *const argv[] = { "xdg-open", url, NULL };
  ns_spawn (argv);
}

static void provider_form (Ak *ak, const char *edit_id);

static void
edit_clicked (GtkWidget *row, gpointer data)
{
  Detail *d = data;
  provider_form (d->ak, d->id);
}

static void
deleted (gboolean yes, gpointer data)
{
  Detail *d = data;
  if (!yes)
    return;
  const char *const args[] = { "providers", "remove", d->id, "--apply", "--json", NULL };
  op_run (d->ak, args, NULL, TRUE, GTK_WIDGET (d->page));
}

static void
delete_clicked (GtkWidget *row, gpointer data)
{
  Detail *d = data;
  JsonObject *p = find_provider (d->ak, d->id);
  g_autofree char *heading = g_strdup_printf ("Delete %s?", ns_json_str (p, "name"));
  ns_confirm (row, heading, "The provider and its key are removed from this phone. Models that use it stop working.",
              "Delete", TRUE, deleted, d);
}

static void
detail_free (Detail *d)
{
  if (d->ak->detail == d)
    d->ak->detail = NULL;
  g_free (d->id);
  g_free (d);
}

static void
provider_open (GtkWidget *row, gpointer data)
{
  Ak *ak = data;
  const char *id = g_object_get_data (G_OBJECT (row), "ak-id");
  JsonObject *p = find_provider (ak, id);
  if (!p)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (ns_json_str (p, "name"), &c);
  Detail *d = g_new0 (Detail, 1);
  d->ak = ak;
  d->id = g_strdup (id);
  d->page = page;
  ak->detail = d;
  g_object_set_data_full (G_OBJECT (page), "ak-detail", d, (GDestroyNotify) detail_free);

  GtkWidget *g = ns_group (c, "Key");
  d->key_row = ns_row (g, NULL, NULL, "API key", "");
  ns_row_on_activate (d->key_row, key_activated, d);
  d->remove_row = ns_row_button (g, "Remove key", TRUE, key_remove_clicked, d);
  const char *docs = ns_json_str (p, "docs_url");
  if (*docs) {
    g_object_set_data_full (G_OBJECT (page), "ak-docs", g_strdup (docs), g_free);
    ns_row_button (g, "Get a key…", FALSE, docs_open, g_object_get_data (G_OBJECT (page), "ak-docs"));
  }

  g = ns_group (c, "Provider");
  d->type = ns_row_info (g, "API type", "");
  d->where = ns_row_info (g, "Endpoint", "");
  d->does = ns_row_info (g, "Used for", "");
  if (!is_builtin (p)) {
    d->models = ns_row_info (g, "Models", "");
    g = ns_group (c, NULL);
    ns_row_button (g, "Edit provider", FALSE, edit_clicked, d);
    ns_row_button (g, "Delete provider", TRUE, delete_clicked, d);
  } else {
    ns_note (c, "A shipped provider: its address cannot be changed here. Only the key is yours.");
  }
  detail_update (d);
  ns_push (page);
}

/* ---- the form: a new endpoint of your own, or editing one ---- */

typedef struct {
  Ak         *ak;
  char       *edit_id;     /* NULL: a new one */
  GtkWidget  *name, *where, *models, *key, *msg;
  GPtrArray  *kinds;       /* ids of the API types that can be added */
  guint       kind;
  AdwNavigationPage *page;
} Form;

static void
form_free (Form *f)
{
  g_free (f->edit_id);
  g_ptr_array_free (f->kinds, TRUE);
  g_free (f);
}

static void
kind_picked (GtkWidget *row, guint index, gpointer data)
{
  ((Form *) data)->kind = index;
}

static void
form_error (Form *f, const char *text)
{
  gtk_label_set_text (GTK_LABEL (f->msg), text);
  gtk_widget_set_visible (f->msg, TRUE);
}

typedef struct {
  Form *f;
  char *key;
} Two;

static void
added (JsonObject *root, const char *err, gpointer data)
{
  Two *t = data;
  Ak *ak = t->f->ak;
  ak->busy = FALSE;
  if (err || !*ns_json_str (root, "id")) {
    g_autofree char *m = g_strdup_printf ("Not saved: %s", err ? err : "no id");
    say (ak, m, TRUE);
    form_error (t->f, m);
  } else {
    const char *const args[] = { "key", "set", ns_json_str (root, "id"), "--apply", "--json", NULL };
    op_run (ak, args, t->key, TRUE, GTK_WIDGET (t->f->page));
  }
  memset (t->key, 0, strlen (t->key));
  g_free (t->key);
  g_free (t);
  ak_refresh (ak);
}

static void
form_submit (GtkWidget *row, gpointer data)
{
  Form *f = data;
  const char *name = ns_row_value_get (f->name), *where = ns_row_value_get (f->where),
             *models = ns_row_value_get (f->models), *key = ns_row_value_get (f->key);
  if (!*name || !*where) {
    form_error (f, "A name and an address are needed.");
    return;
  }
  const char *kind = f->kinds->pdata[f->kind];
  g_autoptr (GPtrArray) a = g_ptr_array_new ();
  g_ptr_array_add (a, (char *) "providers");
  g_ptr_array_add (a, (char *) (f->edit_id ? "edit" : "add"));
  if (f->edit_id)
    g_ptr_array_add (a, f->edit_id);
  const char *flags[] = { "--name", name, "--kind", kind, "--base-url", where, NULL };
  for (guint i = 0; flags[i]; i++)
    g_ptr_array_add (a, (char *) flags[i]);
  if (*models) {
    g_ptr_array_add (a, (char *) "--models");
    g_ptr_array_add (a, (char *) models);
  } else if (f->edit_id) {
    g_ptr_array_add (a, (char *) "--discover");   /* no list given: ask the endpoint */
  }
  g_ptr_array_add (a, (char *) "--apply");
  g_ptr_array_add (a, (char *) "--json");
  g_ptr_array_add (a, NULL);
  gtk_widget_set_visible (f->msg, FALSE);
  Ak *ak = f->ak;
  if (ak->busy) {
    form_error (f, "Still applying the last change…");
    return;
  }
  if (*key && !f->edit_id) {
    /* the key needs the provider's id: add first, then set the key from its answer */
    ak->busy = TRUE;
    say (ak, "Applying…", FALSE);
    Two *t = g_new0 (Two, 1);
    t->f = f;
    t->key = g_strdup (key);
    ns_mc_run (G_OBJECT (ak->page), (const char *const *) a->pdata, NULL, added, t);
    return;
  }
  op_run (ak, (const char *const *) a->pdata, NULL, TRUE, GTK_WIDGET (f->page));
}

static void
provider_form (Ak *ak, const char *edit_id)
{
  JsonObject *p = edit_id ? find_provider (ak, edit_id) : NULL;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (edit_id ? "Edit provider" : "Add provider", &c);
  Form *f = g_new0 (Form, 1);
  f->ak = ak;
  f->edit_id = g_strdup (edit_id);
  f->kinds = g_ptr_array_new_with_free_func (g_free);
  f->page = page;
  g_object_set_data_full (G_OBJECT (page), "ak-form", f, (GDestroyNotify) form_free);

  f->msg = ns_note (c, "");
  gtk_widget_set_visible (f->msg, FALSE);
  gtk_widget_add_css_class (f->msg, "ns-danger");
  gtk_widget_set_margin_top (f->msg, 0);

  GtkWidget *g = ns_group (c, NULL);
  f->name = ns_row_value (g, "Name", p ? ns_json_str (p, "name") : "", FALSE, NULL, NULL);

  /* the API types that can be added: data of minerva-config */
  JsonArray *kinds = ns_json_arr (ak->data, "kinds");
  g_autoptr (GPtrArray) labels = g_ptr_array_new ();
  const char *cur = p ? ns_json_str (p, "kind") : "";
  for (guint i = 0; kinds && i < json_array_get_length (kinds); i++) {
    JsonObject *k = json_array_get_object_element (kinds, i);
    if (!json_object_get_boolean_member (k, "addable"))
      continue;
    if (!g_strcmp0 (ns_json_str (k, "id"), cur))
      f->kind = f->kinds->len;
    g_ptr_array_add (f->kinds, g_strdup (ns_json_str (k, "id")));
    g_ptr_array_add (labels, (char *) ns_json_str (k, "label"));
  }
  g_ptr_array_add (labels, NULL);
  ns_row_pick (g, "API type", (const char *const *) labels->pdata, f->kind, kind_picked, f);
  f->where = ns_row_value (g, "Address", p ? ns_json_str (p, "base_url") : "", FALSE, NULL, NULL);
  g_autoptr (GString) ms = g_string_new ("");
  JsonArray *ma = ns_json_arr (p, "models");
  for (guint i = 0; ma && i < json_array_get_length (ma); i++)
    g_string_append_printf (ms, "%s%s", i ? ", " : "", json_array_get_string_element (ma, i));
  f->models = ns_row_value (g, "Models", ms->str, FALSE, NULL, NULL);
  if (!edit_id)
    f->key = ns_row_value (g, "API key (optional)", "", TRUE, NULL, NULL);
  ns_note (c, "The address is the API root, for example http://my-pc:8080/v1. Models are found by asking the address "
              "when the list is left empty. A provider that can chat can then be picked as Minerva's model. A key "
              "can be added or changed after saving.");

  g = ns_group (c, NULL);
  ns_row_button (g, edit_id ? "Save" : "Add provider", FALSE, form_submit, f);
  ns_push (page);
}

static void
add_clicked (GtkWidget *row, gpointer data)
{
  provider_form (data, NULL);
}

/* ---- import from a QR code ----
 * The camera does the scanning and the import (neo-camera --scan hands a "minerva-key:" code to
 * `minerva-config key import`); this page only opens it and reads the list again when it is done. */

/* the camera's process ended: a key may have been saved */
static void
camera_exited (GObject *src, GAsyncResult *res, gpointer data)
{
  AdwNavigationPage *page = data;
  Ak *ak = g_object_get_data (G_OBJECT (page), "ak");
  g_autoptr (GError) e = NULL;
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, &e);
  if (ak) {
    ak->scanning = FALSE;
    ak_refresh (ak);
  }
  g_object_unref (page);
}

/* a camera that was running already hands the scan over and exits at once: read the list when the window is
 * back in front instead */
static void
window_activity (GObject *window, GParamSpec *pspec, gpointer data)
{
  Ak *ak = g_object_get_data (G_OBJECT (data), "ak");   /* the page: the signal is tied to its life */
  if (ak && gtk_window_is_active (GTK_WINDOW (window)) && ak->scanning) {
    ak->scanning = FALSE;
    ak_refresh (ak);
  }
}

static void
import_clicked (GtkWidget *row, gpointer data)
{
  Ak *ak = data;
  g_autoptr (GError) err = NULL;
  GSubprocess *p = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &err, "neo-camera", "--scan", NULL);
  if (!p) {
    g_autofree char *t = g_strdup_printf ("Could not open the camera: %s", err->message);
    say (ak, t, TRUE);
    return;
  }
  say (ak, "", FALSE);
  ak->scanning = TRUE;
  GtkRoot *root = gtk_widget_get_root (row);
  if (!ak->watching && GTK_IS_WINDOW (root)) {
    ak->watching = TRUE;
    g_signal_connect_object (root, "notify::is-active", G_CALLBACK (window_activity), ak->page, 0);
  }
  g_subprocess_wait_async (p, NULL, camera_exited, g_object_ref (ak->page));
  g_object_unref (p);
}

/* ---- the list ---- */

static void
list_fill (Ak *ak)
{
  ns_group_clear (ak->list);
  JsonArray *a = ns_json_arr (ak->data, "providers");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *p = json_array_get_object_element (a, i);
    g_autofree char *k = key_text (p), *c = caps_text (p);
    g_autofree char *sub = g_strdup_printf ("%s • %s", c, k);
    GtkWidget *row = ns_row (ak->list, NULL, NULL, ns_json_str (p, "name"), sub);
    g_object_set_data_full (G_OBJECT (row), "ak-id", g_strdup (ns_json_str (p, "id")), g_free);
    ns_row_on_activate (row, provider_open, ak);
  }
}

static void
list_done (JsonObject *root, const char *err, gpointer data)
{
  Ak *ak = data;
  if (err || !root) {
    g_autofree char *t = g_strdup_printf ("Could not read the providers: %s", err ? err : "no answer");
    say (ak, t, TRUE);
    return;
  }
  if (ak->data)
    json_object_unref (ak->data);
  ak->data = json_object_ref (root);
  list_fill (ak);
  if (ak->detail)
    detail_update (ak->detail);
}

static void
ak_refresh (Ak *ak)
{
  static const char *const args[] = { "providers", "list", "--json", NULL };
  ns_mc_run (G_OBJECT (ak->page), args, NULL, list_done, ak);
}

static void
ak_free (Ak *ak)
{
  if (ak->data)
    json_object_unref (ak->data);
  g_free (ak);
}

void
ns_page_api_keys (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("API keys", &c);
  Ak *ak = g_new0 (Ak, 1);
  ak->page = page;
  g_object_set_data_full (G_OBJECT (page), "ak", ak, (GDestroyNotify) ak_free);

  ak->msg = ns_note (c, "");
  gtk_widget_set_visible (ak->msg, FALSE);
  gtk_widget_set_margin_top (ak->msg, 0);

  GtkWidget *scan = ns_group (c, NULL);
  GtkWidget *import = ns_row (scan, "camera-photo-symbolic", "blue", "Import from QR code", "Scan a key from another device");
  ns_row_on_activate (import, import_clicked, ak);

  ak->list = ns_group (c, "Providers");
  ns_note (c, "Tap a provider to set or remove its key. A key is never shown again, only its last four characters. "
              "Providers that can chat offer their models to Minerva's brain and fallbacks.");
  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Add provider", FALSE, add_clicked, ak);
  ns_note (c, "Add an endpoint of your own, such as a model running on your computer.");
  ak_refresh (ak);
  ns_push (page);
}
