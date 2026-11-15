/* The search client (search.h): GNOME's search providers over GDBus. Pure GLib/GIO, no GTK: the launcher and the test CLI
 * (search-cli.c) both link it.
 *
 * A generation (Gen) is one query: it owns a list of requests (Req), one per provider, each with its own cancellable
 * and its own timeout. A new query (or search_free) cancels the running generation: its `self` pointer is cleared and
 * every call is cancelled, so no callback of it fires afterwards. Providers (Provider) are refcounted because a running
 * generation can outlive a re-read of the provider list. */
#include "search.h"

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

#define SEARCH_SCHEMA        "org.gnome.desktop.search-providers"
#define PROVIDER_IFACE       "org.gnome.Shell.SearchProvider2"
#define INI_GROUP            "Shell Search Provider"
#define FIRE_TIMEOUT_MS      5000

typedef struct {
  int    ref;
  char  *id;
  char  *desktop_id;
  char  *title;
  char  *icon;
  char  *bus_name;
  char  *object_path;
  char **last_ids;      /* the ids this provider answered for ids_terms, or NULL */
  char **ids_terms;
} Provider;

typedef struct {
  int        ref;
  Search    *self;      /* NULL once cancelled */
  guint      generation;
  gint64     start;     /* monotonic µs */
  char     **terms;
  guint      remaining;
  GPtrArray *reqs;      /* Req*, not owned: a Req takes itself out when freed */
} Gen;

typedef struct {
  int           ref;
  Gen          *gen;
  Provider     *p;
  GCancellable *cancel;
  guint         timeout_id;
  gboolean      done;
  guint         n_total;
} Req;

struct _Search {
  SearchGroupFunc  func;
  gpointer         data;
  GDBusConnection *bus;
  GCancellable    *bus_cancel;
  gboolean         bus_failed;
  gboolean         wait_bus;     /* a generation waits for the bus connection */
  GSettings       *settings;
  gboolean         has_disabled, has_sort;
  GPtrArray       *providers;    /* Provider*, in sort order, without the disabled */
  guint            generation;
  Gen             *gen;
  guint            debounce_id;
  guint            idle_id;
  char            *pending_text;
  char           **terms;        /* of the generation last started: what ActivateResult sends */
};

/* ---- providers ---- */

static Provider *
provider_ref (Provider *p)
{
  p->ref++;
  return p;
}

static void
provider_unref (gpointer data)
{
  Provider *p = data;
  if (--p->ref > 0)
    return;
  g_free (p->id);
  g_free (p->desktop_id);
  g_free (p->title);
  g_free (p->icon);
  g_free (p->bus_name);
  g_free (p->object_path);
  g_strfreev (p->last_ids);
  g_strfreev (p->ids_terms);
  g_free (p);
}

static void
provider_reset_ids (Provider *p)
{
  g_clear_pointer (&p->last_ids, g_strfreev);
  g_clear_pointer (&p->ids_terms, g_strfreev);
}

/* the provider id: the .ini's file name without .search-provider.ini, .SearchProvider.ini or .ini */
static char *
id_from_file_name (const char *name)
{
  static const char *const suffixes[] = { ".search-provider.ini", ".SearchProvider.ini", ".ini", NULL };

  for (guint i = 0; suffixes[i]; i++)
    if (g_str_has_suffix (name, suffixes[i]) && strlen (name) > strlen (suffixes[i]))
      return g_strndup (name, strlen (name) - strlen (suffixes[i]));
  return g_strdup (name);
}

static Provider *
provider_load (const char *path, const char *id)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autoptr (GError) error = NULL;

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &error)) {
    g_debug ("search: %s: %s", path, error->message);
    return NULL;
  }
  g_autofree char *desktop_id = g_key_file_get_string (kf, INI_GROUP, "DesktopId", NULL);
  g_autofree char *bus_name = g_key_file_get_string (kf, INI_GROUP, "BusName", NULL);
  g_autofree char *object_path = g_key_file_get_string (kf, INI_GROUP, "ObjectPath", NULL);
  int version = g_key_file_get_integer (kf, INI_GROUP, "Version", NULL);

  if (version != 2 || !desktop_id || !bus_name || !object_path || !g_dbus_is_name (bus_name) ||
      !g_variant_is_object_path (object_path))
    return NULL;

  Provider *p = g_new0 (Provider, 1);
  p->ref = 1;
  p->id = g_strdup (id);
  p->desktop_id = g_steal_pointer (&desktop_id);
  p->bus_name = g_steal_pointer (&bus_name);
  p->object_path = g_steal_pointer (&object_path);

  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (p->desktop_id);
  if (info) {
    const char *name = g_app_info_get_name (G_APP_INFO (info));
    if (name && *name)
      p->title = g_strdup (name);
    GIcon *icon = g_app_info_get_icon (G_APP_INFO (info));
    if (icon)
      p->icon = g_icon_to_string (icon);
  }
  if (!p->title)
    p->title = g_strdup (id);
  return p;
}

static guint
sort_index (char **order, const char *desktop_id)
{
  for (guint i = 0; order && order[i]; i++)
    if (!g_strcmp0 (order[i], desktop_id))
      return i;
  return G_MAXUINT;
}

static int
provider_compare (gconstpointer a, gconstpointer b, gpointer data)
{
  const Provider *pa = *(Provider *const *) a, *pb = *(Provider *const *) b;
  guint ia = sort_index (data, pa->desktop_id), ib = sort_index (data, pb->desktop_id);

  if (ia != ib)
    return ia < ib ? -1 : 1;
  int c = g_strcmp0 (pa->desktop_id, pb->desktop_id);
  return c ?: g_strcmp0 (pa->id, pb->id);
}

static Provider *
find_provider (GPtrArray *list, const char *id)
{
  for (guint i = 0; list && i < list->len; i++) {
    Provider *p = g_ptr_array_index (list, i);
    if (!g_strcmp0 (p->id, id))
      return p;
  }
  return NULL;
}

/* (re)read the provider list: user dir first (its file of a name wins), then the system dirs */
static void
providers_load (Search *self)
{
  g_auto (GStrv) disabled = NULL;
  g_auto (GStrv) order = NULL;
  if (self->settings && self->has_disabled)
    disabled = g_settings_get_strv (self->settings, "disabled");
  if (self->settings && self->has_sort)
    order = g_settings_get_strv (self->settings, "sort-order");

  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *list = g_ptr_array_new_with_free_func (provider_unref);
  g_autoptr (GPtrArray) dirs = g_ptr_array_new ();
  g_ptr_array_add (dirs, (gpointer) g_get_user_data_dir ());
  for (const char *const *d = g_get_system_data_dirs (); d && *d; d++)
    g_ptr_array_add (dirs, (gpointer) *d);

  for (guint i = 0; i < dirs->len; i++) {
    g_autofree char *dir = g_build_filename (g_ptr_array_index (dirs, i), "gnome-shell", "search-providers", NULL);
    g_autoptr (GDir) gd = g_dir_open (dir, 0, NULL);
    const char *name;

    while (gd && (name = g_dir_read_name (gd))) {
      if (!g_str_has_suffix (name, ".ini") || g_hash_table_contains (seen, name))
        continue;
      g_hash_table_add (seen, g_strdup (name));
      g_autofree char *path = g_build_filename (dir, name, NULL);
      g_autofree char *id = id_from_file_name (name);
      Provider *old = find_provider (self->providers, id);
      Provider *p = provider_load (path, id);
      if (!p)
        continue;
      if (disabled && g_strv_contains ((const char *const *) disabled, p->desktop_id)) {
        provider_unref (p);
        continue;
      }
      /* the same provider as before: keep it (and what it answered) */
      if (old && !g_strcmp0 (old->bus_name, p->bus_name) && !g_strcmp0 (old->object_path, p->object_path) &&
          !g_strcmp0 (old->desktop_id, p->desktop_id)) {
        provider_unref (p);
        p = provider_ref (old);
      }
      g_ptr_array_add (list, p);
    }
  }
  g_ptr_array_sort_with_data (list, provider_compare, order);
  g_clear_pointer (&self->providers, g_ptr_array_unref);
  self->providers = list;
}

static void
settings_changed_cb (GSettings *settings, const char *key, gpointer data)
{
  providers_load (data);
}

/* ---- terms ---- */

static char **
split_terms (const char *text)
{
  g_autoptr (GPtrArray) a = g_ptr_array_new ();
  g_auto (GStrv) parts = g_strsplit_set (text, " \t\r\n", -1);

  for (guint i = 0; parts[i]; i++)
    if (*parts[i])
      g_ptr_array_add (a, g_strdup (parts[i]));
  g_ptr_array_add (a, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&a), FALSE);
}

/* do the new terms narrow the old ones (so a provider's old ids are a superset): the first n-1 the same, the last of
 * the old a prefix of its counterpart, and the new may have more terms */
static gboolean
terms_extend (char **old, char **new)
{
  guint n = old ? g_strv_length (old) : 0, m = g_strv_length (new);

  if (n == 0 || m < n)
    return FALSE;
  for (guint i = 0; i + 1 < n; i++)
    if (strcmp (old[i], new[i]))
      return FALSE;
  return g_str_has_prefix (new[n - 1], old[n - 1]);
}

/* ---- generation and requests ---- */

static Gen *
gen_ref (Gen *gen)
{
  gen->ref++;
  return gen;
}

static void
gen_unref (Gen *gen)
{
  if (--gen->ref > 0)
    return;
  g_strfreev (gen->terms);
  g_ptr_array_unref (gen->reqs);
  g_free (gen);
}

static Req *
req_ref (Req *req)
{
  req->ref++;
  return req;
}

static void
req_unref (gpointer data)
{
  Req *req = data;
  if (--req->ref > 0)
    return;
  g_ptr_array_remove (req->gen->reqs, req);
  gen_unref (req->gen);
  provider_unref (req->p);
  g_object_unref (req->cancel);
  g_free (req);
}

static void
hit_free (gpointer data)
{
  SearchHit *h = data;
  g_free (h->id);
  g_free (h->name);
  g_free (h->description);
  g_free (h->icon);
  g_free (h);
}

/* stop a generation: no callback of it fires after this, every call in flight is cancelled */
static void
gen_cancel (Search *self)
{
  Gen *gen = self->gen;

  if (!gen)
    return;
  self->gen = NULL;
  gen->self = NULL;
  for (guint i = gen->reqs->len; i-- > 0;) {
    Req *req = req_ref (g_ptr_array_index (gen->reqs, i));
    if (req->timeout_id) {
      g_source_remove (req->timeout_id);
      req->timeout_id = 0;
    }
    g_cancellable_cancel (req->cancel);
    req_unref (req);
  }
  gen_unref (gen);
}

/* the provider is done (answered, failed or timed out): its callback. hits is taken (may be NULL). */
static void
req_finish (Req *req, gboolean timed_out, GPtrArray *hits)
{
  Gen *gen = req->gen;
  Search *self = gen->self;

  if (req->done) {
    g_clear_pointer (&hits, g_ptr_array_unref);
    return;
  }
  req->done = TRUE;
  if (req->timeout_id) {
    g_source_remove (req->timeout_id);
    req->timeout_id = 0;
  }
  if (timed_out) {
    g_cancellable_cancel (req->cancel);
    provider_reset_ids (req->p);
  }
  if (!self) {
    g_clear_pointer (&hits, g_ptr_array_unref);
    return;
  }
  if (!hits)
    hits = g_ptr_array_new_with_free_func (hit_free);

  gen->remaining--;
  SearchGroup group = {
    .id = req->p->id,
    .desktop_id = req->p->desktop_id,
    .title = req->p->title,
    .icon = req->p->icon,
    .timed_out = timed_out,
    .hits = hits,
    .n_total = req->n_total,
    .ms = (g_get_monotonic_time () - gen->start) / 1000.0,
  };
  if (self->providers)
    for (guint i = 0; i < self->providers->len; i++)
      if (self->providers->pdata[i] == req->p)
        group.order = i;
  gboolean last = gen->remaining == 0;
  /* the callback may start a new query: it drops `self->gen`, and this request keeps its generation alive */
  self->func (self, gen->generation, &group, last, self->data);
  g_ptr_array_unref (hits);
}

static gboolean
req_timeout_cb (gpointer data)
{
  Req *req = data;

  req->timeout_id = 0;
  req_finish (req, TRUE, NULL);
  return G_SOURCE_REMOVE;
}

static char *
meta_string (GVariant *meta, const char *key)
{
  char *s = NULL;
  if (!g_variant_lookup (meta, key, "s", &s))
    return NULL;
  if (s && !*s)
    g_clear_pointer (&s, g_free);
  return s;
}

static GPtrArray *
parse_metas (GVariant *reply)
{
  GPtrArray *hits = g_ptr_array_new_with_free_func (hit_free);
  g_autoptr (GVariant) arr = NULL;
  GVariantIter iter;
  GVariant *meta;

  g_variant_get (reply, "(@aa{sv})", &arr);
  g_variant_iter_init (&iter, arr);
  while ((meta = g_variant_iter_next_value (&iter))) {
    SearchHit *h = g_new0 (SearchHit, 1);

    h->id = meta_string (meta, "id");
    if (!h->id) {
      hit_free (h);
      g_variant_unref (meta);
      continue;
    }
    h->name = meta_string (meta, "name") ?: g_strdup (h->id);
    h->description = meta_string (meta, "description");
    g_autoptr (GVariant) iv = g_variant_lookup_value (meta, "icon", NULL);
    while (iv && g_variant_is_of_type (iv, G_VARIANT_TYPE_VARIANT)) {
      GVariant *inner = g_variant_get_variant (iv);
      g_variant_unref (iv);
      iv = inner;
    }
    if (iv) {
      g_autoptr (GIcon) icon = g_icon_deserialize (iv);
      if (icon)
        h->icon = g_icon_to_string (icon);
    }
    if (!h->icon)
      h->icon = meta_string (meta, "gicon");
    g_ptr_array_add (hits, h);
    g_variant_unref (meta);
  }
  return hits;
}

static void
metas_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  Req *req = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (reply && !req->done && req->gen->self)
    req_finish (req, FALSE, parse_metas (reply));
  else if (!reply && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_debug ("search: %s GetResultMetas: %s", req->p->id, error->message);
    req_finish (req, TRUE, NULL);
  }
  req_unref (req);
}

static void
ids_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  Req *req = data;
  Search *self = req->gen->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!reply) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug ("search: %s: %s", req->p->id, error->message);
      req_finish (req, TRUE, NULL);
    }
    req_unref (req);
    return;
  }
  if (req->done || !self) {
    req_unref (req);
    return;
  }

  g_auto (GStrv) ids = NULL;
  g_variant_get (reply, "(^as)", &ids);
  guint n = g_strv_length (ids);

  /* kept for the next generation's subsearch */
  provider_reset_ids (req->p);
  req->p->last_ids = g_strdupv (ids);
  req->p->ids_terms = g_strdupv (req->gen->terms);
  req->n_total = n;
  if (n == 0) {
    req_finish (req, FALSE, NULL);
    req_unref (req);
    return;
  }

  guint m = MIN (n, SEARCH_METAS_PER_PROVIDER);
  g_autofree const char **first = g_new0 (const char *, m + 1);
  for (guint i = 0; i < m; i++)
    first[i] = ids[i];
  g_dbus_connection_call (self->bus, req->p->bus_name, req->p->object_path, PROVIDER_IFACE, "GetResultMetas",
                          g_variant_new ("(^as)", first), G_VARIANT_TYPE ("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE,
                          SEARCH_PROVIDER_TIMEOUT_MS + 500, req->cancel, metas_cb, req_ref (req));
  req_unref (req);
}

static void
req_start (Gen *gen, Provider *p)
{
  Search *self = gen->self;
  Req *req = g_new0 (Req, 1);

  req->ref = 1;
  req->gen = gen_ref (gen);
  req->p = provider_ref (p);
  req->cancel = g_cancellable_new ();
  g_ptr_array_add (gen->reqs, req);
  /* without a bus the request fails from the main loop, never inside this call */
  req->timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, self->bus ? SEARCH_PROVIDER_TIMEOUT_MS : 0,
                                        req_timeout_cb, req_ref (req), req_unref);
  if (self->bus) {
    gboolean sub = p->last_ids && p->last_ids[0] && terms_extend (p->ids_terms, gen->terms);
    GVariant *params = sub ? g_variant_new ("(^as^as)", p->last_ids, gen->terms)
                           : g_variant_new ("(^as)", gen->terms);

    g_dbus_connection_call (self->bus, p->bus_name, p->object_path, PROVIDER_IFACE,
                            sub ? "GetSubsearchResultSet" : "GetInitialResultSet", params, G_VARIANT_TYPE ("(as)"),
                            G_DBUS_CALL_FLAGS_NONE, SEARCH_PROVIDER_TIMEOUT_MS + 500, req->cancel, ids_cb,
                            req_ref (req));
  }
  req_unref (req);
}

static gboolean idle_clear_cb (gpointer data);

static void
start_generation (Search *self)
{
  if (!self->bus && !self->bus_failed) {
    self->wait_bus = TRUE;
    return;
  }

  Gen *gen = g_new0 (Gen, 1);
  gen->ref = 1;
  gen->self = self;
  gen->generation = self->generation;
  gen->start = g_get_monotonic_time ();
  gen->terms = split_terms (self->pending_text ?: "");
  gen->reqs = g_ptr_array_new ();
  self->gen = gen;
  g_strfreev (self->terms);
  self->terms = g_strdupv (gen->terms);

  /* no provider: the generation ends at once (the same call as for a text too short) */
  if (self->providers->len == 0) {
    gen_cancel (self);
    self->idle_id = g_idle_add (idle_clear_cb, self);
    return;
  }
  gen->remaining = self->providers->len;
  for (guint i = 0; i < self->providers->len; i++)
    req_start (gen, g_ptr_array_index (self->providers, i));
}

/* ---- the public query calls ---- */

static gboolean
idle_clear_cb (gpointer data)
{
  Search *self = data;

  self->idle_id = 0;
  self->func (self, self->generation, NULL, TRUE, self->data);
  return G_SOURCE_REMOVE;
}

static gboolean
debounce_cb (gpointer data)
{
  Search *self = data;

  self->debounce_id = 0;
  start_generation (self);
  return G_SOURCE_REMOVE;
}

static void
bus_ready_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_finish (res, &error);

  if (!bus && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  Search *self = data;
  if (bus)
    self->bus = bus;
  else {
    g_warning ("search: no session bus: %s", error->message);
    self->bus_failed = TRUE;
  }
  if (self->wait_bus) {
    self->wait_bus = FALSE;
    start_generation (self);
  }
}

Search *
search_new (SearchGroupFunc func, gpointer data)
{
  Search *self = g_new0 (Search, 1);

  self->func = func;
  self->data = data;
  self->terms = g_new0 (char *, 1);
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = src ? g_settings_schema_source_lookup (src, SEARCH_SCHEMA, TRUE) : NULL;
  if (schema) {
    self->has_disabled = g_settings_schema_has_key (schema, "disabled");
    self->has_sort = g_settings_schema_has_key (schema, "sort-order");
    self->settings = g_settings_new_full (schema, NULL, NULL);
    g_settings_schema_unref (schema);
    if (self->has_disabled)
      g_signal_connect (self->settings, "changed::disabled", G_CALLBACK (settings_changed_cb), self);
    if (self->has_sort)
      g_signal_connect (self->settings, "changed::sort-order", G_CALLBACK (settings_changed_cb), self);
  }
  providers_load (self);
  self->bus_cancel = g_cancellable_new ();
  g_bus_get (G_BUS_TYPE_SESSION, self->bus_cancel, bus_ready_cb, self);
  return self;
}

void
search_free (Search *self)
{
  if (!self)
    return;
  gen_cancel (self);
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  g_clear_handle_id (&self->idle_id, g_source_remove);
  g_cancellable_cancel (self->bus_cancel);
  g_object_unref (self->bus_cancel);
  if (self->settings) {
    g_signal_handlers_disconnect_by_data (self->settings, self);
    g_object_unref (self->settings);
  }
  g_clear_object (&self->bus);
  g_clear_pointer (&self->providers, g_ptr_array_unref);
  g_free (self->pending_text);
  g_strfreev (self->terms);
  g_free (self);
}

guint
search_n_providers (Search *self)
{
  return self->providers->len;
}

guint
search_generation (Search *self)
{
  return self->generation;
}

static guint
query (Search *self, const char *text, gboolean now)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (text ?: ""));

  gen_cancel (self);
  self->wait_bus = FALSE;
  g_clear_handle_id (&self->debounce_id, g_source_remove);
  g_clear_handle_id (&self->idle_id, g_source_remove);
  self->generation++;

  if (g_utf8_strlen (trimmed, -1) < SEARCH_MIN_CHARS) {
    for (guint i = 0; i < self->providers->len; i++)
      provider_reset_ids (g_ptr_array_index (self->providers, i));
    g_clear_pointer (&self->pending_text, g_free);
    self->idle_id = g_idle_add (idle_clear_cb, self);
    return self->generation;
  }
  g_free (self->pending_text);
  self->pending_text = g_steal_pointer (&trimmed);
  if (now)
    start_generation (self);
  else
    self->debounce_id = g_timeout_add (SEARCH_DEBOUNCE_MS, debounce_cb, self);
  return self->generation;
}

guint
search_query (Search *self, const char *text)
{
  return query (self, text, FALSE);
}

guint
search_query_now (Search *self, const char *text)
{
  return query (self, text, TRUE);
}

/* ---- activate, launch ---- */

static void
fire_done_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autofree char *label = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!reply)
    g_warning ("search: %s: %s", label, error->message);
}

/* params is consumed */
static void
fire (Search *self, const char *provider, const char *method, GVariant *params)
{
  Provider *p = find_provider (self->providers, provider);

  g_variant_ref_sink (params);
  if (!p || !self->bus) {
    g_warning ("search: %s %s: %s", provider, method, p ? "no session bus" : "no such provider");
    g_variant_unref (params);
    return;
  }
  g_dbus_connection_call (self->bus, p->bus_name, p->object_path, PROVIDER_IFACE, method, params, NULL,
                          G_DBUS_CALL_FLAGS_NONE, FIRE_TIMEOUT_MS, NULL, fire_done_cb,
                          g_strdup_printf ("%s %s", provider, method));
  g_variant_unref (params);
}

void
search_activate (Search *self, const char *provider, const char *result_id)
{
  fire (self, provider, "ActivateResult",
        g_variant_new ("(s^asu)", result_id, self->terms, (guint32) (g_get_real_time () / 1000)));
}

void
search_launch_search (Search *self, const char *provider)
{
  fire (self, provider, "LaunchSearch", g_variant_new ("(^asu)", self->terms, (guint32) (g_get_real_time () / 1000)));
}
