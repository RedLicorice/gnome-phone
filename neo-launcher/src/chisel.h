/* The chisel marks of the theme-v4 wallpapers (tools/parallax-renaissance/build_chisel.py makes the sets): grooves in two
 * corners, each a lit wall and a shadow wall, that one moving light turns on and off. The grooves are pre-rendered
 * into orientation buckets (by their normal); at draw time every bucket is tinted with an alpha from the angle
 * between its normal and the light, plus a soft radial glow at the light. The light is one value (x, y in -1…1). */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdbool.h>

typedef struct Chisel Chisel;

/* from the "chisel" object of a parallax.json whose pictures are in `dir`; NULL when it is not usable */
Chisel *chisel_new    (JsonObject *o, const char *dir);
void    chisel_free   (Chisel *c);
/* the textures, for a screen of w × h px (the gfx renderer must be up) */
void    chisel_load   (Chisel *c, int w, int h);
/* Move the light on by the time since the last call (`now`: monotonic µs): towards the tilt (tx, ty) while `follow`,
 * else along the slow drift of the mockup. True when it is visibly somewhere else than where it was last drawn. */
bool    chisel_update (Chisel *c, gint64 now, double tx, double ty, bool follow);
void    chisel_draw   (Chisel *c);
