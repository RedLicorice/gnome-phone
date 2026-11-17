/* cam-af: contrast-detection autofocus for a camera with a lens actuator (the POCO X3 NFC's main camera: IMX682 with a
 * DW9807 voice coil). libcamera's software ISP has an Af algorithm of its own (continuous, one scan step every 12
 * frames, no region of interest, no trigger), so the app drives the lens itself: this file is the part that needs no
 * camera, no GTK and no lens.
 *
 *   CamAfConfig   every number, read from the de.yesman.Camera schema's "af-*" keys (data, not code)
 *   CamAfSearch   the search as a state machine: a coarse sweep over the lens range (stopped early at a clear peak),
 *                 then `refine-levels` of best ± half the previous distance. Ask for the next position, feed the
 *                 sharpness measured there, until it says done; the result says whether the peak was clear.
 *   sharpness     Tenengrad (mean squared Sobel gradient) of a region of a frame over the mean squared, so a change of
 *                 exposure while the lens moves does not read as a change of focus.
 *
 * The same algorithm, with the same keys, is eyes/minerva_eyes/focus.py in Minerva (the agent's `look`). */
#pragma once

#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define CAM_AF_MAX_POINTS 96     /* coarse steps (32 at most) and refinements (2 per level, 8 levels at most) */

typedef struct {
  int     pos_min, pos_max;      /* the sweep, in the lens' own counts, clamped to what the lens reports */
  int     coarse_steps;
  int     refine_levels;
  int     settle_ms;             /* after a lens move, before frames count ... */
  int     skip_frames;           /* ... and then this many frames are dropped (exposed while the lens moved) */
  double  roi;                   /* side of the measured square, a fraction of the frame's short side */
  double  min_contrast;          /* peak over the worst score: under this the peak is not clear */
  double  min_score;             /* a peak under this: nothing to focus on */
  double  min_luma;              /* a region whose mean grey (0 … 255) is under this is dark: its sharpness is 0 (noise
                                  * over a tiny mean would read as detail) */
  double  early_contrast;        /* the sweep stops at a peak this far over the worst score ... */
  double  early_drop;            /* ... once `early_count` scores in a row fell under this fraction of it */
  int     early_count;
  double  plateau_tol;           /* positions within this fraction of the peak are one plateau: its middle is taken */
  int     timeout_ms;            /* a search that takes longer fails */
  int     start_delay_ms;        /* after the first frame, before the first search (the exposure settles) */
  int     watch_every_ms;        /* continuous AF looks at the sharpness this often ... */
  double  watch_drop;            /* ... and searches again when it fell under this fraction of the reference ... */
  int     watch_count;           /* ... this many looks in a row ... */
  int     watch_gap_ms;          /* ... and not within this time of the last search */
  char   *dt_root;               /* where the kernel shows the devicetree, the V4L2 subdevs' sysfs class and /dev */
  char   *subdev_class;
  char   *dev_dir;
} CamAfConfig;

/* The config from the settings' "af-*" keys. FALSE (with @error) if the numbers cannot work (fewer than 3 coarse
 * steps, a region over the frame, pos-max under pos-min). Free with cam_af_config_clear. */
gboolean cam_af_config_load  (CamAfConfig *config, GSettings *settings, GError **error);
void     cam_af_config_clear (CamAfConfig *config);

typedef enum {
  CAM_AF_REASON_NONE,
  CAM_AF_REASON_FLAT,            /* the best score is under min-score: a dark or blank scene */
  CAM_AF_REASON_LOW_CONTRAST,    /* best over worst under min-contrast: nothing sharper anywhere in the range */
  CAM_AF_REASON_TIMEOUT,
} CamAfReason;

typedef struct {
  int         position;          /* the best (the middle of a plateau); the caller goes back to its last good one
                                  * when !ok */
  double      score, contrast;
  gboolean    ok;
  CamAfReason reason;
  int         evals;
} CamAfResult;

typedef struct _CamAfSearch CamAfSearch;

CamAfSearch *cam_af_search_new    (const CamAfConfig *config, int lo, int hi);
void         cam_af_search_free   (CamAfSearch *search);
/* The next lens position to measure; FALSE when the search is over. Call once, then feed, then call again. */
gboolean     cam_af_search_next   (CamAfSearch *search, int *position);
/* the sharpness measured at the position cam_af_search_next gave */
void         cam_af_search_feed   (CamAfSearch *search, double score);
/* valid once cam_af_search_next returned FALSE */
void         cam_af_search_result (CamAfSearch *search, CamAfResult *result);

/* Where a region of `roi` × the short side centred on (cx, cy) (fractions of the width and height) falls in a frame,
 * kept inside it */
void         cam_af_roi_rect      (int width, int height, double roi, double cx, double cy, int *x, int *y, int *side);

/* Sharpness of a side × side tile whose first pixel is at @data; @pstride bytes between pixels, @stride between rows;
 * 0 for a tile whose mean grey is under @min_luma */
double       cam_af_tile_sharpness (const guint8 *data, int stride, int pstride, int side, double min_luma);

/* Sharpness of the region of a video buffer (any format libcamera gives: the Y plane of a YUV one, else the green
 * channel); FALSE if the buffer cannot be read */
gboolean     cam_af_buffer_sharpness (GstBuffer *buffer, const GstVideoInfo *info, double roi, double min_luma, double cx,
                                      double cy, double *score);

G_END_DECLS
