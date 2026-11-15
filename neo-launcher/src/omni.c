/* Omnisearch (omni.h). The view is one scrolling list of rows built from the groups the search client reports (they
 * arrive per provider, in the sort order the client gives), the matching apps first and a "Chiedi a Minerva" row
 * last. The list is libspongy's, as the drawer's. */
#include "omni.h"

#include <math.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "image.h"
#include "neo.h"
#include "search.h"

#define FIELD_SIDE_DP    16
#define FIELD_H_DP       52
#define FIELD_TOP_GAP_DP 8
#define TEXT_SP          16
#define SMALL_SP         13
#define HEADER_SP        12
#define ROW_SIDE_DP      12
#define ROW_H_DP         64
#define ROW_GAP_DP       4
#define HEADER_H_DP      36
#define MORE_H_DP        44
#define ICON_DP_         40
#define MAX_APPS         4
#define OPEN_MS          200.0
#define CLOSE_MS         160.0
#define CARET_BLINK_MS   500
#define CARET_DP         2
#define ASK_GROUP        "org.minerva.Search.Ask"   /* the provider whose one result the last row activates */

typedef struct {
  char       *id, *name, *desc;
  GfxTex     *icon;
  TextLayout *name_l, *desc_l;
} OHit;

typedef struct {
  char       *id, *title;
  GPtrArray  *hits;              /* OHit* */
  guint       n_total, order;
  TextLayout *title_l, *more_l;
} OGroup;

typedef enum { ROW_HEADER, ROW_APP, ROW_HIT, ROW_MORE, ROW_ASK } RowKind;

typedef struct {
  RowKind  kind;
  OGroup  *g;
  OHit    *h;
  Tile    *t;
  TextLayout *l;         /* an app's name */
  float    y, h_px;              /* from the list's top */
} Row;

static struct {
  bool        open, closing, built;
  double      t0;
  float       f;                 /* 0 hidden .. 1 shown */
  GString    *query;
  TextLayout *query_l, *hint_l, *apps_l, *ask_l, *none_l;
  bool        focused, caret_on;
  guint       caret_id;
  TextFont   *font, *small, *head;
  Search     *search;
  GPtrArray  *groups;            /* OGroup*, by order */
  GPtrArray  *apps;              /* Tile*, the apps matching */
  GPtrArray  *app_names;         /* their names' TextLayout*, parallel */
  char       *ask_hit;           /* the Minerva provider's result id for this text */
  GArray     *rows;              /* Row */
  SpongyList  list;
  float       list_top, list_h;
  bool        down, moved, decided;
  float       down_x, down_y;
} O;

/* ---- helpers ---------------------------------------------------------------------------------------------------- */

static void
hit_free (OHit *h)
{
  g_free (h->id);
  g_free (h->name);
  g_free (h->desc);
  g_clear_pointer (&h->name_l, text_layout_free);
  g_clear_pointer (&h->desc_l, text_layout_free);
  if (h->icon)
    gfx_tex_free (h->icon);
  g_free (h);
}

static void
group_free (OGroup *g)
{
  g_free (g->id);
  g_free (g->title);
  g_ptr_array_unref (g->hits);
  g_clear_pointer (&g->title_l, text_layout_free);
  g_clear_pointer (&g->more_l, text_layout_free);
  g_free (g);
}

static GfxTex *
icon_tex (const char *icon)
{
  if (!icon || !*icon)
    return NULL;
  g_autoptr (GIcon) gi = g_icon_new_for_string (icon, NULL);
  g_autoptr (GdkPixbuf) pb = gi ? image_icon (gi, (int) dp (ICON_DP_)) : NULL;
  if (!pb)
    return NULL;
  /* a symbolic icon is a dark glyph for a light panel: the view is dark, so its glyph takes the text colour */
  if (strstr (icon, "symbolic") && gdk_pixbuf_get_has_alpha (pb)) {
    g_autoptr (GdkPixbuf) copy = gdk_pixbuf_copy (pb);
    guchar *px = gdk_pixbuf_get_pixels (copy);
    int w = gdk_pixbuf_get_width (copy), h = gdk_pixbuf_get_height (copy), rs = gdk_pixbuf_get_rowstride (copy);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        guchar *p = px + y * rs + x * 4;
        p[0] = OMNI_TEXT_RGB >> 16 & 0xff;
        p[1] = OMNI_TEXT_RGB >> 8 & 0xff;
        p[2] = OMNI_TEXT_RGB & 0xff;
      }
    return image_texture (copy);
  }
  return image_texture (pb);
}

static void
tinted (GfxTex *tex, float x, float y, float w, float h, GfxColor c)
{
  GfxInst *q = gfx_push (tex, GFX_PIPE_QUAD);
  float a = q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u1 = q->v1 = 1;
  q->r = c.r * c.a * a; q->g = c.g * c.a * a; q->b = c.b * c.a * a; q->a = c.a * a;
  q->mode = GFX_MODE_TEX;
}

static void
fonts (void)
{
  if (O.font)
    return;
  O.font = text_font_axes ("Google Sans Flex", 500, dp (TEXT_SP), "opsz=16");
  O.small = text_font_axes ("Google Sans Flex", 400, dp (SMALL_SP), "opsz=13");
  O.head = text_font_axes ("Google Sans Flex", 700, dp (HEADER_SP), "opsz=12");
  O.query = g_string_new (NULL);
  O.groups = g_ptr_array_new_with_free_func ((GDestroyNotify) group_free);
  O.apps = g_ptr_array_new ();
  O.app_names = g_ptr_array_new_with_free_func ((GDestroyNotify) text_layout_free);
  O.rows = g_array_new (FALSE, TRUE, sizeof (Row));
}

static float
text_room (void)
{
  return N.W - 2 * dp (ROW_SIDE_DP + 12) - dp (ICON_DP_ + 14);
}

static void
field_rect (float r[4])
{
  r[0] = dp (FIELD_SIDE_DP);
  r[1] = dp (STATUS_BAR_DP + FIELD_TOP_GAP_DP);
  r[2] = N.W - 2 * r[0];
  r[3] = dp (FIELD_H_DP);
}

/* the field: the magnifier, the hint or the text with the caret */
static void
draw_field (float x, float y, float w, float h, TextLayout *text, bool caret, float alpha)
{
  gfx_rect (x, y, w, h, h / 2, GFX_RGBA (OMNI_FIELD_RGB, OMNI_FIELD_ALPHA * alpha));
  float isz = dp (24);
  GfxTex *icon = drawer_search_icon ();
  if (icon)
    tinted (icon, x + dp (14), roundf (y + (h - isz) / 2), isz, isz, GFX_RGBA (OMNI_HINT_RGB, alpha));
  float tx = roundf (x + dp (14 + 24 + 10)), lh = text_line_height (O.font);
  float ty = roundf (y + (h - lh) / 2), room = x + w - dp (20) - tx;
  float tw = text ? text_layout_width (text) : 0;
  float shift = MAX (0, tw + dp (CARET_DP) - room);
  gfx_save ();
  gfx_clip (tx, y, tx + room, y + h);
  text_draw (text ? text : O.hint_l, tx - shift, ty, GFX_RGBA (text ? OMNI_TEXT_RGB : OMNI_HINT_RGB, 1));
  gfx_restore ();
  if (caret)
    gfx_rect (roundf (tx + tw - shift), ty, MAX (1, roundf (dp (CARET_DP))), roundf (lh), 0,
              GFX_RGBA (OMNI_ACCENT_RGB, 1));
}

/* ---- the bar widget ---------------------------------------------------------------------------------------------- */

static void
bar_rect (float top, float h, float r[4])
{
  r[0] = dp (FIELD_SIDE_DP);
  r[2] = N.W - 2 * r[0];
  r[3] = dp (FIELD_H_DP);
  r[1] = top + roundf ((h - r[3]) / 2) - dp (10);   /* a little up: it belongs to the clock above */
}

void
omni_bar_draw (float x, float y, float w, float h)
{
  fonts ();
  if (!O.hint_l)
    O.hint_l = text_layout (O.font, "Cerca o chiedi a Minerva", 0, 1, TEXT_LEFT);
  float r[4];
  bar_rect (y, h, r);
  draw_field (x + r[0], r[1], r[2], r[3], NULL, false, 1);
}

bool
omni_bar_tap (float x, float y, float top, float h)
{
  float r[4];
  bar_rect (top, h, r);
  if (x < r[0] || x >= r[0] + r[2] || y < r[1] || y >= r[1] + r[3])
    return false;
  omni_open ();
  return true;
}

/* ---- rows --------------------------------------------------------------------------------------------------------- */

static void
clear_layouts (void)
{
  for (guint g = 0; g < O.groups->len; g++) {
    OGroup *grp = O.groups->pdata[g];
    g_clear_pointer (&grp->title_l, text_layout_free);
    g_clear_pointer (&grp->more_l, text_layout_free);
    for (guint i = 0; i < grp->hits->len; i++) {
      OHit *h = grp->hits->pdata[i];
      g_clear_pointer (&h->name_l, text_layout_free);
      g_clear_pointer (&h->desc_l, text_layout_free);
    }
  }
  g_ptr_array_set_size (O.app_names, 0);
  g_clear_pointer (&O.apps_l, text_layout_free);
  g_clear_pointer (&O.ask_l, text_layout_free);
  g_clear_pointer (&O.none_l, text_layout_free);
  g_clear_pointer (&O.query_l, text_layout_free);
  if (O.query && O.query->len)
    O.query_l = text_layout (O.font, O.query->str, 0, 1, TEXT_LEFT);
}

static void
row_add (RowKind kind, OGroup *g, OHit *h, Tile *t, float *y, float h_px, float gap)
{
  Row r = { kind, g, h, t, NULL, *y, h_px };
  g_array_append_val (O.rows, r);
  *y += h_px + gap;
}

static void
hit_layouts (OHit *h)
{
  if (!h->name_l)
    h->name_l = text_layout (O.font, h->name, text_room (), 1, TEXT_LEFT);
  if (!h->desc_l && h->desc && *h->desc)
    h->desc_l = text_layout (O.small, h->desc, text_room (), 1, TEXT_LEFT);
}

static void
rows_rebuild (void)
{
  g_array_set_size (O.rows, 0);
  float y = dp (4);
  if (O.apps->len) {
    if (!O.apps_l)
      O.apps_l = text_layout (O.head, "APP", 0, 1, TEXT_LEFT);
    row_add (ROW_HEADER, NULL, NULL, NULL, &y, dp (HEADER_H_DP), 0);
    for (guint i = 0; i < O.apps->len; i++) {
      if (O.app_names->len <= i) {
        Tile *t = O.apps->pdata[i];
        g_ptr_array_add (O.app_names, text_layout (O.font, g_app_info_get_name (t->info), text_room (), 1, TEXT_LEFT));
      }
      row_add (ROW_APP, NULL, NULL, O.apps->pdata[i], &y, dp (ROW_H_DP), dp (ROW_GAP_DP));
      g_array_index (O.rows, Row, O.rows->len - 1).l = O.app_names->pdata[i];
    }
  }
  for (guint g = 0; g < O.groups->len; g++) {
    OGroup *grp = O.groups->pdata[g];
    if (!grp->title_l) {
      g_autofree char *up = g_utf8_strup (grp->title, -1);
      grp->title_l = text_layout (O.head, up, 0, 1, TEXT_LEFT);
    }
    row_add (ROW_HEADER, grp, NULL, NULL, &y, dp (HEADER_H_DP), 0);
    for (guint i = 0; i < grp->hits->len; i++) {
      hit_layouts (grp->hits->pdata[i]);
      row_add (ROW_HIT, grp, grp->hits->pdata[i], NULL, &y, dp (ROW_H_DP), dp (ROW_GAP_DP));
    }
    if (grp->n_total > grp->hits->len) {
      if (!grp->more_l) {
        g_autofree char *s = g_strdup_printf ("Mostra tutti (%u)", grp->n_total);
        grp->more_l = text_layout (O.small, s, 0, 1, TEXT_LEFT);
      }
      row_add (ROW_MORE, grp, NULL, NULL, &y, dp (MORE_H_DP), dp (ROW_GAP_DP));
    }
  }
  if (O.query->len) {
    if (!O.ask_l) {
      g_autofree char *s = g_strdup_printf ("Chiedi a Minerva: «%s»", O.query->str);
      O.ask_l = text_layout (O.font, s, text_room (), 1, TEXT_LEFT);
    }
    y += dp (8);
    row_add (ROW_ASK, NULL, NULL, NULL, &y, dp (ROW_H_DP), 0);
  }
  float content = y + dp (24);
  if (O.list.view_height == 0)
    spongy_list_init (&O.list, O.list_h, content);
  else
    spongy_list_set_size (&O.list, O.list_h, content);
  wl_schedule (N.wl);
}

/* ---- the results ------------------------------------------------------------------------------------------------- */

static void
apps_match (void)
{
  g_ptr_array_set_size (O.apps, 0);
  g_ptr_array_set_size (O.app_names, 0);
  if (!O.query->len)
    return;
  for (guint i = 0; i < N.sorted->len && O.apps->len < MAX_APPS; i++) {
    Tile *t = N.sorted->pdata[i];
    if (g_str_match_string (O.query->str, g_app_info_get_name (t->info), TRUE))
      g_ptr_array_add (O.apps, t);
  }
}

static int
group_find (const char *id)
{
  for (guint i = 0; i < O.groups->len; i++)
    if (!g_strcmp0 (((OGroup *) O.groups->pdata[i])->id, id))
      return (int) i;
  return -1;
}

static void
on_group (Search *s, guint generation, const SearchGroup *grp, gboolean last, gpointer data)
{
  if (generation != search_generation (s) && grp)
    return;
  if (!grp) {
    g_ptr_array_set_size (O.groups, 0);
    g_clear_pointer (&O.ask_hit, g_free);
    rows_rebuild ();
    return;
  }
  if (!g_strcmp0 (grp->id, ASK_GROUP)) {
    g_free (O.ask_hit);
    O.ask_hit = grp->hits->len ? g_strdup (((SearchHit *) grp->hits->pdata[0])->id) : NULL;
    return;
  }
  int at = group_find (grp->id);
  if (at >= 0)
    g_ptr_array_remove_index (O.groups, at);
  if (grp->hits->len) {
    OGroup *g = g_new0 (OGroup, 1);
    g->id = g_strdup (grp->id);
    g->title = g_strdup (grp->title);
    g->n_total = grp->n_total;
    g->order = grp->order;
    g->hits = g_ptr_array_new_with_free_func ((GDestroyNotify) hit_free);
    for (guint i = 0; i < grp->hits->len; i++) {
      SearchHit *sh = grp->hits->pdata[i];
      OHit *h = g_new0 (OHit, 1);
      h->id = g_strdup (sh->id);
      h->name = g_strdup (sh->name);
      h->desc = g_strdup (sh->description);
      h->icon = icon_tex (sh->icon ?: grp->icon);
      g_ptr_array_add (g->hits, h);
    }
    guint pos = 0;
    while (pos < O.groups->len && ((OGroup *) O.groups->pdata[pos])->order < g->order)
      pos++;
    g_ptr_array_insert (O.groups, pos, g);
  }
  rows_rebuild ();
}

static void
query_changed (void)
{
  clear_layouts ();
  apps_match ();
  if (!O.query->len) {
    g_ptr_array_set_size (O.groups, 0);
    g_clear_pointer (&O.ask_hit, g_free);
  }
  rows_rebuild ();
  if (O.search)
    search_query (O.search, O.query->str);
  wl_schedule (N.wl);
}

/* ---- the field's typing ------------------------------------------------------------------------------------------ */

static void
ime_sync (void)
{
  float r[4];
  field_rect (r);
  wl_text_input (N.wl, O.focused && O.open && !O.closing, O.query->str, (int) O.query->len, r[0], r[1], r[2], r[3]);
}

static gboolean
caret_blink (gpointer data)
{
  O.caret_on = !O.caret_on;
  wl_schedule (N.wl);
  return G_SOURCE_CONTINUE;
}

static void
caret_restart (void)
{
  g_clear_handle_id (&O.caret_id, g_source_remove);
  O.caret_on = O.focused;
  if (O.focused)
    O.caret_id = g_timeout_add (CARET_BLINK_MS, caret_blink, NULL);
}

static void
focus (void)
{
  O.focused = true;
  caret_restart ();
  ime_sync ();
  wl_text_input_show (N.wl);
  wl_schedule (N.wl);
}

static void
blur (void)
{
  if (!O.focused)
    return;
  O.focused = false;
  caret_restart ();
  ime_sync ();
  wl_schedule (N.wl);
}

void
omni_text (const char *utf8)
{
  if (!O.open || !O.focused || !utf8 || !g_utf8_validate (utf8, -1, NULL))
    return;
  for (const char *p = utf8; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (!g_unichar_iscntrl (c))
      g_string_append_unichar (O.query, c);
  }
  query_changed ();
  ime_sync ();
}

void
omni_erase (int before, int after)
{
  if (!O.open || !O.focused || before <= 0)
    return;
  gsize cut = (gsize) before >= O.query->len ? 0 : O.query->len - before;
  while (cut > 0 && (O.query->str[cut] & 0xC0) == 0x80)
    cut--;
  g_string_truncate (O.query, cut);
  query_changed ();
  ime_sync ();
}

void
omni_submit (void)
{
  if (!O.open || !O.focused)
    return;
  if (O.search)
    search_query_now (O.search, O.query->str);
  blur ();
}

bool
omni_key (unsigned keysym, const char *utf8)
{
  if (!O.open || !O.focused)
    return false;
  switch (keysym) {
  case XKB_KEY_BackSpace:
    if (O.query->len) {
      const char *prev = g_utf8_find_prev_char (O.query->str, O.query->str + O.query->len);
      g_string_truncate (O.query, prev ? (gsize) (prev - O.query->str) : 0);
      query_changed ();
      ime_sync ();
    }
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    omni_submit ();
    return true;
  case XKB_KEY_Escape:
    omni_close ();
    return true;
  default:
    break;
  }
  if (!utf8 || !*utf8 || !g_utf8_validate (utf8, -1, NULL) || g_unichar_iscntrl (g_utf8_get_char (utf8)))
    return false;
  omni_text (utf8);
  return true;
}

/* ---- open and close ---------------------------------------------------------------------------------------------- */

void
omni_layout (void)
{
  if (!O.font)
    return;
  float r[4];
  field_rect (r);
  O.list_top = r[1] + r[3] + dp (8);
  O.list_h = N.H - O.list_top;
  g_clear_pointer (&O.hint_l, text_layout_free);
  clear_layouts ();
  if (O.list.view_height != 0)
    rows_rebuild ();
}

void
omni_open (void)
{
  fonts ();
  if (!O.search)
    O.search = search_new (on_group, NULL);
  if (!O.hint_l)
    O.hint_l = text_layout (O.font, "Cerca o chiedi a Minerva", 0, 1, TEXT_LEFT);
  omni_layout ();
  g_string_truncate (O.query, 0);
  g_ptr_array_set_size (O.groups, 0);
  g_ptr_array_set_size (O.apps, 0);
  g_clear_pointer (&O.ask_hit, g_free);
  clear_layouts ();
  rows_rebuild ();
  spongy_list_scroll_to (&O.list, 0);
  O.open = true;
  O.closing = false;
  O.t0 = N.now;
  focus ();
  wl_schedule (N.wl);
}

void
omni_close (void)
{
  if (!O.open || O.closing)
    return;
  O.closing = true;
  O.t0 = N.now;
  blur ();
  wl_schedule (N.wl);
}

void
omni_close_now (void)
{
  if (!O.open)
    return;
  O.open = O.closing = false;
  O.f = 0;
  blur ();
  if (O.search)
    search_query (O.search, "");
  wl_schedule (N.wl);
}

bool
omni_is_open (void)
{
  return O.open;
}

bool
omni_back (void)
{
  if (!O.open)
    return false;
  omni_close ();
  return true;
}

bool
omni_frame (void)
{
  if (!O.open)
    return false;
  float stretch_t, stretch_b;
  bool more = spongy_list_frame (&O.list, N.now, &stretch_t, &stretch_b);
  if (O.closing) {
    float p = CLAMP ((N.now - O.t0) / CLOSE_MS, 0, 1);
    O.f = 1 - p * p;
    if (p >= 1) {
      omni_close_now ();
      return true;
    }
    return true;
  }
  float p = CLAMP ((N.now - O.t0) / OPEN_MS, 0, 1);
  O.f = 1 - (1 - p) * (1 - p);
  return more || p < 1;
}

/* ---- touch ------------------------------------------------------------------------------------------------------- */

static int
row_at (float y)
{
  float ly = y - O.list_top + roundf (O.list.offset);
  for (guint i = 0; i < O.rows->len; i++) {
    Row *r = &g_array_index (O.rows, Row, i);
    if (ly >= r->y && ly < r->y + r->h_px)
      return (int) i;
  }
  return -1;
}

bool
omni_touch_down (float x, float y, double t)
{
  if (!O.open)
    return false;
  O.down = true;
  O.moved = O.decided = false;
  O.down_x = x;
  O.down_y = y;
  if (y >= O.list_top)
    spongy_list_down (&O.list, y, t);
  return true;
}

void
omni_touch_move (float x, float y, double t)
{
  if (!O.down)
    return;
  if (hypotf (x - O.down_x, y - O.down_y) > dp (8))
    O.moved = true;
  if (O.down_y >= O.list_top && spongy_list_move (&O.list, y, t)) {
    O.decided = true;
    blur ();                       /* a scroll puts the keyboard away */
  }
  wl_schedule (N.wl);
}

static void
activate_row (const Row *r)
{
  switch (r->kind) {
  case ROW_APP: {
    float x = dp (ROW_SIDE_DP + 12), y = O.list_top + r->y - roundf (O.list.offset) + (r->h_px - dp (ICON_DP_)) / 2;
    omni_close_now ();
    tile_launch (r->t, x, y, dp (ICON_DP_), false);
    break;
  }
  case ROW_HIT:
    search_activate (O.search, r->g->id, r->h->id);
    omni_close ();
    break;
  case ROW_MORE:
    search_launch_search (O.search, r->g->id);
    omni_close ();
    break;
  case ROW_ASK:
    if (O.ask_hit)
      search_activate (O.search, ASK_GROUP, O.ask_hit);
    else
      search_query_now (O.search, O.query->str);   /* the answer to come: tap again */
    if (O.ask_hit)
      omni_close ();
    break;
  default:
    break;
  }
}

void
omni_touch_up (float x, float y, double t)
{
  if (!O.down)
    return;
  O.down = false;
  bool tap = !O.moved && !O.decided && !O.list.dragging;
  if (O.down_y >= O.list_top)
    spongy_list_up (&O.list, t);
  if (!tap)
    return;
  if (O.down_y < O.list_top) {
    float r[4];
    field_rect (r);
    if (O.down_y >= r[1] && O.down_y < r[1] + r[3])
      focus ();
    return;
  }
  int i = row_at (O.down_y);
  if (i >= 0)
    activate_row (&g_array_index (O.rows, Row, i));
  else
    blur ();
}

void
omni_touch_cancel (double t)
{
  if (O.down && O.down_y >= O.list_top)
    spongy_list_cancel (&O.list, t);
  O.down = false;
}

/* ---- drawing ----------------------------------------------------------------------------------------------------- */

static void
draw_row (const Row *r, float top)
{
  float x = dp (ROW_SIDE_DP), w = N.W - 2 * x, y = top + r->y;
  float isz = dp (ICON_DP_), tx = x + dp (12) + isz + dp (14);
  switch (r->kind) {
  case ROW_HEADER: {
    TextLayout *l = r->g ? r->g->title_l : O.apps_l;
    if (l)
      text_draw (l, x + dp (12), roundf (y + r->h_px - text_layout_height (l) - dp (6)), GFX_RGBA (OMNI_HINT_RGB, 1));
    return;
  }
  case ROW_MORE:
    if (r->g->more_l)
      text_draw (r->g->more_l, x + dp (12), roundf (y + (r->h_px - text_layout_height (r->g->more_l)) / 2),
                 GFX_RGBA (OMNI_ACCENT_RGB, 1));
    return;
  case ROW_ASK:
    gfx_rect (x, y, w, r->h_px, dp (16), GFX_RGBA (OMNI_ACCENT_RGB, 0.14f));
    if (O.ask_l)
      text_draw (O.ask_l, x + dp (16), roundf (y + (r->h_px - text_layout_height (O.ask_l)) / 2),
                 GFX_RGBA (OMNI_ACCENT_RGB, 1));
    return;
  default:
    break;
  }
  gfx_rect (x, y, w, r->h_px, dp (16), GFX_RGBA (OMNI_ROW_RGB, OMNI_ROW_ALPHA));
  float iy = roundf (y + (r->h_px - isz) / 2);
  TextLayout *name, *desc = NULL;
  if (r->kind == ROW_APP) {
    tile_draw_icon (r->t, x + dp (12), iy, isz, 1);
    if (r->l)
      text_draw (r->l, tx, roundf (y + (r->h_px - text_layout_height (r->l)) / 2), GFX_RGBA (OMNI_TEXT_RGB, 1));
    return;
  }
  if (r->h->icon)
    gfx_image (r->h->icon, x + dp (12), iy, isz, isz, 0, 1);
  name = r->h->name_l;
  desc = r->h->desc_l;
  float nh = name ? text_layout_height (name) : 0, dh = desc ? text_layout_height (desc) : 0;
  float ty = roundf (y + (r->h_px - nh - dh) / 2);
  if (name)
    text_draw (name, tx, ty, GFX_RGBA (OMNI_TEXT_RGB, 1));
  if (desc)
    text_draw (desc, tx, ty + nh, GFX_RGBA (OMNI_HINT_RGB, 1));
}

void
omni_draw (void)
{
  if (!O.open || O.f <= 0)
    return;
  gfx_save ();
  gfx_opacity (O.f);
  gfx_translate (0, dp (24) * (1 - O.f));
  gfx_rect (0, -dp (24), N.W, N.H + dp (24), 0, GFX_RGBA (OMNI_BG_RGB, OMNI_BG_ALPHA));
  float r[4];
  field_rect (r);
  draw_field (r[0], r[1], r[2], r[3], O.query_l, O.focused && O.caret_on, 1);
  gfx_save ();
  gfx_clip (0, O.list_top, N.W, N.H);
  float top = O.list_top - roundf (O.list.offset), view = O.list_h;
  float off = roundf (O.list.offset);
  for (guint i = 0; i < O.rows->len; i++) {
    Row *row = &g_array_index (O.rows, Row, i);
    if (row->y + row->h_px < off || row->y > off + view)
      continue;
    draw_row (row, top);
  }
  gfx_restore ();
  gfx_restore ();
}
