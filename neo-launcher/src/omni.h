/* Omnisearch: the bar widget under the clock, and the full-screen search view it opens. The results are GNOME's
 * search providers' (search.c) plus the installed apps (matched here, first); tap activates, a group's last row opens
 * the provider's own search (LaunchSearch), the last row of the view asks Minerva. Text comes in as the drawer
 * pill's does: the input method's commit/delete (wl_text_input) or a key. */
#pragma once

#include <stdbool.h>

#include "gfx.h"

/* The colours, all of them, in one place (the drawer's tokens: the theme session retints here) */
#define OMNI_BG_RGB        0x082D31      /* the view's background (the drawer sheet's) */
#define OMNI_BG_ALPHA      1.0f
#define OMNI_FIELD_RGB     0x121F21      /* the bar and the view's field */
#define OMNI_FIELD_ALPHA   0.92f
#define OMNI_TEXT_RGB      0xDDE7E9
#define OMNI_HINT_RGB      0xA2ADAF
#define OMNI_ACCENT_RGB    0xB8EBF3      /* the caret, the "more" and Minerva rows */
#define OMNI_ROW_RGB       0x16363A      /* a result row's card */
#define OMNI_ROW_ALPHA     0.55f

/* the widget (widgets.c): drawn in its rows' box; a tap on the bar opens the view */
void   omni_bar_draw  (float x, float y, float w, float h);
bool   omni_bar_tap   (float x, float y, float top, float h);

void   omni_layout    (void);                        /* the screen changed size */
void   omni_open      (void);
void   omni_close     (void);                        /* animated */
void   omni_close_now (void);
bool   omni_is_open   (void);                        /* showing or closing: the view takes the touches */
bool   omni_frame     (void);                        /* true while it moves */
void   omni_draw      (void);                        /* the screen pass, over the home and the drawer */
bool   omni_touch_down (float x, float y, double t);
void   omni_touch_move (float x, float y, double t);
void   omni_touch_up   (float x, float y, double t);
void   omni_touch_cancel (double t);
bool   omni_key       (unsigned keysym, const char *utf8);   /* true: the field took the key */
void   omni_text      (const char *utf8);
void   omni_erase     (int before, int after);
void   omni_submit    (void);
bool   omni_back      (void);                        /* back closes it: true when it did */
