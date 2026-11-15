/* A parallax wallpaper: transparent layers over a background, each a texture a little larger than the screen, shifted
 * by its depth as the phone tilts. The tilt is the accelerometer's (IIO sysfs), measured against a slowly following
 * rest position so the picture sits centred however the phone is held. */
#pragma once

#include <stdbool.h>

typedef struct Parallax Parallax;

/* `dir` holds parallax.json ({"range": dp, "tilt": g, "layers": [{"file", "depth"}…]}, back to front), and
 * optionally "chisel": marks lit by one light that the tilt moves (chisel.h); a set with it runs without a sensor too */
Parallax *parallax_new    (const char *dir, void (*wake) (void *data), void *data);
/* a still picture: one layer that never moves, and no sensor */
Parallax *parallax_new_image (const char *file);
void      parallax_free   (Parallax *p);
void      parallax_resize (Parallax *p, int w, int h, double scale);
void      parallax_run    (Parallax *p, bool run);     /* follow the tilt while shown */
/* one frame's sensor step; true while the picture moves (another frame is wanted). At rest the sensor is polled
 * slowly and `wake` is called when it moves again. */
bool      parallax_tick   (Parallax *p);
/* the whole screen, zoomed by `zoom` about its centre (1: none) */
void      parallax_draw   (Parallax *p, float zoom);
