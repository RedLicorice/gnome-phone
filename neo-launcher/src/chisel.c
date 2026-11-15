/* The chisel marks (see chisel.h). The lighting is the design mockup's (chisel.html, draw()): for every groove,
 * dt = normal · light direction, str = min(1, .35 + |light| · .9); the lit wall has alpha max(0, dt) · w · str · k_lit,
 * the shadow wall (.25 + max(0, -dt) · .6) · w · str · k_dark. w (the groove's weight, falling off from its corner) is
 * baked into the bucket textures, the rest is the tint's alpha. */
#include "chisel.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>

#include "gfx.h"

#define EASE_30HZ 0.15      /* the mockup eases the light by this much per 1/30 s step */
#define EASE_STEP 33333.0   /* µs of that step */
#define SEEN 0.02           /* the light moved this far since it was drawn (a glow shift of ~10 px): another frame is worth it */
#define GLOW_TEX 128

typedef struct {
  float   nx, ny;           /* the bucket's normal (screen axes, y down) */
  char   *lit_file, *dark_file;
  GfxTex *lit, *dark;       /* R8 coverage: the top right corner's tile over the bottom left's */
} Bucket;

struct Chisel {
  char     *dir;
  int       design_w, design_h;     /* the space the tiles' size and the glow are in: the phone's pixels */
  int       tile_w, tile_h;         /* one corner tile, in design px */
  float     sx, sy;                 /* design px -> screen px */
  GfxColor  lit, dark, glow;        /* straight colours; .a of glow is its strength */
  float     lit_gain, dark_gain, glow_radius, glow_x[2], glow_y[2];
  GArray   *buckets;
  GfxTex   *glow_tex;
  double    lx, ly;                 /* the light now */
  double    shown_x, shown_y;       /* ... and as last drawn */
  gint64    last;                   /* monotonic µs of the last update */
};

static bool
get_rgb (JsonObject *o, const char *name, GfxColor *out)
{
  JsonArray *a = json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
  if (!a || json_array_get_length (a) < 3)
    return false;
  out->r = json_array_get_double_element (a, 0) / 255;
  out->g = json_array_get_double_element (a, 1) / 255;
  out->b = json_array_get_double_element (a, 2) / 255;
  return true;
}

static double
get_num (JsonObject *o, const char *name, double def)
{
  return json_object_has_member (o, name) ? json_object_get_double_member (o, name) : def;
}

static void
bucket_clear (gpointer data)
{
  Bucket *b = data;
  gfx_tex_free (b->lit);
  gfx_tex_free (b->dark);
  g_free (b->lit_file);
  g_free (b->dark_file);
}

Chisel *
chisel_new (JsonObject *o, const char *dir)
{
  JsonArray *bs = json_object_has_member (o, "buckets") ? json_object_get_array_member (o, "buckets") : NULL;
  JsonArray *design = json_object_has_member (o, "design") ? json_object_get_array_member (o, "design") : NULL;
  JsonArray *tile = json_object_has_member (o, "tile") ? json_object_get_array_member (o, "tile") : NULL;
  if (!bs || !design || !tile || json_array_get_length (design) < 2 || json_array_get_length (tile) < 2)
    return NULL;
  Chisel *c = g_new0 (Chisel, 1);
  c->dir = g_strdup (dir);
  c->design_w = json_array_get_int_element (design, 0);
  c->design_h = json_array_get_int_element (design, 1);
  c->tile_w = json_array_get_int_element (tile, 0);
  c->tile_h = json_array_get_int_element (tile, 1);
  c->lit = (GfxColor) { 1, 1, 1, 1 };
  c->dark = (GfxColor) { 0, 0, 0, 1 };
  get_rgb (o, "lit", &c->lit);
  get_rgb (o, "dark", &c->dark);
  c->lit_gain = get_num (o, "lit_gain", 1);
  c->dark_gain = get_num (o, "dark_gain", 1);
  c->glow_radius = 0.9f;
  c->glow_x[0] = 0.5f; c->glow_x[1] = 0.45f;
  c->glow_y[0] = 0.35f; c->glow_y[1] = 0.35f;
  JsonObject *g = json_object_has_member (o, "glow") ? json_object_get_object_member (o, "glow") : NULL;
  if (g && get_rgb (g, "color", &c->glow)) {
    c->glow.a = get_num (g, "alpha", 0.1);
    c->glow_radius = get_num (g, "radius", 0.9);
    JsonArray *gx = json_object_has_member (g, "x") ? json_object_get_array_member (g, "x") : NULL;
    JsonArray *gy = json_object_has_member (g, "y") ? json_object_get_array_member (g, "y") : NULL;
    if (gx && json_array_get_length (gx) >= 2) {
      c->glow_x[0] = json_array_get_double_element (gx, 0);
      c->glow_x[1] = json_array_get_double_element (gx, 1);
    }
    if (gy && json_array_get_length (gy) >= 2) {
      c->glow_y[0] = json_array_get_double_element (gy, 0);
      c->glow_y[1] = json_array_get_double_element (gy, 1);
    }
  }
  c->buckets = g_array_new (FALSE, TRUE, sizeof (Bucket));
  g_array_set_clear_func (c->buckets, bucket_clear);
  for (guint i = 0; i < json_array_get_length (bs); i++) {
    JsonObject *bo = json_array_get_object_element (bs, i);
    if (!bo || !json_object_has_member (bo, "lit") || !json_object_has_member (bo, "dark"))
      continue;
    double deg = get_num (bo, "angle", 180) * G_PI / 180;
    Bucket b = { .nx = cos (deg), .ny = sin (deg),
                 .lit_file = g_build_filename (dir, json_object_get_string_member (bo, "lit"), NULL),
                 .dark_file = g_build_filename (dir, json_object_get_string_member (bo, "dark"), NULL) };
    g_array_append_val (c->buckets, b);
  }
  if (!c->buckets->len || c->design_w <= 0 || c->design_h <= 0 || c->tile_w <= 0 || c->tile_h <= 0) {
    chisel_free (c);
    return NULL;
  }
  c->lx = 0.3;              /* the mockup's first light, the one of the still */
  c->ly = -0.3;
  c->shown_x = c->shown_y = 1e9;
  return c;
}

void
chisel_free (Chisel *c)
{
  if (!c)
    return;
  g_array_unref (c->buckets);
  gfx_tex_free (c->glow_tex);
  g_free (c->dir);
  g_free (c);
}

/* a grey (or RGB) picture's first channel as an R8 texture */
static GfxTex *
load_mask (const char *file)
{
  g_autoptr (GdkPixbuf) pb = gdk_pixbuf_new_from_file (file, NULL);
  if (!pb)
    return NULL;
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  int n = gdk_pixbuf_get_n_channels (pb), stride = gdk_pixbuf_get_rowstride (pb);
  const guint8 *src = gdk_pixbuf_read_pixels (pb);
  g_autofree guint8 *grey = g_malloc ((gsize) w * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      grey[(gsize) y * w + x] = src[(gsize) y * stride + (gsize) x * n];
  return gfx_tex_from_gray (w, h, grey, w);
}

void
chisel_load (Chisel *c, int w, int h)
{
  c->sx = (float) w / c->design_w;
  c->sy = (float) h / c->design_h;
  for (guint i = 0; i < c->buckets->len; i++) {
    Bucket *b = &g_array_index (c->buckets, Bucket, i);
    if (!b->lit) {
      b->lit = load_mask (b->lit_file);
      b->dark = load_mask (b->dark_file);
    }
  }
  if (!c->glow_tex) {                   /* the light's glow: coverage falling off linearly from the middle */
    guint8 px[GLOW_TEX * GLOW_TEX];
    for (int y = 0; y < GLOW_TEX; y++)
      for (int x = 0; x < GLOW_TEX; x++) {
        float r = hypotf (x + 0.5f - GLOW_TEX / 2.f, y + 0.5f - GLOW_TEX / 2.f) / (GLOW_TEX / 2.f);
        px[y * GLOW_TEX + x] = (guint8) lroundf (CLAMP (1 - r, 0, 1) * 255);
      }
    c->glow_tex = gfx_tex_from_gray (GLOW_TEX, GLOW_TEX, px, GLOW_TEX);
  }
  c->shown_x = c->shown_y = 1e9;
}

bool
chisel_update (Chisel *c, gint64 now, double tx, double ty, bool follow)
{
  double gx = tx, gy = ty;
  if (!follow) {                        /* still: the light drifts by itself (the mockup's loop) */
    double t = now / 1000.0;
    gx = 0.35 * sin (t * 0.00031);
    gy = -0.25 + 0.25 * cos (t * 0.00023);
  }
  double dt = c->last ? CLAMP (now - c->last, 0, 500000) : EASE_STEP;
  c->last = now;
  double k = 1 - pow (1 - EASE_30HZ, dt / EASE_STEP);
  c->lx += (CLAMP (gx, -1, 1) - c->lx) * k;
  c->ly += (CLAMP (gy, -1, 1) - c->ly) * k;
  return fabs (c->lx - c->shown_x) > SEEN || fabs (c->ly - c->shown_y) > SEEN;
}

void
chisel_draw (Chisel *c)
{
  double lx = c->lx, ly = c->ly;
  double ln = hypot (lx, ly);
  if (ln == 0)
    ln = 1;
  double str = MIN (1, 0.35 + ln * 0.9);
  c->shown_x = lx;
  c->shown_y = ly;
  float tw = c->tile_w * c->sx, th = c->tile_h * c->sy;
  float W = c->design_w * c->sx, H = c->design_h * c->sy;
  for (guint i = 0; i < c->buckets->len; i++) {
    Bucket *b = &g_array_index (c->buckets, Bucket, i);
    double dt = (b->nx * lx + b->ny * ly) / ln;
    float lit = MAX (0, dt) * str * c->lit_gain;
    float dark = (0.25 + MAX (0, -dt) * 0.6) * str * c->dark_gain;
    for (int pass = 0; pass < 2; pass++) {
      GfxTex *t = pass ? b->dark : b->lit;
      GfxColor col = pass ? c->dark : c->lit;
      col.a = pass ? dark : lit;
      if (!t || col.a < 1 / 512.f)
        continue;
      gfx_mask_uv (t, W - tw, 0, tw, th, 0, 0, 1, 0.5f, col);       /* top right */
      gfx_mask_uv (t, 0, H - th, tw, th, 0, 0.5f, 1, 1, col);       /* bottom left */
    }
  }
  if (c->glow_tex && c->glow.a > 0) {
    float r = c->glow_radius * W;
    float x = W * (c->glow_x[0] + lx * c->glow_x[1]), y = H * (c->glow_y[0] + ly * c->glow_y[1]);
    gfx_mask_uv (c->glow_tex, x - r, y - r, 2 * r, 2 * r, 0, 0, 1, 1, c->glow);
  }
}
