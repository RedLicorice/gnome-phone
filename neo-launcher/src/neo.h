/* Neo Launcher 2: what the home screen, the dock and the app drawer share. Everything is laid out in device pixels
 * from Android's dp values (Launcher3 on the S10e: 5 × 5 "Large Phone" grid, 56 dp icons, 1 dp = 3 px there). The
 * grid, the icons' size, the home labels and the drawer's search pill are settings (de.yesman.neo; main.c). */
#pragma once

#include <gio/gdesktopappinfo.h>
#include <stdbool.h>

#include "gfx.h"
#include "model.h"
#include "spongy.h"
#include "text.h"
#include "wl.h"

/* ---- geometry (dp unless named _PX) ----------------------------------------------------------------------------- */

#define STATUS_BAR_DP            (116 / 3.0)   /* the shell's status bar: the cutout sets it, not the 24 dp default */
#define NAV_GESTURE_DP           32            /* the bottom strip the shell's swipe up starts in */
#define DOCK_SLOTS               5             /* the dock's cells, whatever the pages' grid */
#define ICON_DP                  64            /* × desktop-icon-scale / drawer-icon-scale */
#define WS_SIDE_DP               8             /* cell box x: 8 dp in from each side */
#define WS_TOP_DP                (32 / 3.0)    /* cell box y: below the status bar */
#define WS_CONTENT_DP            85            /* icon + gap + label box: (cell − this)/2 is the icon's top in a cell */
#define LABEL_BASELINE_DP        74.5          /* below the icon's top */
#define LABEL_SP                 12
/* the labels' room in a column of the 5-column grid on a 360 dp phone; another grid's columns add or take what their
 * width differs by (home_label_width, drawer_label_width) */
#define LABEL_HOME_MAX_DP        52            /* "Play St…" fits, "Play Sto…" does not */
#define LABEL_DRAWER_MAX_DP      54.6          /* "Calculat…" fits, "Messagi…" does not */
#define LABEL_REF_COLS           5
#define HOTSEAT_DP               104           /* the dock: icon + 48 dp below */
#define HOTSEAT_CELL_DP          63
#define PAGE_IND_DP              24
#define PAGE_DOT_DP              6
#define PAGE_DOT_GAP_DP          4

#define WALLPAPER_ZOOM           1.10f         /* the wallpaper at home, about the screen centre */
#define CONTENT_SCALE            0.97f         /* workspace + dock behind the drawer and behind a launch */

/* the drawer sheet, its handle, search pill and list (px from the sheet's top, measured on the S10e, as dp) */
#define SHEET_RADIUS_DP          28
#define HANDLE_W_DP              32
#define HANDLE_H_DP              4
#define HANDLE_TOP_DP            16
#define SEARCH_TOP_DP            34            /* 8 dp margin + 26 dp overlap */
#define SEARCH_H_DP              52
#define SEARCH_SIDE_DP           4.5
#define SEARCH_TEXT_SP           16            /* the hint and the query: AOSP's search_container_all_apps textSize */
#define LIST_TOP_DP              90            /* the list's window starts here (y 386 on the S10e) */
                                               /* … or at SEARCH_TOP_DP without the search pill */
#define LIST_ICON_TOP_DP         (77 / 3.0)    /* first icon below the list's top */
#define LIST_ROW_DP              120           /* 104 dp cell + 16 dp border space */
#define LIST_LABEL_BASELINE_DP   (231.5 / 3.0)
#define FASTSCROLL_W_DP          (20 / 3.0)
#define FASTSCROLL_H_DP          52
#define FASTSCROLL_END_DP        (1 / 3.0)
#define FASTSCROLL_TOP_DP        (37 / 3.0)    /* thumb at rest, below the list's top */

typedef struct {
  char        *id;
  GAppInfo    *info;
  GfxTex      *icon;              /* NULL until the icon thread drew it */
  TextLayout  *label;             /* the home label (52 dp) */
  TextLayout  *dlabel;            /* the drawer label (54.6 dp) */
  bool         hidden;            /* its window is growing out of it */
  char        *splash_icon;       /* the icon drawn at the splash's 160 dp (a cached PNG), or NULL: the theme's */
} Tile;

/* the icon pressed under a finger: 1 → 1.1 in 200 ms (t²) on down, back with 1 − (1 − t)² */
typedef struct {
  Tile   *tile;
  double  t0;
  float   from, to;
  bool    up;
} Press;

typedef struct {
  Wl          *wl;
  int          W, H;
  double       dp;
  double       now;               /* ms, the frame being drawn */
  GHashTable  *tiles;             /* id -> Tile* */
  GPtrArray   *sorted;            /* Tile*, the drawer's A→Z order */
  Model       *model;               /* the home pages' grid is the model's (cols × rows) */
  GSettings   *settings;
  /* the rest of Settings' Home screen layout (main.c layout_read) */
  int          drawer_cols;
  float        icon_dp, drawer_icon_dp;   /* ICON_DP × the scale keys; home.c and drawer.c fit them to their cells */
  bool         day_ink;           /* a light Renaissance wallpaper (marble, sea): dark labels and clock */
  bool         labels;            /* the app names under the home screen's icons */
  bool         search;            /* the drawer's search pill */
  TextFont    *label_font;
  Press        press;
  GDBusConnection *bus;

  /* launch: the content shrinks to 0.97 while the window grows out of an icon */
  double       launch_t0;         /* 0: none */
  bool         launch_from_drawer;
} Neo;

extern Neo N;

static inline float dp (double v) { return (float) (v * N.dp); }

/* tiles.c */
void   tiles_load        (int cols, int rows);         /* the apps, their labels, the home layout, the icon thread */
bool   tiles_sync        (void);                       /* the apps again: installed, removed, changed */
void   tiles_reload_icons (void);                      /* every icon again, for new icon settings */
void   tiles_relabel     (void);                       /* every label again, for another grid */
int    tiles_icon_px     (void);                       /* the size the icons are drawn at */
Tile  *tiles_get         (const char *id);
GHashTable *tiles_apps   (void);                       /* id -> GAppInfo, as model_reconcile takes them */
void   tile_draw_icon    (Tile *t, float x, float y, float size, float alpha);
float  press_scale       (Tile *t);                    /* this tile's press scale now */
bool   press_animating   (void);
void   press_down        (Tile *t);
void   press_up          (void);
void   tile_launch       (Tile *t, float x, float y, float size, bool from_drawer);
float  launch_content_scale (void);                    /* the home content's scale while a launch runs */
void   label_draw        (TextLayout *l, float x, float y, GfxColor c, bool shadow);
void   haptic            (const char *event);              /* feedbackd: HAPTIC_LONG_PRESS, HAPTIC_TICK */
#define HAPTIC_LONG_PRESS "window-close"
#define HAPTIC_TICK       "button-released"

/* home.c */
void   home_layout       (void);
void   home_draw         (float content_scale);          /* pages + clock + dock + page dots */
bool   home_frame        (void);                         /* animations; true while moving */
bool   home_touch_down   (float x, float y, double t);
void   home_touch_move   (float x, float y, double t, bool *vertical);
void   home_touch_up     (float x, float y, double t);
void   home_touch_cancel (double t);
float  home_stretch      (void);                         /* the horizontal edge stretch amount */
Tile  *home_tile_at      (float x, float y, float *ix, float *iy, ModelPos *pos);
void   home_model_changed (void);                        /* icons added or taken away: the pages reflowed */
void   home_clock_tick   (void);
void   home_clock_draw   (float x, float y, float w, float h);   /* the clock widget, in its box */
float  home_row_top      (int row, int rows, float *h);          /* the widgets' rows: top and height */
bool   home_widget_tap   (float x, float y);                     /* a tap on a widget: it took it */
bool   home_icon_rect    (const char *id, float r[4]);
/* for a drag. "page" -1 is the dock; a page's cells are in the strip's coordinates (page p from x = p·pitch, before
 * the scroll and the spring-loaded shrink), the dock's on the screen; a page's index is the model's (page 0's start
 * after the clock row) */
void   home_to_screen    (float x, float y, float *sx, float *sy);
void   home_from_screen  (float sx, float sy, float *x, float *y);
float  home_spring_scale (void);
float  home_icon_size    (void);                         /* px, the pages' and the dock's icons */
float  home_label_width  (void);                         /* px */
int    home_n_pages      (void);                         /* with the extra page */
int    home_page         (void);
bool   home_snapping     (void);
void   home_snap         (int page, double ms);          /* ms 0: the scroller's 750 */
void   home_set_extra_page (bool on);
int    home_page_at      (float x);
void   home_card         (int page, float r[4]);         /* the page's CellLayout bounds */
int    home_cells        (int page);
void   home_cell_grid    (int page, int index, int *col, int *row);
void   home_cell_box     (int page, int index, float r[4]);
void   home_icon_pos     (int page, int index, float *x, float *y);
int    home_cell_nearest (int page, float x, float y);   /* never the clock's row; -1 only past it */
bool   home_in_dock      (float sx, float sy);
int    home_dock_nearest (float sx);
void   home_tidy_pages   (void);

/* nav.c: gesture navigation, the launcher's half (the shell drives it over D-Bus) */
void   nav_init          (void);
bool   nav_frame         (void);                         /* true while the home reveal runs */
float  nav_depth         (void);                         /* the depth an app over the launcher puts on it */
bool   nav_blurs         (void);                         /* that depth blurs (not during the reveal) */
float  nav_home_scale    (void);                         /* the workspace and dock's scale and alpha */
float  nav_home_alpha    (void);
float  nav_home_ty       (void);                         /* … and how far down it is moved */
float  nav_scrim_alpha   (void);                         /* back's scrim over it all: black at this alpha */
float  nav_scrim_blur    (void);                         /* … over a blur of this radius (px) */
void   nav_drawer        (bool open);                    /* tell the shell the drawer opened or closed */
void   nav_icons_changed (void);                         /* the page or the dock's icons moved */
void   nav_shown         (guint64 frame);                /* that frame is on screen (wl's shown) */

/* drawer.c */
void   drawer_layout     (void);
float  drawer_progress   (void);                         /* f: 0 home, 1 all apps */
bool   drawer_frame      (void);
void   drawer_draw       (void);
bool   drawer_is_open    (void);
/* a drag handed over from home (finger moved up past the slop); y_down is where it went down */
void   drawer_drag_begin (float y_down, float y, double t);
bool   drawer_touch_down (float x, float y, double t);       /* false: not the drawer's */
void   drawer_touch_move (float x, float y, double t);
void   drawer_touch_up   (float x, float y, double t);
void   drawer_touch_cancel (double t);
void   drawer_close_now  (void);
void   drawer_back       (void);                         /* close on back: 200 ms ACCELERATE */
void   drawer_open       (void);                         /* open it as a settle from where it is */
void   drawer_close_for_drag (void);
/* the tile under a finger held still on the list (a long press), and its icon box; NULL when it scrolled */
Tile  *drawer_hold_tile  (float x, float y, float *ix, float *iy);
bool   drawer_dragging   (void);
float  drawer_icon_size  (void);                         /* px */
float  drawer_label_width (void);                        /* px */
/* the search pill (Launcher3's all-apps search): text typed into it from the on-screen keyboard (text-input-v3's
 * commit and delete, in bytes) or a hardware keyboard (a key with its xkb keysym and text) */
bool   drawer_search_key (guint32 keysym, const char *utf8);   /* true: the field took the key */
void   drawer_search_text (const char *utf8);
void   drawer_search_erase (int before, int after);
void   drawer_search_submit (void);                       /* Enter: the first app found opens */
GfxTex *drawer_search_icon (void);                         /* the magnifier, white, for omni.c */
bool   drawer_search_back (void);                         /* back leaves a search first: true when it did */
/* the swipe up from the bar while the drawer is open: home (false from down: not the drawer's) */
bool   drawer_home_down  (float x, float y, double t);
void   drawer_home_move  (float x, float y, double t);
bool   drawer_home_up    (float x, float y, double t);   /* true: it goes home */
void   drawer_home_cancel (double t);

/* main.c: the shell took the finger (a back swipe began over the launcher): drop it, as on a touch cancel */
void   touch_drop        (void);

/* popup.c: the long-press menus (an icon's, the wallpaper's options) */
void   popup_open_icon   (Tile *t, ModelPos pos, bool from_drawer, float ix, float iy, float isz);
void   popup_open_options (float x, float y);
bool   popup_is_open     (void);                         /* open, not closing: a touch goes to it */
bool   popup_showing     (void);                         /* open or closing: its icon's tile is in use */
void   popup_close       (void);
void   popup_close_now   (void);
bool   popup_frame       (void);
void   popup_draw        (void);
void   popup_touch_down  (float x, float y);
void   popup_touch_move  (float x, float y);
void   popup_touch_up    (float x, float y);
void   popup_touch_cancel (void);
GfxTex *svg_texture      (const char *svg, int px);
void   home_remove_icon  (const char *id, ModelPos pos);   /* Remove: this copy, or the app off the home screen */
void   home_unhide       (const char *id);

/* drag.c: moving an icon (Launcher3's drag and drop) */
void   drag_hold         (Tile *t, ModelPos from, bool from_drawer, float ix, float iy, float down_x, float down_y);
bool   drag_holding      (void);                         /* a held icon, or a drag: the finger is the drag's */
void   drag_move         (float x, float y);
void   drag_up           (float x, float y);
void   drag_cancel       (bool now);                     /* now: no 500 ms before the workspace comes back */
bool   drag_busy         (void);                         /* landing or the workspace coming back: touches wait */
void   drag_hurry        (void);
bool   drag_frame        (void);
float  drag_spring       (void);                         /* 0 normal .. 1 SPRING_LOADED, eased */
int    drag_over_page    (void);                         /* the page under the drag, or -1 */
bool   drag_hides        (int page, int index);
void   drag_offset       (int page, int index, float *dx, float *dy);
void   drag_draw_cells   (int page);                     /* the drop preview, in the page's coordinates */
void   drag_draw_scrim   (void);                         /* over the wallpaper, under the workspace */
void   drag_draw         (void);                         /* the drop bar and the drag view */
Tile  *drag_drawer_tile  (void);                         /* the drawer's icon being held or dragged */
