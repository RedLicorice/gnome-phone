/* The home screen: Launcher3's workspace (cols × rows cells, 5 × 5 unless Settings picked another grid; the clock
 * widget across the first page's top row), the dock (5 cells whatever the grid) and the page dots, with PagedView's paging: 1:1 drag after the paging slop, fling or snap with the quintic
 * scroller, and the edge stretch past the first and last page. Pages sit 10.77 dp apart.
 *
 * While an icon is dragged (drag.c) the workspace is SPRING_LOADED: every page, its CellLayout bounds included,
 * shrinks to fit between the drop bar's room (status bar + 104 dp) and 76 dp above the hotseat, about the
 * workspace's centre, and each shows its card: accent1_100 at 0.35, 28 dp corners, a 2 dp outline of the same on
 * the page under the drag. The hotseat stays as it is. */
#include <math.h>

#include "neo.h"
#include "widgets.h"

#define PAGING_SLOP_DP        16          /* 2 × the 8 dp touch slop */
#define TOUCH_SLOP_DP         8
#define FLING_PX_S_DP         500         /* dp/s */
#define MIN_FLING_DP          250
#define MIN_SNAP_DP           1500
#define SIGNIFICANT_MOVE      0.4f
#define SNAP_MS               750.0
#define MAX_ANGLE             (G_PI / 3)  /* 60°: never a page swipe */
#define DAMP_ANGLE            (G_PI / 6)  /* 30°: the slop grows from here */

#define CLOCK_TIME_DP         51
#define CLOCK_TIME_BASELINE_DP (208.5 / 3.0)   /* below the widget cell's top */
#define CLOCK_DATE_DP         9
#define CLOCK_DATE_BASELINE_DP (247 / 3.0)
#define CLOCK_TIME_RGB        0xE0E0E0      /* a soft grey-white, the owner's choice over the accent (0xA9EEF8) */

#define PAGE_SPACING_DP       10.77         /* Workspace: max(10.77 dp, its padding + 1) */
#define CARD_PAD_TOP_DP       12.7          /* the CellLayout's bounds around its cells, measured shrunk */
#define CARD_PAD_BOTTOM_DP    11.3
#define SPRING_TOP_DP         (32 + 56 + 16)  /* below the status bar: the drop bar and its margins */
#define SPRING_BOTTOM_DP      76            /* above the hotseat */
#define SPRING_SIDE_DP        48            /* the shrunk page leaves at least this at each side */
#define CARD_RADIUS_DP        28            /* dialogCornerRadius */
#define CARD_STROKE_DP        2
#define CARD_FILL_ALPHA       0.35f
#define CARD_RGB              0xB8EBF3      /* system_accent1_100 */
#define DOTS_SPRING_DP        36            /* the page dots above the hotseat while shrunk, measured */

static struct {
  /* geometry */
  float cell_x0, cell_w, cell_y0, cell_h, icon, icon_top, dock_top, dock_icon_top, dots_y;
  float dock_w;                    /* a dock cell's width */
  float label_y;                   /* the label's baseline below the icon's top */

  /* paging */
  float       pitch;               /* px from one page to the next: W and the page spacing */
  float       scroll;              /* px: page × pitch at rest */
  int         page;
  bool        extra;               /* an empty page after the last, while a drag may drop on it */
  bool        down, dragging, vertical, gave_up;
  int         touch_page;
  float       down_x, down_y, last_x, scroll_down;
  SpongyVelocity vel;
  SpongyEdge  left, right;
  float       stretch;             /* this frame's edge stretch amount */
  bool        snapping;
  double      snap_t0, snap_ms;
  float       snap_from, snap_to;

  /* the clock widget */
  TextFont   *time_font, *date_font;
  TextLayout *time, *date;
} H;

static int
n_pages (void)
{
  return MAX (1, (int) N.model->pages->len) + H.extra;
}

/* a page's cells; NULL for the extra page */
static GPtrArray *
page_cells (int p)
{
  return p >= 0 && p < (int) N.model->pages->len ? N.model->pages->pdata[p] : NULL;
}

/* the label's room: LABEL_HOME_MAX_DP in a column of the 5-column grid, and what this grid's columns are wider */
float
home_label_width (void)
{
  float span = N.W - 2 * dp (WS_SIDE_DP);
  return dp (LABEL_HOME_MAX_DP) + span / N.model->cols - span / LABEL_REF_COLS;
}

void
home_layout (void)
{
  H.cell_x0 = dp (WS_SIDE_DP);
  H.cell_w = (N.W - 2 * H.cell_x0) / N.model->cols;
  H.dock_w = (N.W - 2 * H.cell_x0) / N.model->dock_size;
  H.cell_y0 = roundf (dp (STATUS_BAR_DP + WS_TOP_DP));
  float bottom = N.H - dp (HOTSEAT_DP) - dp (PAGE_IND_DP);
  H.cell_h = floorf ((bottom - H.cell_y0) / N.model->rows);
  /* the icon at its set size, as long as it fits its cells (its label under it, the press's 1.1 at its sides) and
   * the dock's */
  float label = dp (WS_CONTENT_DP - ICON_DP);
  float fit = MIN (MIN (H.cell_w, H.dock_w) / 1.1f, H.cell_h - (N.labels ? label : 0));
  H.icon = roundf (MIN (dp (N.icon_dp), fit));
  H.label_y = H.icon + dp (LABEL_BASELINE_DP - ICON_DP);
  /* icon, gap and label centred in the cell; without labels, the icon alone */
  H.icon_top = floorf ((H.cell_h - H.icon - (N.labels ? label : 0)) / 2);
  H.dock_top = N.H - dp (HOTSEAT_DP);
  H.dock_icon_top = H.dock_top + floorf ((dp (HOTSEAT_CELL_DP) - H.icon) / 2);
  H.dots_y = bottom + dp (PAGE_IND_DP) / 2;
  spongy_edge_init (&H.left, N.W);
  spongy_edge_init (&H.right, N.W);
  H.pitch = N.W + dp (PAGE_SPACING_DP);
  H.page = CLAMP (H.page, 0, n_pages () - 1);
  H.scroll = H.page * H.pitch;
  if (!H.time_font) {
    /* the optional Renaissance look (setting de.yesman.GnomePhone.Theme name) sets the clock in the first installed
     * family of the font-display chain (any font installed with tools/phone/install-fonts.sh); read when the launcher
     * starts. Without the look, or with none of the chain installed, the stock face. */
    g_autofree char *family = NULL;
    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                                         "de.yesman.GnomePhone.Theme", TRUE);
    if (schema) {
      g_autoptr (GSettings) theme = g_settings_new_full (schema, NULL, NULL);
      g_autofree char *name = g_settings_get_string (theme, "name");
      if (g_strcmp0 (name, "renaissance") == 0 && g_settings_schema_has_key (schema, "font-display")) {
        g_autofree char *chain = g_settings_get_string (theme, "font-display");
        g_auto (GStrv) names = g_strsplit (chain, ",", -1);
        for (int i = 0; names[i] && !family; i++) {
          g_strstrip (names[i]);
          if (*names[i] && text_family_installed (names[i]))
            family = g_strdup (names[i]);
        }
      }
    }
    H.time_font = family ? text_font (family, 700, dp (CLOCK_TIME_DP))
                         : text_font_axes ("Google Sans Flex", 600, dp (CLOCK_TIME_DP), "opsz=144,wdth=86");
    H.date_font = text_font_axes ("Google Sans Flex", 700, dp (CLOCK_DATE_DP), "opsz=18,ROND=100");
  }
  home_clock_tick ();
}

void
home_clock_tick (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *time = g_date_time_format (now, "%H:%M");
  g_autofree char *date = g_date_time_format (now, "%a, %b %-d");
  text_layout_free (H.time);
  text_layout_free (H.date);
  H.time = text_layout (H.time_font, time, 0, 1, TEXT_CENTER);
  H.date = text_layout (H.date_font, date, 0, 1, TEXT_CENTER);
  wl_schedule (N.wl);
}

/* ---- where things are ----------------------------------------------------------------------------------------- */

/* the icon box of a page's slot (the strip of pages: page p starts at x = p·pitch) */
static void
slot_rect (int page, int slot, float *x, float *y)
{
  if (page == 0)
    slot += N.model->reserved;
  int col = slot % N.model->cols, row = slot / N.model->cols;
  *x = roundf (page * H.pitch + H.cell_x0 + col * H.cell_w + (H.cell_w - H.icon) / 2);
  *y = roundf (H.cell_y0 + row * H.cell_h + H.icon_top);
}

static void
dock_rect (int slot, float *x, float *y)
{
  *x = roundf (H.cell_x0 + slot * H.dock_w + (H.dock_w - H.icon) / 2);
  *y = roundf (H.dock_icon_top);
}

Tile *
home_tile_at (float x, float y, float *ix, float *iy, ModelPos *pos)
{
  for (guint i = 0; i < N.model->dock->len; i++) {
    float rx, ry;
    dock_rect (i, &rx, &ry);
    if (x >= rx - (H.dock_w - H.icon) / 2 && x < rx + H.icon + (H.dock_w - H.icon) / 2 && y >= H.dock_top &&
        y < H.dock_top + dp (HOTSEAT_CELL_DP)) {
      *ix = rx;
      *iy = ry;
      *pos = (ModelPos) { i, -1, -1 };
      return tiles_get (N.model->dock->pdata[i]);
    }
  }
  int page = (int) floorf ((x + H.scroll) / H.pitch);
  GPtrArray *items = page_cells (page);
  for (guint i = 0; items && i < items->len; i++) {
    float rx, ry;
    slot_rect (page, i, &rx, &ry);
    rx -= H.scroll;
    /* the icon and its label, Launcher3's BubbleTextView box */
    float h = N.labels ? H.label_y : H.icon;
    if (x >= rx - dp (8) && x < rx + H.icon + dp (8) && y >= ry - dp (8) && y < ry + h + dp (8)) {
      *ix = rx;
      *iy = ry;
      *pos = (ModelPos) { -1, page, i };
      return tiles_get (items->pdata[i]);
    }
  }
  return NULL;
}

void
home_model_changed (void)
{
  H.page = CLAMP (H.page, 0, n_pages () - 1);
  if (!H.snapping && !H.dragging)
    H.scroll = H.page * H.pitch;
  nav_icons_changed ();
  wl_schedule (N.wl);
}

bool
home_icon_rect (const char *id, float r[4])
{
  for (guint i = 0; i < N.model->dock->len; i++)
    if (!g_strcmp0 (N.model->dock->pdata[i], id)) {
      dock_rect (i, &r[0], &r[1]);
      r[2] = r[3] = H.icon;
      return true;
    }
  GPtrArray *items = page_cells (H.page);
  for (guint i = 0; items && i < items->len; i++)
    if (!g_strcmp0 (items->pdata[i], id)) {
      /* where it rests once the page has settled: the shell reads this 100 ms after a swipe, mid-snap */
      slot_rect (H.page, i, &r[0], &r[1]);
      r[0] -= H.page * H.pitch;
      r[2] = r[3] = H.icon;
      return true;
    }
  return false;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
draw_icon_and_label (Tile *t, float x, float y, bool label)
{
  if (!t)
    return;
  tile_draw_icon (t, x, y, H.icon, 1);
  if (label && N.labels) {
    float lw = home_label_width ();
    label_draw (t->label, x + H.icon / 2 - lw / 2, y + H.label_y - text_ascent (N.label_font),
                N.day_ink ? (GfxColor) { 0.10f, 0.12f, 0.14f, 1 } : (GfxColor) { 1, 1, 1, 1 }, !N.day_ink);
  }
}

/* the clock widget, in its box (widgets.c) */
void
home_clock_draw (float x, float top, float w, float h)
{
  float cx = x + w / 2;
  if (H.time)
    text_draw (H.time, cx, top + dp (CLOCK_TIME_BASELINE_DP) - text_ascent (H.time_font),
               N.day_ink ? (GfxColor) { 0.16f, 0.18f, 0.21f, 1 } : GFX_RGBA (CLOCK_TIME_RGB, 1));
  if (H.date)
    label_draw (H.date, cx, top + dp (CLOCK_DATE_BASELINE_DP) - text_ascent (H.date_font),
                N.day_ink ? (GfxColor) { 0.22f, 0.25f, 0.29f, 1 } : (GfxColor) { 1, 1, 1, 1 }, false);
}

static void
draw_dots (float spring)
{
  int n = n_pages ();
  if (n < 2)
    return;
  float d = dp (PAGE_DOT_DP), pitch = d + dp (PAGE_DOT_GAP_DP);
  float x0 = N.W / 2 - (n * pitch - dp (PAGE_DOT_GAP_DP)) / 2;
  float y = H.dots_y + (H.dock_top - dp (DOTS_SPRING_DP) - H.dots_y) * spring;
  for (int i = 0; i < n; i++)
    gfx_rect (x0 + i * pitch, y - d / 2, d, d, d / 2, (GfxColor) { 1, 1, 1, 0.5f });
  float at = CLAMP (H.scroll / H.pitch, 0, n - 1);
  gfx_rect (x0 + at * pitch, y - d / 2, d, d, d / 2, (GfxColor) { 1, 1, 1, 1 });
}

/* the CellLayout's bounds: its cells and its padding */
static void
card_rect (int page, float r[4])
{
  r[0] = page * H.pitch;
  r[1] = H.cell_y0 - dp (CARD_PAD_TOP_DP);
  r[2] = N.W;
  r[3] = N.model->rows * H.cell_h + dp (CARD_PAD_TOP_DP + CARD_PAD_BOTTOM_DP);
}

/* the spring-loaded transform now: the strip scaled by S about the workspace's centre, then moved TY down */
static void
spring (float *S, float *TY)
{
  float q = drag_spring ();
  *S = 1;
  *TY = 0;
  if (q <= 0)
    return;
  float card[4];
  card_rect (0, card);
  float to_top = roundf (dp (STATUS_BAR_DP + SPRING_TOP_DP)), to_bottom = N.H - dp (HOTSEAT_DP + SPRING_BOTTOM_DP);
  float s = MIN (1, (to_bottom - to_top) / card[3]);
  if (s * N.W > N.W - 2 * dp (SPRING_SIDE_DP))
    s = (N.W - 2 * dp (SPRING_SIDE_DP)) / N.W;
  float P = N.H / 2.0f;
  *S = 1 + (s - 1) * q;
  *TY = (to_top - (P + (card[1] - P) * s)) * q;
}

static void
draw_cell (int page, int i, const char *id, bool label)
{
  if (!id || drag_hides (page, i))
    return;
  float x, y, dx = 0, dy = 0;
  if (page < 0)
    dock_rect (i, &x, &y);
  else
    slot_rect (page, i, &x, &y);
  drag_offset (page, i, &dx, &dy);
  draw_icon_and_label (tiles_get (id), x + dx, y + dy, label);
}

void
home_draw (float content_scale)
{
  float S, TY, q = drag_spring ();
  spring (&S, &TY);
  gfx_save ();
  if (content_scale != 1)
    gfx_scale (content_scale, content_scale, N.W / 2.0f, N.H / 2.0f);
  /* shrunk, the neighbours show at the sides */
  float at = H.scroll / H.pitch;
  int first = MAX (0, (int) floorf (at) - (q > 0)), last = MIN (n_pages () - 1, (int) ceilf (at) + (q > 0));
  gfx_save ();
  if (q > 0) {
    gfx_translate (0, TY);
    gfx_scale (S, S, N.W / 2.0f, N.H / 2.0f);
  }
  gfx_translate (-H.scroll, 0);
  int over = drag_over_page ();
  for (int p = first; p <= last; p++) {
    if (q > 0) {
      float r[4];
      card_rect (p, r);
      gfx_rect (r[0], r[1], r[2], r[3], dp (CARD_RADIUS_DP), GFX_RGBA (CARD_RGB, CARD_FILL_ALPHA * q));
      if (p == over)
        gfx_ring (r[0], r[1], r[2], r[3], dp (CARD_RADIUS_DP), dp (CARD_STROKE_DP), GFX_RGBA (CARD_RGB, q));
    }
    widgets_draw (p, p * H.pitch);
    GPtrArray *items = page_cells (p);
    for (guint i = 0; items && i < items->len; i++)
      draw_cell (p, i, items->pdata[i], true);
    drag_draw_cells (p);
  }
  gfx_restore ();
  draw_dots (q);
  for (guint i = 0; i < N.model->dock->len; i++)
    draw_cell (-1, i, N.model->dock->pdata[i], false);
  drag_draw_cells (-1);
  gfx_restore ();
}

/* ---- for a drag ----------------------------------------------------------------------------------------------- */

void
home_to_screen (float x, float y, float *sx, float *sy)
{
  float S, TY, P = N.H / 2.0f;
  spring (&S, &TY);
  *sx = N.W / 2.0f + (x - H.scroll - N.W / 2.0f) * S;
  *sy = P + (y - P) * S + TY;
}

void
home_from_screen (float sx, float sy, float *x, float *y)
{
  float S, TY, P = N.H / 2.0f;
  spring (&S, &TY);
  *x = N.W / 2.0f + (sx - N.W / 2.0f) / S + H.scroll;
  *y = P + (sy - TY - P) / S;
}

float
home_spring_scale (void)
{
  float S, TY;
  spring (&S, &TY);
  return S;
}

float
home_icon_size (void)
{
  return H.icon;
}

int
home_n_pages (void)
{
  return n_pages ();
}

int
home_page (void)
{
  return H.page;
}

bool
home_snapping (void)
{
  return H.snapping;
}

void
home_set_extra_page (bool on)
{
  H.extra = on;
  wl_schedule (N.wl);
}

int
home_page_at (float x)
{
  return (int) floorf (x / H.pitch);
}

void
home_card (int page, float r[4])
{
  card_rect (page, r);
}

int
home_cells (int page)
{
  return page < 0 ? N.model->dock_size : model_capacity (N.model, page);
}

void
home_cell_grid (int page, int index, int *col, int *row)
{
  int slot = page < 0 ? index : index + (page == 0 ? N.model->reserved : 0);
  *col = page < 0 ? slot : slot % N.model->cols;
  *row = page < 0 ? 0 : slot / N.model->cols;
}

void
home_cell_box (int page, int index, float r[4])
{
  int col, row;
  home_cell_grid (page, index, &col, &row);
  if (page < 0) {
    r[0] = H.cell_x0 + col * H.dock_w;
    r[1] = H.dock_top;
    r[2] = H.dock_w;
    r[3] = dp (HOTSEAT_CELL_DP);
    return;
  }
  r[0] = page * H.pitch + H.cell_x0 + col * H.cell_w;
  r[1] = H.cell_y0 + row * H.cell_h;
  r[2] = H.cell_w;
  r[3] = H.cell_h;
}

void
home_icon_pos (int page, int index, float *x, float *y)
{
  if (page < 0)
    dock_rect (index, x, y);
  else
    slot_rect (page, index, x, y);
}

int
home_cell_nearest (int page, float x, float y)
{
  /* on the first page the clock's row is never one: the row under it is nearest */
  int cols = N.model->cols, top = page == 0 ? N.model->reserved / cols : 0;
  int col = CLAMP ((int) floorf ((x - page * H.pitch - H.cell_x0) / H.cell_w), 0, cols - 1);
  int row = CLAMP ((int) floorf ((y - H.cell_y0) / H.cell_h), top, N.model->rows - 1);
  int slot = row * cols + col, reserved = page == 0 ? N.model->reserved : 0;
  return slot < reserved ? -1 : slot - reserved;
}

bool
home_in_dock (float sx, float sy)
{
  return sy >= H.dock_top && sy < N.H - dp (NAV_GESTURE_DP) && sx >= H.cell_x0 && sx < N.W - H.cell_x0;
}

int
home_dock_nearest (float sx)
{
  return CLAMP ((int) floorf ((sx - H.cell_x0) / H.dock_w), 0, N.model->dock_size - 1);
}

/* the drag is over: the extra page goes, and so does any page it emptied, the one on show staying on show */
void
home_tidy_pages (void)
{
  int before = 0;
  for (int p = 1; p < H.page && p < (int) N.model->pages->len; p++)
    before += ((GPtrArray *) N.model->pages->pdata[p])->len == 0;
  H.extra = false;
  model_tidy (N.model);
  H.page -= before;
  H.scroll -= before * H.pitch;
  H.snap_from -= before * H.pitch;
  H.snap_to -= before * H.pitch;
  home_model_changed ();
}

/* ---- paging --------------------------------------------------------------------------------------------------- */

static void
snap_to (int page, float velocity)
{
  page = CLAMP (page, 0, n_pages () - 1);
  float to = page * H.pitch, delta = to - H.scroll;
  if (page != H.page)
    nav_icons_changed ();
  H.page = page;
  if (fabsf (delta) < 0.5f) {
    H.scroll = to;
    H.snapping = false;
    return;
  }
  double ms = SNAP_MS;
  float v = fabsf (velocity);
  if (v >= dp (MIN_FLING_DP)) {
    /* snapToPageWithVelocity: the distance through a sine so a short swipe isn't slow */
    float r = MIN (1, fabsf (delta) / N.W);
    float dist = N.W / 2.0f + N.W / 2.0f * sinf ((r - 0.5f) * 0.3f * (float) G_PI / 2);
    ms = 4 * lroundf (1000 * dist / MAX (dp (MIN_SNAP_DP), v));
  }
  H.snapping = true;
  H.snap_t0 = N.now;
  H.snap_ms = ms;
  H.snap_from = H.scroll;
  H.snap_to = to;
  wl_schedule (N.wl);
}

bool
home_frame (void)
{
  bool more = false;
  if (H.snapping) {
    float t = CLAMP ((N.now - H.snap_t0) / H.snap_ms, 0, 1);
    H.scroll = H.snap_from + (H.snap_to - H.snap_from) * spongy_curve (SPONGY_DECELERATE_QUINT, t);
    H.snapping = t < 1;
    more |= H.snapping;
  }
  bool a = false, b = false;
  H.stretch = spongy_edge_frame (&H.left, N.now, false, &a) + spongy_edge_frame (&H.right, N.now, true, &b);
  return more || a || b || press_animating ();
}

/* the top and height of the rows [row, row + rows) the widgets of a page use, in the strip's coordinates */
float
home_row_top (int row, int rows, float *h)
{
  *h = rows * H.cell_h;
  return H.cell_y0 + row * H.cell_h;
}

/* a tap on the workspace at rest, on the first page: a widget's */
bool
home_widget_tap (float x, float y)
{
  if (H.page != 0 || H.snapping || fabsf (H.scroll) > 1 || drag_spring () > 0)
    return false;
  return widgets_tap (x, y);
}

float
home_stretch (void)
{
  return H.stretch;
}

bool
home_touch_down (float x, float y, double t)
{
  H.down = true;
  H.dragging = H.vertical = H.gave_up = false;
  H.down_x = H.last_x = x;
  H.down_y = y;
  spongy_velocity_clear (&H.vel);
  spongy_velocity_add (&H.vel, t, x, y);
  /* a finger stops a snap where it is, and catches a receding stretch */
  if (H.snapping) {
    H.snapping = false;
    H.dragging = true;
  }
  H.scroll_down = H.scroll;
  if (!spongy_edge_is_finished (&H.left))
    spongy_edge_pull_distance (&H.left, 0, t);
  if (!spongy_edge_is_finished (&H.right))
    spongy_edge_pull_distance (&H.right, 0, t);
  return true;
}

void
home_touch_move (float x, float y, double t, bool *vertical)
{
  *vertical = false;
  if (!H.down || H.gave_up)
    return;
  spongy_velocity_add (&H.vel, t, x, y);
  float dx = x - H.down_x, dy = y - H.down_y;
  if (!H.dragging) {
    float adx = fabsf (dx), ady = fabsf (dy);
    if (ady > dp (TOUCH_SLOP_DP) && ady > adx) {
      /* up: the drawer; down: the shade (the shell's) */
      H.gave_up = true;
      *vertical = true;
      return;
    }
    double angle = atan2 (ady, adx);
    if (angle > MAX_ANGLE)
      return;
    float slop = dp (PAGING_SLOP_DP);
    if (angle > DAMP_ANGLE)
      slop *= 1 + 4 * sqrtf ((angle - DAMP_ANGLE) / DAMP_ANGLE);
    if (adx <= slop)
      return;
    H.dragging = true;
    H.last_x = x;
    press_up ();
    return;
  }
  float delta = H.last_x - x;
  H.last_x = x;
  float max = (n_pages () - 1) * H.pitch;
  float next = H.scroll + delta;
  /* a stretch showing shrinks first when the finger goes back */
  if (!spongy_edge_is_finished (&H.left) && delta > 0) {
    float took = spongy_edge_pull_distance (&H.left, -delta / N.W, t);
    next = H.scroll + delta + took * N.W;
  } else if (!spongy_edge_is_finished (&H.right) && delta < 0) {
    float took = spongy_edge_pull_distance (&H.right, delta / N.W, t);
    next = H.scroll + delta - took * N.W;
  }
  if (next < 0) {
    spongy_edge_pull_distance (&H.left, -next / N.W, t);
    next = 0;
  } else if (next > max) {
    spongy_edge_pull_distance (&H.right, (next - max) / N.W, t);
    next = max;
  }
  H.scroll = next;
  wl_schedule (N.wl);
}

void
home_touch_up (float x, float y, double t)
{
  if (!H.down)
    return;
  H.down = false;
  spongy_edge_release (&H.left, t);
  spongy_edge_release (&H.right, t);
  if (!H.dragging) {
    if (H.scroll != H.page * H.pitch)
      snap_to (lroundf (H.scroll / H.pitch), 0);
    return;
  }
  H.dragging = false;
  float vx, vy;
  spongy_velocity_get (&H.vel, t, &vx, &vy);
  vx = CLAMP (vx, -dp (8000), dp (8000));
  float delta = x - H.down_x;
  int page = (int) floorf (H.scroll_down / H.pitch + 0.5f);
  bool fling = fabsf (delta) > dp (PAGING_SLOP_DP) && fabsf (vx) > dp (FLING_PX_S_DP);
  bool significant = fabsf (delta) > N.W * SIGNIFICANT_MOVE;
  if ((significant && !fling && delta > 0) || (fling && vx > 0))
    snap_to (page - 1, vx);
  else if ((significant && !fling && delta < 0) || (fling && vx < 0))
    snap_to (page + 1, vx);
  else
    snap_to (lroundf (H.scroll / H.pitch), 0);
  wl_schedule (N.wl);
}

void
home_touch_cancel (double t)
{
  H.down = false;
  H.dragging = false;
  spongy_edge_release (&H.left, t);
  spongy_edge_release (&H.right, t);
  snap_to (lroundf (H.scroll / H.pitch), 0);
}

/* a drag waiting at a neighbouring page: there, on the scroller's 750 ms (or `ms`) */
void
home_snap (int page, double ms)
{
  snap_to (page, 0);
  if (H.snapping && ms > 0)
    H.snap_ms = ms;
}
