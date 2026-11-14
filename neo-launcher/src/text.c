/* Text (see text.h). */
#include "text.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include <glib.h>
#include <hb-ft.h>
#include <hb.h>
#include <math.h>
#include <string.h>

#define ATLAS     2048
#define SUBPIXELS 4            /* horizontal glyph positions per pixel */

typedef struct {
  FT_Face    face;
  hb_font_t *hb;
} Face;

struct TextFont {
  int        id;
  float      px;
  int        weight;
  FcFontSet *set;              /* the family and its fallbacks, best first */
  Face      *faces;            /* loaded on first use, one per font in set */
  float      ascent, descent;
  char      *axes;             /* "opsz=18,ROND=0": variable font axes besides the weight */
};

typedef struct {
  float u0, v0, u1, v1;
  int   left, top, w, h;       /* the bitmap from the pen position, y up */
} Glyph;

typedef struct {
  int    x, y;                 /* the bitmap's top left from the layout's */
  Glyph *g;
} Placed;

struct TextLayout {
  GArray *placed;              /* Placed */
  int     lines;
  float   width, line_h;
};

/* A shaped glyph before the lines are broken */
typedef struct {
  int    face;
  guint  gid;
  float  advance, dx, dy;      /* pixels */
  guint  cluster;              /* byte offset in the string */
} Shaped;

static struct {
  FT_Library  ft;
  GHashTable *fonts;           /* "family|weight|px" -> TextFont */
  GHashTable *glyphs;          /* guint64* -> Glyph */
  GfxTex     *atlas;
  int         shelf_x, shelf_y, shelf_h;
  bool        full;
  int         nfonts;
} T;

bool
text_init (void)
{
  if (FT_Init_FreeType (&T.ft))
    return false;
  FcInit ();
  T.fonts = g_hash_table_new (g_str_hash, g_str_equal);
  T.glyphs = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_free);
  T.atlas = gfx_tex_new (ATLAS, ATLAS, VK_FORMAT_R8_UNORM);
  if (!T.atlas)
    return false;
  guint8 *zero = g_malloc0 (ATLAS * ATLAS);
  gfx_tex_write (T.atlas, 0, 0, ATLAS, ATLAS, zero, ATLAS);
  g_free (zero);
  return true;
}

static Face *
face_get (TextFont *f, int i)
{
  Face *fc = &f->faces[i];
  if (fc->face)
    return fc;
  FcChar8 *file = NULL;
  int index = 0;
  FcPatternGetString (f->set->fonts[i], FC_FILE, 0, &file);
  FcPatternGetInteger (f->set->fonts[i], FC_INDEX, 0, &index);
  if (!file || FT_New_Face (T.ft, (const char *) file, index, &fc->face)) {
    g_warning ("text: cannot open %s", file);
    fc->face = NULL;
    return i ? face_get (f, 0) : NULL;
  }
  if (FT_HAS_MULTIPLE_MASTERS (fc->face)) {
    /* a variable font: its weight axis at the asked weight */
    FT_MM_Var *mm = NULL;
    if (!FT_Get_MM_Var (fc->face, &mm)) {
      FT_Fixed coords[16];
      FT_UInt n = MIN (mm->num_axis, 16);
      FT_Get_Var_Design_Coordinates (fc->face, n, coords);
      g_auto (GStrv) axes = g_strsplit (f->axes ? f->axes : "", ",", -1);
      for (FT_UInt a = 0; a < n; a++) {
        FT_ULong tag = mm->axis[a].tag;
        double v = NAN;
        if (tag == FT_MAKE_TAG ('w', 'g', 'h', 't'))
          v = f->weight;
        for (int i = 0; axes[i]; i++)
          if (strlen (axes[i]) > 5 && axes[i][4] == '=' &&
              tag == FT_MAKE_TAG (axes[i][0], axes[i][1], axes[i][2], axes[i][3]))
            v = g_ascii_strtod (axes[i] + 5, NULL);
        if (!isnan (v))
          coords[a] = CLAMP ((FT_Fixed) lround (v * 65536), mm->axis[a].minimum, mm->axis[a].maximum);
      }
      FT_Set_Var_Design_Coordinates (fc->face, n, coords);
      FT_Done_MM_Var (T.ft, mm);
    }
  }
  FT_Set_Char_Size (fc->face, 0, (FT_F26Dot6) lroundf (f->px * 64), 72, 72);
  fc->hb = hb_ft_font_create_referenced (fc->face);
  hb_ft_font_set_load_flags (fc->hb, FT_LOAD_NO_HINTING);
  return fc;
}

bool
text_family_installed (const char *family)
{
  FcPattern *pat = FcPatternCreate ();
  FcPatternAddString (pat, FC_FAMILY, (const FcChar8 *) family);
  FcConfigSubstitute (NULL, pat, FcMatchPattern);
  FcDefaultSubstitute (pat);
  FcResult res;
  FcPattern *match = FcFontMatch (NULL, pat, &res);
  FcChar8 *got = NULL;
  bool ok = match && FcPatternGetString (match, FC_FAMILY, 0, &got) == FcResultMatch && got &&
                g_ascii_strcasecmp ((const char *) got, family) == 0;
  if (match)
    FcPatternDestroy (match);
  FcPatternDestroy (pat);
  return ok;
}

TextFont *
text_font (const char *family, int weight, float px)
{
  return text_font_axes (family, weight, px, NULL);
}

TextFont *
text_font_axes (const char *family, int weight, float px, const char *axes)
{
  char *key = g_strdup_printf ("%s|%d|%.2f|%s", family, weight, px, axes ? axes : "");
  TextFont *f = g_hash_table_lookup (T.fonts, key);
  if (f) {
    g_free (key);
    return f;
  }
  f = g_new0 (TextFont, 1);
  f->id = ++T.nfonts;
  f->px = px;
  f->weight = weight;
  f->axes = g_strdup (axes);
  FcPattern *pat = FcPatternCreate ();
  FcPatternAddString (pat, FC_FAMILY, (const FcChar8 *) family);
  FcPatternAddInteger (pat, FC_WEIGHT, FcWeightFromOpenType (weight));
  FcPatternAddDouble (pat, FC_PIXEL_SIZE, px);
  FcConfigSubstitute (NULL, pat, FcMatchPattern);
  FcDefaultSubstitute (pat);
  FcResult res;
  f->set = FcFontSort (NULL, pat, FcTrue, NULL, &res);
  FcPatternDestroy (pat);
  if (!f->set || !f->set->nfont) {
    g_warning ("text: no font for %s", family);
    return NULL;
  }
  f->faces = g_new0 (Face, f->set->nfont);
  Face *fc = face_get (f, 0);
  if (fc) {
    float upem = fc->face->units_per_EM;
    f->ascent = fc->face->ascender * px / upem;
    f->descent = -fc->face->descender * px / upem;
  }
  g_hash_table_insert (T.fonts, key, f);
  return f;
}

float text_ascent (TextFont *f) { return f->ascent; }
float text_descent (TextFont *f) { return f->descent; }
float text_line_height (TextFont *f) { return f->ascent + f->descent; }

/* ---- glyphs --------------------------------------------------------------------------------------------------- */

static Glyph *
glyph_get (TextFont *f, int face, guint gid, int sub)
{
  guint64 k = ((guint64) f->id << 40) | ((guint64) face << 32) | ((guint64) gid << 2) | (guint64) sub;
  Glyph *g = g_hash_table_lookup (T.glyphs, &k);
  if (g)
    return g;
  g = g_new0 (Glyph, 1);
  g_hash_table_insert (T.glyphs, g_memdup2 (&k, sizeof k), g);
  Face *fc = face_get (f, face);
  if (!fc || FT_Load_Glyph (fc->face, gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
    return g;
  FT_GlyphSlot slot = fc->face->glyph;
  if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
    FT_Outline_Translate (&slot->outline, sub * 64 / SUBPIXELS, 0);
  if (FT_Render_Glyph (slot, FT_RENDER_MODE_NORMAL) || slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
    return g;
  FT_Bitmap *b = &slot->bitmap;
  int w = b->width, h = b->rows;
  if (!w || !h)
    return g;
  /* a shelf in the atlas, with a pixel of nothing round the glyph so a scaled quad samples no neighbour */
  int pw = w + 2, ph = h + 2;
  if (T.shelf_x + pw > ATLAS) {
    T.shelf_y += T.shelf_h;
    T.shelf_x = 0;
    T.shelf_h = 0;
  }
  if (T.shelf_y + ph > ATLAS) {
    if (!T.full)
      g_warning ("text: the glyph atlas is full");
    T.full = true;
    return g;
  }
  int ax = T.shelf_x, ay = T.shelf_y;
  T.shelf_x += pw;
  T.shelf_h = MAX (T.shelf_h, ph);
  guint8 *buf = g_malloc0 ((gsize) pw * ph);
  for (int y = 0; y < h; y++)
    memcpy (buf + (gsize) (y + 1) * pw + 1, b->buffer + (gsize) y * b->pitch, w);
  gfx_tex_write (T.atlas, ax, ay, pw, ph, buf, pw);
  g_free (buf);
  g->u0 = (ax + 1) / (float) ATLAS;
  g->v0 = (ay + 1) / (float) ATLAS;
  g->u1 = (ax + 1 + w) / (float) ATLAS;
  g->v1 = (ay + 1 + h) / (float) ATLAS;
  g->left = slot->bitmap_left;
  g->top = slot->bitmap_top;
  g->w = w;
  g->h = h;
  return g;
}

/* ---- shaping -------------------------------------------------------------------------------------------------- */

static int
face_for (TextFont *f, gunichar c, int current)
{
  /* spaces, joiners and marks stay in the run they are in */
  if (current >= 0 && (g_unichar_isspace (c) || g_unichar_ismark (c) || c == 0x200d || (c >= 0xfe00 && c <= 0xfe0f)))
    return current;
  for (int i = 0; i < f->set->nfont; i++) {
    FcCharSet *cs = NULL;
    if (FcPatternGetCharSet (f->set->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch && FcCharSetHasChar (cs, c))
      return i;
  }
  return current >= 0 ? current : 0;
}

static void
shape_run (TextFont *f, int face, const char *utf8, int len, int start, int run_len, GArray *out)
{
  Face *fc = face_get (f, face);
  if (!fc)
    return;
  hb_buffer_t *buf = hb_buffer_create ();
  hb_buffer_add_utf8 (buf, utf8, len, start, run_len);
  hb_buffer_guess_segment_properties (buf);
  hb_shape (fc->hb, buf, NULL, 0);
  unsigned n;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos (buf, &n);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions (buf, &n);
  for (unsigned i = 0; i < n; i++) {
    Shaped s = { face, info[i].codepoint, pos[i].x_advance / 64.f, pos[i].x_offset / 64.f, pos[i].y_offset / 64.f,
                 info[i].cluster };
    g_array_append_val (out, s);
  }
  hb_buffer_destroy (buf);
}

static GArray *
shape (TextFont *f, const char *utf8)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Shaped));
  int len = strlen (utf8);
  int run_start = 0, run_face = -1;
  for (const char *p = utf8; *p; p = g_utf8_next_char (p)) {
    int face = face_for (f, g_utf8_get_char (p), run_face);
    if (run_face >= 0 && face != run_face) {
      shape_run (f, run_face, utf8, len, run_start, (int) (p - utf8) - run_start, out);
      run_start = p - utf8;
    }
    run_face = face;
  }
  if (run_face >= 0)
    shape_run (f, run_face, utf8, len, run_start, len - run_start, out);
  return out;
}

static float
span_width (GArray *g, guint a, guint b)
{
  float w = 0;
  for (guint i = a; i < b; i++)
    w += g_array_index (g, Shaped, i).advance;
  return w;
}

/* ---- layout --------------------------------------------------------------------------------------------------- */

TextLayout *
text_layout (TextFont *f, const char *utf8, float max_width, int max_lines, TextAlign align)
{
  TextLayout *l = g_new0 (TextLayout, 1);
  l->placed = g_array_new (FALSE, FALSE, sizeof (Placed));
  if (!f)
    return l;
  l->line_h = text_line_height (f);
  if (!utf8 || !*utf8)
    return l;
  GArray *g = shape (f, utf8);
  if (max_width <= 0 || max_lines < 1)
    max_lines = 1;

  /* lines as [start, end) of g: greedy, breaking after a space, or mid-word when one word is too wide */
  guint starts[16], ends[16];
  int nl = 0;
  guint at = 0;
  while (at < g->len && nl < MIN (max_lines, 16)) {
    if (nl == max_lines - 1 || max_width <= 0) {
      starts[nl] = at;
      ends[nl++] = g->len;
      break;
    }
    float w = 0;
    guint i = at;
    int brk = -1;
    for (; i < g->len; i++) {
      Shaped *s = &g_array_index (g, Shaped, i);
      if (w + s->advance > max_width && i > at)
        break;
      w += s->advance;
      if (utf8[s->cluster] == ' ')
        brk = i;
    }
    starts[nl] = at;
    if (i == g->len) {
      ends[nl++] = i;
      break;
    }
    if (brk > (int) at) {
      ends[nl++] = brk;
      at = brk + 1;
    } else {
      ends[nl++] = i;
      at = i;
    }
    while (at < g->len && utf8[g_array_index (g, Shaped, at).cluster] == ' ')
      at++;
  }

  /* the last line too wide: cut it short and end it in an ellipsis */
  GArray *ell = NULL;
  if (max_width > 0 && nl && span_width (g, starts[nl - 1], ends[nl - 1]) > max_width) {
    ell = shape (f, "\xe2\x80\xa6");
    float ew = span_width (ell, 0, ell->len);
    guint e = ends[nl - 1];
    while (e > starts[nl - 1] && span_width (g, starts[nl - 1], e) + ew > max_width)
      e--;
    while (e > starts[nl - 1] && utf8[g_array_index (g, Shaped, e - 1).cluster] == ' ')
      e--;
    ends[nl - 1] = e;
  }

  for (int line = 0; line < nl; line++) {
    float lw = span_width (g, starts[line], ends[line]);
    if (ell && line == nl - 1)
      lw += span_width (ell, 0, ell->len);
    l->width = MAX (l->width, lw);
    float x = 0;
    if (align == TEXT_CENTER)
      x = max_width > 0 ? (max_width - lw) / 2 : -lw / 2;
    else if (align == TEXT_RIGHT)
      x = max_width > 0 ? max_width - lw : -lw;
    float baseline = roundf (line * l->line_h + f->ascent);
    for (int part = 0; part < 2; part++) {
      GArray *src = part ? ell : g;
      if (!src)
        continue;
      guint a = part ? 0 : starts[line], b = part ? src->len : ends[line];
      if (part && line != nl - 1)
        continue;
      for (guint i = a; i < b; i++) {
        Shaped *s = &g_array_index (src, Shaped, i);
        float pen = x + s->dx;
        float fl = floorf (pen);
        int sub = (int) lroundf ((pen - fl) * SUBPIXELS);
        if (sub == SUBPIXELS) {
          sub = 0;
          fl += 1;
        }
        Glyph *gl = glyph_get (f, s->face, s->gid, sub);
        if (gl->w) {
          Placed p = { (int) fl + gl->left, (int) (baseline - roundf (s->dy)) - gl->top, gl };
          g_array_append_val (l->placed, p);
        }
        x += s->advance;
      }
    }
  }
  l->lines = nl;
  g_array_unref (g);
  if (ell)
    g_array_unref (ell);
  return l;
}

void
text_layout_free (TextLayout *l)
{
  if (!l)
    return;
  g_array_unref (l->placed);
  g_free (l);
}

float text_layout_width (TextLayout *l) { return l->width; }
float text_layout_height (TextLayout *l) { return l->lines * l->line_h; }
int text_layout_lines (TextLayout *l) { return l->lines; }

void
text_draw (TextLayout *l, float x, float y, GfxColor c)
{
  if (!l || !l->placed->len)
    return;
  x = roundf (x);
  y = roundf (y);
  for (guint i = 0; i < l->placed->len; i++) {
    Placed *p = &g_array_index (l->placed, Placed, i);
    GfxInst *q = gfx_push (T.atlas, GFX_PIPE_QUAD);
    float a = c.a * q->a;
    q->x = x + p->x;
    q->y = y + p->y;
    q->w = p->g->w;
    q->h = p->g->h;
    q->u0 = p->g->u0; q->v0 = p->g->v0; q->u1 = p->g->u1; q->v1 = p->g->v1;
    q->r = c.r * a; q->g = c.g * a; q->b = c.b * a; q->a = a;
    q->mode = GFX_MODE_MASK;
  }
}

float
text_width (TextFont *f, const char *utf8)
{
  GArray *g = shape (f, utf8);
  float w = span_width (g, 0, g->len);
  g_array_unref (g);
  return w;
}
