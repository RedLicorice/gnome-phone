/* Minerva > Federazione: this phone in the Minerva federation (what it is, what it offers, its limits, its mode), the
 * Minervas and devices of the federation with each device's inventory, adding a Minerva and leaving.
 * Everything comes from `minerva-config federation status --json` (Minerva's config/minerva_config/federation.py): the
 * texts (federation_ui.json, in the phone's language), this node, its choices, the nodes with their tags and
 * inventories. A change of this phone's choices is `minerva-config federation set SECTION.KEY VALUE`, which asks the
 * node's root service (org.minerva.Inventory1); the owner may be asked on the device first, so the answer can take a
 * while. Other devices are shown read-only: only a device itself changes its offers and limits. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct {
  AdwNavigationPage *page;
  GtkWidget  *content, *msg;
  GtkWidget  *this_g, *offers_g, *limits_g, *minervas_g, *nodes_g;
  GtkWidget  *mode_row;
  GHashTable *switches;   /* offer or boolean limit "section.key" -> GtkSwitch */
  GHashTable *values;     /* numeric limit key -> its row */
  JsonObject *st;         /* the last status */
  gboolean    built;
} Fd;

/* a numeric limit's row: what its callback needs (freed with the row) */
typedef struct {
  Fd   *fd;
  char *key;
} ValueCtx;

static void
value_ctx_free (ValueCtx *v)
{
  g_free (v->key);
  g_free (v);
}

/* one change of a switch or a value: lives until its answer */
typedef struct {
  Fd   *fd;
  char *section, *key;
} Change;

static void fd_refresh (Fd *fd);

static void
fd_free (Fd *fd)
{
  g_hash_table_unref (fd->switches);
  g_hash_table_unref (fd->values);
  if (fd->st)
    json_object_unref (fd->st);
  g_free (fd);
}

static JsonObject *
ui (Fd *fd, const char *part)
{
  return obj_of (obj_of (fd->st, "ui"), part);
}

static const char *
label (Fd *fd, const char *name)
{
  return str_of (ui (fd, "labels"), name);
}

/* "{n} devices" with n */
static char *
subst (const char *tpl, const char *name, const char *value)
{
  g_autofree char *mark = g_strdup_printf ("{%s}", name);
  g_auto (GStrv) parts = g_strsplit (tpl, mark, -1);
  return g_strjoinv (value, parts);
}

static void
say (Fd *fd, const char *text)
{
  gtk_label_set_text (GTK_LABEL (fd->msg), text ? text : "");
  gtk_widget_set_visible (fd->msg, text && *text);
}

static gboolean
bool_member (JsonObject *o, const char *name, gboolean dflt)
{
  return o && json_object_has_member (o, name) ? json_object_get_boolean_member_with_default (o, name, dflt) : dflt;
}

static gboolean
number_member (JsonObject *o, const char *name, double *out)
{
  JsonNode *n = o && json_object_has_member (o, name) ? json_object_get_member (o, name) : NULL;
  if (!n || JSON_NODE_TYPE (n) != JSON_NODE_VALUE)
    return FALSE;
  GType t = json_node_get_value_type (n);
  if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
    return FALSE;
  *out = json_node_get_double (n);
  return TRUE;
}

static gboolean
is_bool (JsonObject *o, const char *name)
{
  JsonNode *n = o && json_object_has_member (o, name) ? json_object_get_member (o, name) : NULL;
  return n && JSON_NODE_TYPE (n) == JSON_NODE_VALUE && json_node_get_value_type (n) == G_TYPE_BOOLEAN;
}

/* ---- this phone's choices ---- */

static void
change_done (JsonObject *root, const char *err, gpointer data)
{
  Change *c = data;
  Fd *fd = c->fd;
  if (err || !root || !json_object_get_boolean_member_with_default (root, "ok", FALSE)) {
    g_autofree char *t = g_strdup_printf ("%s: %s", label (fd, "error"), err ? err : str_of (root, "error"));
    say (fd, t);
  } else {
    say (fd, label (fd, "saved"));
  }
  g_free (c->section);
  g_free (c->key);
  g_free (c);
  fd_refresh (fd);   /* the switch shows what the node holds now, whatever the answer */
}

static void
change (Fd *fd, const char *section, const char *key, const char *value)
{
  Change *c = g_new0 (Change, 1);
  c->fd = fd;
  c->section = g_strdup (section);
  c->key = g_strdup (key);
  g_autofree char *sk = g_strdup_printf ("%s.%s", section, key);
  const char *const args[] = { "federation", "set", sk, value, "--json", NULL };
  say (fd, label (fd, "waiting_approval"));
  ns_mc_run (G_OBJECT (fd->page), args, NULL, change_done, c);
}

static void
switch_moved (GObject *sw, GParamSpec *ps, gpointer data)
{
  (void) ps;
  Fd *fd = data;
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  const char *sk = g_object_get_data (sw, "ns-fd-key");
  g_auto (GStrv) p = g_strsplit (sk, ".", 2);
  change (fd, p[0], p[1], gtk_switch_get_active (GTK_SWITCH (sw)) ? "true" : "false");
}

static void
value_entered (const char *text, gpointer data)
{
  ValueCtx *vc = data;
  Fd *fd = vc->fd;
  char *end = NULL;
  if (!text || !*text)
    return;
  g_ascii_strtod (text, &end);
  if (end && *end) {
    say (fd, label (fd, "error"));
    return;
  }
  change (fd, "limits", vc->key, text);
}

static void
mode_picked (GtkWidget *row, guint index, gpointer data)
{
  (void) row;
  Fd *fd = data;
  static const char *const modes[] = { "follower", "peer" };
  if (index < G_N_ELEMENTS (modes) && g_strcmp0 (modes[index], str_of (obj_of (fd->st, "self"), "mode")))
    change (fd, "self", "mode", modes[index]);
}

static GtkWidget *
switch_row (Fd *fd, GtkWidget *g, const char *section, const char *key, JsonObject *texts)
{
  GtkWidget *r = ns_row_toggle (g, NULL, NULL, str_of (texts, "title"), str_of (texts, "subtitle"));
  GtkWidget *sw = ns_row_get_switch (r);
  g_object_set_data_full (G_OBJECT (sw), "ns-fd-key", g_strdup_printf ("%s.%s", section, key), g_free);
  g_signal_connect (sw, "notify::active", G_CALLBACK (switch_moved), fd);
  g_hash_table_insert (fd->switches, g_strdup_printf ("%s.%s", section, key), sw);
  return r;
}

/* ---- another device, read-only ---- */

static void
info_if (GtkWidget *g, const char *title, const char *value)
{
  if (value && *value)
    ns_row_info (g, title, value);
}

/* the names of a list of devices ({name, location}), one per line */
static char *
device_names (JsonArray *a, const char *none)
{
  if (!a || !json_array_get_length (a))
    return g_strdup (none);
  g_autoptr (GString) s = g_string_new (NULL);
  for (guint i = 0; i < json_array_get_length (a); i++) {
    JsonObject *d = json_array_get_object_element (a, i);
    if (s->len)
      g_string_append_c (s, '\n');
    g_string_append (s, str_of (d, "name"));
    if (*str_of (d, "location"))
      g_string_append_printf (s, " (%s)", str_of (d, "location"));
  }
  return g_strdup (s->str);
}

static char *
mb_text (double mb)
{
  return mb >= 1024 ? g_strdup_printf ("%.1f GB", mb / 1024) : g_strdup_printf ("%.0f MB", mb);
}

/* the yes/no of every key of an object of booleans and numbers, under the texts' titles */
static void
choices_card (Fd *fd, GtkWidget *content, const char *title, JsonObject *values, JsonObject *texts)
{
  JsonObject *d = ui (fd, "detail");
  GtkWidget *g = ns_group (content, title);
  g_autoptr (GList) keys = values ? json_object_get_members (values) : NULL;
  for (GList *k = keys; k; k = k->next) {
    const char *key = k->data, *t = str_of (obj_of (texts, key), "title");
    double v;
    g_autofree char *text = NULL;
    if (is_bool (values, key))
      text = g_strdup (str_of (d, bool_member (values, key, FALSE) ? "yes" : "no"));
    else if (number_member (values, key, &v))
      text = g_strdup_printf ("%g%s", v, str_of (obj_of (texts, key), "unit"));
    else
      text = g_strdup (str_of (values, key));
    ns_row_info (g, *t ? t : key, text);
  }
}

static void
detail_open (GtkWidget *row, gpointer data)
{
  (void) data;
  Fd *fd = g_object_get_data (G_OBJECT (row), "ns-fd");
  JsonObject *node = g_object_get_data (G_OBJECT (row), "ns-fd-node");
  JsonObject *inv = obj_of (node, "inventory"), *d = ui (fd, "detail"), *sens = ui (fd, "sensors");
  GtkWidget *content;
  AdwNavigationPage *page = ns_page_new (str_of (node, "name"), &content);
  if (!inv) {
    ns_note (content, label (fd, "unavailable"));
    ns_push (page);
    return;
  }
  GtkWidget *g = ns_group (content, str_of (d, "hardware"));
  info_if (g, str_of (d, "model"), str_of (inv, "model"));
  JsonObject *cpu = obj_of (inv, "cpu");
  JsonArray *cl = arr_of (cpu, "clusters");
  g_autoptr (GString) cs = g_string_new (NULL);
  g_autofree char *cores_n = g_strdup_printf ("%d", (int) json_object_get_int_member_with_default (cpu, "cores", 0));
  g_autofree char *cores = subst (str_of (d, "cores"), "n", cores_n);
  g_string_append (cs, cores);
  for (guint i = 0; cl && i < json_array_get_length (cl); i++) {
    JsonObject *c = json_array_get_object_element (cl, i);
    g_string_append_printf (cs, "%s%d × %d MHz", i ? " + " : " • ", (int) json_object_get_int_member_with_default (c, "cores", 0),
                            (int) json_object_get_int_member_with_default (c, "max_mhz", 0));
  }
  ns_row_info (g, str_of (d, "cpu"), cs->str);
  g_autofree char *ram = mb_text (json_object_get_int_member_with_default (inv, "ram_mb", 0));
  ns_row_info (g, str_of (d, "ram"), ram);
  JsonArray *sto = arr_of (inv, "storage");
  for (guint i = 0; sto && i < json_array_get_length (sto); i++) {
    JsonObject *s = json_array_get_object_element (sto, i);
    g_autofree char *fr = mb_text (json_object_get_int_member_with_default (s, "free_mb", 0));
    g_autofree char *to = mb_text (json_object_get_int_member_with_default (s, "total_mb", 0));
    g_autofree char *a = subst (str_of (d, "free"), "free", fr);
    g_autofree char *b = subst (a, "total", to);
    g_autofree char *t = g_strdup_printf ("%s %s", str_of (d, "storage"), str_of (s, "path"));
    ns_row_info (g, t, b);
  }

  g = ns_group (content, str_of (d, "sensors"));
  JsonArray *sa = arr_of (inv, "sensors");
  g_autoptr (GString) ss = g_string_new (NULL);
  for (guint i = 0; sa && i < json_array_get_length (sa); i++) {
    const char *id = json_array_get_string_element (sa, i), *t = str_of (sens, id);
    g_string_append_printf (ss, "%s%s", ss->len ? ", " : "", *t ? t : id);
  }
  ns_row_info (g, str_of (d, "sensors"), ss->len ? ss->str : str_of (d, "nothing"));
  g_autofree char *cams = device_names (arr_of (inv, "cameras"), str_of (d, "nothing"));
  ns_row_info (g, str_of (d, "cameras"), cams);
  g_autofree char *mics = device_names (arr_of (inv, "microphones"), str_of (d, "nothing"));
  ns_row_info (g, str_of (d, "microphones"), mics);
  JsonObject *loc = obj_of (inv, "location");
  g_autofree char *lt = g_strdup_printf ("%s: %s • %s: %s", str_of (d, "gps"), str_of (d, bool_member (loc, "gps", FALSE) ? "yes" : "no"),
                                         str_of (d, "geoclue"), str_of (d, bool_member (loc, "geoclue", FALSE) ? "yes" : "no"));
  ns_row_info (g, str_of (d, "location"), lt);
  g_autofree char *fp = device_names (arr_of (inv, "fingerprint"), str_of (d, "nothing"));
  ns_row_info (g, str_of (d, "fingerprint"), fp);
  JsonArray *mo = arr_of (inv, "modems");
  g_autoptr (GString) ms = g_string_new (NULL);
  for (guint i = 0; mo && i < json_array_get_length (mo); i++) {
    JsonObject *m = json_array_get_object_element (mo, i);
    g_string_append_printf (ms, "%s%s %s • %s • %s", ms->len ? "\n" : "", str_of (m, "manufacturer"), str_of (m, "model"),
                            str_of (m, "state"), bool_member (m, "sim", FALSE) ? str_of (d, "sim") : str_of (d, "no_sim"));
  }
  ns_row_info (g, str_of (d, "modems"), ms->len ? ms->str : str_of (d, "nothing"));

  choices_card (fd, content, str_of (d, "offers"), obj_of (inv, "offers"), ui (fd, "offers"));
  choices_card (fd, content, str_of (d, "limits"), obj_of (inv, "limits"), ui (fd, "limits"));
  gint64 up = json_object_get_int_member_with_default (inv, "updated", 0);
  if (up) {
    g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (up);
    g_autofree char *t = g_date_time_format (dt, "%d/%m %H:%M");
    g_autofree char *line = g_strdup_printf ("%s %s", str_of (d, "updated"), t);
    ns_note (content, line);
  }
  /* the page keeps the node it shows */
  g_object_set_data_full (G_OBJECT (page), "ns-fd-node", json_object_ref (node), (GDestroyNotify) json_object_unref);
  ns_push (page);
}

/* the capability chips of a node: what it offers to think and listen, and the hardware its inventory found */
static GtkWidget *
chips (Fd *fd, JsonObject *node)
{
  JsonObject *c = ui (fd, "chips"), *tags = obj_of (node, "tags"), *inv = obj_of (node, "inventory");
  GtkWidget *box = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (box), 8);
  gtk_widget_set_can_target (box, FALSE);
  struct { const char *id; gboolean on; } list[] = {
    { "think", bool_member (tags, "think", FALSE) },
    { "listen", bool_member (tags, "listen", FALSE) },
    { "cameras", arr_of (inv, "cameras") && json_array_get_length (arr_of (inv, "cameras")) },
    { "microphones", arr_of (inv, "microphones") && json_array_get_length (arr_of (inv, "microphones")) },
    { "gps", bool_member (obj_of (inv, "location"), "gps", FALSE) },
    { "fingerprint", arr_of (inv, "fingerprint") && json_array_get_length (arr_of (inv, "fingerprint")) },
    { "modem", arr_of (inv, "modems") && json_array_get_length (arr_of (inv, "modems")) },
  };
  for (guint i = 0; i < G_N_ELEMENTS (list); i++) {
    if (!list[i].on || !*str_of (c, list[i].id))
      continue;
    GtkWidget *l = gtk_label_new (str_of (c, list[i].id));
    gtk_widget_add_css_class (l, "ns-chip");
    gtk_flow_box_append (GTK_FLOW_BOX (box), l);
  }
  return box;
}

static char *
node_subtitle (Fd *fd, JsonObject *node)
{
  JsonObject *tags = obj_of (node, "tags"), *modes = ui (fd, "modes");
  g_autoptr (GString) s = g_string_new (label (fd, bool_member (node, "alive", FALSE) ? "alive" : "offline"));
  double v;
  if (number_member (tags, "bat", &v))
    g_string_append_printf (s, " • %.0f %%%s%s", v, bool_member (tags, "chg", FALSE) ? " " : "",
                            bool_member (tags, "chg", FALSE) ? label (fd, "charging") : "");
  if (number_member (tags, "load1", &v))
    g_string_append_printf (s, " • %s %.2g", label (fd, "load"), v);
  if (*str_of (tags, "link"))
    g_string_append_printf (s, " • %s %s", label (fd, "link"), str_of (tags, "link"));
  if (*str_of (node, "mode"))
    g_string_append_printf (s, " • %s", str_of (modes, str_of (node, "mode")));
  return g_strdup (s->str);
}

/* ---- the page ---- */

static void
leave_done (JsonObject *root, const char *err, gpointer data)
{
  Fd *fd = data;
  JsonObject *l = ui (fd, "leave");
  if (root && json_object_get_boolean_member_with_default (root, "ok", FALSE))
    say (fd, str_of (l, "started"));
  else if (root && json_object_get_boolean_member_with_default (root, "not_in_federation", FALSE))
    say (fd, str_of (l, "not_in_federation"));
  else {
    g_autofree char *t = g_strdup_printf ("%s: %s", label (fd, "error"), err ? err : str_of (root, "error"));
    say (fd, t);
  }
}

static void
leave_answered (gboolean yes, gpointer data)
{
  Fd *fd = data;
  if (!yes)
    return;
  const char *const args[] = { "federation", "leave", "--json", NULL };
  ns_mc_run (G_OBJECT (fd->page), args, NULL, leave_done, fd);
}

static void
leave_ask (GtkWidget *row, gpointer data)
{
  Fd *fd = data;
  JsonObject *l = ui (fd, "leave");
  ns_confirm (row, str_of (l, "heading"), str_of (l, "body"), str_of (l, "confirm"), TRUE, leave_answered, fd);
}

static void
build (Fd *fd)
{
  JsonObject *sec = ui (fd, "sections"), *notes = ui (fd, "notes"), *modes = ui (fd, "modes");
  ns_page_set_title (fd->page, str_of (obj_of (fd->st, "ui"), "title"));
  fd->this_g = ns_group (fd->content, str_of (sec, "this"));
  if (!json_object_get_boolean_member_with_default (fd->st, "in_federation", FALSE))
    ns_note (fd->content, str_of (notes, "not_in_federation"));
  (void) modes;   /* the rows of this card are made by fill () */
  ns_note (fd->content, str_of (notes, "mode"));

  fd->offers_g = ns_group (fd->content, str_of (sec, "offers"));
  JsonObject *offers = ui (fd, "offers"), *limits = ui (fd, "limits");
  g_autoptr (GList) ok = json_object_get_members (offers);
  for (GList *k = ok; k; k = k->next)
    switch_row (fd, fd->offers_g, "offers", k->data, obj_of (offers, k->data));
  ns_note (fd->content, str_of (notes, "offers"));

  fd->limits_g = ns_group (fd->content, str_of (sec, "limits"));
  g_autoptr (GList) lk = json_object_get_members (limits);
  JsonObject *cur = obj_of (fd->st, "limits");
  for (GList *k = lk; k; k = k->next) {
    const char *key = k->data;
    JsonObject *t = obj_of (limits, key);
    if (is_bool (cur, key)) {
      switch_row (fd, fd->limits_g, "limits", key, t);
    } else {
      ValueCtx *vc = g_new0 (ValueCtx, 1);
      vc->fd = fd;
      vc->key = g_strdup (key);
      GtkWidget *r = ns_row_value (fd->limits_g, str_of (t, "title"), "", FALSE, value_entered, vc);
      g_object_set_data_full (G_OBJECT (r), "ns-fd-value", vc, (GDestroyNotify) value_ctx_free);
      g_hash_table_insert (fd->values, g_strdup (key), r);
    }
  }

  ns_group (fd->content, str_of (sec, "cooperation"));
  ns_note (fd->content, str_of (notes, "cooperation"));
  fd->minervas_g = ns_group (fd->content, str_of (sec, "minervas"));
  fd->nodes_g = ns_group (fd->content, str_of (sec, "nodes"));

  GtkWidget *mg = ns_group (fd->content, str_of (sec, "manage"));
  JsonObject *add = ui (fd, "add"), *leave = ui (fd, "leave");
  GtkWidget *ar = ns_row (mg, "list-add-symbolic", "blue", str_of (add, "title"), str_of (add, "subtitle"));
  gtk_widget_set_focusable (ar, FALSE);
  if (json_object_get_boolean_member_with_default (fd->st, "in_federation", FALSE))
    ns_row_button (mg, str_of (leave, "title"), TRUE, leave_ask, fd);
  fd->built = TRUE;
}

static void
fill (Fd *fd)
{
  JsonObject *self = obj_of (fd->st, "self"), *roles = ui (fd, "roles");
  /* this phone: name, Minerva, role (rebuilt: few rows) */
  ns_group_clear (fd->this_g);
  const char *labels[] = { str_of (ui (fd, "modes"), "follower"), str_of (ui (fd, "modes"), "peer"), NULL };
  fd->mode_row = ns_row_pick (fd->this_g, label (fd, "mode"), labels, !g_strcmp0 (str_of (self, "mode"), "peer"),
                              mode_picked, fd);
  ns_row_info (fd->this_g, label (fd, "name"), *str_of (self, "name") ? str_of (self, "name") : label (fd, "unnamed"));
  ns_row_info (fd->this_g, label (fd, "minerva"), str_of (self, "minerva"));
  if (*str_of (self, "role"))
    ns_row_info (fd->this_g, label (fd, "role"), str_of (roles, str_of (self, "role")));
  if (*str_of (self, "master"))
    ns_row_info (fd->this_g, label (fd, "master"), str_of (self, "master"));

  /* the switches and values follow what the node holds */
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, fd->switches);
  while (g_hash_table_iter_next (&it, &k, &v)) {
    g_auto (GStrv) p = g_strsplit (k, ".", 2);
    ns_switch_quiet (v, bool_member (obj_of (fd->st, p[0]), p[1], FALSE));
  }
  g_hash_table_iter_init (&it, fd->values);
  while (g_hash_table_iter_next (&it, &k, &v)) {
    double n;
    g_autofree char *t = number_member (obj_of (fd->st, "limits"), k, &n) ? g_strdup_printf ("%g", n) : g_strdup ("");
    ns_row_value_set (v, t);
  }

  /* the Minervas */
  ns_group_clear (fd->minervas_g);
  JsonArray *ms = arr_of (fd->st, "minervas");
  for (guint i = 0; ms && i < json_array_get_length (ms); i++) {
    JsonObject *m = json_array_get_object_element (ms, i);
    guint n = json_array_get_length (arr_of (m, "nodes"));
    g_autofree char *num = g_strdup_printf ("%u", n);
    g_autofree char *count = n == 1 ? g_strdup (label (fd, "nodes_one")) : subst (label (fd, "nodes_many"), "n", num);
    g_autofree char *sub = *str_of (m, "master") ? g_strdup_printf ("%s • %s %s", count, label (fd, "master"), str_of (m, "master"))
                                                 : g_strdup (count);
    ns_row_info (fd->minervas_g, *str_of (m, "name") ? str_of (m, "name") : label (fd, "none"), sub);
  }
  ns_group_set_visible (fd->minervas_g, ms && json_array_get_length (ms));

  /* the devices: a row each (a tap opens its inventory), its chips under it */
  ns_group_clear (fd->nodes_g);
  JsonArray *ns = arr_of (fd->st, "nodes");
  for (guint i = 0; ns && i < json_array_get_length (ns); i++) {
    JsonObject *n = json_array_get_object_element (ns, i);
    const char *name = *str_of (n, "name") ? str_of (n, "name") : label (fd, "unnamed");
    g_autofree char *title = bool_member (n, "self", FALSE) && *str_of (n, "name")
                               ? g_strdup_printf ("%s (%s)", name, label (fd, "you")) : g_strdup (name);
    g_autofree char *sub = node_subtitle (fd, n);
    GtkWidget *r = ns_row (fd->nodes_g, "phone-symbolic", bool_member (n, "alive", FALSE) ? "green" : "grey", title, sub);
    g_object_set_data (G_OBJECT (r), "ns-fd", fd);
    g_object_set_data_full (G_OBJECT (r), "ns-fd-node", json_object_ref (n), (GDestroyNotify) json_object_unref);
    ns_row_on_activate (r, detail_open, NULL);
    ns_row_widget (fd->nodes_g, NULL, chips (fd, n));
  }
}

static void
fd_status (JsonObject *root, const char *err, gpointer data)
{
  Fd *fd = data;
  if (err || !root || !*str_of (obj_of (root, "ui"), "title")) {
    say (fd, err ? err : "minerva-config federation: no data");
    return;
  }
  if (fd->st)
    json_object_unref (fd->st);
  fd->st = json_object_ref (root);
  if (!fd->built)
    build (fd);
  fill (fd);
}

static void
fd_refresh (Fd *fd)
{
  static const char *const args[] = { "federation", "status", "--json", NULL };
  ns_mc_run (G_OBJECT (fd->page), args, NULL, fd_status, fd);
}

void
ns_page_federation (GtkWidget *row, gpointer data)
{
  (void) row;
  (void) data;
  Fd *fd = g_new0 (Fd, 1);
  fd->switches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  fd->values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  fd->page = ns_page_new ("", &fd->content);
  g_object_set_data_full (G_OBJECT (fd->page), "ns-fd", fd, (GDestroyNotify) fd_free);
  fd->msg = ns_note (fd->content, "");
  gtk_widget_set_visible (fd->msg, FALSE);
  ns_push (fd->page);
  fd_refresh (fd);
}
