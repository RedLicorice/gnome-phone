/* Gesture navigation, the launcher's half. The shell runs the gestures and moves the windows; it tells the launcher
 * what to look like behind them (de.yesman.NeoLauncher.State), and the launcher tells the shell where its icons are
 * (de.yesman.GnomePhone.Icons), so a window going home can fly into its own icon.
 *
 *   background  an app is being dragged: Launcher3's BACKGROUND_APP, depth 0.7 (wallpaper blurred to 30 dp and
 *               zoomed out), no workspace
 *   reveal      the app went home: ScalingWorkspaceRevealAnim from t0, the workspace and dock 0.85 → 1 over
 *               1000 ms on SCALE_REVEAL, their alpha 0 → 1 over the first 200 ms, depth 0.7 → 0 on the same curve
 *               with the blur dropped in the first frame; the app's icon stays hidden until the shell says
 *   home        recents went home (OVERVIEW → NORMAL): the workspace 0.92 → 1 on DECELERATE and its alpha on
 *               ACCELERATE over 250 ms, depth 0.7 → 0 LINEAR, the blur following the depth
 *   home-reveal recents went home on a swipe up from the bar: WorkspaceRevealAnim, 0.88 → 1 and alpha 0 → 1, and
 *               the depth, all on DECELERATED_EASE over 350 ms
 *   back        the back swipe from an app at its root (LauncherBackAnimationController): the workspace and dock
 *               set to 0.85, depth 0.7 with its blur paused, the app's icon hidden, and the scrim over it all:
 *               black at 0.8 with a blur of 90 px, which the shell then moves with the progress (Scrim)
 *   back-reveal the back swipe committed: the reveal's scale and depth on SCALE_REVEAL over 1000 ms, but no alpha
 *               (playAlphaReveal = false) and still no depth blur; the scrim's alpha and blur from where the shell
 *               left them to 0 over 233 ms LINEAR; the icon stays hidden until the shell says
 *   normal      at rest
 *
 * Pose: the shell drives the workspace frame by frame (the swipe up from the bar on the home screen: HINT_STATE
 * under the finger, then OVERVIEW or back to NORMAL), its scale, alpha, depth and offset; it stays until a State. */
#include <math.h>

#include "neo.h"
#include "omni.h"

#define REVEAL_MS          1000.0
#define REVEAL_FADE_MS     200.0
#define REVEAL_SCALE_FROM  0.85f
#define BACKGROUND_DEPTH   0.7f
#define ICONS_DELAY_MS     100
#define HOME_MS            250.0
#define HOME_SCALE_FROM    0.92f
#define HOME_REVEAL_MS     350.0
#define HOME_REVEAL_FROM   0.88f
#define BACK_SCRIM_ALPHA   0.8f           /* the dark theme's */
#define BACK_SCRIM_BLUR    90.0f          /* px: max_depth_blur_radius_enhanced */
#define BACK_SCRIM_FADE_MS 233.0

typedef enum {
  NAV_NORMAL, NAV_BACKGROUND, NAV_REVEAL, NAV_HOME, NAV_HOME_REVEAL, NAV_BACK, NAV_BACK_REVEAL, NAV_POSE
} NavState;

static struct {
  NavState state;
  double   t0;              /* ms on the frame clock */
  char    *hidden;          /* the app whose icon the window is flying into */
  struct { float scale, alpha, depth, ty; } pose;
  struct { float alpha, blur; } scrim;  /* back: the scrim as the shell last set it */
  int      drawer;          /* the drawer as the shell last heard of it: -1 never told, 0 closed, 1 open */
  guint    owner_id, watch_id, icons_id;
  bool     shell;           /* the shell's service is on the bus */
  GSList  *show_waits;      /* ShowIcon calls waiting for their frame (ShowWait) */
} V;

typedef struct {
  GDBusMethodInvocation *inv;
  guint64                frame;
} ShowWait;

/* ---- curves --------------------------------------------------------------------------------------------------- */

static float
bezier (float t, float a, float b, float c, float d)
{
  float u = 1 - t;
  return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

/* PathInterpolator over cubic segments {x0,y0, x1,y1, x2,y2, x3,y3} */
static float
path (const float (*seg)[8], int n, float x)
{
  if (x <= 0)
    return 0;
  if (x >= 1)
    return 1;
  int i = 0;
  while (i < n - 1 && x > seg[i][6])
    i++;
  const float *s = seg[i];
  float lo = 0, hi = 1;
  for (int k = 0; k < 24; k++) {
    float mid = (lo + hi) / 2;
    if (bezier (mid, s[0], s[2], s[4], s[6]) < x)
      lo = mid;
    else
      hi = mid;
  }
  return bezier ((lo + hi) / 2, s[1], s[3], s[5], s[7]);
}

/* ScalingWorkspaceRevealAnim.SCALE_REVEAL */
static float
scale_reveal (float x)
{
  static const float seg[2][8] = {
    { 0, 0, 0.045f, 0.0356f, 0.0975f, 0.2055f, 0.15f, 0.3952f },
    { 0.15f, 0.3952f, 0.235f, 0.6855f, 0.235f, 1, 1, 1 },
  };
  return path (seg, 2, x);
}

/* Interpolators.DECELERATED_EASE */
static float
decelerated_ease (float x)
{
  static const float seg[1][8] = { { 0, 0, 0, 0, 0.2f, 1, 1, 1 } };
  return path (seg, 1, x);
}

/* ---- what the home looks like now ------------------------------------------------------------------------------ */

static float
state_t (void)
{
  return (float) MAX (0, N.now - V.t0);
}

static double
state_ms (void)
{
  switch (V.state) {
  case NAV_REVEAL:
  case NAV_BACK_REVEAL: return REVEAL_MS;
  case NAV_HOME:        return HOME_MS;
  case NAV_HOME_REVEAL: return HOME_REVEAL_MS;
  default:              return 0;
  }
}

/* progress through the state's own animation, 0 … 1 */
static float
state_p (void)
{
  double ms = state_ms ();
  return ms > 0 ? CLAMP (state_t () / ms, 0, 1) : 1;
}

float
nav_depth (void)
{
  switch (V.state) {
  case NAV_BACKGROUND:
  case NAV_BACK:        return BACKGROUND_DEPTH;
  case NAV_REVEAL:
  case NAV_BACK_REVEAL: return BACKGROUND_DEPTH * (1 - scale_reveal (state_p ()));
  case NAV_HOME:        return BACKGROUND_DEPTH * (1 - state_p ());
  case NAV_HOME_REVEAL: return BACKGROUND_DEPTH * (1 - decelerated_ease (state_p ()));
  case NAV_POSE:        return V.pose.depth;
  default:              return 0;
  }
}

/* whether the blur follows the depth (the reveal drops it in its first frame) */
bool
nav_blurs (void)
{
  return V.state == NAV_BACKGROUND || V.state == NAV_HOME || V.state == NAV_HOME_REVEAL || V.state == NAV_POSE;
}

float
nav_home_scale (void)
{
  float p = state_p ();
  switch (V.state) {
  case NAV_BACKGROUND:
  case NAV_BACK:        return REVEAL_SCALE_FROM;
  case NAV_REVEAL:
  case NAV_BACK_REVEAL: return REVEAL_SCALE_FROM + (1 - REVEAL_SCALE_FROM) * scale_reveal (p);
  case NAV_HOME:        return HOME_SCALE_FROM + (1 - HOME_SCALE_FROM) * (1 - (1 - p) * (1 - p));
  case NAV_HOME_REVEAL: return HOME_REVEAL_FROM + (1 - HOME_REVEAL_FROM) * decelerated_ease (p);
  case NAV_POSE:        return V.pose.scale;
  default:              return 1;
  }
}

float
nav_home_alpha (void)
{
  switch (V.state) {
  case NAV_BACKGROUND:  return 0;
  case NAV_REVEAL:      return CLAMP (state_t () / REVEAL_FADE_MS, 0, 1);
  case NAV_HOME:        return state_p () * state_p ();
  case NAV_HOME_REVEAL: return decelerated_ease (state_p ());
  case NAV_POSE:        return V.pose.alpha;
  default:              return 1;
  }
}

/* back's scrim over the whole launcher: its black's alpha, and the blur behind it in px */
static float
scrim_fade (void)
{
  return V.state == NAV_BACK_REVEAL ? 1 - CLAMP (state_t () / BACK_SCRIM_FADE_MS, 0, 1) : 1;
}

float
nav_scrim_alpha (void)
{
  bool back = V.state == NAV_BACK || V.state == NAV_BACK_REVEAL;
  return back ? V.scrim.alpha * scrim_fade () : 0;
}

float
nav_scrim_blur (void)
{
  bool back = V.state == NAV_BACK || V.state == NAV_BACK_REVEAL;
  return back ? V.scrim.blur * scrim_fade () : 0;
}

/* px down */
float
nav_home_ty (void)
{
  return V.state == NAV_POSE ? V.pose.ty : 0;
}

bool
nav_frame (void)
{
  if (state_ms () > 0 && state_t () >= state_ms ())
    V.state = NAV_NORMAL;
  return state_ms () > 0;
}

static void
show_icon (void)
{
  Tile *t = tiles_get (V.hidden);
  if (t)
    t->hidden = false;
  g_clear_pointer (&V.hidden, g_free);
  wl_schedule (N.wl);
}

/* ---- the icons, for the shell ---------------------------------------------------------------------------------- */

/* Every app's picture, and where its icon is when it is on the current page or the dock (else a zero rect): the
 * shell flies windows home into the rects and shows the pictures on its recents cards */
static gboolean
push_icons (gpointer data)
{
  V.icons_id = 0;
  if (!V.shell || !N.bus || !N.model)
    return G_SOURCE_REMOVE;
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(s(dddd)s)"));
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, N.tiles);
  while (g_hash_table_iter_next (&it, &key, &value)) {
    Tile *t = value;
    float r[4] = { 0 };
    if (!home_icon_rect (t->id, r) && !t->splash_icon)
      continue;
    g_variant_builder_add (&b, "(s(dddd)s)", t->id, (double) r[0], (double) r[1], (double) r[2], (double) r[3],
                           t->splash_icon ? t->splash_icon : "");
  }
  g_dbus_connection_call (N.bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone", "Icons",
                          g_variant_new ("(a(s(dddd)s))", &b), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
  return G_SOURCE_REMOVE;
}

/* the icons on the page or the dock moved: tell the shell, once things settle */
void
nav_shown (guint64 frame)
{
  while (V.show_waits && ((ShowWait *) V.show_waits->data)->frame <= frame) {
    ShowWait *w = V.show_waits->data;
    V.show_waits = g_slist_delete_link (V.show_waits, V.show_waits);
    g_dbus_method_invocation_return_value (w->inv, NULL);
    g_free (w);
  }
}

void
nav_icons_changed (void)
{
  if (V.icons_id)
    g_source_remove (V.icons_id);
  V.icons_id = g_timeout_add (ICONS_DELAY_MS, push_icons, NULL);
}

/* the drawer opened or closed: the shell leaves a swipe up from the bar to it while it is open */
void
nav_drawer (bool open)
{
  if (!V.shell || !N.bus || V.drawer == (int) open)
    return;
  V.drawer = open;
  g_dbus_connection_call (N.bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone", "Drawer",
                          g_variant_new ("(b)", open), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

/* ---- de.yesman.NeoLauncher ------------------------------------------------------------------------------------- */

static const char IFACE[] =
  "<node>"
  "  <interface name='de.yesman.NeoLauncher'>"
  /* state: normal | background | reveal | home | home-reveal | back | back-reveal; t0: when it starts (µs,
   * CLOCK_MONOTONIC); app_id (reveal, back, back-reveal): the icon a window is flying into, hidden until ShowIcon */
  "    <method name='State'>"
  "      <arg type='s' name='state' direction='in'/>"
  "      <arg type='x' name='t0' direction='in'/>"
  "      <arg type='s' name='app_id' direction='in'/>"
  "    </method>"
  "    <method name='ShowIcon'/>"
  /* back: the scrim's alpha, and its blur in px */
  "    <method name='Scrim'>"
  "      <arg type='d' name='alpha' direction='in'/>"
  "      <arg type='d' name='blur' direction='in'/>"
  "    </method>"
  /* back, as the launcher takes it: the drawer closes */
  "    <method name='Back'/>"
  /* a back swipe began over the launcher: the finger is the shell's from now on (Android pilfers it) */
  "    <method name='Cancel'/>"
  /* the lock screen came up: Launcher3 goes to NORMAL as the screen goes off, so no menu and no drawer are left
   * behind it */
  "    <method name='Locked'/>"
  /* the workspace and dock now: scale (about the screen's centre), alpha, depth, and px down */
  "    <method name='Pose'>"
  "      <arg type='d' name='scale' direction='in'/>"
  "      <arg type='d' name='alpha' direction='in'/>"
  "      <arg type='d' name='depth' direction='in'/>"
  "      <arg type='d' name='ty' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static void
method_call (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *method,
             GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
  if (!g_strcmp0 (method, "State")) {
    const char *state, *app;
    gint64 t0;
    g_variant_get (params, "(&sx&s)", &state, &t0, &app);
    show_icon ();
    if (g_strcmp0 (state, "normal")) {
      popup_close_now ();           /* an app over the launcher, or going home: no menu or drag left behind */
      drag_cancel (true);
    }
    if (!g_strcmp0 (state, "background")) {
      V.state = NAV_BACKGROUND;
      omni_close_now ();
      drawer_close_now ();
    } else if (!g_strcmp0 (state, "reveal") || !g_strcmp0 (state, "back") || !g_strcmp0 (state, "back-reveal")) {
      V.state = state[0] == 'r' ? NAV_REVEAL : state[4] ? NAV_BACK_REVEAL : NAV_BACK;
      V.t0 = t0 / 1000.0;
      if (V.state == NAV_BACK) {
        drawer_close_now ();
        V.scrim.alpha = BACK_SCRIM_ALPHA;
        V.scrim.blur = BACK_SCRIM_BLUR;
      }
      Tile *t = tiles_get (app);
      if (t) {
        t->hidden = true;
        V.hidden = g_strdup (app);
      }
    } else if (!g_strcmp0 (state, "home") || !g_strcmp0 (state, "home-reveal")) {
      V.state = state[4] ? NAV_HOME_REVEAL : NAV_HOME;
      omni_close_now ();
      V.t0 = t0 / 1000.0;
    } else {
      V.state = NAV_NORMAL;
    }
    wl_schedule (N.wl);
  } else if (!g_strcmp0 (method, "ShowIcon")) {
    if (V.hidden) {
      show_icon ();
      /* answered once the next frame, the first with the icon, is on screen: the shell keeps its flying icon over
       * this one until then, so that no frame has neither */
      ShowWait *w = g_new (ShowWait, 1);
      w->inv = inv;
      w->frame = wl_frames (N.wl) + 1;
      V.show_waits = g_slist_append (V.show_waits, w);
      return;
    }
  } else if (!g_strcmp0 (method, "Back")) {
    if (drag_holding ())
      touch_drop ();                /* back drops the drag (a held icon too) */
    else if (popup_is_open ())
      popup_close ();
    else if (omni_back ())          /* the search view closes first */
      ;
    else if (!drawer_search_back ())  /* a search ends first, then the drawer closes */
      drawer_back ();
  } else if (!g_strcmp0 (method, "Cancel")) {
    touch_drop ();
  } else if (!g_strcmp0 (method, "Locked")) {
    touch_drop ();
    drag_cancel (true);
    popup_close_now ();
    omni_close_now ();
    drawer_close_now ();
    wl_schedule (N.wl);
  } else if (!g_strcmp0 (method, "Scrim")) {
    double alpha, blur;
    g_variant_get (params, "(dd)", &alpha, &blur);
    V.scrim.alpha = alpha;
    V.scrim.blur = blur;
    wl_schedule (N.wl);
  } else if (!g_strcmp0 (method, "Pose")) {
    double scale, alpha, depth, ty;
    g_variant_get (params, "(dddd)", &scale, &alpha, &depth, &ty);
    if (V.state != NAV_POSE) {
      drawer_close_now ();
      popup_close_now ();
      drag_cancel (true);
    }
    V.state = NAV_POSE;
    V.pose.scale = scale;
    V.pose.alpha = alpha;
    V.pose.depth = depth;
    V.pose.ty = ty;
    wl_schedule (N.wl);
  }
  g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL };

static void
shell_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  V.shell = true;
  V.drawer = -1;
  nav_icons_changed ();
}

static void
shell_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  V.shell = false;
}

void
nav_init (void)
{
  if (!N.bus)
    N.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!N.bus)
    return;
  g_autoptr (GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (IFACE, NULL);
  g_dbus_connection_register_object (N.bus, "/de/yesman/NeoLauncher", info->interfaces[0], &vtable, NULL, NULL,
                                     NULL);
  V.owner_id = g_bus_own_name_on_connection (N.bus, "de.yesman.NeoLauncher", G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                             NULL, NULL, NULL, NULL);
  V.watch_id = g_bus_watch_name_on_connection (N.bus, "de.yesman.GnomePhone", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               shell_appeared, shell_vanished, NULL, NULL);
}
