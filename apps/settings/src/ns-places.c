/* Minerva > Luoghi: the places of her world, the shipped ones and her own. Every text is a line of minerva_ui.json
 * (pages.places.labels, through `minerva-config status`); every read and every change goes through
 * `minerva-config places … --json`, which asks the kiosk: it owns the places, so a rename or a delete is validated and
 * written there, and a refusal comes back as the sentence to show. Nothing here blocks: the page fills when the
 * answers come, and changes nothing while an answer is on its way. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define str_of ns_json_str
#define obj_of ns_json_obj
#define arr_of ns_json_arr

typedef struct {
  AdwNavigationPage *page;
  GtkWidget *intro, *note, *group_shipped, *group_hers, *empty_hers;
  GtkWidget *hers_title;   /* the label above her card, filled from the data */
  JsonObject *ui;          /* minerva_ui.json.pages.places, kept for the page's life */
  gboolean busy;
} Pl;

/* one of her places, while its manage page is open */
typedef struct {
  Pl   *p;
  char *id, *name;
  AdwNavigationPage *page;
} Plc;

/* a text of the data with "{key}" replaced, NULL ends the pairs */
static char *
fmt (const char *tpl, ...)
{
  GString *s = g_string_new (tpl ? tpl : "");
  va_list ap;
  va_start (ap, tpl);
  const char *key;
  while ((key = va_arg (ap, const char *))) {
    const char *val = va_arg (ap, const char *);
    g_autofree char *mark = g_strdup_printf ("{%s}", key);
    g_string_replace (s, mark, val ? val : "", 0);
  }
  va_end (ap);
  return g_string_free (s, FALSE);
}

static const char *
label (Pl *p, const char *key)
{
  const char *t = str_of (obj_of (p->ui, "labels"), key);
  return *t ? t : "";
}

static double
num_of (JsonObject *o, const char *member)
{
  return o ? json_object_get_double_member_with_default (o, member, 0) : 0;
}

static gint64
int_of (JsonObject *o, const char *member)
{
  return o ? json_object_get_int_member_with_default (o, member, 0) : 0;
}

static void say (Pl *p, const char *text, gboolean bad);

static void load (Pl *p);

/* her liking (-1..1) as one of the data's words */
static const char *
liking_word (Pl *p, double liking)
{
  JsonArray *words = arr_of (obj_of (p->ui, "labels"), "liking_words");
  guint n = words ? json_array_get_length (words) : 0;
  if (n == 0)
    return "";
  guint i = (guint) (((liking + 1.0) / 2.0) * (double) (n - 1) + 0.5);
  return json_array_get_string_element (words, i < n ? i : n - 1);
}

static char *
visits_text (Pl *p, JsonObject *place)
{
  if (int_of (place, "count") == 0)
    return g_strdup (label (p, "never"));
  g_autofree char *count = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (place, "count"));
  g_autofree char *minutes = g_strdup_printf ("%" G_GINT64_FORMAT, int_of (place, "seconds") / 60);
  return fmt (label (p, int_of (place, "count") == 1 ? "visits_one" : "visits"), "count", count, "minutes", minutes, NULL);
}

static void
say (Pl *p, const char *text, gboolean bad)
{
  if (!p->note)
    return;
  gtk_label_set_text (GTK_LABEL (p->note), text ? text : "");
  if (bad)
    gtk_widget_add_css_class (p->note, "ns-danger");
  else
    gtk_widget_remove_css_class (p->note, "ns-danger");
  gtk_widget_set_visible (p->note, text && *text);
}

/* ---- one of her places: its page ---- */

static void
place_free (Plc *c)
{
  g_free (c->id);
  g_free (c->name);
  g_free (c);
}

static void
changed_done (JsonObject *root, const char *err, gpointer data)
{
  Plc *c = data;
  Pl *p = c->p;
  gboolean deleted = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (c->page), "ns-deleted"));
  p->busy = FALSE;
  (void) root;
  if (err)
    say (p, fmt (label (p, "refused"), "error", err, NULL), TRUE);
  else
    say (p, label (p, deleted ? "deleted" : "renamed"), FALSE);
  gtk_widget_activate_action (GTK_WIDGET (p->page), "navigation.pop", NULL);
  load (p);
}

static void
run_change (Plc *c, gboolean deleted, const char *const *args)
{
  Pl *p = c->p;
  if (p->busy)
    return;
  p->busy = TRUE;
  g_object_set_data (G_OBJECT (c->page), "ns-deleted", GINT_TO_POINTER (deleted));
  ns_mc_run (G_OBJECT (c->page), args, NULL, changed_done, c);
}

static void
renamed (const char *text, gpointer data)
{
  Plc *c = data;
  const char *const args[] = { "places", "rename", c->id, text, "--json", NULL };
  run_change (c, FALSE, args);
}

static void
delete_yes (gboolean yes, gpointer data)
{
  Plc *c = data;
  if (!yes)
    return;
  const char *const args[] = { "places", "forget", c->id, "--json", NULL };
  run_change (c, TRUE, args);
}

static void
delete_tapped (GtkWidget *row, gpointer data)
{
  Plc *c = data;
  Pl *p = c->p;
  g_autofree char *heading = fmt (label (p, "delete_heading"), "name", c->name, NULL);
  ns_confirm (row, heading, label (p, "delete_body"), label (p, "delete_ok"), TRUE, delete_yes, c);
}

static void
place_page (GtkWidget *row, gpointer data)
{
  Pl *p = data;
  JsonObject *place = g_object_get_data (G_OBJECT (row), "ns-place");
  if (!place)
    return;
  Plc *c = g_new0 (Plc, 1);
  GtkWidget *content;
  c->p = p;
  c->id = g_strdup (str_of (place, "id"));
  c->name = g_strdup (str_of (place, "name"));
  c->page = ns_page_new (c->name, &content);
  g_object_set_data_full (G_OBJECT (c->page), "ns-placec", c, (GDestroyNotify) place_free);

  GtkWidget *g = ns_group (content, NULL);
  g_autofree char *visits = visits_text (p, place);
  ns_row_value (g, label (p, "rename"), c->name, FALSE, renamed, c);
  ns_row_info (g, label (p, "note"), *str_of (place, "note") ? str_of (place, "note") : label (p, "no_note"));
  ns_row_info (g, label (p, "liking"), liking_word (p, num_of (place, "liking")));
  ns_row_info (g, label (p, "visits_row"), visits);

  GtkWidget *d = ns_group (content, NULL);
  ns_row_button (d, label (p, "delete"), TRUE, delete_tapped, c);
  ns_push (c->page);
}

/* ---- the list ---- */

static void
row_for (Pl *p, GtkWidget *group, JsonObject *place, gboolean hers)
{
  const char *note = str_of (place, "note");
  g_autofree char *visits = visits_text (p, place);
  g_autofree char *here = fmt (label (p, "here"), NULL);
  gboolean current = !g_strcmp0 (str_of (place, "id"), str_of (p->ui, "current"));
  g_autofree char *sub = fmt (label (p, "row"), "note", *note ? note : label (p, "no_note"), "visits", visits,
                              "liking", liking_word (p, num_of (place, "liking")), "here", current ? here : "", NULL);
  GtkWidget *r = ns_row (group, hers ? "starred-symbolic" : "mark-location-symbolic", hers ? "green" : "grey",
                         str_of (place, "name"), sub);
  if (hers) {
    g_object_set_data_full (G_OBJECT (r), "ns-place", json_object_ref (place), (GDestroyNotify) json_object_unref);
    ns_row_on_activate (r, place_page, p);
  }
}

static void
list_done (JsonObject *root, const char *err, gpointer data)
{
  Pl *p = data;
  guint hers = 0, shipped = 0;
  ns_group_clear (p->group_shipped);
  ns_group_clear (p->group_hers);
  if (err) {
    say (p, fmt (label (p, "refused"), "error", err, NULL), TRUE);
    return;
  }
  JsonArray *places = arr_of (root, "places");
  for (guint i = 0; places && i < json_array_get_length (places); i++) {
    JsonObject *o = json_array_get_object_element (places, i);
    gboolean mine = !g_strcmp0 (str_of (o, "created_by"), "minerva");
    row_for (p, mine ? p->group_hers : p->group_shipped, o, mine);
    if (mine)
      hers++;
    else
      shipped++;
  }
  gtk_widget_set_visible (g_object_get_data (G_OBJECT (p->group_shipped), "ns-group-title"), shipped > 0);
  ns_group_set_visible (p->group_shipped, shipped > 0);
  gtk_widget_set_visible (p->hers_title, hers > 0);
  ns_group_set_visible (p->group_hers, hers > 0);
  gtk_label_set_text (GTK_LABEL (p->empty_hers), label (p, "empty_hers"));
  gtk_widget_set_visible (p->empty_hers, hers == 0);
  if (shipped + hers == 0)
    say (p, label (p, "empty_all"), TRUE);
  else if (!json_object_get_boolean_member_with_default (root, "kiosk", TRUE))
    say (p, label (p, "not_kiosk"), FALSE);   /* the list came from the files: it can be read, not changed */
}

static void
load (Pl *p)
{
  const char *const args[] = { "places", "list", "--json", NULL };
  ns_mc_run (G_OBJECT (p->page), args, NULL, list_done, p);
}

/* the labels arrive with the status; until then the page is blank (nothing is guessed) */
static void
status_done (JsonObject *root, const char *err, gpointer data)
{
  Pl *p = data;
  JsonObject *ui = err ? NULL : obj_of (obj_of (obj_of (root, "minerva_ui"), "pages"), "places");
  if (!ui || !*str_of (ui, "title")) {
    if (err)
      say (p, err, TRUE);
    return;   /* no data file, or an older minerva-config: nothing is invented */
  }
  g_clear_pointer (&p->ui, json_object_unref);
  p->ui = json_object_ref (ui);
  ns_page_set_title (p->page, str_of (ui, "title"));
  gtk_label_set_text (GTK_LABEL (p->intro), label (p, "intro"));
  gtk_widget_set_visible (p->intro, *label (p, "intro") != 0);
  gtk_label_set_text (GTK_LABEL (p->hers_title), label (p, "group_hers"));
  gtk_widget_set_visible (p->hers_title, *label (p, "group_hers") != 0);
  load (p);
}

void
ns_page_minerva_places (GtkWidget *row, gpointer data)
{
  (void) row;
  (void) data;
  Pl *p = g_new0 (Pl, 1);
  GtkWidget *content;
  p->page = ns_page_new ("", &content);
  g_object_set_data_full (G_OBJECT (p->page), "ns-places", p, g_free);
  p->intro = ns_note (content, "");
  gtk_widget_set_visible (p->intro, FALSE);
  p->group_shipped = ns_group (content, "");
  p->hers_title = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (p->hers_title), 0);
  gtk_widget_add_css_class (p->hers_title, "ns-group-title");
  gtk_box_append (GTK_BOX (content), p->hers_title);
  p->group_hers = ns_group (content, NULL);
  p->empty_hers = ns_note (content, "");
  gtk_widget_set_visible (p->empty_hers, FALSE);
  p->note = ns_note (content, "");
  gtk_widget_set_visible (p->note, FALSE);
  ns_push (p->page);
  const char *const args[] = { "status", "--json", "--fast", NULL };
  ns_mc_run (G_OBJECT (p->page), args, NULL, status_done, p);
}
