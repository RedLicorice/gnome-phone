/* The Camera's measures: spec/camera.md §25.5, every value from Aperture's layouts, drawables and animators or a
 * measurement of the reference phone (the § says where). Sizes are dp, which are GTK's pixels on the phone. */
#pragma once

/* bands, chained bottom-up from the window's bottom edge (§2) */
#define CAM_MODE_SELECTOR_H        80       /* 16 + 48 + 16                                  §2     */
#define CAM_MODE_PAD               16
#define CAM_MODE_BUTTON_W          88
#define CAM_MODE_BUTTON_H          48
#define CAM_MODE_COLUMN_W          104      /* landscape: the 88 dp buttons and 8 dp each side            */
#define CAM_MODE_PILL_H            40       /* 4 dp insets top and bottom                    §3     */
#define CAM_PRIMARY_BAR_H          91       /* 16 padding + 75                               §2     */
#define CAM_PRIMARY_BAR_PAD        16
#define CAM_SHUTTER_VIEW           75
#define CAM_SHUTTER_DRAW           72       /* the vector, drawn unscaled                    §4.1   */
#define CAM_GALLERY_SIZE           60
#define CAM_GALLERY_MARGIN         32
#define CAM_GALLERY_RADIUS         8
#define CAM_GALLERY_ICON           30       /* the 60 dp ImageView at scale 0.5              §5.1   */
#define CAM_FLIP_SIZE              60
#define CAM_FLIP_ICON              32
#define CAM_PAUSE_SIZE             32
#define CAM_SECONDARY_BAR_H        72       /*                                               §2     */
#define CAM_BAR_BUTTON             48
#define CAM_BAR_BUTTON_PAD         4
#define CAM_BAR_BUTTON_MARGIN      40
#define CAM_BAR_ICON               24
#define CAM_LENS_BUTTON            32
#define CAM_LENS_MARGIN            (5.0 / 3)   /* 5 px, not dp (LensSelectorLayout.kt)       §6.2   */
#define CAM_LENS_RADIUS            8
#define CAM_LENS_PAD               8        /* a wider pill's side padding (not in Aperture: it has the ratio only) */
#define CAM_DIVIDER_H              1
#define CAM_PRO_ROW_H              (253.0 / 3) /* measured: 1298…1551 px                     §7     */
#define CAM_PRO_ROW_PAD_V          8
#define CAM_PRO_ROW_SPACER         16
#define CAM_PRO_BUTTON_W           96
#define CAM_PRO_ICON               24
#define CAM_PRO_ICON_TOP           9        /* the icon from the button's top (glyphs measured)   §7     */
#define CAM_PRO_LABEL_TOP          46       /* the label's cap top, 138 px                   §7     */
#define CAM_ISLAND_H               36
#define CAM_ISLAND_RADIUS          18
#define CAM_ISLAND_MARGIN          12
#define CAM_FOCUS_RING             75
#define CAM_FOCUS_RING_INSET       8        /* padding 0 → 8 dp while focusing              §9     */
#define CAM_ZOOM_SLIDER_W          250
#define CAM_ZOOM_SLIDER_H          32
#define CAM_TUNE_MARGIN            8        /* the sheet's distance from the viewfinder's edges and bottom           */
#define CAM_TUNE_LABEL_W           74       /* the tuning sheet: a slider's name                                 */
#define CAM_FLING_MIN              75       /*                                               §1     */
#define CAM_FLING_MIN_VELOCITY     50       /* ViewConfiguration's minimum fling, dp/s              */
#define CAM_RIPPLE_RADIUS          34       /* half the 48 dp button's diagonal (102 px)     §6.1   */

/* text (§0.4) */
#define CAM_SP_MODE_LABEL          14       /* Droid Sans Mono, synthetic bold                      */
#define CAM_SP_PRO_LABEL           12       /* Droid Sans Mono, synthetic bold, ≤ 3 lines           */
#define CAM_SP_DURATION            14       /* sans 800                                             */
#define CAM_DP_LENS_TEXT           10       /* dp, sans 800                                         */
#define CAM_SP_COUNTDOWN           125      /* sans 700                                             */
#define CAM_DP_SLIDER_TEXT         10

/* colours (standard contrast, §0.5) */
#define CAM_C_REC_RED              0xE95950
#define CAM_C_GRAY_10              0xF6FAFA
#define CAM_C_GRAY_60              0x2A3232
#define CAM_C_DARK_GREY            0x444444
#define CAM_C_YELLOW               0xE9B650
#define CAM_C_PRO_ROW_BG           0x99000000 /* ARGB                                               */
#define CAM_C_GRID                 0x7FFFFFFF /* ARGB, 2 px lines; leveler 4 px                     */
#define CAM_C_ZOOM_TRACK           0x7FCCCCCC /* ARGB                                               */

/* timing (ms) */
#define CAM_MS_DEFAULT             300      /* the platform's default animator, AD          §0.3   */
#define CAM_MS_PHOTO_PULSE         100      /* ×2 (reverse), FOSI, scale 0.625              §4.2   */
#define CAM_MS_SHUTTER_MORPH       300      /* FOSI; colour 300 AD                          §4.3   */
#define CAM_MS_FLIP                500      /* AD, 0 → −180°                                §5.2   */
#define CAM_MS_PAUSE_MORPH         200      /* + 100 delay                                  §5.3   */
#define CAM_MS_SLIDE_UP            250      /* the pro row                                  §7     */
#define CAM_MS_SLIDE_DOWN          200
#define CAM_MS_SLIDER_HIDE         2000
#define CAM_MS_FOCUS_HIDE          500
#define CAM_MS_PINCH_GUARD         500      /* no mode fling after a pinch                  §8     */
#define CAM_MS_COUNTDOWN_FADE      800      /* scale 1 → 2.5, alpha 1 → 0, AD               §10    */
#define CAM_MS_COUNTDOWN_TICK      1000
#define CAM_MS_START_SOUND_GAP     500      /* before a recording starts                    §11.1  */
#define CAM_MS_BLINK               300      /* 0 → 255 → 0 black, AD                        §12.1  */
#define CAM_MS_ISLAND_FADE         200
#define CAM_MS_THUMB_CROSSFADE     200
#define CAM_MS_TOAST               2000
#define CAM_MS_ROTATE              300      /* View.animate().rotationBy, AD                §17    */

#define CAM_PHOTO_PULSE_SCALE      0.625
#define CAM_VIDEO_DOT_SCALE        0.3125
#define CAM_VIDEO_DOT_END_SCALE    0.0625
#define CAM_COUNTDOWN_END_SCALE    2.5
#define CAM_LEVEL_ZONE_DEG         2
#define CAM_LEVEL_LINE_FRAC        0.15
#define CAM_LEVEL_BASE_X_FRAC      0.15
#define CAM_LEVEL_BASE_Y_FRAC      0.01
#define CAM_GOLDEN_RATIO           0.618
#define CAM_JPEG_QUALITY           90
#define CAM_BLUR_RADIUS            (25.0 / 3) /* RenderEffect radius 25 px                   §1     */

/* the stream: about 2 MP for the aspect asked for (owner, 11-10), from the sizes libcamera lists */
#define CAM_STREAM_TARGET_PIXELS   2000000
#define CAM_THUMB_PX               225      /* Coil decodes the thumbnail at 75 dp          §5.1   */

#define CAM_C(rgb)    ((GdkRGBA) { (((rgb) >> 16) & 0xff) / 255.f, (((rgb) >> 8) & 0xff) / 255.f, \
                                   ((rgb) & 0xff) / 255.f, 1 })
#define CAM_CA(argb)  ((GdkRGBA) { (((argb) >> 16) & 0xff) / 255.f, (((argb) >> 8) & 0xff) / 255.f, \
                                   ((argb) & 0xff) / 255.f, (((guint32) (argb) >> 24) & 0xff) / 255.f })
