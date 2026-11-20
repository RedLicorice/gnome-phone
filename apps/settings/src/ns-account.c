/* Accounts > the accounts of the phone's agent: a command-line tool signed in to a service (a code host…), next to
 * Accounts > API keys. Which accounts exist, their names and their wording are data of `minerva-config accounts`
 * (providers.json); this file names none of them. Signing in is a device flow: the tool gets a one-time code, the
 * owner enters it at an address on any device (or scans the address as a QR code), and this page asks for the state
 * every two seconds until it is approved. The token stays in the tool's own storage: it is in no output of
 * minerva-config, so this page never sees one. */
#include "msg-assistant.h"
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <string.h>

typedef struct {
  AdwNavigationPage *page;
  char       *id;          /* NULL until the first answer when opened with --page */
  JsonObject *acc;         /* the last Account */
  GtkWidget  *row;         /* the Accounts page's row, kept current (a weak pointer) */
  GtkWidget  *msg, *missing;
  GtkWidget  *status, *login, *scopes, *host;
  GtkWidget  *code_group, *how, *code, *url, *qr_box, *expires;
  GtkWidget  *actions, *connect, *disconnect;
  GtkWidget  *web;
  char       *qr_url, *docs;
  guint       poll;
  gboolean    busy;        /* connect, cancel or disconnect is running */
  gboolean    reading;     /* a status is on its way */
} Det;

/* ---- the data ---- */

static gboolean
jbool (JsonObject *o, const char *m)
{
  JsonNode *n = o && json_object_has_member (o, m) ? json_object_get_member (o, m) : NULL;
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_BOOLEAN && json_node_get_boolean (n);
}

static gint64
jint (JsonObject *o, const char *m)
{
  JsonNode *n = o && json_object_has_member (o, m) ? json_object_get_member (o, m) : NULL;
  if (!n || !JSON_NODE_HOLDS_VALUE (n))
    return 0;
  GType t = json_node_get_value_type (n);
  return t == G_TYPE_INT64 ? json_node_get_int (n) : t == G_TYPE_DOUBLE ? (gint64) json_node_get_double (n) : 0;
}

/* "repo, read:org": a list of strings in words */
static char *
join (JsonArray *a)
{
  g_autoptr (GString) s = g_string_new ("");
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonNode *n = json_array_get_element (a, i);
    if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
      g_string_append_printf (s, "%s%s", s->len ? ", " : "", json_node_get_string (n));
  }
  return g_strdup (s->str);
}

static const char *
kind_label (JsonObject *a)
{
  const char *l = ns_json_str (a, "kind_label");
  return *l ? l : ns_json_str (a, "name");
}

/* what the row and the Status line say */
static char *
state_text (JsonObject *a)
{
  const char *st = ns_json_str (a, "state"), *login = ns_json_str (a, "login"), *e = ns_json_str (a, "error");
  if (!jbool (a, "installed"))
    return g_strdup ("Not available on this phone");
  if (!g_strcmp0 (st, "connected"))
    return *login ? g_strdup_printf ("Connected as %s", login) : g_strdup ("Connected");
  if (!g_strcmp0 (st, "pending"))
    return g_strdup ("Waiting for approval");
  if (!g_strcmp0 (st, "error"))
    return *e ? g_strdup_printf ("Error: %s", e) : g_strdup ("Error");
  return g_strdup ("Not connected");
}

/* ---- an account page ---- */

static void
say (Det *d, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (d->msg), text);
  gtk_widget_set_visible (d->msg, text && *text);
  if (error)
    gtk_widget_add_css_class (d->msg, "ns-danger");
  else
    gtk_widget_remove_css_class (d->msg, "ns-danger");
}

static void det_status (Det *d);

static gboolean
poll_tick (gpointer data)
{
  det_status (data);
  return G_SOURCE_CONTINUE;
}

static void
qr_set (Det *d, const char *url)
{
  if (!g_strcmp0 (url, d->qr_url))
    return;
  g_free (d->qr_url);
  d->qr_url = g_strdup (url);
  GtkWidget *old = gtk_widget_get_first_child (d->qr_box);
  if (old)
    gtk_box_remove (GTK_BOX (d->qr_box), old);
  GtkWidget *qr = *url ? ns_qr_new (url) : NULL;   /* NULL: libqrencode is not installed; the address is there */
  if (qr) {
    gtk_widget_set_size_request (qr, 220, 220);
    gtk_box_append (GTK_BOX (d->qr_box), qr);
  }
  gtk_widget_set_visible (d->qr_box, qr != NULL);
}

static void
det_update (Det *d)
{
  JsonObject *a = d->acc;
  if (!a)
    return;
  if (!d->id && *ns_json_str (a, "id"))
    d->id = g_strdup (ns_json_str (a, "id"));
  const char *name = ns_json_str (a, "name"), *st = ns_json_str (a, "state"), *login = ns_json_str (a, "login"),
             *kl = kind_label (a);
  if (*name)
    adw_navigation_page_set_title (d->page, name);
  gboolean inst = jbool (a, "installed");
  gboolean pending = inst && !g_strcmp0 (st, "pending"), conn = inst && !g_strcmp0 (st, "connected");

  g_autofree char *s = state_text (a);
  ns_row_set_subtitle (d->status, s);
  if (d->row) {
    ns_row_set_subtitle (d->row, s);
    g_object_set_data_full (G_OBJECT (d->row), "ns-acc", json_object_ref (a), (GDestroyNotify) json_object_unref);
  }
  ns_row_set_subtitle (d->login, login);
  gtk_widget_set_visible (d->login, conn && *login);
  g_autofree char *granted = join (ns_json_arr (a, "granted_scopes")), *asked = join (ns_json_arr (a, "scopes"));
  g_autofree char *scopes = conn && *granted ? g_strdup (granted)
                          : *asked ? g_strdup_printf ("%s (asked for)", asked) : g_strdup ("None");
  ns_row_set_subtitle (d->scopes, scopes);
  ns_row_set_subtitle (d->host, ns_json_str (a, "host"));
  gtk_widget_set_visible (d->host, *ns_json_str (a, "host") != '\0');

  /* the tool is missing: say which, in the data's words where it has them */
  if (!inst) {
    const char *hint = ns_json_str (a, "install_hint"), *cli = ns_json_str (a, "cli");
    g_autofree char *t = *hint ? g_strdup (hint)
                       : *cli ? g_strdup_printf ("%s needs the program “%s”, which is not installed on this phone.", kl, cli)
                       : g_strdup_printf ("The program %s needs is not installed on this phone, so it cannot be "
                                          "connected.", kl);
    gtk_label_set_text (GTK_LABEL (d->missing), t);
  }
  gtk_widget_set_visible (d->missing, !inst);

  ns_group_set_visible (d->code_group, pending);
  if (pending) {
    g_autofree char *how = g_strdup_printf ("Enter this code at the address below, on any device where you are "
                                            "signed in to %s, or scan the address with it.", kl);
    gtk_label_set_text (GTK_LABEL (d->how), how);
    gtk_label_set_text (GTK_LABEL (d->code), ns_json_str (a, "code"));
    gtk_label_set_text (GTK_LABEL (d->url), ns_json_str (a, "url"));
    qr_set (d, ns_json_str (a, "url"));
    gint64 left = jint (a, "expires_in");
    g_autofree char *exp = left > 0 ? g_strdup_printf ("The code expires in %d:%02d", (int) (left / 60), (int) (left % 60))
                                    : g_strdup ("");
    gtk_label_set_text (GTK_LABEL (d->expires), exp);
    gtk_widget_set_visible (d->expires, *exp != '\0');
  }

  gtk_widget_set_visible (d->connect, inst && !pending && !conn);
  gtk_widget_set_visible (d->disconnect, conn);
  ns_group_set_visible (d->actions, inst && !pending);

  g_free (d->docs);
  d->docs = g_strdup (ns_json_str (a, "docs_url"));
  ns_group_set_visible (d->web, *d->docs != '\0');

  /* waiting for approval: ask again every two seconds, as long as the page lives */
  if (pending && !d->poll)
    d->poll = g_timeout_add_seconds (2, poll_tick, d);
  else if (!pending)
    g_clear_handle_id (&d->poll, g_source_remove);
}

static void
status_done (JsonObject *root, const char *err, gpointer data)
{
  Det *d = data;
  d->reading = FALSE;
  if (d->busy)   /* an action answers with the newer state */
    return;
  if (err || !root) {
    g_autofree char *t = g_strdup_printf ("Could not read the account: %s", err ? err : "no answer");
    say (d, t, TRUE);
    g_clear_handle_id (&d->poll, g_source_remove);
    return;
  }
  if (d->acc)
    json_object_unref (d->acc);
  d->acc = json_object_ref (root);
  det_update (d);
}

static void
det_status (Det *d)
{
  if (d->reading || d->busy)
    return;
  d->reading = TRUE;
  const char *const with_id[] = { "accounts", "status", d->id, "--json", NULL };
  const char *const first[] = { "accounts", "status", "--json", NULL };   /* minerva-config picks the first one */
  ns_mc_run (G_OBJECT (d->page), d->id ? with_id : first, NULL, status_done, d);
}

static void
act_done (JsonObject *root, const char *err, gpointer data)
{
  Det *d = data;
  d->busy = FALSE;
  if (err || !root) {
    g_autofree char *t = g_strdup_printf ("Did not work: %s", err ? err : "no answer");
    say (d, t, TRUE);
    return;
  }
  say (d, "", FALSE);
  if (d->acc)
    json_object_unref (d->acc);
  d->acc = json_object_ref (root);
  det_update (d);
}

static void
act_run (Det *d, const char *action, const char *progress)
{
  if (d->busy || !d->id)
    return;
  d->busy = TRUE;
  say (d, progress, FALSE);
  const char *const args[] = { "accounts", action, d->id, "--json", NULL };
  ns_mc_run (G_OBJECT (d->page), args, NULL, act_done, d);
}

static void
connect_clicked (GtkWidget *row, gpointer data)
{
  act_run (data, "connect", "Asking for a code…");
}

static void
cancel_clicked (GtkWidget *row, gpointer data)
{
  act_run (data, "cancel", "Cancelling…");
}

static void
disconnected (gboolean yes, gpointer data)
{
  if (yes)
    act_run (data, "disconnect", "Disconnecting…");
}

static void
disconnect_clicked (GtkWidget *row, gpointer data)
{
  Det *d = data;
  const char *kl = kind_label (d->acc);
  g_autofree char *heading = g_strdup_printf ("Disconnect %s?", ns_json_str (d->acc, "name"));
  g_autofree char *who = msg_assistant_fill ("{assistant}");
  g_autofree char *body = g_strdup_printf ("This phone signs out of %s. %s's agent can no longer use the account "
                                           "until you connect it again.", kl, who);
  ns_confirm (row, heading, body, "Disconnect", TRUE, disconnected, d);
}

static void
copy_code (GtkWidget *row, gpointer data)
{
  Det *d = data;
  const char *code = ns_json_str (d->acc, "code");
  if (*code) {
    gdk_clipboard_set_text (gtk_widget_get_clipboard (row), code);
    say (d, "Code copied.", FALSE);
  }
}

static void
open_url (GtkWidget *row, gpointer data)
{
  Det *d = data;
  const char *url = ns_json_str (d->acc, "url");
  if (*url) {
    const char *const argv[] = { "xdg-open", url, NULL };
    ns_spawn (argv);
  }
}

static void
open_docs (GtkWidget *row, gpointer data)
{
  Det *d = data;
  if (d->docs && *d->docs) {
    const char *const argv[] = { "xdg-open", d->docs, NULL };
    ns_spawn (argv);
  }
}

static void
det_free (Det *d)
{
  g_clear_handle_id (&d->poll, g_source_remove);
  if (d->row)
    g_object_remove_weak_pointer (G_OBJECT (d->row), (gpointer *) &d->row);
  if (d->acc)
    json_object_unref (d->acc);
  g_free (d->id);
  g_free (d->qr_url);
  g_free (d->docs);
  g_free (d);
}

static GtkWidget *
label (const char *css1, const char *css2)
{
  GtkWidget *l = gtk_label_new ("");
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
  if (css1)
    gtk_widget_add_css_class (l, css1);
  if (css2)
    gtk_widget_add_css_class (l, css2);
  return l;
}

static void
det_open (const char *id, JsonObject *acc, GtkWidget *row)
{
  GtkWidget *c;
  const char *name = acc ? ns_json_str (acc, "name") : "";
  AdwNavigationPage *page = ns_page_new (*name ? name : "Account", &c);
  Det *d = g_new0 (Det, 1);
  d->page = page;
  d->id = id && *id ? g_strdup (id) : NULL;
  if (row) {
    d->row = row;
    g_object_add_weak_pointer (G_OBJECT (row), (gpointer *) &d->row);
  }
  g_object_set_data_full (G_OBJECT (page), "ns-account", d, (GDestroyNotify) det_free);

  d->msg = ns_note (c, "");
  gtk_widget_set_visible (d->msg, FALSE);
  gtk_widget_set_margin_top (d->msg, 0);
  d->missing = ns_note (c, "");
  gtk_widget_set_visible (d->missing, FALSE);

  GtkWidget *g = ns_group (c, "Account");
  d->status = ns_row_info (g, "Status", "…");
  d->login = ns_row_info (g, "Signed in as", "");
  gtk_widget_set_visible (d->login, FALSE);
  d->scopes = ns_row_info (g, "Permissions", "");
  d->host = ns_row_info (g, "Server", "");

  /* the one-time code, big, with the address and its QR code */
  d->code_group = ns_group (c, "Sign in");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  d->how = label ("dim-label", NULL);
  gtk_box_append (GTK_BOX (box), d->how);
  d->code = label ("title-1", "monospace");
  gtk_label_set_selectable (GTK_LABEL (d->code), TRUE);
  gtk_box_append (GTK_BOX (box), d->code);
  d->url = label (NULL, NULL);
  gtk_label_set_wrap_mode (GTK_LABEL (d->url), PANGO_WRAP_CHAR);
  gtk_label_set_selectable (GTK_LABEL (d->url), TRUE);
  gtk_box_append (GTK_BOX (box), d->url);
  d->qr_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (d->qr_box, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (d->qr_box, FALSE);
  gtk_box_append (GTK_BOX (box), d->qr_box);
  d->expires = label ("dim-label", "caption");
  gtk_box_append (GTK_BOX (box), d->expires);
  ns_row_widget (d->code_group, NULL, box);
  ns_row_button (d->code_group, "Copy code", FALSE, copy_code, d);
  ns_row_button (d->code_group, "Open the address on this phone", FALSE, open_url, d);
  ns_row_button (d->code_group, "Cancel", TRUE, cancel_clicked, d);
  ns_group_set_visible (d->code_group, FALSE);

  d->actions = ns_group (c, NULL);
  d->connect = ns_row_button (d->actions, "Connect", FALSE, connect_clicked, d);
  d->disconnect = ns_row_button (d->actions, "Disconnect", TRUE, disconnect_clicked, d);
  ns_group_set_visible (d->actions, FALSE);

  d->web = ns_group (c, NULL);
  ns_row_button (d->web, "Manage access on the web…", FALSE, open_docs, d);
  ns_group_set_visible (d->web, FALSE);

  g_autofree char *kept = msg_assistant_fill ("The sign-in is kept by the account's own program on this phone, readable "
                                              "by you only; {assistant} never shows it. The agent asks you before it "
                                              "publishes or changes anything with this account.");
  ns_note (c, kept);

  if (acc) {
    d->acc = json_object_ref (acc);
    det_update (d);
  }
  det_status (d);
  ns_push (page);
}

/* `--page`: the first account minerva-config knows */
void
ns_page_account (GtkWidget *row, gpointer data)
{
  det_open (NULL, NULL, NULL);
}

/* ---- the card on the Accounts page ---- */

static void
row_open (GtkWidget *row, gpointer data)
{
  JsonObject *a = g_object_get_data (G_OBJECT (row), "ns-acc");
  if (a)
    det_open (ns_json_str (a, "id"), a, row);
}

static void
list_done (JsonObject *root, const char *err, gpointer data)
{
  GtkWidget *g = data;
  ns_group_clear (g);
  /* an error (an older minerva-config without accounts) leaves the card hidden */
  JsonArray *arr = err || !root ? NULL : ns_json_arr (root, "accounts");
  guint n = 0;
  for (guint i = 0; arr && i < json_array_get_length (arr); i++) {
    JsonNode *node = json_array_get_element (arr, i);
    if (!JSON_NODE_HOLDS_OBJECT (node))
      continue;
    JsonObject *a = json_node_get_object (node);
    const char *icon = ns_json_str (a, "icon");
    g_autofree char *s = state_text (a);
    GtkWidget *row = ns_row (g, *icon ? icon : "network-server-symbolic", "grey", ns_json_str (a, "name"), s);
    g_object_set_data_full (G_OBJECT (row), "ns-acc", json_object_ref (a), (GDestroyNotify) json_object_unref);
    ns_row_on_activate (row, row_open, NULL);
    n++;
  }
  ns_group_set_visible (g, n > 0);
}

void
ns_accounts_minerva (GtkWidget *content)
{
  static const char *const args[] = { "accounts", "list", "--json", NULL };
  GtkWidget *g = ns_group (content, NULL);
  ns_group_set_visible (g, FALSE);
  ns_mc_run (G_OBJECT (g), args, NULL, list_done, g);
}
