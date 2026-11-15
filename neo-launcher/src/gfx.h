/* The 2D renderer: every frame is a list of quads (images, flat colours, text masks), each with its own rounded
 * corners, transform, opacity and clip, drawn in batches by an instanced pipeline. Offscreen targets turn a group
 * into a texture for effects (blur, the stretch overscroll). Textures hold premultiplied BGRA (cairo's ARGB32). */
#pragma once

#include "vk.h"

typedef struct GfxTex GfxTex;

typedef enum {
  GFX_MODE_TEX = 0,     /* texture × colour */
  GFX_MODE_SOLID = 1,   /* colour */
  GFX_MODE_MASK = 2,    /* colour × texture.r (glyphs) */
  GFX_MODE_CORNERS = 3, /* colour, the corners' radii in u0 v0 u1 v1 (top left, top right, bottom right, left) */
  GFX_MODE_RING = 4,    /* colour, an outline u0 wide inside the rounded rect */
} GfxMode;

typedef enum {
  GFX_PIPE_QUAD,
  GFX_PIPE_BLUR_DOWN,
  GFX_PIPE_BLUR_UP,
  GFX_PIPE_STRETCH,
  GFX_N_PIPES
} GfxPipe;

/* One instance, as the vertex shader reads it */
typedef struct {
  float x, y, w, h;          /* the rect in its own space */
  float u0, v0, u1, v1;      /* texture coordinates of its corners */
  float r, g, b, a;          /* premultiplied */
  float m[4];                /* screen = [m0 m1; m2 m3] · p + t */
  float tx, ty;
  float radius;              /* rounded corners, in the rect's own units */
  float mode;                /* GfxMode */
  float clip[4];             /* screen x0 y0 x1 y1; x1 <= x0 = no clip */
} GfxInst;

typedef struct { float r, g, b, a; } GfxColor;   /* straight alpha; premultiplied when drawn */

#define GFX_RGBA(rgb, alpha) ((GfxColor) { ((rgb) >> 16 & 0xff) / 255.f, ((rgb) >> 8 & 0xff) / 255.f, \
                                           ((rgb) & 0xff) / 255.f, (alpha) })

struct GfxTex {
  VkImage         image;
  VkDeviceMemory  mem;
  VkImageView     view;
  VkDescriptorSet set;
  VkFramebuffer   fb;         /* render targets only */
  VkFormat        format;
  VkImageLayout   layout;
  int             w, h;
};

bool     gfx_init            (Vk *vk);
Vk      *gfx_vk              (void);

GfxTex  *gfx_tex_new         (int w, int h, VkFormat format);
GfxTex  *gfx_tex_from_bgra   (int w, int h, const void *pixels, int stride);
GfxTex  *gfx_tex_from_gray   (int w, int h, const void *pixels, int stride);   /* one byte a pixel, for gfx_mask */
void     gfx_tex_write       (GfxTex *t, int x, int y, int w, int h, const void *pixels, int stride);
GfxTex  *gfx_target_new      (int w, int h);
void     gfx_tex_free        (GfxTex *t);      /* once no frame in flight uses it */
GfxTex  *gfx_white           (void);

/* A frame: begin (false: nothing to draw into now), passes, end. */
bool     gfx_frame_begin     (void);
void     gfx_pass_begin      (GfxTex *target, GfxColor clear);   /* target NULL = the screen */
void     gfx_pass_end        (void);
void     gfx_frame_end       (void);
int      gfx_pass_width      (void);
int      gfx_pass_height     (void);

/* Drawing state: an affine transform, an opacity and a clip, pushed and popped around groups */
void     gfx_save            (void);
void     gfx_restore         (void);
void     gfx_translate       (float x, float y);
void     gfx_scale           (float sx, float sy, float px, float py);   /* about (px, py) */
void     gfx_rotate          (float radians, float px, float py);        /* clockwise on screen, about (px, py) */
void     gfx_opacity         (float a);
void     gfx_clip            (float x0, float y0, float x1, float y1);  /* in the current space */

/* A quad under the current state; zeroed but for its transform/clip/opacity, fill in the rest */
GfxInst *gfx_push            (GfxTex *tex, GfxPipe pipe);
void     gfx_effect_params   (const float p[8]);

void     gfx_rect            (float x, float y, float w, float h, float radius, GfxColor c);
/* r: top left, top right, bottom right, bottom left */
void     gfx_rect_corners    (float x, float y, float w, float h, const float r[4], GfxColor c);
void     gfx_ring            (float x, float y, float w, float h, float radius, float width, GfxColor c);
void     gfx_image           (GfxTex *t, float x, float y, float w, float h, float radius, float alpha);
/* colour × the texture's red channel (an R8 coverage map): a tinted overlay, premultiplied as it is drawn */
void     gfx_mask_uv         (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                              GfxColor c);
void     gfx_image_uv        (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                              float radius, float alpha);

/* Blur src into a texture of 1/2^levels its size (dual Kawase); returns a target owned by gfx (valid until the next
 * call with the same slot) */
GfxTex  *gfx_blur            (GfxTex *src, int levels, float offset, int slot);
