/* Security and privacy > Permissions > Agenti nei canali: which agent of another machine may write into, or read,
 * which chat (docs/external-agents-channels.md, section 2). A viewer of `minerva-config messaging agents --json`
 * (every word and every label comes from there: messaging_ui.json "agents", messaging.conf [agents] and
 * [agent-levels]), and its changes: `grant add` (minerva-config asks the finger or the PIN on the phone's sheet,
 * for exactly the grant's sentence), `grant revoke` and `grant block|unblock` (no proof: they take a privilege away).
 * Each agent x chat x direction has a level: off (never), ask (the default: every message asks), until (a grant,
 * always with an end). A grant is made from a request: an agent asked, and the owner picks the level here. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#define str_of ns_json_str
#define arr_of ns_json_arr
#define obj_of ns_json_obj

typedef struct {
  GObject   *owner;        /* the page: kept alive by every run */
  GtkWidget *group, *none, *msg;
  JsonObject *data;        /* the last `messaging agents --json` */
  gboolean   busy;
} Agents;

typedef struct {
  Agents    *ag;
  JsonObject *row;         /* the request, grant or blocked row it is about */
  GtkWidget *from;
  guint      level, validity, depth;
  GtkWidget *validity_row, *depth_row;
} Pick;

static void agents_refresh (Agents *ag);

static const char *
ui (Agents *ag, const char *key)
{
  return str_of (obj_of (ag->data, "ui"), key);
}

/* a template of the ui with one {name} filled */
static char *
fill (const char *template, const char *name, const char *value)
{
  g_autoptr(GString) s = g_string_new (template);
  g_autofree char *mark = g_strdup_printf ("{%s}", name);

  g_string_replace (s, mark, value, 0);
  return g_strdup (s->str);
}

static void
say (Agents *ag, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (ag->msg), text ? text : "");
  gtk_widget_set_visible (ag->msg, text && *text);
  if (error)
    gtk_widget_add_css_class (ag->msg, "ns-danger");
  else
    gtk_widget_remove_css_class (ag->msg, "ns-danger");
}

static void
run_done (JsonObject *root, const char *err, gpointer data)
{
  Agents *ag = data;

  ag->busy = FALSE;
  if (err)
    {
      g_autofree char *t = fill (ui (ag, "failed"), "why", g_str_has_prefix (err, "error: ") ? err + 7 : err);
      say (ag, t, TRUE);
    }
  else
    say (ag, ui (ag, "saved"), FALSE);
  agents_refresh (ag);
}

static void
run (Agents *ag, const char *const *args)
{
  if (ag->busy)
    return;
  ag->busy = TRUE;
  say (ag, "", FALSE);
  ns_mc_run (ag->owner, args, NULL, run_done, ag);
}

/* "agent/pc-x · Mamma · WhatsApp" */
static char *
title_of (Agents *ag, JsonObject *r)
{
  return g_strdup_printf ("%s · %s · %s", str_of (r, "agent"), str_of (r, "name"),
                          str_of (obj_of (ag->data, "labels"), str_of (r, "provider")));
}

static const char *
direction_of (Agents *ag, JsonObject *r)
{
  return ui (ag, !g_strcmp0 (str_of (r, "direction"), "read") ? "read" : "send");
}

/* ---- a grant: a tap revokes it, after a confirmation ---- */

static void
revoke_answered (gboolean yes, gpointer data)
{
  Pick *p = data;

  if (yes)
    {
      const char *const args[] = { "messaging", "grant", "revoke", str_of (p->row, "id"), "--json", NULL };
      run (p->ag, args);
    }
  json_object_unref (p->row);
  g_free (p);
}

static void
grant_tapped (GtkWidget *row, gpointer data)
{
  Agents *ag = data;
  Pick *p = g_new0 (Pick, 1);

  p->ag = ag;
  p->row = json_object_ref (g_object_get_data (G_OBJECT (row), "agents-row"));
  ns_confirm (row, ui (ag, "revoke_heading"), ui (ag, "revoke_body"), ui (ag, "revoke_yes"), TRUE, revoke_answered, p);
}

static void
unblock_answered (gboolean yes, gpointer data)
{
  Pick *p = data;

  if (yes)
    {
      const char *const args[] = { "messaging", "grant", "unblock", str_of (p->row, "agent"), str_of (p->row, "provider"),
                                   str_of (p->row, "chat"), str_of (p->row, "direction"), "--json", NULL };
      run (p->ag, args);
    }
  json_object_unref (p->row);
  g_free (p);
}

static void
blocked_tapped (GtkWidget *row, gpointer data)
{
  Agents *ag = data;
  Pick *p = g_new0 (Pick, 1);

  p->ag = ag;
  p->row = json_object_ref (g_object_get_data (G_OBJECT (row), "agents-row"));
  ns_confirm (row, ui (ag, "unblock_heading"), "", ui (ag, "unblock_yes"), FALSE, unblock_answered, p);
}

/* ---- a request: its page picks the level, the validity and (a read) the depth ---- */

static void
pick_free (Pick *p)
{
  json_object_unref (p->row);
  g_free (p);
}

static void
level_picked (GtkWidget *row, guint index, gpointer data)
{
  Pick *p = data;
  JsonArray *order = arr_of (obj_of (p->ag->data, "levels"), "order");
  gboolean until = order && index < json_array_get_length (order)
                   && !g_strcmp0 (json_array_get_string_element (order, index), "until");

  p->level = index;
  gtk_widget_set_visible (p->validity_row, until);
  if (p->depth_row)
    gtk_widget_set_visible (p->depth_row, until);
}

static void
validity_picked (GtkWidget *row, guint index, gpointer data)
{
  ((Pick *) data)->validity = index;
}

static void
depth_picked (GtkWidget *row, guint index, gpointer data)
{
  ((Pick *) data)->depth = index;
}

/* the labels of a choices array ({s, label}), NULL-terminated, and the index of the default seconds */
static GStrv
labels_of (JsonArray *a, gint64 preferred, guint *selected)
{
  GStrvBuilder *b = g_strv_builder_new ();

  *selected = 0;
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    {
      JsonObject *o = json_array_get_object_element (a, i);

      g_strv_builder_add (b, str_of (o, "label"));
      if (json_object_get_int_member_with_default (o, "s", 0) == preferred)
        *selected = i;
    }
  return g_strv_builder_unref_to_strv (b);
}

static gint64
seconds_at (JsonArray *a, guint index)
{
  return a && index < json_array_get_length (a)
         ? json_object_get_int_member_with_default (json_array_get_object_element (a, index), "s", 0) : 0;
}

static void
apply_clicked (GtkWidget *button, gpointer data)
{
  Pick *p = data;
  Agents *ag = p->ag;
  JsonArray *order = arr_of (obj_of (ag->data, "levels"), "order");
  const char *level = order && p->level < json_array_get_length (order)
                      ? json_array_get_string_element (order, p->level) : "ask";
  const char *agent = str_of (p->row, "agent"), *provider = str_of (p->row, "provider");
  const char *chat = str_of (p->row, "chat"), *direction = str_of (p->row, "direction");

  if (!g_strcmp0 (level, "off"))
    {
      const char *const args[] = { "messaging", "grant", "block", agent, provider, chat, direction, "--json", NULL };
      run (ag, args);
    }
  else if (!g_strcmp0 (level, "until"))
    {
      g_autofree char *validity = g_strdup_printf ("%" G_GINT64_FORMAT,
                                                   seconds_at (arr_of (ag->data, "validity_choices"), p->validity));
      g_autofree char *depth = g_strdup_printf ("%" G_GINT64_FORMAT,
                                                seconds_at (arr_of (ag->data, "depth_choices"), p->depth));
      gboolean read = !g_strcmp0 (direction, "read");
      const char *const args[] = { "messaging", "grant", "add", agent, provider, chat, direction,
                                   "--validity", validity, read ? "--backlog" : "--json", read ? depth : NULL,
                                   read ? "--json" : NULL, NULL };
      run (ag, args);
    }
  /* ask: nothing to write, the request stays and every message asks */
  gtk_widget_activate_action (button, "navigation.pop", NULL);
}

static void
request_tapped (GtkWidget *row, gpointer data)
{
  Agents *ag = data;
  JsonObject *r = g_object_get_data (G_OBJECT (row), "agents-row");
  JsonObject *levels = obj_of (ag->data, "levels"), *labels = obj_of (levels, "labels");
  JsonObject *limits = obj_of (ag->data, "limits");
  JsonArray *order = arr_of (levels, "order");
  g_autofree char *title = title_of (ag, r);
  GtkWidget *c, *g;
  AdwNavigationPage *page = ns_page_new (direction_of (ag, r), &c);
  Pick *p = g_new0 (Pick, 1);
  GStrvBuilder *lb = g_strv_builder_new ();
  g_auto(GStrv) level_labels = NULL, validity_labels = NULL, depth_labels = NULL;
  guint ask = 0, v = 0, d = 0;

  p->ag = ag;
  p->row = json_object_ref (r);
  g_object_set_data_full (G_OBJECT (page), "agents-pick", p, (GDestroyNotify) pick_free);
  for (guint i = 0; order && i < json_array_get_length (order); i++)
    {
      const char *id = json_array_get_string_element (order, i);

      g_strv_builder_add (lb, str_of (labels, id));
      if (!g_strcmp0 (id, "ask"))
        ask = i;
    }
  level_labels = g_strv_builder_unref_to_strv (lb);
  validity_labels = labels_of (arr_of (ag->data, "validity_choices"),
                               json_object_get_int_member_with_default (limits, "grant_default_s", 0), &v);
  depth_labels = labels_of (arr_of (ag->data, "depth_choices"),
                            json_object_get_int_member_with_default (limits, "backlog_default_s", 0), &d);
  p->level = ask;
  p->validity = v;
  p->depth = d;

  g = ns_group (c, title);
  ns_row_pick (g, ui (ag, "level"), (const char *const *) level_labels, ask, level_picked, p);
  p->validity_row = ns_row_pick (g, ui (ag, "validity"), (const char *const *) validity_labels, v, validity_picked, p);
  gtk_widget_set_visible (p->validity_row, FALSE);
  if (!g_strcmp0 (str_of (r, "direction"), "read"))
    {
      p->depth_row = ns_row_pick (g, ui (ag, "depth_pick"), (const char *const *) depth_labels, d, depth_picked, p);
      gtk_widget_set_visible (p->depth_row, FALSE);
    }
  g = ns_group (c, NULL);
  ns_row_button (g, ui (ag, "grant_button"), FALSE, apply_clicked, p);
  ns_note (c, ui (ag, "grant_note"));
  ns_push (page);
}

/* ---- the card ---- */

static GtkWidget *
add_row (Agents *ag, JsonObject *r, const char *icon, const char *color, const char *sub, NsActivate tapped)
{
  g_autofree char *title = title_of (ag, r);
  GtkWidget *row = ns_row (ag->group, icon, color, title, sub);

  g_object_set_data_full (G_OBJECT (row), "agents-row", json_object_ref (r), (GDestroyNotify) json_object_unref);
  ns_row_on_activate (row, tapped, ag);
  return row;
}

static void
agents_done (JsonObject *root, const char *err, gpointer data)
{
  Agents *ag = data;
  JsonArray *a;
  guint n = 0;

  if (err || !root)
    return;  /* minerva-config without agents: the card stays hidden */
  if (ag->data)
    json_object_unref (ag->data);
  ag->data = json_object_ref (root);
  ns_group_clear (ag->group);
  a = arr_of (root, "grants");
  for (guint i = 0; a && i < json_array_get_length (a); i++, n++)
    {
      JsonObject *g = json_array_get_object_element (a, i);
      const char *left = str_of (g, "left_label");
      g_autofree char *when = *left ? fill (ui (ag, "left"), "left", left) : g_strdup (ui (ag, "expired"));
      g_autofree char *depth = *str_of (g, "depth_label") ? fill (ui (ag, "depth"), "depth", str_of (g, "depth_label"))
                                                          : NULL;
      g_autofree char *sub = depth ? g_strdup_printf ("%s • %s • %s", direction_of (ag, g), when, depth)
                                   : g_strdup_printf ("%s • %s", direction_of (ag, g), when);
      add_row (ag, g, "object-select-symbolic", *left ? "green" : "grey", sub, grant_tapped);
    }
  a = arr_of (root, "requests");
  for (guint i = 0; a && i < json_array_get_length (a); i++, n++)
    {
      JsonObject *r = json_array_get_object_element (a, i);
      g_autofree char *count = g_strdup_printf ("%" G_GINT64_FORMAT, json_object_get_int_member_with_default (r, "count", 0));
      /* one request, or {n} of them: both forms are data ("count_one", "count") */
      g_autofree char *times = json_object_get_int_member_with_default (r, "count", 0) == 1
                               ? g_strdup (ui (ag, "count_one")) : fill (ui (ag, "count"), "n", count);
      g_autofree char *sub = g_strdup_printf ("%s • %s • %s", direction_of (ag, r),
                                              str_of (obj_of (obj_of (root, "ui"), "answers"), str_of (r, "answer")), times);
      add_row (ag, r, "dialog-question-symbolic", "orange", sub, request_tapped);
    }
  a = arr_of (root, "blocked");
  for (guint i = 0; a && i < json_array_get_length (a); i++, n++)
    {
      JsonObject *b = json_array_get_object_element (a, i);
      g_autofree char *sub = g_strdup_printf ("%s • %s", direction_of (ag, b),
                                              str_of (obj_of (obj_of (root, "levels"), "labels"), "off"));
      add_row (ag, b, "action-unavailable-symbolic", "red", sub, blocked_tapped);
    }
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (ag->group), "ns-group-title")), ui (ag, "title"));
  ns_group_set_visible (ag->group, TRUE);
  gtk_label_set_text (GTK_LABEL (ag->none), n ? ui (ag, "description") : ui (ag, "none"));
  gtk_widget_set_visible (ag->none, TRUE);
}

static void
agents_refresh (Agents *ag)
{
  static const char *const args[] = { "messaging", "agents", "--json", NULL };

  ns_mc_run (ag->owner, args, NULL, agents_done, ag);
}

static void
agents_free (Agents *ag)
{
  if (ag->data)
    json_object_unref (ag->data);
  g_free (ag);
}

static void
page_showing (AdwNavigationPage *page, gpointer data)
{
  agents_refresh (data);
}

void
ns_agents_card (AdwNavigationPage *page, GtkWidget *content)
{
  Agents *ag;

  if (!ns_minerva_available ())
    return;
  ag = g_new0 (Agents, 1);
  ag->owner = G_OBJECT (page);
  g_object_set_data_full (G_OBJECT (page), "agents", ag, (GDestroyNotify) agents_free);
  ag->group = ns_group (content, "");  /* its title comes with the data */
  ns_group_set_visible (ag->group, FALSE);
  ag->none = ns_note (content, "");
  gtk_widget_set_visible (ag->none, FALSE);
  ag->msg = ns_note (content, "");
  gtk_widget_set_visible (ag->msg, FALSE);
  g_signal_connect (page, "showing", G_CALLBACK (page_showing), ag);
  agents_refresh (ag);
}
