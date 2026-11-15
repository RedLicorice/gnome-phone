/* Neo Launcher 2: the home screen as a Vulkan Wayland client. Launcher3 on Android 16, 1:1: the parallax
 * wallpaper zoomed ×1.10, the workspace and dock, and the app drawer sheet whose depth zooms the wallpaper out,
 * shrinks the home to 0.97 and blurs both behind it. */
#include <glib.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "image.h"
#include "neo.h"
#include "omni.h"
#include "widgets.h"
#include "parallax.h"

#define DRAWER_DEPTH       0.33f      /* bottomSheetDepth */
#define SPRING_DEPTH       0.15f      /* SPRING_LOADED's: the wallpaper zooms out and blurs behind a sharp workspace */
#define BLUR_MAX_DP        30         /* max_depth_blur_radius_enhanced */
#define BLUR_FULL_DEPTH    0.3f
#define HOLD_MS            375        /* a long press: the menu, measured on the S10e */
#define APPS_SETTLE_MS     300        /* an install writes several files: the app list is read once they are in */
#define DRAG_SLOP_DP       16         /* a held finger moving this far leaves the menu */

void drawer_prepare (void);

Neo N;

/* OWNER_POPUP: a touch on an open menu; OWNER_HELD: the finger whose long press opened it, still down;
 * OWNER_BAR: a swipe up from the bar while the drawer is open (home); OWNER_OMNI: the search view (omni.c) */
typedef enum { OWNER_NONE, OWNER_HOME, OWNER_DRAWER, OWNER_POPUP, OWNER_HELD, OWNER_BAR, OWNER_OMNI } Owner;

static struct {
  Vk          vk;
  bool        ready;
  int         vk_w, vk_h;
  GMainLoop  *loop;
  Parallax   *par;
  bool        suspended;    /* the compositor's: an app covers the launcher */
  GSettings  *bg, *iface;         /* GNOME's wallpaper, and the style that picks its light or dark picture */
  guint       wallpaper_id;       /* a reload, once the keys that changed together are all in */
  GAppInfoMonitor *apps_monitor;  /* apps installed and removed */
  guint       apps_id;            /* the app list's read, once they settle and no tile is held */
  GfxTex     *home_target, *bg_target;

  int         touch;              /* the finger being followed, or -1 */
  Owner       owner;
  float       down_x, down_y;
  float       last_x, last_y;     /* where it was last: wl_touch.up has no position of its own */
  bool        moved;
  bool        debug_touch;        /* NEO_DEBUG_TOUCH or ~/.config/neo-launcher/debug-touch */
  Tile       *down_tile;
  float       down_ix, down_iy;
  ModelPos    down_pos;
  guint       hold_id;            /* the long press's timeout */

  /* frame timing (NEO_STATS) */
  TextFont   *stats_font;
  TextLayout *stats;
  gint64      prev, stats_t0;
  int         frames, late;
  double      worst_ms, cpu_ms;
} A = { .touch = -1 };

static gint64
mono_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (gint64) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* an event's time (ms, 32 bits of CLOCK_MONOTONIC) on the frame clock's scale */
static double
event_ms (guint32 t)
{
  double now = mono_ns () / 1e6;
  guint32 behind = (guint32) (guint64) now - t;
  return behind < 10000 ? now - behind : now;
}

static void
wake (void *data)
{
  wl_schedule (N.wl);
}

static gboolean
clock_tick (gpointer data)
{
  home_clock_tick ();
  /* on the next minute's boundary */
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int ms = (60 - g_date_time_get_second (now)) * 1000 + 20;
  g_timeout_add (MAX (ms, 1000), clock_tick, NULL);
  return G_SOURCE_REMOVE;
}

/* ---- the blur ------------------------------------------------------------------------------------------------- */

/* σ (full-size px) of gfx_blur on a half-size picture, per levels and offset 0.5 … 4 (simulated, then drawn back
 * up bilinearly): pick the levels whose range holds σ, interpolate the offset */
static const float SIGMA[4][8] = {
  { 1.7f, 1.7f, 2.1f, 2.4f, 2.5f, 2.6f, 3.1f, 3.3f },
  { 4.2f, 6.0f, 8.4f, 10.7f, 12.7f, 14.9f, 17.6f, 20.0f },
  { 8.7f, 13.0f, 18.5f, 23.5f, 27.9f, 33.0f, 38.8f, 44.2f },
  { 17.6f, 26.5f, 37.9f, 48.0f, 57.1f, 67.4f, 79.2f, 90.3f },
};

static void
blur_params (float sigma, int *levels, float *offset)
{
  int l = sigma <= 3.3f ? 0 : sigma <= 14.9f ? 1 : sigma <= 33.0f ? 2 : 3;
  const float *row = SIGMA[l];
  float o = 4;
  for (int i = 0; i < 7; i++)
    if (sigma <= row[i + 1]) {
      float span = row[i + 1] - row[i];
      o = 0.5f + 0.5f * (i + (span > 0 ? CLAMP ((sigma - row[i]) / span, 0, 1) : 1));
      break;
    }
  if (sigma < row[0])
    o = 0.5f;
  *levels = l + 1;
  *offset = o;
}

static GfxTex *
target (GfxTex **t, int w, int h)
{
  if (!*t || (*t)->w != w || (*t)->h != h) {
    gfx_tex_free (*t);
    *t = gfx_target_new (w, h);
  }
  return *t;
}

/* ---- frames --------------------------------------------------------------------------------------------------- */

/* A GNOME slideshow (an .xml: Debian's default wallpaper, desktop-base's desktop-background.xml, is one) as one
 * picture: its first slide's, and of the sizes that lists, the one nearest the screen's shape */
typedef struct {
  GString *text;
  char    *best;
  double   best_d, aspect;
  int      w, h;
  gboolean in_file, done;
} Slides;

static void
slides_start (GMarkupParseContext *ctx, const char *name, const char **names, const char **values, gpointer data,
              GError **error)
{
  Slides *s = data;
  if (s->done || (strcmp (name, "file") && strcmp (name, "size")))
    return;
  s->in_file = TRUE;
  g_string_truncate (s->text, 0);
  s->w = s->h = 0;
  for (int i = 0; names[i]; i++) {
    if (!strcmp (names[i], "width"))
      s->w = atoi (values[i]);
    else if (!strcmp (names[i], "height"))
      s->h = atoi (values[i]);
  }
}

static void
slides_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Slides *s = data;
  if (s->in_file && !s->done)
    g_string_append_len (s->text, text, len);
}

static void
slides_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Slides *s = data;
  gboolean size = !strcmp (name, "size"), file = !strcmp (name, "file");
  if (s->done || (!size && !file))
    return;
  g_strstrip (s->text->str);
  s->text->len = strlen (s->text->str);
  if (s->text->len && (size || !s->best)) {
    double d = s->w > 0 && s->h > 0 ? fabs (log ((double) s->w / s->h / s->aspect)) : G_MAXDOUBLE / 2;
    if (!s->best || d < s->best_d) {
      g_free (s->best);
      s->best = g_strdup (s->text->str);
      s->best_d = d;
    }
  }
  g_string_truncate (s->text, 0);
  if (file) {
    s->in_file = FALSE;
    s->done = s->best != NULL;
  }
}

static char *
slideshow_picture (const char *xml, int w, int h)
{
  static const GMarkupParser parser = { slides_start, slides_end, slides_text, NULL, NULL };
  g_autofree char *contents = NULL;
  gsize len;
  if (!g_file_get_contents (xml, &contents, &len, NULL))
    return NULL;
  Slides s = { g_string_new (NULL), NULL, 0, w > 0 && h > 0 ? (double) w / h : 0.5, 0, 0, FALSE, FALSE };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &s, NULL);
  if (!g_markup_parse_context_parse (ctx, contents, len, NULL))
    g_clear_pointer (&s.best, g_free);
  g_markup_parse_context_free (ctx);
  g_string_free (s.text, TRUE);
  return s.best;
}

/* The wallpaper is GNOME's (org.gnome.desktop.background, its dark picture under the dark style), which Settings'
 * Background panel sets: a picture in a folder with a parallax.json is that parallax set (its picture is the set at
 * rest, for the panel and the lock screen), any other picture a still one. NEO_PARALLAX names a set instead; with no
 * picture at all, the old de.yesman.neo wallpaper-parallax. */
static void
wallpaper_load (void)
{
  const char *env = g_getenv ("NEO_PARALLAX");
  g_autofree char *scheme = g_settings_get_string (A.iface, "color-scheme");
  g_autofree char *uri = g_settings_get_string (A.bg, !g_strcmp0 (scheme, "prefer-dark") ? "picture-uri-dark"
                                                                                       : "picture-uri");
  if (!*uri) {
    g_free (uri);
    uri = g_settings_get_string (A.bg, "picture-uri");
  }
  g_autofree char *file = g_filename_from_uri (uri, NULL, NULL);
  if (file && g_str_has_suffix (file, ".xml")) {
    char *picture = slideshow_picture (file, N.W, N.H);
    g_free (file);
    file = picture;
  }
  g_autofree char *dir = file ? g_path_get_dirname (file) : NULL;
  g_autofree char *json = dir ? g_build_filename (dir, "parallax.json", NULL) : NULL;
  g_autofree char *set = g_settings_get_string (N.settings, "wallpaper-parallax");
  Parallax *p = NULL;
  if (env)
    p = parallax_new (env, wake, NULL);
  else if (json && g_file_test (json, G_FILE_TEST_EXISTS))
    p = parallax_new (dir, wake, NULL);
  else if (file)
    p = parallax_new_image (file);
  else if (*set)
    p = parallax_new (set, wake, NULL);
  parallax_free (A.par);
  A.par = p;
  if (p) {
    parallax_resize (p, N.W, N.H, N.dp);
    parallax_run (p, !A.suspended);
  }
  wl_schedule (N.wl);
}

static gboolean
wallpaper_reload (gpointer data)
{
  A.wallpaper_id = 0;
  wallpaper_load ();
  return G_SOURCE_REMOVE;
}

static void
wallpaper_changed (GSettings *settings, const char *key, gpointer data)
{
  if (!A.wallpaper_id)
    A.wallpaper_id = g_idle_add (wallpaper_reload, NULL);
}

/* An app installed or removed (a package, a Flatpak, an Android app in Waydroid) comes or goes at once. The tiles wait
 * while one may be held: under a finger, pressed, launching, in a popup or a drag. */
static gboolean
apps_sync (gpointer data)
{
  if (A.touch >= 0 || N.press.tile || N.launch_t0 || popup_showing () || drag_holding () || drag_busy ())
    return G_SOURCE_CONTINUE;
  A.apps_id = 0;
  A.down_tile = NULL;
  if (tiles_sync ()) {
    home_layout ();
    drawer_layout ();
    wl_schedule (N.wl);
  }
  return G_SOURCE_REMOVE;
}

static void
apps_changed (GAppInfoMonitor *monitor, gpointer data)
{
  if (A.apps_id)
    g_source_remove (A.apps_id);
  A.apps_id = g_timeout_add (APPS_SETTLE_MS, apps_sync, NULL);
}

/* Settings' Home screen changed how icons look: drawn again once the changes settle (a pack and a shape picked one
 * after the other make one pass, not two) */
static guint icons_id;

static gboolean
icons_reload (gpointer data)
{
  icons_id = 0;
  tiles_reload_icons ();
  return G_SOURCE_REMOVE;
}

static void
icon_settings_changed (GSettings *s, const char *key, gpointer data)
{
  if (icons_id)
    g_source_remove (icons_id);
  icons_id = g_timeout_add (300, icons_reload, NULL);
}

/* ---- Settings' Home screen layout ------------------------------------------------------------------------------ */

#define LAYOUT_SETTLE_MS 150      /* the grid's columns and rows come as two changes: one new layout for both */

static guint layout_id;

static float
icon_scale (const char *key)
{
  return CLAMP (g_settings_get_double (N.settings, key), 0.5, 1.5);
}

/* the layout keys (de.yesman.neo) into N; the home grid's columns and rows for the model */
static void
layout_read (int *cols, int *rows)
{
  *cols = CLAMP (g_settings_get_int (N.settings, "desktop-grid-columns"), 3, 8);
  *rows = CLAMP (g_settings_get_int (N.settings, "desktop-grid-rows"), 3, 8);
  N.drawer_cols = CLAMP (g_settings_get_int (N.settings, "drawer-grid-columns"), 3, 8);
  N.icon_dp = ICON_DP * icon_scale ("desktop-icon-scale");
  N.drawer_icon_dp = ICON_DP * icon_scale ("drawer-icon-scale");
  N.labels = !g_settings_get_boolean (N.settings, "desktop-hide-app-labels");
  N.search = g_settings_get_boolean (N.settings, "search-drawer-enabled");
}

/* The layout keys are older than this launcher's reading them: the GTK Neo read them, the launcher after it did not,
 * so a phone may keep values from back then that its screen does not show (the S10e had 4 columns and no search
 * pill). The first start that finds a layout from before the grid was a setting puts them back to their defaults, so
 * that nothing on the screen changes; from then on they are Settings'. */
static void
layout_forget_old (void)
{
  static const char *const keys[] = { "desktop-grid-columns", "desktop-grid-rows", "drawer-grid-columns",
                                      "desktop-icon-scale", "drawer-icon-scale", "desktop-hide-app-labels",
                                      "search-drawer-enabled" };
  if (!model_layout_predates_grid ())
    return;
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    g_settings_reset (N.settings, keys[i]);
}

/* A layout setting changed: the pages on the new grid (model_set_grid: icons the new grid has no cell for move to the
 * next free one), the labels at their new width, the icons drawn at their new size. Not while a finger, a menu or a
 * drag holds an icon: their cells would move under them. */
static gboolean
layout_apply (gpointer data)
{
  if (A.touch >= 0 || N.launch_t0 || popup_showing () || drag_holding () || drag_busy ())
    return G_SOURCE_CONTINUE;
  layout_id = 0;
  int px = tiles_icon_px (), cols, rows;
  layout_read (&cols, &rows);
  model_set_grid (N.model, cols, rows);
  model_set_reserved (N.model, widgets_slots (0, N.model->cols));
  tiles_relabel ();
  home_layout ();
  drawer_layout ();
  omni_layout ();
  home_model_changed ();
  if (tiles_icon_px () != px)
    tiles_reload_icons ();
  wl_schedule (N.wl);
  return G_SOURCE_REMOVE;
}

static void
layout_changed (GSettings *s, const char *key, gpointer data)
{
  if (layout_id)
    g_source_remove (layout_id);
  layout_id = g_timeout_add (LAYOUT_SETTLE_MS, layout_apply, NULL);
}

static bool
init_gpu (void)
{
  if (!vk_init (&A.vk, wl_get_display (N.wl), wl_get_surface (N.wl), N.W, N.H) || !gfx_init (&A.vk) || !text_init ())
    return false;
  A.vk_w = N.W;
  A.vk_h = N.H;
  wallpaper_load ();
  g_signal_connect (A.bg, "changed", G_CALLBACK (wallpaper_changed), NULL);
  g_signal_connect (A.iface, "changed::color-scheme", G_CALLBACK (wallpaper_changed), NULL);
  int cols, rows;
  layout_forget_old ();
  layout_read (&cols, &rows);
  tiles_load (cols, rows);
  static const char *const layout_keys[] = { "changed::desktop-grid-columns", "changed::desktop-grid-rows",
                                             "changed::drawer-grid-columns", "changed::desktop-icon-scale",
                                             "changed::drawer-icon-scale", "changed::desktop-hide-app-labels",
                                             "changed::search-drawer-enabled" };
  for (guint i = 0; i < G_N_ELEMENTS (layout_keys); i++)
    g_signal_connect (N.settings, layout_keys[i], G_CALLBACK (layout_changed), NULL);
  static const char *const icon_keys[] = { "changed::icon-pack", "changed::icon-shape", "changed::icon-wrap-unthemed",
                                           "changed::icon-shape-legacy" };
  for (guint i = 0; i < G_N_ELEMENTS (icon_keys); i++)
    g_signal_connect (N.settings, icon_keys[i], G_CALLBACK (icon_settings_changed), NULL);
  A.apps_monitor = g_app_info_monitor_get ();
  g_signal_connect (A.apps_monitor, "changed", G_CALLBACK (apps_changed), NULL);
  home_layout ();
  drawer_layout ();
  nav_init ();
  clock_tick (NULL);
  if (g_getenv ("NEO_STATS"))
    A.stats_font = text_font ("Roboto Mono", 400, dp (12));
  A.ready = true;
  return true;
}

static void
stats_frame (gint64 time, gint64 cpu_start)
{
  gint64 refresh = wl_refresh_ns (N.wl);
  if (A.prev) {
    double dt = (time - A.prev) / 1e6;
    if (dt < 100) {
      A.frames++;
      A.worst_ms = MAX (A.worst_ms, dt);
      if (refresh && time - A.prev > refresh * 3 / 2)
        A.late++;
    }
  }
  A.prev = time;
  A.cpu_ms = MAX (A.cpu_ms, (mono_ns () - cpu_start) / 1e6);
  if (!A.stats_t0)
    A.stats_t0 = time;
  if (time - A.stats_t0 > 1000000000) {
    double secs = (time - A.stats_t0) / 1e9;
    g_autofree char *s = g_strdup_printf ("%.1f fps  late %d  worst %.1f ms  cpu %.1f ms", A.frames / secs, A.late,
                                          A.worst_ms, A.cpu_ms);
    if (A.stats_font) {
      text_layout_free (A.stats);
      A.stats = text_layout (A.stats_font, s, 0, 1, TEXT_LEFT);
    }
    A.frames = A.late = 0;
    A.worst_ms = A.cpu_ms = 0;
    A.stats_t0 = time;
  }
}

/* the workspace and dock, through the page stretch when one shows */
static void
draw_home (float scale, float stretch)
{
  float alpha = nav_home_alpha ();
  if (alpha <= 0)
    return;
  gfx_save ();
  gfx_opacity (alpha);
  float ty = nav_home_ty ();
  if (ty != 0)
    gfx_translate (0, ty);
  if (stretch != 0 && A.home_target) {
    float p[8] = { stretch, 0, N.W, N.H };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (A.home_target, GFX_PIPE_STRETCH);
    q->w = N.W;
    q->h = N.H;
    q->u1 = q->v1 = 1;
    q->r = q->g = q->b = q->a;
    q->mode = GFX_MODE_TEX;
  } else {
    home_draw (scale);
  }
  gfx_restore ();
}

static void
on_frame (gpointer data, gint64 time)
{
  gint64 cpu_start = mono_ns ();
  N.now = time / 1e6;
  if (!A.ready && !init_gpu ()) {
    g_main_loop_quit (A.loop);
    return;
  }
  if (A.vk_w != N.W || A.vk_h != N.H) {
    vk_resize (&A.vk, N.W, N.H);
    A.vk_w = N.W;
    A.vk_h = N.H;
    if (A.par)
      parallax_resize (A.par, N.W, N.H, N.dp);
    tiles_relabel ();               /* the columns' widths changed */
    home_layout ();
    drawer_layout ();
    omni_layout ();
    nav_icons_changed ();
  }
  bool more = A.par ? parallax_tick (A.par) : false;
  more |= home_frame ();
  more |= drawer_frame ();
  more |= popup_frame ();
  more |= omni_frame ();
  more |= drag_frame ();
  nav_drawer (drawer_progress () > 0 || drawer_dragging ());
  more |= nav_frame ();
  more |= N.launch_t0 != 0;

  /* the drawer's depth, or an app's over the launcher: the wallpaper zooms out, the home shrinks, both blur
   * (BaseDepthController) */
  float f = drawer_progress ();
  float de = spongy_curve (SPONGY_LINEAR_OUT_SLOW_IN, f);           /* DECELERATED_EASE (0, 0, 0.2, 1) */
  float sdepth = SPRING_DEPTH * drag_spring ();
  bool sharp_home = sdepth > 0;      /* a drag: only the wallpaper blurs, the workspace stays sharp over the scrim */
  float depth = floorf (MAX (MAX (DRAWER_DEPTH * de, nav_depth ()), sdepth) * 256) / 256;
  float blurring = floorf (MAX (MAX (DRAWER_DEPTH * de, nav_blurs () ? nav_depth () : 0), sdepth) * 256) / 256;
  float radius = MAX (floorf (MIN (1, blurring / BLUR_FULL_DEPTH) * dp (BLUR_MAX_DP)), nav_scrim_blur ());
  float scrim = nav_scrim_alpha ();
  float zoom = 1 + (WALLPAPER_ZOOM - 1) * (1 - depth);
  float scale = (1 + (CONTENT_SCALE - 1) * de) * launch_content_scale () * nav_home_scale ();
  float stretch = home_stretch ();

  if (!gfx_frame_begin ()) {
    wl_schedule (N.wl);
    return;
  }
  if (stretch != 0) {
    gfx_pass_begin (target (&A.home_target, N.W, N.H), (GfxColor) { 0, 0, 0, 0 });
    home_draw (scale);
    gfx_pass_end ();
  }
  GfxTex *blurred = NULL;
  if (radius >= 1) {
    /* the wallpaper and home at half size, blurred to hwui's σ = 0.57735·r + 0.5 */
    gfx_pass_begin (target (&A.bg_target, N.W / 2, N.H / 2), (GfxColor) { 0, 0, 0, 1 });
    gfx_scale (0.5f, 0.5f, 0, 0);
    if (A.par)
      parallax_draw (A.par, zoom);
    if (!sharp_home)
      draw_home (scale, stretch);
    gfx_pass_end ();
    int levels;
    float offset;
    blur_params (0.57735f * radius + 0.5f, &levels, &offset);
    blurred = gfx_blur (A.bg_target, levels, offset, 0);
  }
  drawer_prepare ();

  gfx_pass_begin (NULL, (GfxColor) { 0, 0, 0, 1 });
  if (blurred) {
    gfx_image (blurred, 0, 0, N.W, N.H, 0, 1);
  } else if (A.par) {
    parallax_draw (A.par, zoom);
  }
  if (sharp_home)
    drag_draw_scrim ();
  if (!blurred || sharp_home)
    draw_home (scale, stretch);
  drawer_draw ();
  omni_draw ();
  popup_draw ();
  drag_draw ();
  if (scrim > 0)
    gfx_rect (0, 0, N.W, N.H, 0, (GfxColor) { 0, 0, 0, scrim });
  if (A.stats)
    text_draw (A.stats, dp (16), dp (48), (GfxColor) { 1, 1, 1, 0.9f });
  gfx_pass_end ();
  wl_frame_requested (N.wl);       /* before the present, which commits it */
  gfx_frame_end ();
  stats_frame (time, cpu_start);
  if (more || g_getenv ("NEO_SPIN"))
    wl_schedule (N.wl);
}

/* ---- input ---------------------------------------------------------------------------------------------------- */

static void
hold_cancel (void)
{
  if (A.hold_id)
    g_source_remove (A.hold_id);
  A.hold_id = 0;
}

/* a finger held still: the icon's menu, or on the wallpaper the home's options; the finger stays the menu's */
static gboolean
on_hold (gpointer data)
{
  A.hold_id = 0;
  if (A.touch < 0 || A.moved)
    return G_SOURCE_REMOVE;
  N.now = MAX (N.now, mono_ns () / 1e6);
  float isz = A.owner == OWNER_DRAWER ? drawer_icon_size () : home_icon_size ();
  if (A.owner == OWNER_HOME) {
    if (nav_home_alpha () < 1 || launch_content_scale () != 1)
      return G_SOURCE_REMOVE;
    home_touch_cancel (N.now);
    haptic (HAPTIC_LONG_PRESS);
    if (A.down_tile) {
      popup_open_icon (A.down_tile, A.down_pos, false, A.down_ix, A.down_iy, isz);
      drag_hold (A.down_tile, A.down_pos, false, A.down_ix, A.down_iy, A.down_x, A.down_y);
    } else {
      popup_open_options (A.down_x, A.down_y);
    }
  } else if (A.owner == OWNER_DRAWER) {
    float ix, iy;
    Tile *t = drawer_hold_tile (A.down_x, A.down_y, &ix, &iy);
    if (!t)
      return G_SOURCE_REMOVE;
    drawer_touch_cancel (N.now);
    haptic (HAPTIC_LONG_PRESS);
    popup_open_icon (t, MODEL_POS_NONE, true, ix, iy, isz);
    drag_hold (t, MODEL_POS_NONE, true, ix, iy, A.down_x, A.down_y);
  } else {
    return G_SOURCE_REMOVE;
  }
  A.owner = OWNER_HELD;
  return G_SOURCE_REMOVE;
}

static void
on_touch (gpointer data, WlTouchType type, int id, double x, double y, guint32 time)
{
  if (!A.ready)
    return;
  double t = event_ms (time);
  N.now = MAX (N.now, t);
  if (type == WL_TOUCH_UP && id == A.touch) {
    x = A.last_x;
    y = A.last_y;
  } else if (type == WL_TOUCH_MOTION && id == A.touch) {
    A.last_x = x;
    A.last_y = y;
  }
  if (A.debug_touch && type != WL_TOUCH_MOTION)
    g_message ("touch %d id %d at %.0f,%.0f owner %d", type, id, x, y, A.owner);
  switch (type) {
  case WL_TOUCH_DOWN: {
    if (A.touch >= 0)
      return;
    /* the shell's from the bar: on the workspace that swipe is the hint and recents, never the drawer; with the
     * drawer open it is ours, and home */
    bool bar = y >= N.H - dp (NAV_GESTURE_DP);
    if (bar && drawer_progress () == 0)
      return;
    /* a drop landing, the workspace about to come back: that comes now, and the touch is spent */
    if (drag_busy ()) {
      drag_hurry ();
      return;
    }
    A.touch = id;
    A.down_x = A.last_x = x;
    A.down_y = A.last_y = y;
    A.moved = false;
    A.down_tile = NULL;
    hold_cancel ();
    if (omni_is_open () && omni_touch_down (x, y, t)) {
      A.owner = OWNER_OMNI;
      break;
    }
    if (bar && drawer_home_down (x, y, t)) {
      A.owner = OWNER_BAR;
      break;
    }
    if (popup_is_open ()) {
      A.owner = OWNER_POPUP;
      popup_touch_down (x, y);
      break;
    }
    A.hold_id = g_timeout_add (HOLD_MS, on_hold, NULL);
    if (drawer_touch_down (x, y, t)) {
      A.owner = OWNER_DRAWER;
      break;
    }
    A.owner = OWNER_HOME;
    home_touch_down (x, y, t);
    A.down_tile = home_tile_at (x, y, &A.down_ix, &A.down_iy, &A.down_pos);
    if (A.down_tile)
      press_down (A.down_tile);
    break;
  }
  case WL_TOUCH_MOTION:
    if (id != A.touch)
      return;
    if (hypot (x - A.down_x, y - A.down_y) > dp (8)) {
      A.moved = true;
      hold_cancel ();
    }
    if (A.owner == OWNER_OMNI) {
      omni_touch_move (x, y, t);
    } else if (A.owner == OWNER_BAR) {
      drawer_home_move (x, y, t);
    } else if (A.owner == OWNER_POPUP) {
      popup_touch_move (x, y);
    } else if (A.owner == OWNER_HELD) {
      /* an icon's: the drag; the wallpaper's options just go */
      if (drag_holding ()) {
        drag_move (x, y);
      } else if (hypot (x - A.down_x, y - A.down_y) > dp (DRAG_SLOP_DP)) {
        popup_close ();
        press_up ();
        A.owner = OWNER_NONE;
      }
    } else if (A.owner == OWNER_HOME) {
      bool vertical = false;
      home_touch_move (x, y, t, &vertical);
      if (A.moved)
        press_up ();
      if (vertical) {
        if (y < A.down_y) {
          A.owner = OWNER_DRAWER;
          drawer_drag_begin (A.down_y, y, t);
        } else {
          A.owner = OWNER_NONE;     /* down: the shell's notification shade */
        }
      }
    } else if (A.owner == OWNER_DRAWER) {
      drawer_touch_move (x, y, t);
    }
    break;
  case WL_TOUCH_UP:
    if (id != A.touch)
      return;
    hold_cancel ();
    if (A.owner == OWNER_OMNI) {
      omni_touch_up (x, y, t);
    } else if (A.owner == OWNER_BAR) {
      if (drawer_home_up (x, y, t))
        popup_close ();               /* home closes every open view */
    } else if (A.owner == OWNER_POPUP) {
      popup_touch_up (x, y);
    } else if (A.owner == OWNER_HELD) {
      if (drag_holding ())
        drag_up (x, y);
      N.press.tile = NULL;          /* the held icon snaps back to 1 as the finger lifts */
    } else if (A.owner == OWNER_HOME) {
      home_touch_up (x, y, t);
      press_up ();
      if (!A.moved && A.down_tile)
        tile_launch (A.down_tile, A.down_ix, A.down_iy, home_icon_size (), false);
      else if (!A.moved && !A.down_tile)
        home_widget_tap (x, y);
    } else if (A.owner == OWNER_DRAWER) {
      drawer_touch_up (x, y, t);
    }
    A.touch = -1;
    A.owner = OWNER_NONE;
    break;
  case WL_TOUCH_CANCEL:
    hold_cancel ();
    if (A.owner == OWNER_OMNI)
      omni_touch_cancel (t);
    else if (A.owner == OWNER_BAR)
      drawer_home_cancel (t);
    else if (A.owner == OWNER_POPUP)
      popup_touch_cancel ();
    else if (A.owner == OWNER_HELD) {
      drag_cancel (true);
      N.press.tile = NULL;
    } else if (A.owner == OWNER_HOME)
      home_touch_cancel (t);
    else if (A.owner == OWNER_DRAWER)
      drawer_touch_cancel (t);
    press_up ();
    A.touch = -1;
    A.owner = OWNER_NONE;
    break;
  }
  wl_schedule (N.wl);
}

void
touch_drop (void)
{
  if (A.touch >= 0)
    on_touch (NULL, WL_TOUCH_CANCEL, -1, 0, 0, (guint32) (mono_ns () / 1000000));
}

static void
on_key (gpointer data, guint32 key, guint32 keysym, const char *utf8, bool pressed)
{
  /* the search view's field, or the drawer's, takes the typing while it has it */
  if (pressed && omni_key (keysym, utf8)) {
    wl_schedule (N.wl);
    return;
  }
  if (pressed && drawer_search_key (keysym, utf8)) {
    wl_schedule (N.wl);
    return;
  }
  if (pressed && key == KEY_Q)
    g_main_loop_quit (A.loop);
}

/* the input method's (the on-screen keyboard's) text for the search field */
static void
on_text (gpointer data, const char *utf8)
{
  if (omni_is_open ())
    omni_text (utf8);
  else
    drawer_search_text (utf8);
}

static void
on_erase (gpointer data, int before, int after)
{
  if (omni_is_open ())
    omni_erase (before, after);
  else
    drawer_search_erase (before, after);
}

static void
on_submit (gpointer data)
{
  if (omni_is_open ())
    omni_submit ();
  else
    drawer_search_submit ();
}

static void
on_resize (gpointer data, int w, int h, double scale)
{
  N.W = w;
  N.H = h;
  /* a dp is a logical pixel of the compositor's, which picks the scale from the panel's DPI (3 on the S10e, Android's
   * own density there); NEO_DENSITY overrides it */
  const char *d = g_getenv ("NEO_DENSITY");
  N.dp = d ? g_ascii_strtod (d, NULL) : scale > 0 ? scale : 3.0;
  spongy_set_density (N.dp);
}

static void
on_close (gpointer data)
{
  g_main_loop_quit (A.loop);
}

static void
on_shown (gpointer data, guint64 frame)
{
  nav_shown (frame);
}

/* an app over the launcher (Launcher3 is stopped then): the wallpaper stops following the phone, its sensor polls
 * and its frames with it, until the launcher shows again */
static void
on_suspended (gpointer data, bool suspended)
{
  g_message ("%s", suspended ? "suspended: the parallax stops" : "shown again: the parallax follows the phone");
  A.suspended = suspended;
  parallax_run (A.par, !suspended);
  if (!suspended)
    wl_schedule (N.wl);
}

static gboolean
on_signal (gpointer data)
{
  g_main_loop_quit (A.loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  static const WlCallbacks cb = { on_resize, on_frame,  on_touch, on_key,   on_close,
                                  on_shown,  on_suspended, on_text, on_erase, on_submit };
  A.loop = g_main_loop_new (NULL, FALSE);
  N.settings = g_settings_new ("de.yesman.neo");
  A.bg = g_settings_new ("org.gnome.desktop.background");
  A.iface = g_settings_new ("org.gnome.desktop.interface");
  g_autofree char *flag = g_build_filename (g_get_user_config_dir (), "neo-launcher", "debug-touch", NULL);
  A.debug_touch = g_getenv ("NEO_DEBUG_TOUCH") || g_file_test (flag, G_FILE_TEST_EXISTS);
  N.wl = wl_open ("de.yesman.NeoLauncher", &cb, NULL);
  if (!N.wl)
    return 1;
  g_unix_signal_add (SIGTERM, on_signal, NULL);
  g_unix_signal_add (SIGINT, on_signal, NULL);
  wl_schedule (N.wl);
  g_main_loop_run (A.loop);
  if (A.ready)
    vkDeviceWaitIdle (A.vk.dev);
  return 0;
}
