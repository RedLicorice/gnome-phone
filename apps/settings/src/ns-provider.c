/* Minerva > Messaggi, and one page per messaging provider. Every text is data (messaging_ui.json, through
 * `minerva-config messaging ui`), as are the provider ids, their labels, the scopes and their order (messaging.conf),
 * and every change goes through `minerva-config messaging … --json`, which writes messaging.json. The page never
 * names a provider: the index draws what `messaging list --json` lists. */
#include "ns-pages.h"
#include "ns-provider.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <string.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

static gboolean
ok_of (JsonObject *root)
{
  return root && json_object_get_boolean_member_with_default (root, "ok", FALSE);
}

static void
say (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
  if (error)
    gtk_widget_add_css_class (label, "ns-danger");
  else
    gtk_widget_remove_css_class (label, "ns-danger");
}

static const char *
bad_of (JsonObject *root, const char *err, JsonObject *ui)
{
  if (err)
    return err;
  if (!root || !ok_of (root))
    return root ? str_of (root, "error") : str_of (obj_of (ui, "labels"), "error");
  return NULL;
}

/* the data's words for a provider of that pairing kind in that state: messaging_ui.json "pairing_kinds" > kind >
 * "state:label" (the daemon's own label, "offline:token mancante"), else "state". Members: status (the state's
 * name), hint (what the owner does now), action (qr | scan | token), button (the action's row), wait (before the
 * code is there) */
static JsonObject *
entry_of (JsonObject *ui, const char *kind, const char *state, const char *label)
{
  JsonObject *k = obj_of (obj_of (ui, "pairing_kinds"), kind ? kind : "");
  g_autofree char *key = g_strdup_printf ("%s:%s", state ? state : "", label ? label : "");
  JsonObject *e = obj_of (k, key);
  return e ? e : obj_of (k, state ? state : "");
}

/* what a status answer says, in the data's words: the state ("Collegato"), and the daemon's own label after it
 * ("token mancante"). A service that does not answer is "not running", whatever the program said */
static char *
status_text (JsonObject *root, const char *err, JsonObject *ui, const char *kind)
{
  JsonObject *labels = obj_of (ui, "labels");
  if (err || !root || !ok_of (root))
    return g_strdup (str_of (labels, "not_running"));
  const char *state = str_of (root, "state");
  if (!*state)
    state = str_of (root, "status");
  const char *label = str_of (root, "label");
  const char *word = str_of (entry_of (ui, kind, state, label), "status");
  if (!*word)
    word = *state ? str_of (obj_of (ui, "states"), state) : "";
  if (!*word)
    word = *state ? state : str_of (labels, "status_unknown");
  const char *said = str_of (obj_of (ui, "words"), label);
  if (*said)
    label = said;
  return *label ? g_strdup_printf ("%s \xc2\xb7 %s", word, label) : g_strdup (word);
}

/* ---- the index: one row per provider ---- */

typedef struct {
  char      *id, *kind;
  GtkWidget *row;
} ProviderRow;

typedef struct {
  GObject           *owner;
  AdwNavigationPage *page;
  GtkWidget         *content, *msg, *group;
  JsonObject        *ui;
  GPtrArray         *rows;   /* ProviderRow* */
} Index;

static void
provider_row_free (ProviderRow *r)
{
  g_free (r->id);
  g_free (r->kind);
  g_free (r);
}

static void
index_free (Index *ix)
{
  g_ptr_array_free (ix->rows, TRUE);
  if (ix->ui)
    json_object_unref (ix->ui);
  g_free (ix);
}

static void
index_open (GtkWidget *row, gpointer data)
{
  const char *id = g_object_get_data (G_OBJECT (row), "mp-id");
  ns_page_provider (row, (gpointer) id);
}

static void
index_status_done (JsonObject *root, const char *err, gpointer data)
{
  ProviderRow *r = data;
  Index *ix = g_object_get_data (G_OBJECT (r->row), "mp-index");
  g_autofree char *text = status_text (root, err, ix->ui, r->kind);
  ns_row_set_subtitle (r->row, text);
}

static void
index_loaded (JsonObject *root, const char *err, gpointer data)
{
  Index *ix = data;
  const char *bad = bad_of (root, err, ix->ui);
  if (bad) {
    say (ix->msg, bad, TRUE);
    return;
  }
  JsonArray *providers = arr_of (root, "providers");
  for (guint i = 0; providers && i < json_array_get_length (providers); i++) {
    JsonObject *o = json_array_get_object_element (providers, i);
    const char *id = str_of (o, "id");
    if (!*id || (json_object_has_member (o, "page") && !json_object_get_boolean_member (o, "page")))
      continue;
    ProviderRow *r = g_new0 (ProviderRow, 1);
    r->id = g_strdup (id);
    r->kind = g_strdup (str_of (o, "pairing"));
    r->row = ns_row (ix->group, "chat-message-new-symbolic", "green", str_of (o, "label"), "");
    g_object_set_data (G_OBJECT (r->row), "mp-index", ix);
    g_object_set_data_full (G_OBJECT (r->row), "mp-id", g_strdup (id), g_free);
    ns_row_on_activate (r->row, index_open, ix);
    g_ptr_array_add (ix->rows, r);
    const char *args[] = { "messaging", "status", r->id, "--json", NULL };
    ns_mc_run (ix->owner, args, NULL, index_status_done, r);
  }
  if (ix->rows->len == 0)
    ns_row (ix->group, NULL, NULL, str_of (obj_of (ix->ui, "labels"), "no_providers"), NULL);
}

static void
index_ui_done (JsonObject *root, const char *err, gpointer data)
{
  Index *ix = data;
  if (err || !ok_of (root)) {
    say (ix->msg, err ? err : str_of (obj_of (ix->ui, "labels"), "error"), TRUE);
    return;
  }
  ix->ui = json_object_ref (root);
  const char *title = str_of (root, "title");
  if (*title)
    ns_page_set_title (ix->page, title);
  ix->group = ns_group (ix->content, str_of (obj_of (root, "sections"), "providers"));
  const char *args[] = { "messaging", "list", "--json", NULL };
  ns_mc_run (ix->owner, args, NULL, index_loaded, ix);
}

void
ns_page_messaging (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("", &c);
  Index *ix = g_new0 (Index, 1);
  ix->owner = G_OBJECT (page);
  ix->page = page;
  ix->content = c;
  ix->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) provider_row_free);
  g_object_set_data_full (G_OBJECT (page), "mp-index", ix, (GDestroyNotify) index_free);
  ix->msg = ns_note (c, "");
  gtk_widget_set_visible (ix->msg, FALSE);
  const char *args[] = { "messaging", "ui", "--json", NULL };
  ns_mc_run (ix->owner, args, NULL, index_ui_done, ix);
  ns_push (page);
}

/* ---- one provider ---- */

typedef struct {
  GObject           *owner;
  AdwNavigationPage *page;
  GtkWidget         *content, *msg;
  char              *id, *label;
  JsonObject        *ui;
  GtkWidget         *status_g, *status_row, *pair_g, *qr_row, *picture, *hint, *wait_row, *scan_row, *token_row, *retry_row;
  GtkWidget         *read_row, *write_row, *unread_row, *ask_row, *whitelist_row;
  GBytes            *qr_bytes;    /* what it held: a new one only when the bytes change */
  GStrv              order;       /* the scopes, in the order the pickers show them */
  GStrv              scope_names; /* their labels, in that order */
  GStrv              whitelist;
  guint              poll_id, poll_s, stuck_s;
  gint64             idle_since;  /* monotonic µs: since when the page has seen neither a code nor a connection */
  char              *kind;        /* the pairing kind of messaging.conf ("" when the provider does not pair) */
  char              *state, *word; /* the last status: its state and the daemon's own label */
  gboolean           scanning;
} Prov;

static void prov_refresh_status (Prov *p);
static void prov_load_list (Prov *p);

static void
prov_free (Prov *p)
{
  if (p->poll_id)
    g_source_remove (p->poll_id);
  if (p->qr_bytes)
    g_bytes_unref (p->qr_bytes);
  if (p->ui)
    json_object_unref (p->ui);
  g_free (p->id);
  g_free (p->label);
  g_free (p->kind);
  g_free (p->state);
  g_free (p->word);
  g_strfreev (p->order);
  g_strfreev (p->scope_names);
  g_strfreev (p->whitelist);
  g_free (p);
}

static const char *
prov_text (Prov *p, const char *section, const char *key)
{
  return str_of (obj_of (p->ui, section), key);
}

static void
prov_saved (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  say (p->msg, bad ? bad : prov_text (p, "labels", "saved"), bad != NULL);
  if (!bad)
    prov_refresh_status (p);
}

/* ---- the pairing section ----
 * What it shows is data: the entry of messaging_ui.json for this kind of provider in the state it is in (entry_of).
 * action "qr": the provider draws a code and the page shows it; "scan": the owner shows the phone's own camera a
 * code the bot sent; "token": the bot has no token yet. */

static JsonObject *
prov_entry (Prov *p)
{
  return entry_of (p->ui, p->kind, p->state, p->word);
}

static gboolean
prov_pairs (Prov *p)
{
  return p->kind && *p->kind && g_strcmp0 (p->kind, "none");
}

/* the code as a picture, decoded from memory: the string it stands for is a pairing secret and is never written to a file */
static GdkTexture *
qr_texture (GBytes *bytes)
{
  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_bytes (bytes);
  g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream (in, NULL, NULL);
  if (!pixbuf)
    return NULL;
  g_autoptr (GdkPixbuf) rgba = gdk_pixbuf_get_has_alpha (pixbuf) ? g_object_ref (pixbuf)
                                                                 : gdk_pixbuf_add_alpha (pixbuf, FALSE, 0, 0, 0);
  g_autoptr (GBytes) pixels = g_bytes_new (gdk_pixbuf_read_pixels (rgba), gdk_pixbuf_get_byte_length (rgba));
  return gdk_memory_texture_new (gdk_pixbuf_get_width (rgba), gdk_pixbuf_get_height (rgba), GDK_MEMORY_R8G8B8A8,
                                 pixels, gdk_pixbuf_get_rowstride (rgba));
}

static void
qr_forget (Prov *p)
{
  if (p->qr_bytes)
    g_bytes_unref (p->qr_bytes);
  p->qr_bytes = NULL;
}

static void
qr_draw (Prov *p, const char *png_b64, const char *svg)
{
  gsize len = 0;
  g_autofree guchar *raw = *png_b64 ? g_base64_decode (png_b64, &len) : NULL;
  g_autoptr (GBytes) fresh = raw ? g_bytes_new (raw, len) : (*svg ? g_bytes_new (svg, strlen (svg)) : NULL);
  if (!fresh) {
    qr_forget (p);   /* an expired or missing code is never left on the screen: it would be scanned in vain */
    return;
  }
  if (p->qr_bytes && g_bytes_equal (p->qr_bytes, fresh))
    return;
  g_autoptr (GdkTexture) texture = qr_texture (fresh);
  if (!texture)
    return;
  GtkWidget *old = p->picture;
  p->picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
  gtk_picture_set_content_fit (GTK_PICTURE (p->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (p->picture, 240, 240);
  if (!p->qr_row) {
    p->qr_row = ns_row_widget (p->pair_g, prov_text (p, "labels", "pairing_title"), p->picture);
  } else {
    GtkWidget *box = gtk_widget_get_parent (old);
    gtk_box_remove (GTK_BOX (box), old);
    gtk_box_append (GTK_BOX (box), p->picture);
  }
  qr_forget (p);
  p->qr_bytes = g_bytes_ref (fresh);
  p->idle_since = g_get_monotonic_time ();
}

static void prov_refresh_status (Prov *p);

static void
token_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (prov_text (p, "labels", "token_title"), &c);
  ns_messaging_token_add (c, page);
  ns_push (page);
}

static gboolean
prov_stuck (Prov *p)
{
  return !p->qr_bytes && p->stuck_s && (g_get_monotonic_time () - p->idle_since) / G_USEC_PER_SEC >= p->stuck_s;
}

static void
retry_done (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  say (p->msg, bad ? bad : prov_text (p, "labels", "retrying"), bad != NULL);
  p->idle_since = g_get_monotonic_time ();
  prov_refresh_status (p);
}

/* "Riprova": the provider drops its connection and starts a clean one now; the session stays */
static void
retry_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  const char *args[] = { "messaging", "reconnect", p->id, "--json", NULL };
  say (p->msg, prov_text (p, "labels", "retrying"), FALSE);
  p->idle_since = g_get_monotonic_time ();
  ns_mc_run (p->owner, args, NULL, retry_done, p);
}

/* the camera's process ended: the pairing may be done (a camera that was running already hands the scan over and
 * exits at once; the poll catches that up) */
static void
camera_exited (GObject *src, GAsyncResult *res, gpointer data)
{
  AdwNavigationPage *page = data;
  Prov *p = g_object_get_data (G_OBJECT (page), "mp-prov");
  g_autoptr (GError) e = NULL;
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, &e);
  if (p) {
    p->scanning = FALSE;
    prov_refresh_status (p);
  }
  g_object_unref (page);
}

static void
scan_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  g_autoptr (GError) err = NULL;
  if (p->scanning)
    return;
  GSubprocess *proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &err, "neo-camera", "--scan", NULL);
  if (!proc) {
    say (p->msg, err->message, TRUE);
    return;
  }
  say (p->msg, "", FALSE);
  p->scanning = TRUE;
  g_subprocess_wait_async (proc, NULL, camera_exited, g_object_ref (p->page));
  g_object_unref (proc);
}

static void
row_show (GtkWidget *row, gboolean on)
{
  if (row)
    gtk_widget_set_visible (row, on);
}

/* the section as the data says it is for the state the provider is in */
static void
prov_apply (Prov *p)
{
  if (!prov_pairs (p) || !p->pair_g)
    return;
  JsonObject *e = prov_entry (p);
  const char *hint = str_of (e, "hint"), *action = str_of (e, "action"), *button = str_of (e, "button");
  const char *retry = str_of (e, "retry"), *late = str_of (e, "late_hint");
  gboolean stuck = *retry && prov_stuck (p);
  if (stuck && *late)
    hint = late;
  gboolean qr = !g_strcmp0 (action, "qr");
  gboolean scan = !g_strcmp0 (action, "scan"), token = !g_strcmp0 (action, "token");

  gtk_label_set_text (GTK_LABEL (p->hint), hint);
  gtk_widget_set_visible (p->hint, *hint);
  if (qr && !p->wait_row)
    p->wait_row = ns_row_info (p->pair_g, str_of (e, "wait"), "");
  if (scan && !p->scan_row)
    p->scan_row = ns_row_button (p->pair_g, button, FALSE, scan_open, p);
  if (token && !p->token_row)
    p->token_row = ns_row_button (p->pair_g, button, FALSE, token_open, p);
  if (stuck && !p->retry_row)
    p->retry_row = ns_row_button (p->pair_g, retry, FALSE, retry_open, p);
  row_show (p->retry_row, stuck);
  row_show (p->qr_row, qr && p->qr_bytes);
  row_show (p->wait_row, qr && !p->qr_bytes);
  row_show (p->scan_row, scan);
  row_show (p->token_row, token);
  ns_group_set_visible (p->pair_g, qr || scan || token || stuck);
}

static void
qr_done (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  if (err || !ok_of (root))
    return;
  qr_draw (p, str_of (root, "png_base64"), str_of (root, "svg"));
  /* the code answer carries the state: a change (paired, dropped) asks for the full status (the account's number) */
  if (g_strcmp0 (str_of (root, "state"), p->state))
    prov_refresh_status (p);
  prov_apply (p);
}

static void
qr_run (Prov *p)
{
  const char *args[] = { "messaging", "qr", p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, qr_done, p);
}

/* while the page is open: the code when the provider draws one (its answer has the state too), the status otherwise */
static gboolean
poll_tick (gpointer data)
{
  Prov *p = data;
  if (!gtk_widget_get_root (p->content)) {
    p->poll_id = 0;
    return G_SOURCE_REMOVE;
  }
  if (!g_strcmp0 (str_of (prov_entry (p), "action"), "qr"))
    qr_run (p);
  else
    prov_refresh_status (p);
  return G_SOURCE_CONTINUE;
}

static void
poll_start (Prov *p)
{
  if (!prov_pairs (p) || p->poll_id)
    return;
  p->poll_id = g_timeout_add_seconds (p->poll_s ? p->poll_s : 2, poll_tick, p);
}

/* ---- the status ---- */

static void
status_loaded (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  gboolean ok = !err && ok_of (root);
  const char *state = ok ? str_of (root, "state") : "";
  const char *word = ok ? str_of (root, "label") : "";
  if (g_strcmp0 (state, p->state) || g_strcmp0 (word, p->word))
    p->idle_since = g_get_monotonic_time ();
  g_free (p->state);
  g_free (p->word);
  p->state = g_strdup (state);
  p->word = g_strdup (word);
  if (g_strcmp0 (state, "pairing"))
    qr_forget (p);   /* no pairing, no code: a picture left from before would also hide the retry */
  g_autofree char *text = status_text (root, err, p->ui, p->kind);
  ns_row_set_subtitle (p->status_row, text);
  if (!g_strcmp0 (state, "pairing") && !g_strcmp0 (str_of (prov_entry (p), "action"), "qr") && !p->qr_bytes)
    qr_run (p);   /* the page opens on a pairing: the code now, not at the next tick */
  prov_apply (p);
  poll_start (p);
}

static void
prov_refresh_status (Prov *p)
{
  const char *args[] = { "messaging", "status", p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, status_loaded, p);
}

/* ---- the whitelist ---- */

typedef struct {
  Prov      *p;
  char      *chat;
} Chat;

static void
chat_free (Chat *c)
{
  g_free (c->chat);
  g_free (c);
}

static gboolean
whitelisted (Prov *p, const char *chat)
{
  return p->whitelist && g_strv_contains ((const char *const *) p->whitelist, chat);
}

static void
whitelist_fill_row (Prov *p)
{
  guint n = p->whitelist ? g_strv_length (p->whitelist) : 0;
  g_autofree char *sub = n ? g_strdup_printf ("%u", n) : g_strdup (prov_text (p, "labels", "whitelist_none"));
  ns_row_set_subtitle (p->whitelist_row, sub);
}

static void
chat_saved (JsonObject *root, const char *err, gpointer data)
{
  Chat *c = data;
  const char *bad = bad_of (root, err, c->p->ui);
  if (bad) {
    say (c->p->msg, bad, TRUE);
    return;
  }
  JsonArray *wl = arr_of (obj_of (root, "policy"), "whitelist");
  if (wl) {
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < json_array_get_length (wl); i++)
      g_ptr_array_add (a, g_strdup (json_array_get_string_element (wl, i)));
    g_ptr_array_add (a, NULL);
    g_strfreev (c->p->whitelist);
    c->p->whitelist = (GStrv) g_ptr_array_free (a, FALSE);
  }
  whitelist_fill_row (c->p);
}

static void
chat_toggled (GtkWidget *sw, GParamSpec *pspec, gpointer data)
{
  Chat *c = data;
  if (ns_switch_syncing (sw))
    return;
  const char *args[] = { "messaging", "whitelist", c->p->id,
                         gtk_switch_get_active (GTK_SWITCH (sw)) ? "add" : "remove", c->chat, "--json", NULL };
  ns_mc_run (c->p->owner, args, NULL, chat_saved, c);
}

static void
chats_loaded (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (prov_text (p, "labels", "whitelist"), &c);
  if (bad) {
    ns_note (c, bad);
    ns_push (page);
    return;
  }
  GtkWidget *g = ns_group (c, NULL);
  JsonArray *chats = arr_of (root, "chats");
  guint shown = 0;
  for (guint i = 0; chats && i < json_array_get_length (chats); i++) {
    JsonObject *o = json_array_get_object_element (chats, i);
    const char *chat = str_of (o, "chat");
    if (!*chat)
      continue;
    const char *label = str_of (o, "label");
    g_autofree char *title = g_strdup (*label ? label : chat);
    g_autofree char *sub = g_strdup_printf ("%d", (int) json_object_get_int_member_with_default (o, "messages", 0));
    GtkWidget *row = ns_row_toggle (g, NULL, NULL, title, sub);
    Chat *ch = g_new0 (Chat, 1);
    ch->p = p;
    ch->chat = g_strdup (chat);
    ns_switch_quiet (ns_row_get_switch (row), whitelisted (p, chat));
    g_object_set_data_full (G_OBJECT (row), "mp-chat", ch, (GDestroyNotify) chat_free);
    g_signal_connect (ns_row_get_switch (row), "notify::active", G_CALLBACK (chat_toggled), ch);
    shown++;
  }
  if (shown == 0)
    ns_note (c, prov_text (p, "labels", "no_chats"));
  ns_push (page);
}

static void
whitelist_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  const char *args[] = { "messaging", "chats", p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, chats_loaded, p);
}

/* ---- the scopes, the flags, Disconnect ---- */

typedef struct {
  Prov  *p;
  char  *kind;   /* read_scope | write_scope */
} Scope;

static void
scope_free (Scope *s)
{
  g_free (s->kind);
  g_free (s);
}

static void
scope_picked (GtkWidget *row, guint index, gpointer data)
{
  Scope *s = data;
  if (!s->p->order || index >= g_strv_length (s->p->order))
    return;
  const char *args[] = { "messaging", "set", s->p->id, s->kind, s->p->order[index], "--json", NULL };
  ns_mc_run (s->p->owner, args, NULL, prov_saved, s->p);
}

typedef struct {
  Prov *p;
  char *name;    /* count_unread | ask_first */
} Flag;

static void
flag_free (Flag *f)
{
  g_free (f->name);
  g_free (f);
}

static void
flag_toggled (GtkWidget *sw, GParamSpec *pspec, gpointer data)
{
  Flag *f = data;
  if (ns_switch_syncing (sw))
    return;
  const char *args[] = { "messaging", "set", f->p->id, f->name,
                         gtk_switch_get_active (GTK_SWITCH (sw)) ? "true" : "false", "--json", NULL };
  ns_mc_run (f->p->owner, args, NULL, prov_saved, f->p);
}

static void
disconnected (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  if (bad) {
    say (p->msg, bad, TRUE);
    return;
  }
  gtk_widget_activate_action (p->content, "navigation.pop", NULL);
}

static void
disconnect_yes (gboolean yes, gpointer data)
{
  Prov *p = data;
  if (!yes)
    return;
  const char *args[] = { "messaging", "disconnect", p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, disconnected, p);
}

static void
disconnect_now (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  g_autofree char *heading = g_strdup_printf (prov_text (p, "labels", "disconnect_heading"), p->label);
  ns_confirm (p->content, heading, prov_text (p, "labels", "disconnect_body"),
              prov_text (p, "labels", "disconnect"), TRUE, disconnect_yes, p);
}

static GtkWidget *
scope_row (Prov *p, GtkWidget *group, const char *kind, const char *label, const char *current)
{
  g_autoptr (GPtrArray) labels = g_ptr_array_new ();
  guint selected = 0;
  for (guint i = 0; p->scope_names && p->scope_names[i]; i++) {
    g_ptr_array_add (labels, p->scope_names[i]);
    if (!g_strcmp0 (p->order[i], current))
      selected = i;
  }
  g_ptr_array_add (labels, NULL);
  Scope *s = g_new0 (Scope, 1);
  s->p = p;
  s->kind = g_strdup (kind);
  GtkWidget *row = ns_row_pick (group, label, (const char *const *) labels->pdata, selected, scope_picked, s);
  g_object_set_data_full (G_OBJECT (row), "mp-scope", s, (GDestroyNotify) scope_free);
  return row;
}

static GtkWidget *
flag_row (Prov *p, GtkWidget *group, const char *name, gboolean on)
{
  GtkWidget *row = ns_row_toggle (group, NULL, NULL, prov_text (p, "labels", name),
                                  prov_text (p, "labels", g_strcmp0 (name, "count_unread") ? "ask_first_hint"
                                                                                        : "count_unread_hint"));
  Flag *f = g_new0 (Flag, 1);
  f->p = p;
  f->name = g_strdup (name);
  ns_switch_quiet (ns_row_get_switch (row), on);
  g_object_set_data_full (G_OBJECT (row), "mp-flag", f, (GDestroyNotify) flag_free);
  g_signal_connect (ns_row_get_switch (row), "notify::active", G_CALLBACK (flag_toggled), f);
  return row;
}

static void
prov_build (Prov *p, JsonObject *root)
{
  JsonArray *providers = arr_of (root, "providers");
  JsonObject *self = NULL, *policy = NULL, *scopes = obj_of (root, "scopes"), *limits = obj_of (root, "limits");
  for (guint i = 0; providers && i < json_array_get_length (providers); i++) {
    JsonObject *o = json_array_get_object_element (providers, i);
    if (!g_strcmp0 (str_of (o, "id"), p->id))
      self = o;
  }
  policy = obj_of (obj_of (root, "policy"), p->id);
  const char *label = str_of (self, "label");
  g_free (p->label);
  p->label = g_strdup (*label ? label : p->id);
  ns_page_set_title (p->page, p->label);
  p->poll_s = (guint) json_object_get_int_member_with_default (limits, "page_poll_s", 2);
  p->stuck_s = (guint) json_object_get_int_member_with_default (limits, "stuck_hint_s", 0);

  JsonArray *ord = arr_of (scopes, "order");
  JsonObject *scope_labels = obj_of (scopes, "labels");
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; ord && i < json_array_get_length (ord); i++) {
    const char *sid = json_array_get_string_element (ord, i);
    g_ptr_array_add (ids, g_strdup (sid));
    g_ptr_array_add (names, g_strdup (str_of (scope_labels, sid)));
  }
  g_ptr_array_add (ids, NULL);
  g_ptr_array_add (names, NULL);
  g_strfreev (p->order);
  p->order = (GStrv) g_ptr_array_free (ids, FALSE);
  g_strfreev (p->scope_names);
  p->scope_names = (GStrv) g_ptr_array_free (names, FALSE);

  JsonArray *wl = arr_of (policy, "whitelist");
  GPtrArray *w = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; wl && i < json_array_get_length (wl); i++)
    g_ptr_array_add (w, g_strdup (json_array_get_string_element (wl, i)));
  g_ptr_array_add (w, NULL);
  g_strfreev (p->whitelist);
  p->whitelist = (GStrv) g_ptr_array_free (w, FALSE);

  p->status_g = ns_group (p->content, prov_text (p, "sections", "status"));
  p->status_row = ns_row_info (p->status_g, str_of (obj_of (p->ui, "labels"), "connection"), "");

  g_free (p->kind);
  p->kind = g_strdup (str_of (self, "pairing"));
  p->hint = ns_note (p->content, "");
  gtk_widget_set_visible (p->hint, FALSE);
  p->pair_g = ns_group (p->content, prov_text (p, "sections", "pairing"));
  ns_group_set_visible (p->pair_g, FALSE);

  const char *note = str_of (obj_of (p->ui, "provider_notes"), p->id);
  if (*note)
    ns_note (p->content, note);

  GtkWidget *sc = ns_group (p->content, prov_text (p, "sections", "scopes"));
  p->read_row = scope_row (p, sc, "read_scope", prov_text (p, "labels", "read_scope"),
                           str_of (policy, "read_scope"));
  p->write_row = scope_row (p, sc, "write_scope", prov_text (p, "labels", "write_scope"),
                            str_of (policy, "write_scope"));

  GtkWidget *op = ns_group (p->content, prov_text (p, "sections", "options"));
  p->unread_row = flag_row (p, op, "count_unread",
                            json_object_get_boolean_member_with_default (policy, "count_unread", TRUE));
  p->ask_row = flag_row (p, op, "ask_first",
                         json_object_get_boolean_member_with_default (policy, "ask_first", TRUE));

  GtkWidget *wg = ns_group (p->content, prov_text (p, "sections", "whitelist"));
  p->whitelist_row = ns_row (wg, "system-users-symbolic", "grey", prov_text (p, "labels", "whitelist"), "");
  whitelist_fill_row (p);
  ns_row_on_activate (p->whitelist_row, whitelist_open, p);
  ns_note (p->content, prov_text (p, "notes", "whitelist"));

  GtkWidget *dg = ns_group (p->content, NULL);
  ns_row_button (dg, prov_text (p, "labels", "disconnect"), TRUE, disconnect_now, p);
}

static void
prov_list_loaded (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  if (bad) {
    say (p->msg, bad, TRUE);
    return;
  }
  prov_build (p, root);
  prov_refresh_status (p);
}

static void
prov_load_list (Prov *p)
{
  const char *args[] = { "messaging", "list", "--json", NULL };
  ns_mc_run (p->owner, args, NULL, prov_list_loaded, p);
}

static void
prov_ui_done (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  if (err || !ok_of (root)) {
    say (p->msg, err ? err : str_of (obj_of (p->ui, "labels"), "error"), TRUE);
    return;
  }
  p->ui = json_object_ref (root);
  prov_load_list (p);
}

void
ns_page_provider (GtkWidget *row, gpointer data)
{
  const char *id = data;
  if (!id || !*id)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("", &c);
  Prov *p = g_new0 (Prov, 1);
  p->owner = G_OBJECT (page);
  p->page = page;
  p->content = c;
  p->id = g_strdup (id);
  p->label = g_strdup (id);
  p->poll_s = 2;
  p->idle_since = g_get_monotonic_time ();
  g_object_set_data_full (G_OBJECT (page), "mp-prov", p, (GDestroyNotify) prov_free);
  p->msg = ns_note (c, "");
  gtk_widget_set_visible (p->msg, FALSE);
  const char *args[] = { "messaging", "ui", "--json", NULL };
  ns_mc_run (p->owner, args, NULL, prov_ui_done, p);
  ns_push (page);
}

/* ---- Accounts: the Telegram bot token ---- */

typedef struct {
  GObject           *owner;
  GtkWidget         *content, *row, *msg;
  char              *saved, *error;
} Token;

static void
token_free (Token *t)
{
  g_free (t->saved);
  g_free (t->error);
  g_free (t);
}

static void
token_saved (JsonObject *root, const char *err, gpointer data)
{
  Token *t = data;
  if (err || !ok_of (root)) {
    say (t->msg, err ? err : str_of (root, "error"), TRUE);
    return;
  }
  ns_row_value_set (t->row, "");
  say (t->msg, t->saved, FALSE);
}

static void
token_entered (const char *text, gpointer data)
{
  Token *t = data;
  if (!text || !*text)
    return;
  const char *args[] = { "key", "set", "telegram", "--json", NULL };
  ns_mc_run (t->owner, args, text, token_saved, t);
}

static void
token_ui_done (JsonObject *root, const char *err, gpointer data)
{
  Token *t = data;
  if (err || !ok_of (root))
    return;
  JsonObject *labels = obj_of (root, "labels");
  t->saved = g_strdup (str_of (labels, "token_saved"));
  t->error = g_strdup (str_of (labels, "error"));
  GtkWidget *g = ns_group (t->content, NULL);
  t->row = ns_row_value (g, str_of (labels, "token_title"), "", TRUE, token_entered, t);
  ns_note (t->content, str_of (labels, "token_hint"));
  t->msg = ns_note (t->content, "");
  gtk_widget_set_visible (t->msg, FALSE);
}

void
ns_messaging_token_add (GtkWidget *content, AdwNavigationPage *page)
{
  Token *t = g_new0 (Token, 1);
  t->owner = G_OBJECT (page);
  t->content = content;
  g_object_set_data_full (G_OBJECT (page), "mp-token", t, (GDestroyNotify) token_free);
  const char *args[] = { "messaging", "ui", "--json", NULL };
  ns_mc_run (t->owner, args, NULL, token_ui_done, t);
}
