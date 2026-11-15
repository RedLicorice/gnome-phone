/* The apps as tiles: an icon texture (drawn by a thread, so the first frame never waits for cairo), the two labels,
 * the press feedback, and the launch hand-off to the shell. */
#include <math.h>

#include "apps.h"
#include "icons.h"
#include "image.h"
#include "neo.h"
#include "widgets.h"

#define PRESS_SCALE   1.10f
#define PRESS_MS      200.0
#define LAUNCH_SCALE_DELAY_MS 16.0
#define LAUNCH_SCALE_MS       350.0
#define LAUNCH_END_MS         600.0

/* ---- the icon thread ---------------------------------------------------------------------------------------------- */

#define SPLASH_ICON_DP 160

typedef struct {
  char      *id;
  GAppInfo  *info;
  int        px;                   /* the size to draw it at (tiles_icon_px when it was queued) */
  GdkPixbuf *pb;
  char      *splash;
} IconJob;

/* the icons are drawn at the larger of the home's and the drawer's sizes (Settings' icon sizes), so that neither
 * scales one up */
int
tiles_icon_px (void)
{
  return (int) lroundf (dp (MAX (N.icon_dp, N.drawer_icon_dp)));
}

static void
icon_job_free (IconJob *j)
{
  g_free (j->id);
  g_free (j->splash);
  g_object_unref (j->info);
  g_clear_object (&j->pb);
  g_free (j);
}

/* on the main thread: the texture for a drawn icon */
static gboolean
icon_ready (gpointer data)
{
  IconJob *j = data;
  Tile *t = tiles_get (j->id);
  if (t && j->splash) {
    g_free (t->splash_icon);
    t->splash_icon = g_steal_pointer (&j->splash);
    nav_icons_changed ();
  }
  if (t && j->pb) {
    gfx_tex_free (t->icon);
    t->icon = image_texture (j->pb);
    wl_schedule (N.wl);
  }
  icon_job_free (j);
  return G_SOURCE_REMOVE;
}

/* Every icon in order (the dock and the first page first): the cached or stock one at once, then the pack's
 * drawing where it still has to be made. Only this thread touches the icon theme and the pack, one at a time: an app
 * installed while the first one draws waits for it. */
static GMutex icon_lock;

static gpointer
icon_thread (gpointer data)
{
  GPtrArray *jobs = data;
  g_mutex_lock (&icon_lock);
  g_autoptr (GSettings) settings = g_settings_new ("de.yesman.neo");
  Icons *icons = icons_new (settings);
  GPtrArray *pending = g_ptr_array_new (), *splash = g_ptr_array_new ();
  for (guint i = 0; i < jobs->len; i++) {
    IconJob *j = jobs->pdata[i];
    int px = j->px;
    IconJob *s = g_new0 (IconJob, 1);
    s->id = g_strdup (j->id);
    s->info = g_object_ref (j->info);
    g_ptr_array_add (splash, s);
    gboolean later = FALSE;
    j->pb = icons_lookup (icons, j->info, px, &later);
    IconJob *again = later ? g_new0 (IconJob, 1) : NULL;
    if (again) {
      again->id = g_strdup (j->id);
      again->info = g_object_ref (j->info);
      again->px = px;
      g_ptr_array_add (pending, again);
    }
    g_idle_add_full (G_PRIORITY_DEFAULT, icon_ready, j, NULL);
  }
  for (guint i = 0; i < pending->len; i++) {
    IconJob *j = pending->pdata[i];
    j->pb = icons_render (icons, j->info, j->px);
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, icon_ready, j, NULL);
  }
  g_ptr_array_unref (pending);
  /* the same icons at the splash's size, for the shell (drawn once, then cached) */
  int big = (int) lroundf (dp (SPLASH_ICON_DP));
  for (guint i = 0; i < splash->len; i++) {
    IconJob *j = splash->pdata[i];
    gboolean later = FALSE;
    g_autoptr (GdkPixbuf) pb = icons_lookup (icons, j->info, big, &later);
    if (later) {
      g_clear_object (&pb);
      pb = icons_render (icons, j->info, big);
    }
    j->splash = icons_cached_path (icons, j->info, big);
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, icon_ready, j, NULL);
  }
  g_ptr_array_unref (splash);
  g_ptr_array_unref (jobs);
  icons_free (icons);
  g_mutex_unlock (&icon_lock);
  return NULL;
}

/* ---- tiles -------------------------------------------------------------------------------------------------------- */

static void
tile_free (Tile *t)
{
  g_free (t->id);
  g_free (t->splash_icon);
  g_object_unref (t->info);
  gfx_tex_free (t->icon);
  text_layout_free (t->label);
  text_layout_free (t->dlabel);
  g_free (t);
}

Tile *
tiles_get (const char *id)
{
  return id ? g_hash_table_lookup (N.tiles, id) : NULL;
}

GHashTable *
tiles_apps (void)
{
  GHashTable *apps = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, N.tiles);
  while (g_hash_table_iter_next (&it, &key, &value))
    g_hash_table_insert (apps, key, ((Tile *) value)->info);
  return apps;
}

static void
queue_icon (GPtrArray *jobs, GHashTable *queued, const char *id)
{
  Tile *t = tiles_get (id);
  if (!t || g_hash_table_contains (queued, id))
    return;
  g_hash_table_add (queued, (gpointer) t->id);
  IconJob *j = g_new0 (IconJob, 1);
  j->id = g_strdup (t->id);
  j->info = g_object_ref (t->info);
  j->px = tiles_icon_px ();
  g_ptr_array_add (jobs, j);
}

/* the widths the labels were laid out at: another grid's columns give them more or less room */
static float label_w, dlabel_w;

static void
tile_label (Tile *t)
{
  const char *name = app_name (t->info);
  text_layout_free (t->label);
  text_layout_free (t->dlabel);
  t->label = text_layout (N.label_font, name, label_w, 1, TEXT_CENTER);
  t->dlabel = text_layout (N.label_font, name, dlabel_w, 1, TEXT_CENTER);
}

static bool
label_widths (void)
{
  float w = home_label_width (), dw = drawer_label_width ();
  bool changed = w != label_w || dw != dlabel_w;
  label_w = w;
  dlabel_w = dw;
  return changed;
}

void
tiles_relabel (void)
{
  if (!label_widths ())
    return;
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, N.tiles);
  while (g_hash_table_iter_next (&it, &key, &value))
    tile_label (value);
}

static Tile *
tile_new (GAppInfo *info)
{
  Tile *t = g_new0 (Tile, 1);
  t->info = g_object_ref (info);
  t->id = g_strdup (g_app_info_get_id (info));
  tile_label (t);
  g_hash_table_insert (N.tiles, t->id, t);
  g_ptr_array_add (N.sorted, t);
  return t;
}

static gint
tile_compare (gconstpointer a, gconstpointer b)
{
  return app_compare ((*(Tile *const *) a)->info, (*(Tile *const *) b)->info);
}

static char *
icon_string (GAppInfo *info)
{
  GIcon *icon = g_app_info_get_icon (info);
  return icon ? g_icon_to_string (icon) : NULL;
}

/* every tile's icon to draw, the ones the first frame shows first: the dock, the home pages, then the drawer */
static GPtrArray *
all_icon_jobs (void)
{
  GPtrArray *jobs = g_ptr_array_new ();
  g_autoptr (GHashTable) queued = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < N.model->dock->len; i++)
    queue_icon (jobs, queued, N.model->dock->pdata[i]);
  for (guint p = 0; p < N.model->pages->len; p++) {
    GPtrArray *page = N.model->pages->pdata[p];
    for (guint i = 0; i < page->len; i++)
      queue_icon (jobs, queued, page->pdata[i]);
  }
  for (guint i = 0; i < N.sorted->len; i++)
    queue_icon (jobs, queued, ((Tile *) N.sorted->pdata[i])->id);
  return jobs;
}

/* the apps and the home layout on the grid Settings picked (cols × rows) */
void
tiles_load (int cols, int rows)
{
  g_autoptr (GHashTable) apps = apps_installed ();
  N.tiles = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) tile_free);
  N.sorted = g_ptr_array_new ();
  N.label_font = text_font_axes ("Google Sans Flex", 700, dp (LABEL_SP), "opsz=72,ROND=100");

  N.model = model_new (cols, rows, DOCK_SLOTS);
  model_set_reserved (N.model, widgets_slots (0, cols));   /* the widgets' rows on the first page */
  label_widths ();
  GList *infos = g_list_sort (g_hash_table_get_values (apps), app_compare);
  for (GList *l = infos; l; l = l->next)
    tile_new (l->data);
  g_list_free (infos);

  g_auto (GStrv) hidden = g_settings_get_strv (N.settings, "hidden-from-home");
  model_set_hidden (N.model, hidden);
  model_load (N.model, apps);
  model_set_reserved (N.model, widgets_slots (0, N.model->cols));
  g_thread_unref (g_thread_new ("neo-icons", icon_thread, all_icon_jobs ()));
}

/* The icon settings changed (Settings' Home screen: the pack, the shape, wrapping, legacy icons): every icon is
 * drawn again, in the same order as at start. The thread reads the settings afresh (icons_new), and the icon cache's
 * key covers them, so a look used before comes from its cache at once; one not drawn yet shows the stock icon
 * until it is (icon_ready swaps each tile's texture, as at start). */
void
tiles_reload_icons (void)
{
  g_thread_unref (g_thread_new ("neo-icons", icon_thread, all_icon_jobs ()));
}

/* The installed apps again, as Launcher3 follows the package manager: a new app gets a tile (its icon from the thread)
 * and the first free home cell, a removed one's tile goes and leaves its cells empty, a changed one takes its new
 * entry (relabelled, its icon drawn again if that changed), and the drawer is sorted again. The caller makes sure
 * no tile is held (under a finger, pressed, launching, in a popup or a drag). Whether anything changed. */
bool
tiles_sync (void)
{
  g_autoptr (GHashTable) apps = apps_installed ();
  GPtrArray *jobs = g_ptr_array_new ();
  g_autoptr (GHashTable) queued = g_hash_table_new (g_str_hash, g_str_equal);
  bool changed = false;

  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, N.tiles);
  while (g_hash_table_iter_next (&it, &key, &value)) {
    if (g_hash_table_contains (apps, key))
      continue;
    g_ptr_array_remove (N.sorted, value);
    g_hash_table_iter_remove (&it);
    changed = true;
  }

  g_hash_table_iter_init (&it, apps);
  while (g_hash_table_iter_next (&it, &key, &value)) {
    GAppInfo *info = value;
    Tile *t = tiles_get (key);
    if (!t) {
      queue_icon (jobs, queued, tile_new (info)->id);
      changed = true;
      continue;
    }
    bool renamed = g_strcmp0 (app_name (t->info), app_name (info));
    g_autofree char *was = icon_string (t->info), *icon = icon_string (info);
    g_set_object (&t->info, info);
    if (renamed)
      tile_label (t);
    if (g_strcmp0 (was, icon))
      queue_icon (jobs, queued, t->id);
    changed |= renamed;
  }
  if (changed)
    g_ptr_array_sort (N.sorted, tile_compare);

  if (model_reconcile (N.model, apps)) {
    model_tidy (N.model);
    model_save (N.model);
    changed = true;
  }

  if (jobs->len) {
    g_thread_unref (g_thread_new ("neo-icons", icon_thread, jobs));
    changed = true;
  } else {
    g_ptr_array_unref (jobs);
  }
  return changed;
}

/* the icon, or its place-holder circle until the thread has drawn it */
void
tile_draw_icon (Tile *t, float x, float y, float size, float alpha)
{
  if (t->hidden)
    return;
  float s = press_scale (t);
  if (s != 1) {
    gfx_save ();
    gfx_scale (s, s, x + size / 2, y + size / 2);
  }
  if (t->icon)
    gfx_image (t->icon, x, y, size, size, 0, alpha);
  else
    gfx_rect (x + size * 0.04f, y + size * 0.04f, size * 0.92f, size * 0.92f, size * 0.46f,
              (GfxColor) { 1, 1, 1, 0.18f * alpha });
  if (s != 1)
    gfx_restore ();
}

/* ---- press feedback ----------------------------------------------------------------------------------------------- */

static float
press_value (void)
{
  Press *p = &N.press;
  if (!p->tile)
    return 1;
  float t = CLAMP ((N.now - p->t0) / PRESS_MS, 0, 1);
  float e = p->up ? 1 - (1 - t) * (1 - t) : t * t;
  return p->from + (p->to - p->from) * e;
}

float
press_scale (Tile *t)
{
  return t && t == N.press.tile ? press_value () : 1;
}

bool
press_animating (void)
{
  if (!N.press.tile)
    return false;
  if (N.now - N.press.t0 < PRESS_MS)
    return true;
  if (N.press.up)
    N.press.tile = NULL;            /* back at 1 */
  return false;
}

void
press_down (Tile *t)
{
  N.press = (Press) { .tile = t, .t0 = N.now, .from = 1, .to = PRESS_SCALE };
  wl_schedule (N.wl);
}

void
press_up (void)
{
  if (!N.press.tile || N.press.up)
    return;
  float v = press_value ();
  N.press = (Press) { .tile = N.press.tile, .t0 = N.now, .from = v, .to = 1, .up = true };
  wl_schedule (N.wl);
}

/* ---- launch ------------------------------------------------------------------------------------------------------- */

static gboolean
launch_end (gpointer data)
{
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, N.tiles);
  while (g_hash_table_iter_next (&it, &key, &value))
    ((Tile *) value)->hidden = false;
  N.launch_t0 = 0;
  if (N.launch_from_drawer)
    drawer_close_now ();
  N.launch_from_drawer = false;
  wl_schedule (N.wl);
  return G_SOURCE_REMOVE;
}

/* A vibration from feedbackd's theme: HAPTIC_LONG_PRESS is window-close (a 25 ms rumble, the theme's heaviest
 * short one), a tick is button-released (12 ms) */
void
haptic (const char *event)
{
  if (!N.bus)
    return;
  g_dbus_connection_call (N.bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback",
                          "TriggerFeedback",
                          g_variant_new ("(ss@a{sv}i)", "de.yesman.NeoLauncher", event,
                                         g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0), -1),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* The shell starts the app and grows its window (or splash) out of the icon's box; the icon hides meanwhile and
 * the home content behind shrinks to 0.97 (Launcher3's QuickstepTransitionManager). */
void
tile_launch (Tile *t, float x, float y, float size, bool from_drawer)
{
  if (!N.bus)
    N.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!N.bus)
    return;
  g_dbus_connection_call (N.bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone", "Launch",
                          g_variant_new ("(s(dddd)s)", t->id, (double) x, (double) y, (double) size, (double) size,
                                         t->splash_icon ? t->splash_icon : ""),
                          NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
  N.press.tile = NULL;              /* the press scale is dropped at launch */
  t->hidden = true;
  N.launch_t0 = N.now;
  N.launch_from_drawer = from_drawer;
  g_timeout_add ((guint) LAUNCH_END_MS, launch_end, NULL);
  wl_schedule (N.wl);
}

/* the home content's scale for a launch at time now (1 when none runs) */
float
launch_content_scale (void)
{
  if (!N.launch_t0)
    return 1;
  float t = CLAMP ((N.now - N.launch_t0 - LAUNCH_SCALE_DELAY_MS) / LAUNCH_SCALE_MS, 0, 1);
  float e = 1 - powf (1 - t, 3);    /* DECELERATE_1_5 */
  return 1 + (CONTENT_SCALE - 1) * e;
}

/* A label; on the wallpaper with Launcher3's two shadows (ambient #40000000 blur 1.5 dp, key #89000000 blur
 * 0.5 dp offset 0.5 dp), the blur stood in for by a ring of faint copies. */
void
label_draw (TextLayout *l, float x, float y, GfxColor c, bool shadow)
{
  if (shadow) {
    float o = dp (0.5), r = dp (1.5) * 0.6f;
    for (int i = 0; i < 8; i++) {
      float a = i * (float) G_PI / 4;
      text_draw (l, x + r * cosf (a), y + r * sinf (a), (GfxColor) { 0, 0, 0, 0.07f * c.a });
    }
    text_draw (l, x + o, y + o, (GfxColor) { 0, 0, 0, 0.54f * c.a });
  }
  text_draw (l, x, y, c);
}
