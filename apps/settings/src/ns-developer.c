/* Settings > Developer options: the section Log (Events, Retention) and the section Retries (services, cognitive
 * backend).
 * The texts and the Events page's data come from `minerva-config developer --json` (Minerva's
 * config/minerva_config/developer_ui.json): categories as journal matches, row titles by a field's value, levels,
 * periods. Retention and Services are brain.conf groups (settings_ui.json pages log_retention and retries), embedded
 * with ns_minerva_embed; the root minerva-system-settings applies them to systemd.
 * The Events page reads the system journal itself (sd-journal; the desktop user is in systemd-journal): nothing is
 * copied anywhere. Newest first, a page at a time, followed live while the page is open. */
#include <glib-unix.h>
#include <grp.h>
#include <syslog.h>
#include <unistd.h>
#include <systemd/sd-journal.h>

#include "ns-mc.h"
#include "ns-pages.h"
#include "ns-ui.h"

/* ---- the data ---- */

static const char *
text (JsonObject *ui, const char *key)
{
  return ns_json_str (ns_json_obj (ui, "texts"), key);
}

static gint64
number (JsonObject *ui, const char *key)
{
  return ui ? json_object_get_int_member_with_default (ui, key, 0) : 0;
}

/* whether this process can read the system journal: its groups hold `group` (the data's journal_group; from the next
 * login on, after minerva's postinst added the user) */
static gboolean
journal_readable (const char *group)
{
  struct group *g = group && *group ? getgrnam (group) : NULL;
  if (!g)
    return FALSE;
  if (getegid () == g->gr_gid)
    return TRUE;
  int n = getgroups (0, NULL);
  if (n <= 0)
    return FALSE;
  g_autofree gid_t *gids = g_new (gid_t, n);
  n = getgroups (n, gids);
  for (int i = 0; i < n; i++)
    if (gids[i] == g->gr_gid)
      return TRUE;
  return FALSE;
}

/* one field of the entry the journal is on, without "FIELD=", valid UTF-8, or NULL */
static char *
field (sd_journal *j, const char *name)
{
  const void *data;
  size_t len, plen = strlen (name) + 1;
  if (sd_journal_get_data (j, name, &data, &len) < 0 || len < plen)
    return NULL;
  return g_utf8_make_valid ((const char *) data + plen, len - plen);
}

static char *
cursor_here (sd_journal *j)
{
  char *c = NULL;
  if (sd_journal_get_cursor (j, &c) < 0)
    return NULL;
  char *own = g_strdup (c);
  free (c);
  return own;
}

/* ---- the Events page ---- */

typedef struct {
  JsonObject *ui;
  sd_journal *j;
  AdwNavigationPage *page;
  GtkWidget  *list;          /* the card of the entries */
  GtkWidget  *more;          /* "more" at its end, while there are older ones */
  GtkWidget  *empty;         /* the line shown when nothing matches */
  GtkWidget  *search;
  GtkWidget  *component_row, *level_row, *period_row;
  GPtrArray  *components;    /* "FIELD=value" of the component filter; index 0 is "all" (NULL) */
  guint       category, level, period, component;
  guint       shown;
  char       *head, *tail;   /* the newest entry read (new ones come after it), the oldest (the next page before it) */
  guint       watch, reload;
} Ev;

static void
ev_free (Ev *e)
{
  g_clear_handle_id (&e->watch, g_source_remove);
  g_clear_handle_id (&e->reload, g_source_remove);
  g_clear_pointer (&e->j, sd_journal_close);
  g_clear_pointer (&e->ui, json_object_unref);
  g_clear_pointer (&e->components, g_ptr_array_unref);
  g_free (e->head);
  g_free (e->tail);
  g_free (e);
}

static JsonObject *
nth (JsonObject *ui, const char *member, guint i)
{
  JsonArray *a = ns_json_arr (ui, member);
  return a && i < json_array_get_length (a) ? json_array_get_object_element (a, i) : NULL;
}

static void
match (Ev *e, const char *m)
{
  if (sd_journal_add_match (e->j, m, 0) < 0)
    g_warning ("events: not a journal match: %s", m);
}

/* the matches of the filters: the category's groups (any of them), and the component, and the priorities up to the
 * level's */
static void
ev_matches (Ev *e)
{
  sd_journal_flush_matches (e->j);
  JsonArray *groups = ns_json_arr (nth (e->ui, "categories", e->category), "match");
  for (guint g = 0; groups && g < json_array_get_length (groups); g++) {
    JsonArray *group = json_array_get_array_element (groups, g);
    if (g > 0)
      sd_journal_add_disjunction (e->j);
    for (guint m = 0; m < json_array_get_length (group); m++)
      match (e, json_array_get_string_element (group, m));
  }
  const char *component = e->component ? g_ptr_array_index (e->components, e->component) : NULL;
  if (component) {
    sd_journal_add_conjunction (e->j);
    match (e, component);
  }
  JsonObject *level = nth (e->ui, "levels", e->level);
  gint64 max = level ? json_object_get_int_member_with_default (level, "max_priority", LOG_DEBUG) : LOG_DEBUG;
  if (max < LOG_DEBUG) {
    sd_journal_add_conjunction (e->j);
    for (gint64 p = 0; p <= max; p++) {
      g_autofree char *m = g_strdup_printf ("PRIORITY=%" G_GINT64_FORMAT, p);
      match (e, m);
    }
  }
}

/* the row's title: the data's title for one of the entry's field values, else the message's first line */
static char *
ev_title (Ev *e, sd_journal *j, const char *message)
{
  JsonObject *titles = ns_json_obj (e->ui, "titles");
  g_autoptr (GList) names = titles ? json_object_get_members (titles) : NULL;
  for (GList *l = names; l; l = l->next) {
    g_autofree char *v = field (j, l->data);
    const char *t = v ? ns_json_str (ns_json_obj (titles, l->data), v) : "";
    if (*t)
      return g_strdup (t);
  }
  const char *nl = message ? strchr (message, '\n') : NULL;
  return message ? (nl ? g_strndup (message, nl - message) : g_strdup (message)) : g_strdup ("");
}

static char *
ev_component (Ev *e)
{
  JsonArray *fields = ns_json_arr (e->ui, "component_fields");
  for (guint i = 0; fields && i < json_array_get_length (fields); i++) {
    char *v = field (e->j, json_array_get_string_element (fields, i));
    if (v)
      return v;
  }
  return g_strdup ("");
}

static void ev_open_entry (GtkWidget *row, gpointer data);

/* the row of the entry the journal is on, when the search text is in it; NULL when it is not */
static GtkWidget *
ev_add_row (Ev *e, const char *needle)
{
  g_autofree char *message = field (e->j, "MESSAGE");
  g_autofree char *title = ev_title (e, e->j, message);
  if (needle && *needle) {
    g_autofree char *hay = g_utf8_casefold (message ? message : "", -1);
    g_autofree char *t = g_utf8_casefold (title, -1);
    if (!strstr (hay, needle) && !strstr (t, needle))
      return NULL;
  }
  uint64_t usec = 0;
  sd_journal_get_realtime_usec (e->j, &usec);
  g_autoptr (GDateTime) when = g_date_time_new_from_unix_local (usec / G_USEC_PER_SEC);
  g_autofree char *at = when ? g_date_time_format (when, ns_json_str (e->ui, "time_format")) : g_strdup ("");
  g_autofree char *comp = ev_component (e);
  g_autofree char *prio = field (e->j, "PRIORITY");
  guint p = prio ? (guint) g_ascii_strtoull (prio, NULL, 10) : LOG_INFO;
  JsonArray *colors = ns_json_arr (e->ui, "level_colors");
  const char *color = colors && p < json_array_get_length (colors) ? json_array_get_string_element (colors, p) : NULL;
  /* the message under the data's title, when the title is not the message */
  const char *msg = message && g_strcmp0 (title, message) ? message : "";
  g_autofree char *sub = *msg ? g_strdup_printf ("%s · %s · %s", at, comp, msg) : g_strdup_printf ("%s · %s", at, comp);
  GtkWidget *row = ns_row (e->list, "media-record-symbolic", color, title, sub);
  g_object_set_data_full (G_OBJECT (row), "ev-cursor", cursor_here (e->j), g_free);
  ns_row_on_activate (row, ev_open_entry, e);
  e->shown++;
  return row;
}

static void ev_more (GtkWidget *row, gpointer data);

/* the next page: entries older than the oldest read, at most page_size rows and scan_limit entries examined */
static void
ev_page (Ev *e)
{
  if (e->more) {
    gtk_list_box_remove (GTK_LIST_BOX (e->list), e->more);
    e->more = NULL;
  }
  g_autofree char *needle = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (e->search)), -1);
  JsonObject *period = nth (e->ui, "periods", e->period);
  gint64 secs = period ? json_object_get_int_member_with_default (period, "seconds", 0) : 0;
  uint64_t since = secs > 0 ? (uint64_t) (g_get_real_time () - secs * G_USEC_PER_SEC) : 0;
  gint64 size = number (e->ui, "page_size"), scan = number (e->ui, "scan_limit");
  if (e->tail) {
    sd_journal_seek_cursor (e->j, e->tail);
    sd_journal_previous (e->j);   /* onto the oldest read: the loop starts before it */
  } else {
    sd_journal_seek_tail (e->j);
  }
  gint64 added = 0, examined = 0;
  gboolean older = FALSE;
  while (sd_journal_previous (e->j) > 0) {
    uint64_t usec = 0;
    if (since && sd_journal_get_realtime_usec (e->j, &usec) >= 0 && usec < since)
      break;
    g_free (e->tail);
    e->tail = cursor_here (e->j);
    if (!e->head)
      e->head = g_strdup (e->tail);
    if (ev_add_row (e, needle))
      added++;
    examined++;
    if ((size > 0 && added >= size) || (scan > 0 && examined >= scan)) {
      older = sd_journal_previous (e->j) > 0;   /* only when there is one */
      break;
    }
  }
  gtk_widget_set_visible (e->empty, e->shown == 0);
  if (older)
    e->more = ns_row_button (e->list, text (e->ui, "more"), FALSE, ev_more, e);
}

static void
ev_more (GtkWidget *row, gpointer data)
{
  ev_page (data);
}

/* the list again from the newest entry, with the filters as they are */
static void
ev_fill (Ev *e)
{
  ns_group_clear (e->list);
  e->more = NULL;
  e->shown = 0;
  g_clear_pointer (&e->head, g_free);
  g_clear_pointer (&e->tail, g_free);
  ev_matches (e);
  ev_page (e);
}

/* the entries newer than the newest read, each on top of the list: the rows read so far and the pages stay */
static gboolean
ev_follow (gpointer data)
{
  Ev *e = data;
  e->reload = 0;
  if (!e->head) {
    ev_fill (e);
    return G_SOURCE_REMOVE;
  }
  g_autofree char *needle = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (e->search)), -1);
  sd_journal_seek_cursor (e->j, e->head);
  sd_journal_next (e->j);   /* onto the newest read: the loop starts after it */
  while (sd_journal_next (e->j) > 0) {
    g_free (e->head);
    e->head = cursor_here (e->j);
    GtkWidget *row = ev_add_row (e, needle);
    if (row) {
      g_object_ref (row);
      gtk_list_box_remove (GTK_LIST_BOX (e->list), row);
      gtk_list_box_insert (GTK_LIST_BOX (e->list), row, 0);
      g_object_unref (row);
    }
  }
  gtk_widget_set_visible (e->empty, e->shown == 0);
  return G_SOURCE_REMOVE;
}

/* the journal changed while the page is open (new entries, or a new file after a rotation): the new entries are added
 * once they stop coming for reload_ms */
static gboolean
ev_journal_changed (int fd, GIOCondition cond, gpointer data)
{
  Ev *e = data;
  int r = sd_journal_process (e->j);
  if (r == SD_JOURNAL_APPEND || r == SD_JOURNAL_INVALIDATE) {
    g_clear_handle_id (&e->reload, g_source_remove);
    e->reload = g_timeout_add ((guint) MAX (number (e->ui, "reload_ms"), 0), ev_follow, e);
  }
  return G_SOURCE_CONTINUE;
}

static void
ev_chip_toggled (GtkToggleButton *b, gpointer data)
{
  Ev *e = data;
  if (!gtk_toggle_button_get_active (b))
    return;
  e->category = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "ev-index"));
  ev_fill (e);
}

static void
ev_picked (GtkWidget *row, guint index, gpointer data)
{
  Ev *e = data;
  if (row == e->level_row)
    e->level = index;
  else if (row == e->period_row)
    e->period = index;
  else
    e->component = index;
  ev_fill (e);
}

static void
ev_search_changed (GtkSearchEntry *s, gpointer data)
{
  ev_fill (data);
}

/* the labels of a list of {title} objects */
static GStrv
titles_of (JsonObject *ui, const char *member)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  JsonArray *a = ns_json_arr (ui, member);
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    g_strv_builder_add (b, ns_json_str (json_array_get_object_element (a, i), "title"));
  return g_strv_builder_end (b);
}

/* the component filter's choices: every value the journal has for the component fields, each once (the first field
 * that has it) */
static GStrv
ev_components (Ev *e)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  e->components = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (e->components, NULL);
  g_strv_builder_add (b, text (e->ui, "component_all"));
  JsonArray *fields = ns_json_arr (e->ui, "component_fields");
  for (guint i = 0; fields && i < json_array_get_length (fields); i++) {
    const char *f = json_array_get_string_element (fields, i);
    g_autoptr (GPtrArray) values = g_ptr_array_new_with_free_func (g_free);
    const void *data;
    size_t len, plen = strlen (f) + 1;
    if (sd_journal_query_unique (e->j, f) < 0)
      continue;
    SD_JOURNAL_FOREACH_UNIQUE (e->j, data, len)
      if (len > plen)
        g_ptr_array_add (values, g_utf8_make_valid ((const char *) data + plen, len - plen));
    g_ptr_array_sort_values (values, (GCompareFunc) g_strcmp0);
    for (guint v = 0; v < values->len; v++) {
      const char *value = g_ptr_array_index (values, v);
      if (!g_hash_table_add (seen, g_strdup (value)))
        continue;
      g_ptr_array_add (e->components, g_strdup_printf ("%s=%s", f, value));
      g_strv_builder_add (b, value);
    }
  }
  return g_strv_builder_end (b);
}

static void
ev_page_open (GtkWidget *row, gpointer data)
{
  JsonObject *ui = data;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (text (ui, "events"), &c);
  Ev *e = g_new0 (Ev, 1);
  e->ui = json_object_ref (ui);
  e->page = page;
  g_object_set_data_full (G_OBJECT (page), "ns-events-log", e, (GDestroyNotify) ev_free);
  gboolean readable = journal_readable (ns_json_str (ui, "journal_group"));
  if (!readable)
    ns_note (c, text (ui, "no_access"));
  if (sd_journal_open (&e->j, SD_JOURNAL_LOCAL_ONLY) < 0) {
    if (readable)
      ns_note (c, text (ui, "no_access"));
    ns_push (page);
    return;
  }

  e->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (e->search), text (ui, "search"));
  gtk_widget_set_margin_start (e->search, 14);
  gtk_widget_set_margin_end (e->search, 14);
  gtk_widget_set_margin_bottom (e->search, 10);
  gtk_box_append (GTK_BOX (c), e->search);
  g_signal_connect (e->search, "search-changed", G_CALLBACK (ev_search_changed), e);

  /* the categories: one at a time (a group of toggles, like radio buttons) */
  GtkWidget *chips = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (chips), GTK_SELECTION_NONE);
  gtk_widget_set_margin_start (chips, 14);
  gtk_widget_set_margin_end (chips, 14);
  gtk_widget_set_margin_bottom (chips, 10);
  JsonArray *cats = ns_json_arr (ui, "categories");
  GtkToggleButton *first = NULL;
  for (guint i = 0; cats && i < json_array_get_length (cats); i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label (ns_json_str (json_array_get_object_element (cats, i), "title"));
    gtk_widget_add_css_class (b, "pill");
    if (first)
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), first);
    else
      first = GTK_TOGGLE_BUTTON (b);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), i == 0);
    g_object_set_data (G_OBJECT (b), "ev-index", GUINT_TO_POINTER (i));
    g_signal_connect (b, "toggled", G_CALLBACK (ev_chip_toggled), e);
    gtk_flow_box_append (GTK_FLOW_BOX (chips), b);
  }
  gtk_box_append (GTK_BOX (c), chips);

  GtkWidget *g = ns_group (c, text (ui, "filters"));
  g_auto (GStrv) comps = ev_components (e);
  g_auto (GStrv) levels = titles_of (ui, "levels");
  g_auto (GStrv) periods = titles_of (ui, "periods");
  e->component_row = ns_row_pick (g, text (ui, "component"), (const char *const *) comps, 0, ev_picked, e);
  e->level_row = ns_row_pick (g, text (ui, "level"), (const char *const *) levels, 0, ev_picked, e);
  e->period_row = ns_row_pick (g, text (ui, "period"), (const char *const *) periods, 0, ev_picked, e);

  e->empty = ns_note (c, text (ui, "empty"));
  e->list = ns_group (c, NULL);
  ev_fill (e);
  int fd = sd_journal_get_fd (e->j);
  if (fd >= 0)
    e->watch = g_unix_fd_add (fd, G_IO_IN, ev_journal_changed, e);
  ns_push (page);
}

/* ---- one entry: every field, a long press copies one ---- */

static void
free_text (gpointer text, GClosure *closure)
{
  g_free (text);
}

static void
copy_pressed (GtkGestureLongPress *g, double x, double y, gpointer data)
{
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g));
  gdk_clipboard_set_text (gtk_widget_get_clipboard (row), data);
}

static void
ev_open_entry (GtkWidget *row, gpointer data)
{
  Ev *e = data;
  const char *cursor = g_object_get_data (G_OBJECT (row), "ev-cursor");
  sd_journal *j = NULL;
  if (!cursor || sd_journal_open (&j, SD_JOURNAL_LOCAL_ONLY) < 0)
    return;
  if (sd_journal_seek_cursor (j, cursor) < 0 || sd_journal_next (j) <= 0) {
    sd_journal_close (j);
    return;
  }
  g_autofree char *message = field (j, "MESSAGE");
  g_autofree char *title = ev_title (e, j, message);
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (title, &c);
  GtkWidget *g = ns_group (c, text (e->ui, "fields"));
  const void *d;
  size_t len;
  SD_JOURNAL_FOREACH_DATA (j, d, len) {
    const char *eq = memchr (d, '=', len);
    if (!eq)
      continue;
    g_autofree char *name = g_strndup (d, eq - (const char *) d);
    g_autofree char *value = g_utf8_make_valid (eq + 1, len - (eq + 1 - (const char *) d));
    GtkWidget *r = ns_row_info (g, name, value);
    GtkGesture *lp = gtk_gesture_long_press_new ();
    g_signal_connect_data (lp, "pressed", G_CALLBACK (copy_pressed), g_strdup (value), free_text, 0);
    gtk_widget_add_controller (r, GTK_EVENT_CONTROLLER (lp));
  }
  sd_journal_close (j);
  ns_note (c, text (e->ui, "copied_hint"));
  ns_push (page);
}

/* ---- the Developer options page ---- */

static void
retention_open (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (text (data, "retention"), &c);
  ns_minerva_embed (page, c, "log_retention");
  ns_push (page);
}

static void
retries_open (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (text (data, "retries"), &c);
  ns_minerva_embed (page, c, "retries");
  ns_push (page);
}

static void
dev_loaded (JsonObject *root, const char *err, gpointer data)
{
  AdwNavigationPage *page = data;
  GtkWidget *c = g_object_get_data (G_OBJECT (page), "ns-dev-content");
  if (!root) {
    ns_note (c, err);
    return;
  }
  JsonObject *ui = json_object_ref (root);
  g_object_set_data_full (G_OBJECT (page), "ns-dev-ui", ui, (GDestroyNotify) json_object_unref);
  ns_page_set_title (page, text (ui, "title"));
  GtkWidget *g = ns_group (c, text (ui, "log"));
  GtkWidget *r = ns_row (g, "view-list-symbolic", "blue", text (ui, "events"), text (ui, "events_subtitle"));
  ns_row_on_activate (r, ev_page_open, ui);
  r = ns_row (g, "document-open-recent-symbolic", "green", text (ui, "retention"), NULL);
  ns_row_on_activate (r, retention_open, ui);
  g = ns_group (c, text (ui, "retries"));
  r = ns_row (g, "view-refresh-symbolic", "orange", text (ui, "retries_row"), NULL);
  ns_row_on_activate (r, retries_open, ui);
}

void
ns_page_developer (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("", &c);
  g_object_set_data (G_OBJECT (page), "ns-dev-content", c);
  static const char *const args[] = { "developer", "--json", NULL };
  ns_mc_run (G_OBJECT (page), args, NULL, dev_loaded, page);
  ns_push (page);
}
