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

/* a limit of messaging.conf: `messaging list` hands them over as the strings the file has */
static guint
limit_of (JsonObject *limits, const char *key, guint fallback)
{
  const char *text = str_of (limits, key);
  return *text ? (guint) g_ascii_strtoull (text, NULL, 10) : fallback;
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
 * "state:label" (the daemon's own label, "offline:token mancante"), else "state"; for an account that is already
 * linked the same keys with "paired:" in front come first. Members: status (the state's name), hint (what the owner
 * does now), action (start | qr | busy | scan | token), button (the action's row), wait (what is being done while the
 * answer is awaited), cancel and retry (the labels of the rows offered when it takes long) */
static JsonObject *
entry_of (JsonObject *ui, const char *kind, const char *state, const char *label, gboolean paired)
{
  JsonObject *k = obj_of (obj_of (ui, "pairing_kinds"), kind ? kind : "");
  state = state ? state : "";
  label = label ? label : "";
  JsonObject *e;
  if (paired) {
    g_autofree char *pkey = g_strdup_printf ("paired:%s:%s", state, label);
    if ((e = obj_of (k, pkey)))
      return e;
    g_autofree char *skey = g_strdup_printf ("paired:%s", state);
    if ((e = obj_of (k, skey)))
      return e;
  }
  g_autofree char *key = g_strdup_printf ("%s:%s", state, label);
  e = obj_of (k, key);
  return e ? e : obj_of (k, state);
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
  gboolean paired = json_object_get_boolean_member_with_default (root, "paired", FALSE);
  const char *word = str_of (entry_of (ui, kind, state, label, paired), "status");
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
  NsBusy            *busy;
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
  ns_busy_stop (ix->busy);
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
    ns_busy_stop (ix->busy);
    say (ix->msg, err ? err : str_of (obj_of (ix->ui, "labels"), "error"), TRUE);
    return;
  }
  ix->ui = json_object_ref (root);
  /* local and quick: the line saying what it does, without the seconds (the limits come with the list) */
  NsBusyText loading = { .what = str_of (obj_of (root, "labels"), "loading_providers") };
  ns_busy_start (ix->busy, &loading);
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
  ix->busy = ns_busy_new (c);
  NsBusyText wait = { .what = "" };   /* the words are what is being loaded: the spinner alone until they are here */
  ns_busy_start (ix->busy, &wait);
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
  GtkWidget         *status_g, *status_row, *pair_g, *qr_row, *picture, *hint, *start_row, *scan_row, *token_row;
  NsBusy            *busy;        /* what is being waited for: the code, the connection, the camera, the disconnect */
  GtkWidget         *read_row, *write_row, *unread_row, *ask_row, *whitelist_row;
  GBytes            *qr_bytes;    /* what it held: a new one only when the bytes change */
  GStrv              order;       /* the scopes, in the order the pickers show them */
  GStrv              scope_names; /* their labels, in that order */
  GStrv              whitelist;
  char              *read_scope;  /* the read scope as saved: a chat has a page of its own only under "whitelist" */
  int                history_days; /* how far back the agents read, in days (0: no limit) */
  GHashTable        *chat_history; /* chat -> its own depth (GINT_TO_POINTER (days + 1)) */
  GArray            *history_choices; /* the picker's depths, in days ([history] choices_days) */
  GtkWidget         *history_row;
  guint              poll_id, poll_s, slow_s;
  char              *kind;        /* the pairing kind of messaging.conf ("" when the provider does not pair) */
  char              *state, *word; /* the last status: its state and the daemon's own label */
  gboolean           paired;      /* the account is linked (the daemon says so) */
  gboolean           asked;       /* the owner tapped the button and the provider has not answered with a state yet */
  gint64             asked_at;    /* monotonic µs */
  char              *doing;       /* an action of the page that is running (the disconnect): its words, over the rest */
  GSubprocess       *camera;      /* neo-camera --scan while it runs */
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
  g_free (p->doing);
  g_clear_object (&p->camera);
  g_strfreev (p->order);
  g_strfreev (p->scope_names);
  g_strfreev (p->whitelist);
  g_free (p->read_scope);
  g_clear_pointer (&p->chat_history, g_hash_table_unref);
  g_clear_pointer (&p->history_choices, g_array_unref);
  g_free (p);
}

static const char *
prov_text (Prov *p, const char *section, const char *key)
{
  return str_of (obj_of (p->ui, section), key);
}

/* ---- how far back the agents read (messaging.json history_days, and a chat's own under "whitelist") ---- */

/* what the policy says now: the read scope, the provider's depth and the chats' own */
static void
prov_policy (Prov *p, JsonObject *policy)
{
  if (!policy)
    return;
  if (json_object_has_member (policy, "read_scope")) {
    g_free (p->read_scope);
    p->read_scope = g_strdup (str_of (policy, "read_scope"));
  }
  if (json_object_has_member (policy, "history_days"))
    p->history_days = (int) json_object_get_int_member_with_default (policy, "history_days", 0);
  JsonObject *chats = obj_of (policy, "chats");
  if (!chats)
    return;
  if (!p->chat_history)
    p->chat_history = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_remove_all (p->chat_history);
  g_autoptr (GList) names = json_object_get_members (chats);
  for (GList *l = names; l; l = l->next) {
    JsonObject *own = obj_of (chats, l->data);
    if (own && json_object_has_member (own, "history_days"))
      g_hash_table_insert (p->chat_history, g_strdup (l->data),
                           GINT_TO_POINTER ((int) json_object_get_int_member (own, "history_days") + 1));
  }
}

/* "Nessun limite", "Ultimo giorno", "Ultimi 7 giorni" */
static char *
history_label (Prov *p, int days)
{
  if (days <= 0)
    return g_strdup (prov_text (p, "labels", "history_none"));
  if (days == 1)
    return g_strdup (prov_text (p, "labels", "history_day"));
  return g_strdup_printf (prov_text (p, "labels", "history_days"), days);
}

/* the picker's depths, with `current` among them when it was set to one the picker does not offer */
static GArray *
history_options (Prov *p, int current)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (int));
  gboolean seen = current < 0;
  for (guint i = 0; p->history_choices && i < p->history_choices->len; i++) {
    int d = g_array_index (p->history_choices, int, i);
    g_array_append_val (a, d);
    seen = seen || d == current;
  }
  if (!seen)
    g_array_append_val (a, current);
  return a;
}

static int
chat_history_of (Prov *p, const char *chat)
{
  gpointer v = p->chat_history ? g_hash_table_lookup (p->chat_history, chat) : NULL;
  return v ? GPOINTER_TO_INT (v) - 1 : -1;
}

static void
prov_saved (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  say (p->msg, bad ? bad : prov_text (p, "labels", "saved"), bad != NULL);
  if (!bad) {
    prov_policy (p, obj_of (root, "policy"));
    prov_refresh_status (p);
  }
}

/* ---- the pairing section ----
 * What it shows is data: the entry of messaging_ui.json for this kind of provider in the state it is in (entry_of).
 * action "qr": the provider draws a code and the page shows it; "scan": the owner shows the phone's own camera a
 * code the bot sent; "token": the bot has no token yet. */

static JsonObject *
prov_entry (Prov *p)
{
  return entry_of (p->ui, p->kind, p->state, p->word, p->paired);
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
}

static void
token_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (prov_text (p, "labels", "token_title"), &c);
  ns_messaging_token_add (c, page);
  ns_push (page);
}

/* a command of the provider whose answer only matters when it failed; the poll shows the rest */
static void
prov_told (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  const char *bad = bad_of (root, err, p->ui);
  if (bad) {
    say (p->msg, bad, TRUE);
    p->asked = FALSE;
  }
  prov_refresh_status (p);
}

static void
prov_tell (Prov *p, const char *verb)
{
  const char *args[] = { "messaging", verb, p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, prov_told, p);
}

static void prov_apply (Prov *p);

/* "Collega": one pairing session of the provider, with a code for as long as this page keeps asking */
static void
start_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  say (p->msg, "", FALSE);
  p->asked = TRUE;
  p->asked_at = g_get_monotonic_time ();
  prov_apply (p);   /* the card now, not when the provider has answered */
  prov_tell (p, "pair-start");
}

/* the card's Cancel: the session is closed and the provider leaves the network alone */
static void
busy_cancel (gpointer data)
{
  Prov *p = data;
  p->asked = FALSE;
  if (p->camera)
    g_subprocess_force_exit (p->camera);
  else
    prov_tell (p, "pair-stop");
  prov_apply (p);
}

/* the card's Riprova: the provider drops its connection and starts a clean one now; the session stays */
static void
busy_retry (gpointer data)
{
  Prov *p = data;
  ns_busy_restart (p->busy);
  prov_tell (p, "reconnect");
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
    g_clear_object (&p->camera);
    prov_apply (p);
    prov_refresh_status (p);
  }
  g_object_unref (page);
}

static void
scan_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  g_autoptr (GError) err = NULL;
  if (p->camera)
    return;
  GSubprocess *proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &err, "neo-camera", "--scan", NULL);
  if (!proc) {
    say (p->msg, err->message, TRUE);
    return;
  }
  say (p->msg, "", FALSE);
  p->camera = proc;
  g_subprocess_wait_async (proc, NULL, camera_exited, g_object_ref (p->page));
  prov_apply (p);
}

static void
row_show (GtkWidget *row, gboolean on)
{
  if (row)
    gtk_widget_set_visible (row, on);
}

/* the card of what is being waited for, with the words of the data's entry for the state (wait, cancel, retry) */
static void
prov_wait (Prov *p, const char *what, const char *cancel, const char *retry)
{
  NsBusyText t = { .what = what, .slow = prov_text (p, "labels", "busy_slow"), .slow_s = p->slow_s,
                   .cancel = cancel && *cancel ? cancel : NULL, .retry = retry && *retry ? retry : NULL,
                   .on_cancel = busy_cancel, .on_retry = busy_retry, .data = p };
  ns_busy_start (p->busy, &t);
}

/* the section as the data says it is for the state the provider is in. An action of the page that runs (the
 * disconnect) is all it shows */
static void
prov_apply (Prov *p)
{
  if (!prov_pairs (p) || !p->pair_g)
    return;
  if (p->doing) {
    prov_wait (p, p->doing, NULL, NULL);
    row_show (p->hint, FALSE);
    ns_group_set_visible (p->pair_g, FALSE);
    return;
  }
  JsonObject *e = prov_entry (p);
  const char *hint = str_of (e, "hint"), *action = str_of (e, "action"), *button = str_of (e, "button");
  gboolean qr = !g_strcmp0 (action, "qr"), start = !g_strcmp0 (action, "start"), busy = !g_strcmp0 (action, "busy");
  gboolean scan = !g_strcmp0 (action, "scan"), token = !g_strcmp0 (action, "token");
  gboolean waiting = (qr && !p->qr_bytes) || busy || (start && p->asked) || (scan && p->camera);

  gtk_label_set_text (GTK_LABEL (p->hint), hint);
  gtk_widget_set_visible (p->hint, *hint);
  if (start && !p->start_row)
    p->start_row = ns_row_button (p->pair_g, button, FALSE, start_open, p);
  if (scan && !p->scan_row)
    p->scan_row = ns_row_button (p->pair_g, button, FALSE, scan_open, p);
  if (token && !p->token_row)
    p->token_row = ns_row_button (p->pair_g, button, FALSE, token_open, p);
  row_show (p->start_row, start && !p->asked);
  row_show (p->qr_row, qr && p->qr_bytes);
  row_show (p->scan_row, scan && !p->camera);
  row_show (p->token_row, token);
  if (waiting)
    prov_wait (p, str_of (e, "wait"), str_of (e, "cancel"), str_of (e, "retry"));
  else
    ns_busy_stop (p->busy);
  ns_group_set_visible (p->pair_g, (qr && p->qr_bytes) || (start && !p->asked) || (scan && !p->camera) || token);
}

static void
qr_done (JsonObject *root, const char *err, gpointer data)
{
  Prov *p = data;
  if (err || !ok_of (root))
    return;
  qr_draw (p, str_of (root, "png_base64"), str_of (root, "svg"));
  prov_apply (p);
}

static void
qr_run (Prov *p)
{
  const char *args[] = { "messaging", "qr", p->id, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, qr_done, p);
}

/* while the page is open: the status, every poll_s; the provider counts these questions as the page being there */
static gboolean
poll_tick (gpointer data)
{
  Prov *p = data;
  if (!gtk_widget_get_root (p->content)) {
    p->poll_id = 0;
    return G_SOURCE_REMOVE;
  }
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
  g_free (p->state);
  g_free (p->word);
  p->state = g_strdup (state);
  p->word = g_strdup (word);
  p->paired = ok && json_object_get_boolean_member_with_default (root, "paired", FALSE);
  const char *action = str_of (prov_entry (p), "action");
  /* the button was tapped: the provider's first answers may still be the idle one; after that it is on its way */
  if (g_strcmp0 (action, "start"))
    p->asked = FALSE;
  else if (p->asked && (g_get_monotonic_time () - p->asked_at) / G_USEC_PER_SEC >= 2 * (gint64) (p->poll_s ? p->poll_s : 2))
    p->asked = FALSE;
  if (g_strcmp0 (state, "pairing"))
    qr_forget (p);   /* no pairing, no code: a picture left from before would also hide the card */
  g_autofree char *text = status_text (root, err, p->ui, p->kind);
  ns_row_set_subtitle (p->status_row, text);
  if (!g_strcmp0 (state, "pairing") && !g_strcmp0 (action, "qr"))
    qr_run (p);   /* a code is shown the moment it exists, and asked for again while it is */
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
  char      *title;
} Chat;

static void
chat_free (Chat *c)
{
  g_free (c->chat);
  g_free (c->title);
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
  prov_policy (c->p, obj_of (root, "policy"));
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

/* One chat's page: how far back the agents read it, its own depth or the provider's. It has one only under "Solo le
 * chat scelte"; otherwise a tap on the row turns the chat on or off, as the switch does. */

typedef struct {
  Prov      *p;
  GObject   *page;      /* the page, which owns this: held while an answer is coming */
  char      *chat;
  GtkWidget *msg;
  GArray    *options;   /* the depths after the first entry ("as every chat"), in days */
} ChatPage;

static void
chat_page_free (ChatPage *cp)
{
  g_free (cp->chat);
  g_array_unref (cp->options);
  g_free (cp);
}

static void
chat_history_saved (JsonObject *root, const char *err, gpointer data)
{
  ChatPage *cp = data;
  GObject *page = cp->page;
  const char *bad = bad_of (root, err, cp->p->ui);
  say (cp->msg, bad ? bad : prov_text (cp->p, "labels", "saved"), bad != NULL);
  if (!bad)
    prov_policy (cp->p, obj_of (root, "policy"));
  g_object_unref (page);
}

static void
chat_history_picked (GtkWidget *row, guint index, gpointer data)
{
  ChatPage *cp = data;
  g_autofree char *days = index == 0 || index > cp->options->len ? g_strdup ("inherit")
                                     : g_strdup_printf ("%d", g_array_index (cp->options, int, index - 1));
  const char *args[] = { "messaging", "history", cp->p->id, days, cp->chat, "--json", NULL };
  g_object_ref (cp->page);   /* the chat's page and the provider's both stay until the answer is here */
  ns_mc_run (cp->p->owner, args, NULL, chat_history_saved, cp);
}

static void
chat_activate (GtkWidget *row, gpointer data)
{
  Chat *c = data;
  Prov *p = c->p;
  if (g_strcmp0 (p->read_scope, "whitelist")) {
    GtkWidget *sw = ns_row_get_switch (row);
    gtk_switch_set_active (GTK_SWITCH (sw), !gtk_switch_get_active (GTK_SWITCH (sw)));
    return;
  }
  GtkWidget *content;
  AdwNavigationPage *page = ns_page_new (c->title, &content);
  ChatPage *cp = g_new0 (ChatPage, 1);
  cp->p = p;
  cp->page = G_OBJECT (page);
  cp->chat = g_strdup (c->chat);
  int own = chat_history_of (p, c->chat);
  cp->options = history_options (p, own);
  g_object_set_data_full (G_OBJECT (page), "mp-chat-page", cp, (GDestroyNotify) chat_page_free);
  cp->msg = ns_note (content, "");
  gtk_widget_set_visible (cp->msg, FALSE);
  GtkWidget *g = ns_group (content, prov_text (p, "sections", "chat_history"));
  g_autofree char *global = history_label (p, p->history_days);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (labels, g_strdup_printf (prov_text (p, "labels", "history_inherit"), global));
  guint selected = 0;
  for (guint i = 0; i < cp->options->len; i++) {
    int d = g_array_index (cp->options, int, i);
    g_ptr_array_add (labels, history_label (p, d));
    if (d == own)
      selected = i + 1;
  }
  g_ptr_array_add (labels, NULL);
  ns_row_pick (g, prov_text (p, "labels", "history"), (const char *const *) labels->pdata, selected,
               chat_history_picked, cp);
  ns_note (content, prov_text (p, "notes", "chat_history"));
  ns_push (page);
}

/* The page of the chats the owner may authorize. It opens at once with the card of what is being done: the
 * provider reads the account's contacts and groups (`messaging chats ID --sync`), which may take a while. Cancel shows
 * what the provider already knows. Rows come in batches so a long address book does not freeze the page. */

#define CHAT_ROWS_PER_TICK 40   /* rows built per idle turn */

typedef struct {
  Prov       *p;
  GtkWidget  *content, *group, *note, *search;
  NsBusy     *busy;
  JsonArray  *chats;      /* the answer being drawn (owned) */
  GArray     *order;      /* the indexes of chats as drawn: the enabled ones first, each part in the answer's order */
  guint       next, gen, idle_id;
  GStrv       words;      /* what the search field says, one lower case word each */
} Wl;

static void wl_load (Wl *w, gboolean sync);

static void
wl_free (Wl *w)
{
  if (w->idle_id)
    g_source_remove (w->idle_id);
  if (w->chats)
    json_array_unref (w->chats);
  if (w->order)
    g_array_unref (w->order);
  g_strfreev (w->words);
  g_free (w);
}

static gboolean
wl_match (GtkListBoxRow *row, gpointer data)
{
  Wl *w = data;
  const char *text = g_object_get_data (G_OBJECT (row), "ns-text");
  if (!w->words || !text)
    return TRUE;
  g_autofree char *hay = g_utf8_casefold (text, -1);
  for (guint i = 0; w->words[i]; i++)
    if (*w->words[i] && !strstr (hay, w->words[i]))
      return FALSE;
  return TRUE;
}

static void
wl_search_changed (GtkSearchEntry *entry, gpointer data)
{
  Wl *w = data;
  g_autofree char *fold = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  g_strfreev (w->words);
  w->words = g_strsplit (fold, " ", -1);
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (w->group));
}

/* "Mario • Gruppo • 12/11 18:30": what the row says about a chat, never a word of what was said. A name the other
 * side chose for itself is not the title: it is said as such under the number */
static void
wl_texts (Prov *p, JsonObject *o, char **title, char **sub)
{
  JsonObject *labels = obj_of (p->ui, "labels");
  const char *chat = str_of (o, "chat"), *name = str_of (o, "name"), *label = str_of (o, "label");
  gboolean push = json_object_get_boolean_member_with_default (o, "push", FALSE);
  gint64 at = json_object_get_int_member_with_default (o, "at", 0);
  int messages = (int) json_object_get_int_member_with_default (o, "messages", 0);
  GPtrArray *parts = g_ptr_array_new_with_free_func (g_free);
  *title = g_strdup (*name && !push ? name : (*label ? label : chat));
  if (!g_strcmp0 (str_of (o, "kind"), "group"))
    g_ptr_array_add (parts, g_strdup (str_of (labels, "kind_group")));
  if (*name && push)
    g_ptr_array_add (parts, g_strdup_printf (str_of (labels, "chat_push"), name));
  /* two contacts hold this number: the number is the title, and nobody is guessed
   * TODO(X-MINERVA-CHAT-<provider>): offer the choice here; it is written to the chosen contact's vCard */
  if (json_object_get_boolean_member_with_default (o, "ambiguous", FALSE))
    g_ptr_array_add (parts, g_strdup (str_of (labels, "chat_ambiguous")));
  /* a contact's number says which of two with one name it is; a group's id says nothing */
  if (*name && !push && g_strcmp0 (name, chat) && g_strcmp0 (str_of (o, "kind"), "group"))
    g_ptr_array_add (parts, g_strdup (chat));
  if (at > 0) {
    g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (at);
    g_ptr_array_add (parts, g_date_time_format (t, str_of (labels, "chat_time_format")));
  }
  if (messages > 0)
    g_ptr_array_add (parts, g_strdup_printf (str_of (labels, "chat_messages"), messages));
  g_ptr_array_add (parts, NULL);
  *sub = g_strjoinv (" \xe2\x80\xa2 ", (char **) parts->pdata);
  g_ptr_array_free (parts, TRUE);
}

static gboolean
wl_batch (gpointer data)
{
  Wl *w = data;
  Prov *p = w->p;
  guint n = w->order ? w->order->len : 0;
  for (guint k = 0; k < CHAT_ROWS_PER_TICK && w->next < n; k++, w->next++) {
    JsonObject *o = json_array_get_object_element (w->chats, g_array_index (w->order, guint, w->next));
    const char *chat = str_of (o, "chat");
    if (!*chat)
      continue;
    g_autofree char *title = NULL, *sub = NULL;
    wl_texts (p, o, &title, &sub);
    GtkWidget *row = ns_row_toggle (w->group, NULL, NULL, title, sub);
    Chat *ch = g_new0 (Chat, 1);
    ch->p = p;
    ch->chat = g_strdup (chat);
    ch->title = g_strdup (title);
    ns_switch_quiet (ns_row_get_switch (row), whitelisted (p, chat));
    ns_row_on_activate (row, chat_activate, ch);
    g_object_set_data_full (G_OBJECT (row), "mp-chat", ch, (GDestroyNotify) chat_free);
    g_signal_connect (ns_row_get_switch (row), "notify::active", G_CALLBACK (chat_toggled), ch);
  }
  if (w->next < n)
    return G_SOURCE_CONTINUE;
  w->idle_id = 0;
  if (n == 0)
    say (w->note, prov_text (p, "labels", "no_chats"), FALSE);
  return G_SOURCE_REMOVE;
}

typedef struct {
  Wl   *w;
  guint gen;
} WlCall;

static void
wl_loaded (JsonObject *root, const char *err, gpointer data)
{
  WlCall *call = data;
  Wl *w = call->w;
  Prov *p = w->p;
  gboolean current = call->gen == w->gen;
  g_free (call);
  if (!current)
    return;   /* the owner cancelled or asked again: this answer is old */
  ns_busy_stop (w->busy);
  const char *bad = bad_of (root, err, p->ui);
  if (bad) {
    say (w->note, bad, TRUE);
    return;
  }
  JsonObject *sync = obj_of (root, "sync");
  if (sync && !json_object_get_boolean_member_with_default (sync, "ok", TRUE)) {
    g_autofree char *partial = g_strdup_printf (prov_text (p, "labels", "sync_partial"), str_of (sync, "message"));
    say (w->note, partial, TRUE);
  } else {
    say (w->note, "", FALSE);
  }
  ns_group_clear (w->group);
  if (w->chats)
    json_array_unref (w->chats);
  JsonArray *chats = arr_of (root, "chats");
  w->chats = chats ? json_array_ref (chats) : NULL;
  if (w->order)
    g_array_unref (w->order);
  w->order = g_array_new (FALSE, FALSE, sizeof (guint));
  guint total = w->chats ? json_array_get_length (w->chats) : 0;
  for (int on = 1; on >= 0; on--)   /* the enabled chats on top, as they are when the page opens */
    for (guint i = 0; i < total; i++)
      if (whitelisted (p, str_of (json_array_get_object_element (w->chats, i), "chat")) == on)
        g_array_append_val (w->order, i);
  w->next = 0;
  gtk_widget_set_visible (w->search, TRUE);
  if (w->idle_id)
    g_source_remove (w->idle_id);
  w->idle_id = g_idle_add (wl_batch, w);
}

static void
wl_cancel (gpointer data)
{
  Wl *w = data;
  w->gen++;
  wl_load (w, FALSE);   /* what the provider already knows, without waiting for the account */
}

static void
wl_retry (gpointer data)
{
  Wl *w = data;
  wl_load (w, TRUE);
}

static void
wl_load (Wl *w, gboolean sync)
{
  Prov *p = w->p;
  w->gen++;
  WlCall *call = g_new0 (WlCall, 1);
  call->w = w;
  call->gen = w->gen;
  NsBusyText t = { .what = prov_text (p, "labels", sync ? "chats_sync" : "chats_read"), .slow = prov_text (p, "labels", "busy_slow"),
                   .slow_s = p->slow_s, .cancel = sync ? prov_text (p, "labels", "chats_skip") : NULL,
                   .retry = prov_text (p, "labels", "busy_retry"), .on_cancel = wl_cancel, .on_retry = wl_retry, .data = w };
  ns_busy_start (w->busy, &t);
  const char *args_sync[] = { "messaging", "chats", p->id, "--sync", "--json", NULL };
  const char *args_read[] = { "messaging", "chats", p->id, "--json", NULL };
  ns_mc_run (p->owner, sync ? args_sync : args_read, NULL, wl_loaded, call);
}

static void
whitelist_open (GtkWidget *row, gpointer data)
{
  Prov *p = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (prov_text (p, "labels", "whitelist"), &c);
  Wl *w = g_new0 (Wl, 1);
  w->p = p;
  w->content = c;
  g_object_set_data_full (G_OBJECT (page), "mp-wl", w, (GDestroyNotify) wl_free);
  w->note = ns_note (c, "");
  gtk_widget_set_visible (w->note, FALSE);
  w->busy = ns_busy_new (c);
  w->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (w->search), prov_text (p, "labels", "chats_search"));
  gtk_widget_set_visible (w->search, FALSE);
  gtk_box_append (GTK_BOX (c), w->search);
  w->group = ns_group (c, NULL);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (w->group), wl_match, w, NULL);
  g_signal_connect (w->search, "search-changed", G_CALLBACK (wl_search_changed), w);
  ns_push (page);
  wl_load (w, TRUE);
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

static void
history_picked (GtkWidget *row, guint index, gpointer data)
{
  Prov *p = data;
  g_autoptr (GArray) options = history_options (p, p->history_days);
  if (index >= options->len)
    return;
  g_autofree char *days = g_strdup_printf ("%d", g_array_index (options, int, index));
  const char *args[] = { "messaging", "history", p->id, days, "--json", NULL };
  ns_mc_run (p->owner, args, NULL, prov_saved, p);
}

static GtkWidget *
history_row (Prov *p, GtkWidget *group)
{
  g_autoptr (GArray) options = history_options (p, p->history_days);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  guint selected = 0;
  for (guint i = 0; i < options->len; i++) {
    int d = g_array_index (options, int, i);
    g_ptr_array_add (labels, history_label (p, d));
    if (d == p->history_days)
      selected = i;
  }
  g_ptr_array_add (labels, NULL);
  return ns_row_pick (group, prov_text (p, "labels", "history"), (const char *const *) labels->pdata, selected,
                      history_picked, p);
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
  g_clear_pointer (&p->doing, g_free);
  const char *bad = bad_of (root, err, p->ui);
  if (bad) {
    say (p->msg, bad, TRUE);
    prov_apply (p);
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
  g_free (p->doing);
  p->doing = g_strdup (prov_text (p, "labels", "disconnecting"));
  prov_apply (p);
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
  p->poll_s = limit_of (limits, "page_poll_s", 2);
  p->slow_s = limit_of (limits, "slow_hint_s", 0);

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

  JsonArray *hc = arr_of (obj_of (root, "history"), "choices");
  g_clear_pointer (&p->history_choices, g_array_unref);
  p->history_choices = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; hc && i < json_array_get_length (hc); i++) {
    int d = (int) json_array_get_int_element (hc, i);
    g_array_append_val (p->history_choices, d);
  }
  prov_policy (p, policy);

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
  p->history_row = history_row (p, sc);
  ns_note (p->content, prov_text (p, "notes", "history"));

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
  ns_busy_stop (p->busy);
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
    ns_busy_stop (p->busy);
    say (p->msg, err ? err : str_of (obj_of (p->ui, "labels"), "error"), TRUE);
    return;
  }
  p->ui = json_object_ref (root);
  NsBusyText loading = { .what = prov_text (p, "labels", "loading") };   /* the limits (when to say "still going") come with the list */
  ns_busy_start (p->busy, &loading);
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
  g_object_set_data_full (G_OBJECT (page), "mp-prov", p, (GDestroyNotify) prov_free);
  p->msg = ns_note (c, "");
  gtk_widget_set_visible (p->msg, FALSE);
  p->busy = ns_busy_new (c);
  /* the words are what is being loaded: until they are here the card is the spinner alone */
  NsBusyText wait = { .what = "" };
  ns_busy_start (p->busy, &wait);
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
