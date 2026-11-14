/* Text: fontconfig picks the faces, HarfBuzz shapes, FreeType rasterises into one glyph atlas (an R8 texture), and a
 * laid-out string draws as glyph quads in a single batch. */
#pragma once

#include <stdbool.h>

#include "gfx.h"

typedef struct TextFont TextFont;
typedef struct TextLayout TextLayout;

typedef enum {
  TEXT_LEFT,
  TEXT_CENTER,
  TEXT_RIGHT,
} TextAlign;

bool        text_init          (void);
/* family as fontconfig takes it ("Roboto", "sans-serif"), weight 100..900, size in pixels; cached, never freed */
TextFont   *text_font          (const char *family, int weight, float px);
/* the same with a variable font's other axes set: "opsz=18,ROND=0" */
TextFont   *text_font_axes     (const char *family, int weight, float px, const char *axes);
float       text_ascent        (TextFont *f);     /* above the baseline, positive */
float       text_descent       (TextFont *f);     /* below it, positive */
float       text_line_height   (TextFont *f);

/* `utf8` shaped and broken into at most max_lines lines no wider than max_width (0: unbounded, one line); the last
 * line that still does not fit ends in an ellipsis. Glyphs new to the atlas are uploaded before the next pass. */
TextLayout *text_layout        (TextFont *f, const char *utf8, float max_width, int max_lines, TextAlign align);
void        text_layout_free   (TextLayout *l);
float       text_layout_width  (TextLayout *l);   /* the widest line */
float       text_layout_height (TextLayout *l);   /* lines × line height */
int         text_layout_lines  (TextLayout *l);
/* draws with the top of its first line at y; align is within max_width from x (or around x when it was 0) */
void        text_draw          (TextLayout *l, float x, float y, GfxColor c);
float       text_width         (TextFont *f, const char *utf8);
