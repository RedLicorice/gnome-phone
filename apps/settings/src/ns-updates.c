/* Software update, where Minerva is installed: the phone's scheduled updates (Minerva's updates service), read and
 * set through `minerva-config updates` (ns-mc.c). The page shows the last run and the next one, runs one now, and
 * edits the schedule (days, start time, stagger across the federation's phones); GNOME Software stays one tap away
 * for everything else. Every text is the command's ("texts" of `updates get --json`); the titles here are only
 * fallbacks for a command that does not answer. Without Minerva the menu entry opens GNOME Software (main.c). */
#include <neo.h>

#include "ns-mc.h"
#include "ns-pages.h"
#include "ns-ui.h"

/* how often the page asks again while an update runs: the app's setting, this only where its schema is missing */
#define UP_SCHEMA "de.yesman.NeoSettings"
#define UP_POLL_KEY "updates-poll-s"
static const guint up_poll_fallback_s = 3;

/* GNOME Software on its updates: the page's "other" row, and the menu entry without Minerva (main.c) */
const char *const ns_software_updates_argv[] = { "gnome-software", "--mode=updates", NULL };

typedef struct {
  AdwNavigationPage *page;
  GtkWidget *content;
  GtkWidget *error;                       /* a note above the cards */
  GtkWidget *last, *next, *now;           /* the status card, built with the first answer */
  GtkWidget *days, *start, *stagger;      /* the schedule card, likewise */
  GPtrArray *day_values;                  /* days_options' values, in the picker's order */
  char      *start_value;                 /* schedule.start, HH:MM */
  GSettings *settings;                    /* de.yesman.NeoSettings, or NULL */
  guint      poll;
  gboolean   built, running, syncing;
} Up;

static void up_refresh (Up *u);

static void
up_free (Up *u)
{
  if (u->poll)
    g_source_remove (u->poll);
  g_clear_pointer (&u->day_values, g_ptr_array_unref);
  g_free (u->start_value);
  g_free (u);
}

/* what minerva-config said when it failed, above the cards; hidden once it answers */
static void
up_error (Up *u, const char *err)
{
  gtk_widget_set_visible (u->error, err != NULL);
  if (err)
    gtk_label_set_text (GTK_LABEL (u->error), err);
}

static void up_now_tapped (GtkWidget *row, gpointer data);

/* an ISO-8601 time as the phone shows dates and times; a time without a zone is the phone's own */
static char *
up_when (const char *iso)
{
  if (!iso || !*iso)
    return NULL;
  g_autoptr (GTimeZone) local = g_time_zone_new_local ();
  g_autoptr (GDateTime) t = g_date_time_new_from_iso8601 (iso, local);
  if (!t)
    return g_strdup (iso);
  g_autoptr (GDateTime) here = g_date_time_to_local (t);
  return g_date_time_format (here, neo_clock_uses_24h () ? "%x %H:%M" : "%x %l:%M %p");
}

static void
up_show_last (Up *u, JsonObject *last, JsonObject *texts)
{
  g_autofree char *when = up_when (ns_json_str (last, "time"));
  if (!when) {
    ns_row_set_subtitle (u->last, ns_json_str (texts, "never"));
    return;
  }
  const char *result = ns_json_str (last, "result");
  g_autoptr (GString) t = g_string_new (when);
  const char *label = ns_json_str (ns_json_obj (texts, "results"), result);
  if (*label)
    g_string_append_printf (t, " • %s", label);
  JsonArray *installed = ns_json_arr (last, "installed");
  if (installed && json_array_get_length (installed))
    g_string_append_printf (t, " (%u)", json_array_get_length (installed));
  const char *reason = ns_json_str (last, "reason");
  if (*reason && (g_str_equal (result, "skipped") || g_str_equal (result, "deferred") || g_str_equal (result, "failed")))
    g_string_append_printf (t, " • %s", reason);
  ns_row_set_subtitle (u->last, t->str);
}

static void
up_saved (JsonObject *root, const char *err, gpointer data)
{
  Up *u = data;
  up_error (u, err);
  up_refresh (u);
}

/* one key of the schedule, saved at once (each control changes one value per user action) */
static void
up_save (Up *u, const char *key, const char *value)
{
  g_autofree char *pair = g_strdup_printf ("updates.%s=%s", key, value);
  const char *const args[] = { "set", "--json", "--quiet", pair, NULL };
  ns_mc_run (G_OBJECT (u->page), args, NULL, up_saved, u);
}

static void
up_days_picked (GtkWidget *row, guint index, gpointer data)
{
  Up *u = data;
  if (!u->syncing && index < u->day_values->len)
    up_save (u, "days", u->day_values->pdata[index]);
}

static void
up_start_chosen (int hour, int minute, gpointer data)
{
  g_autofree char *v = g_strdup_printf ("%02d:%02d", hour, minute);
  up_save (data, "start", v);
}

static void
up_start_tapped (GtkWidget *row, gpointer data)
{
  Up *u = data;
  int hour = 0, minute = 0;
  if (u->start_value)
    sscanf (u->start_value, "%d:%d", &hour, &minute);
  neo_time_picker_dialog (row, hour, minute, up_start_chosen, u, NULL);
}

static void
up_stagger_entered (const char *text, gpointer data)
{
  Up *u = data;
  char *end = NULL;
  gint64 n = g_ascii_strtoll (text ? text : "", &end, 10);
  if (!text || end == text || n < 0) {
    up_refresh (u);   /* not a number of minutes: the saved value again */
    return;
  }
  g_autofree char *v = g_strdup_printf ("%" G_GINT64_FORMAT, n);
  up_save (u, "stagger_min", v);
}

static void
up_other_tapped (GtkWidget *row, gpointer data)
{
  ns_spawn (ns_software_updates_argv);
}

static void
up_build (Up *u, JsonObject *root, JsonObject *texts)
{
  u->built = TRUE;
  ns_page_set_title (u->page, ns_json_str (texts, "title"));

  GtkWidget *st = ns_group (u->content, NULL);
  u->last = ns_row_info (st, ns_json_str (texts, "last"), "");
  u->next = ns_row_info (st, ns_json_str (texts, "next"), "");
  u->now = ns_row_button (st, ns_json_str (texts, "now"), FALSE, up_now_tapped, u);

  GtkWidget *g = ns_group (u->content, NULL);
  u->day_values = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  JsonArray *opts = ns_json_arr (root, "days_options");
  for (guint i = 0; opts && i < json_array_get_length (opts); i++) {
    JsonObject *o = json_array_get_object_element (opts, i);
    g_ptr_array_add (u->day_values, g_strdup (ns_json_str (o, "value")));
    g_ptr_array_add (labels, g_strdup (ns_json_str (o, "label")));
  }
  /* a schedule set by hand to a value the options do not list: shown as it is, so the picker never lies */
  const char *days = ns_json_str (ns_json_obj (root, "schedule"), "days");
  gboolean listed = FALSE;
  for (guint i = 0; i < u->day_values->len; i++)
    listed |= g_str_equal (u->day_values->pdata[i], days);
  if (!listed) {
    g_ptr_array_add (u->day_values, g_strdup (days));
    g_ptr_array_add (labels, g_strdup (days));
  }
  g_ptr_array_add (labels, NULL);
  u->days = ns_row_pick (g, ns_json_str (texts, "days"), (const char *const *) labels->pdata, 0, up_days_picked, u);
  u->start = ns_row (g, NULL, NULL, ns_json_str (texts, "start"), NULL);
  ns_row_on_activate (u->start, up_start_tapped, u);
  g_autofree char *stagger = g_strdup_printf ("%s (%s)", ns_json_str (texts, "stagger"),
                                              ns_json_str (texts, "stagger_unit"));
  u->stagger = ns_row_value (g, stagger, "", FALSE, up_stagger_entered, u);

  GtkWidget *o = ns_group (u->content, NULL);
  GtkWidget *other = ns_row (o, "software-update-available-symbolic", "blue", ns_json_str (texts, "other"),
                             ns_json_str (texts, "other_subtitle"));
  ns_row_on_activate (other, up_other_tapped, u);
  g_autofree char *software = g_find_program_in_path (ns_software_updates_argv[0]);
  gtk_widget_set_visible (other, software != NULL);

  const char *note = ns_json_str (texts, "note");
  if (*note)
    ns_note (u->content, note);
}

static gboolean
up_poll_tick (gpointer data)
{
  Up *u = data;
  u->poll = 0;
  up_refresh (u);
  return G_SOURCE_REMOVE;
}

static void
up_status (JsonObject *root, const char *err, gpointer data)
{
  Up *u = data;
  up_error (u, err);
  if (err || !root)
    return;
  JsonObject *texts = ns_json_obj (root, "texts"), *schedule = ns_json_obj (root, "schedule");
  if (!u->built)
    up_build (u, root, texts);

  u->running = json_object_get_boolean_member_with_default (root, "running", FALSE);
  gtk_widget_set_sensitive (u->now, !u->running);
  up_show_last (u, ns_json_obj (root, "last"), texts);
  g_autofree char *next = up_when (ns_json_str (root, "next"));
  ns_row_set_subtitle (u->next, next ? next : ns_json_str (schedule, "install_at"));

  const char *days = ns_json_str (schedule, "days");
  for (guint i = 0; i < u->day_values->len; i++)
    if (g_str_equal (u->day_values->pdata[i], days)) {
      u->syncing = TRUE;
      ns_row_pick_select (u->days, i);
      u->syncing = FALSE;
    }
  g_free (u->start_value);
  u->start_value = g_strdup (ns_json_str (schedule, "start"));
  /* this phone's turn in the stagger: its own install time beside the federation's start */
  gint64 offset = json_object_get_int_member_with_default (schedule, "offset_min", 0);
  g_autofree char *start = offset > 0 ? g_strdup_printf ("%s • %s", u->start_value, ns_json_str (schedule, "install_at"))
                                      : g_strdup (u->start_value);
  ns_row_set_subtitle (u->start, start);
  g_autofree char *stagger = g_strdup_printf ("%" G_GINT64_FORMAT,
                                              json_object_get_int_member_with_default (schedule, "stagger_min", 0));
  ns_row_value_set (u->stagger, stagger);

  if (u->running && !u->poll) {
    guint every = u->settings ? (guint) g_settings_get_int (u->settings, UP_POLL_KEY) : up_poll_fallback_s;
    u->poll = g_timeout_add_seconds (every, up_poll_tick, u);
  }
}

static void
up_refresh (Up *u)
{
  static const char *const args[] = { "updates", "get", "--json", NULL };
  ns_mc_run (G_OBJECT (u->page), args, NULL, up_status, u);
}

static void
up_ran (JsonObject *root, const char *err, gpointer data)
{
  Up *u = data;
  up_error (u, err);
  up_refresh (u);
}

static void
up_now_tapped (GtkWidget *row, gpointer data)
{
  Up *u = data;
  if (u->running)
    return;
  u->running = TRUE;
  gtk_widget_set_sensitive (u->now, FALSE);
  static const char *const args[] = { "updates", "run", "--json", NULL };
  ns_mc_run (G_OBJECT (u->page), args, NULL, up_ran, u);
}

void
ns_page_updates (GtkWidget *row, gpointer data)
{
  if (!ns_minerva_available ()) {   /* --page updates without Minerva: GNOME Software, as the menu does */
    ns_spawn (ns_software_updates_argv);
    return;
  }
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Software update", &c);
  Up *u = g_new0 (Up, 1);
  u->page = page;
  u->content = c;
  g_object_set_data_full (G_OBJECT (page), "ns-updates", u, (GDestroyNotify) up_free);
  GSettings *s = ns_page_settings (page, UP_SCHEMA);
  u->settings = s && ns_has_key (s, UP_POLL_KEY) ? s : NULL;

  /* the cards come with the first answer, which carries their texts */
  u->error = ns_note (c, "");
  gtk_widget_set_visible (u->error, FALSE);
  ns_push (page);
  up_refresh (u);
}
