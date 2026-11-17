/* cam-af: see cam-af.h. */
#include "cam-af.h"

#include <math.h>
#include <string.h>

/* ---- config ---------------------------------------------------------------------------------------------------- */

gboolean
cam_af_config_load (CamAfConfig *c, GSettings *s, GError **error)
{
  memset (c, 0, sizeof *c);
  c->pos_min = g_settings_get_int (s, "af-pos-min");
  c->pos_max = g_settings_get_int (s, "af-pos-max");
  c->coarse_steps = g_settings_get_int (s, "af-coarse-steps");
  c->refine_levels = g_settings_get_int (s, "af-refine-levels");
  c->settle_ms = g_settings_get_int (s, "af-settle-ms");
  c->skip_frames = g_settings_get_int (s, "af-skip-frames");
  c->roi = g_settings_get_double (s, "af-roi");
  c->min_contrast = g_settings_get_double (s, "af-min-contrast");
  c->min_score = g_settings_get_double (s, "af-min-score");
  c->min_luma = g_settings_get_double (s, "af-min-luma");
  c->early_contrast = g_settings_get_double (s, "af-early-contrast");
  c->early_drop = g_settings_get_double (s, "af-early-drop");
  c->early_count = g_settings_get_int (s, "af-early-count");
  c->plateau_tol = g_settings_get_double (s, "af-plateau-tol");
  c->timeout_ms = g_settings_get_int (s, "af-timeout-ms");
  c->start_delay_ms = g_settings_get_int (s, "af-start-delay-ms");
  c->watch_every_ms = g_settings_get_int (s, "af-watch-every-ms");
  c->watch_drop = g_settings_get_double (s, "af-watch-drop");
  c->watch_count = g_settings_get_int (s, "af-watch-count");
  c->watch_gap_ms = g_settings_get_int (s, "af-watch-gap-ms");
  c->dt_root = g_settings_get_string (s, "af-dt-root");
  c->subdev_class = g_settings_get_string (s, "af-subdev-class");
  c->dev_dir = g_settings_get_string (s, "af-dev-dir");
  if (c->coarse_steps < 3 || c->coarse_steps > CAM_AF_MAX_POINTS / 3 || c->roi <= 0 || c->roi > 1
      || c->pos_max <= c->pos_min || c->refine_levels < 0 || c->early_count < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "autofocus keys: need 3 or more coarse steps, 0 < roi <= 1 and af-pos-max above af-pos-min");
      cam_af_config_clear (c);
      return FALSE;
    }
  return TRUE;
}

void
cam_af_config_clear (CamAfConfig *c)
{
  g_clear_pointer (&c->dt_root, g_free);
  g_clear_pointer (&c->subdev_class, g_free);
  g_clear_pointer (&c->dev_dir, g_free);
}

/* ---- the search ------------------------------------------------------------------------------------------------ */

typedef struct {
  int    pos;
  double score;
} Point;

typedef enum { PHASE_COARSE, PHASE_REFINE, PHASE_DONE } Phase;

struct _CamAfSearch {
  CamAfConfig c;                 /* the numbers only: the strings are not used here */
  int         lo, hi;
  double      step;
  Point       pts[CAM_AF_MAX_POINTS];
  int         n;
  Phase       phase;
  int         coarse_i, level, cand;
  double      dist;
  int         best;
  int         pending;
  gboolean    asked;
  CamAfResult result;
};

CamAfSearch *
cam_af_search_new (const CamAfConfig *config, int lo, int hi)
{
  CamAfSearch *s = g_new0 (CamAfSearch, 1);

  s->c = *config;
  s->c.dt_root = s->c.subdev_class = s->c.dev_dir = NULL;
  s->lo = lo;
  s->hi = MAX (hi, lo + 1);
  s->step = (double) (s->hi - s->lo) / (s->c.coarse_steps - 1);
  return s;
}

void
cam_af_search_free (CamAfSearch *s)
{
  g_free (s);
}

static int
clamp_round (CamAfSearch *s, double p)
{
  return CLAMP ((int) floor (p + 0.5), s->lo, s->hi);
}

static gboolean
seen (CamAfSearch *s, int pos)
{
  for (int i = 0; i < s->n; i++)
    if (s->pts[i].pos == pos)
      return TRUE;
  return FALSE;
}

/* the first of the best scores in the order they were measured */
static int
best_index (CamAfSearch *s)
{
  int best = 0;

  for (int i = 1; i < s->n; i++)
    if (s->pts[i].score > s->pts[best].score)
      best = i;
  return best;
}

static double
worst_score (CamAfSearch *s)
{
  double worst = s->pts[0].score;

  for (int i = 1; i < s->n; i++)
    worst = MIN (worst, s->pts[i].score);
  return MAX (worst, 1e-12);
}

static int
cmp_point (const void *a, const void *b)
{
  return ((const Point *) a)->pos - ((const Point *) b)->pos;
}

/* the best position; neighbours within plateau-tol of the peak make a plateau, and its middle is the safest place on it */
static int
pick (CamAfSearch *s)
{
  Point sorted[CAM_AF_MAX_POINTS];
  Point best = s->pts[best_index (s)];
  int lo, hi;

  memcpy (sorted, s->pts, sizeof (Point) * s->n);
  qsort (sorted, s->n, sizeof (Point), cmp_point);
  for (lo = 0; sorted[lo].pos != best.pos; lo++)
    ;
  hi = lo;
  while (lo > 0 && sorted[lo - 1].score >= best.score * (1 - s->c.plateau_tol))
    lo--;
  while (hi < s->n - 1 && sorted[hi + 1].score >= best.score * (1 - s->c.plateau_tol))
    hi++;
  return (int) floor ((sorted[lo].pos + sorted[hi].pos) / 2.0 + 0.5);
}

static void
finish (CamAfSearch *s)
{
  CamAfResult *r = &s->result;
  double peak = s->pts[best_index (s)].score;

  s->phase = PHASE_DONE;
  r->score = peak;
  r->contrast = peak / worst_score (s);
  r->reason = peak < s->c.min_score ? CAM_AF_REASON_FLAT
            : r->contrast < s->c.min_contrast ? CAM_AF_REASON_LOW_CONTRAST : CAM_AF_REASON_NONE;
  r->ok = r->reason == CAM_AF_REASON_NONE;
  r->position = pick (s);
  r->evals = s->n;
}

gboolean
cam_af_search_next (CamAfSearch *s, int *position)
{
  g_return_val_if_fail (!s->asked, FALSE);
  while (s->phase != PHASE_DONE)
    {
      if (s->phase == PHASE_COARSE)
        {
          while (s->coarse_i < s->c.coarse_steps)
            {
              int p = clamp_round (s, s->lo + s->coarse_i++ * s->step);

              if (!seen (s, p))
                {
                  s->pending = p;
                  s->asked = TRUE;
                  *position = p;
                  return TRUE;
                }
            }
          s->phase = PHASE_REFINE;
          s->level = s->cand = 0;
          s->best = s->pts[best_index (s)].pos;
          s->dist = s->step / 2;
        }
      if (s->phase == PHASE_REFINE)
        {
          while (s->level < s->c.refine_levels)
            {
              while (s->cand < 2)
                {
                  int p = clamp_round (s, s->best + (s->cand++ == 0 ? -s->dist : s->dist));

                  if (!seen (s, p))
                    {
                      s->pending = p;
                      s->asked = TRUE;
                      *position = p;
                      return TRUE;
                    }
                }
              /* the level is measured: the next one goes round the best so far, half as far */
              s->best = s->pts[best_index (s)].pos;
              s->level++;
              s->cand = 0;
              s->dist /= 2;
            }
          finish (s);
        }
    }
  return FALSE;
}

void
cam_af_search_feed (CamAfSearch *s, double score)
{
  g_return_if_fail (s->asked && s->n < CAM_AF_MAX_POINTS);
  s->asked = FALSE;
  s->pts[s->n].pos = s->pending;
  s->pts[s->n].score = score;
  s->n++;
  if (s->phase == PHASE_COARSE && s->n > s->c.early_count)
    {
      double peak = s->pts[best_index (s)].score;
      gboolean falling = TRUE;

      /* a clear peak, and the last scores all well under it: the sweep has gone past it */
      for (int i = s->n - s->c.early_count; i < s->n; i++)
        falling = falling && s->pts[i].score < s->c.early_drop * peak;
      if (falling && peak / worst_score (s) >= s->c.early_contrast)
        s->coarse_i = s->c.coarse_steps;
    }
}

void
cam_af_search_result (CamAfSearch *s, CamAfResult *result)
{
  *result = s->result;
}

/* ---- sharpness ------------------------------------------------------------------------------------------------- */

void
cam_af_roi_rect (int width, int height, double roi, double cx, double cy, int *x, int *y, int *side)
{
  int s = MAX (8, (int) (roi * MIN (width, height)));

  s = MIN (s, MIN (width, height));
  *side = s;
  *x = (int) CLAMP (cx * width - s / 2.0, 0, width - s);
  *y = (int) CLAMP (cy * height - s / 2.0, 0, height - s);
}

#define PX(r, c) ((int) data[(r) * stride + (c) * pstride])

double
cam_af_tile_sharpness (const guint8 *data, int stride, int pstride, int side, double min_luma)
{
  guint64 grad = 0, sum = 0;
  double mean, count = (double) (side - 2) * (side - 2);

  if (side < 3)
    return 0;
  for (int r = 0; r < side; r++)
    for (int c = 0; c < side; c++)
      sum += PX (r, c);
  for (int r = 1; r < side - 1; r++)
    for (int c = 1; c < side - 1; c++)
      {
        int gx = PX (r - 1, c + 1) - PX (r - 1, c - 1) + 2 * (PX (r, c + 1) - PX (r, c - 1))
               + PX (r + 1, c + 1) - PX (r + 1, c - 1);
        int gy = PX (r + 1, c - 1) - PX (r - 1, c - 1) + 2 * (PX (r + 1, c) - PX (r - 1, c))
               + PX (r + 1, c + 1) - PX (r - 1, c + 1);

        grad += (guint64) (gx * gx + gy * gy);
      }
  mean = (double) sum / ((double) side * side);
  if (mean < min_luma)
    return 0;
  return (double) grad / count / (mean * mean + 1.0);
}

gboolean
cam_af_buffer_sharpness (GstBuffer *buffer, const GstVideoInfo *info, double roi, double min_luma, double cx, double cy,
                         double *score)
{
  GstVideoFrame frame;
  const GstVideoFormatInfo *finfo = info->finfo;
  int x, y, side, comp;

  if (!gst_video_frame_map (&frame, info, buffer, GST_MAP_READ))
    return FALSE;
  /* a YUV or grey frame: the Y plane; an RGB one: the green channel (most of the luma), one byte per sample */
  comp = GST_VIDEO_FORMAT_INFO_IS_RGB (finfo) && GST_VIDEO_FORMAT_INFO_N_COMPONENTS (finfo) > 1 ? 1 : 0;
  if (GST_VIDEO_FORMAT_INFO_DEPTH (finfo, comp) != 8)
    {
      gst_video_frame_unmap (&frame);
      return FALSE;
    }
  cam_af_roi_rect (GST_VIDEO_FRAME_WIDTH (&frame), GST_VIDEO_FRAME_HEIGHT (&frame), roi, cx, cy, &x, &y, &side);
  {
    int pstride = GST_VIDEO_FRAME_COMP_PSTRIDE (&frame, comp);
    int stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, comp);
    const guint8 *base = GST_VIDEO_FRAME_COMP_DATA (&frame, comp);

    /* the tile's first sample: the component's own offset is in COMP_DATA, the position adds to it */
    *score = cam_af_tile_sharpness (base + (gsize) y * stride + (gsize) x * pstride, stride, pstride, side,
                                      min_luma);
  }
  gst_video_frame_unmap (&frame);
  return TRUE;
}
