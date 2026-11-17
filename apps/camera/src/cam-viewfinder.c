/* cam-viewfinder: see cam-viewfinder.h. */
#include "cam-viewfinder.h"
#include "cam-constants.h"

#include <math.h>
#include <neo.h>

#define TOUCH_SLOP 8            /* dp, ViewConfiguration */

struct _CamViewfinder {
  GtkWidget     parent;
  GdkPaintable *paintable;
  int           orientation;
  gboolean      mirror, cover;
  int           frame_w, frame_h;
  double        zoom;
  GskRenderNode *frozen;
  CamGrid       grid;
  gboolean      leveler;
  double        tilt;
  /* the focus ring */
  gboolean      focusing, focus_ok;
  double        focus_x, focus_y, focus_pad, focus_angle;
  guint         focus_tick;
  NeoAnim      *focus_anim;
  guint         focus_hide;
  /* the capture blink, 0 … 1 */
  double        blink;
  NeoAnim      *blink_anim;
  /* touch */
  double        drag_dx, drag_dy;
  gboolean      dragged, pinching;
};

G_DEFINE_FINAL_TYPE (CamViewfinder, cam_viewfinder, GTK_TYPE_WIDGET)

enum { SIG_TAPPED, SIG_FLING, SIG_PINCH_BEGIN, SIG_PINCH, SIG_PINCH_END, N_SIGNALS };
static guint signals[N_SIGNALS];

/* the picture's rectangle before the zoom (it may stand out of the view when it covers), and its scale */
static void
picture_rect (CamViewfinder *self, graphene_rect_t *rect, double *scale)
{
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
  double fw = self->frame_w > 0 ? self->frame_w : 4, fh = self->frame_h > 0 ? self->frame_h : 3;
  double rw = self->orientation % 180 ? fh : fw, rh = self->orientation % 180 ? fw : fh;
  double s = self->cover ? MAX (w / rw, h / rh) : MIN (w / rw, h / rh);

  graphene_rect_init (rect, (w - rw * s) / 2, (h - rh * s) / 2, rw * s, rh * s);
  if (scale)
    *scale = s;
}

void
cam_viewfinder_get_picture (CamViewfinder *self, graphene_rect_t *rect)
{
  graphene_rect_t full = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (GTK_WIDGET (self)),
                                             gtk_widget_get_height (GTK_WIDGET (self)));

  picture_rect (self, rect, NULL);
  /* the visible part (GridView: the surface's size, coerced into the PreviewView's) */
  graphene_rect_intersection (rect, &full, rect);
}

static void
draw_preview (CamViewfinder *self, GtkSnapshot *snapshot, GdkPaintable *picture)
{
  graphene_rect_t rect, visible;
  double scale;
  int pw, ph;

  if (!picture)
    return;
  picture_rect (self, &rect, &scale);
  cam_viewfinder_get_picture (self, &visible);
  pw = gdk_paintable_get_intrinsic_width (picture);
  ph = gdk_paintable_get_intrinsic_height (picture);
  if (pw <= 0 || ph <= 0)
    pw = self->frame_w, ph = self->frame_h;
  if (pw <= 0 || ph <= 0)
    return;
  /* a frame of another size than expected (the last one of the camera before) is fitted the same way */
  scale = self->orientation % 180 ? MIN (rect.size.width / ph, rect.size.height / pw)
                                  : MIN (rect.size.width / pw, rect.size.height / ph);
  gtk_snapshot_push_clip (snapshot, &visible);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (visible.origin.x + visible.size.width / 2,
                                                          visible.origin.y + visible.size.height / 2));
  /* the mirror is the picture's, after the turn */
  gtk_snapshot_scale (snapshot, (self->mirror ? -1 : 1) * scale * self->zoom, scale * self->zoom);
  gtk_snapshot_rotate (snapshot, self->orientation);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-pw / 2.0, -ph / 2.0));
  gdk_paintable_snapshot (picture, snapshot, pw, ph);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
line (GtkSnapshot *snapshot, double x0, double y0, double x1, double y1, double width, const GdkRGBA *color)
{
  g_autoptr(GskPathBuilder) builder = gsk_path_builder_new ();
  g_autoptr(GskPath) path = NULL;
  g_autoptr(GskStroke) stroke = gsk_stroke_new (width);

  gsk_path_builder_move_to (builder, x0, y0);
  gsk_path_builder_line_to (builder, x1, y1);
  path = gsk_path_builder_to_path (builder);
  gtk_snapshot_append_stroke (snapshot, path, stroke, color);
}

static void
draw_grid (CamViewfinder *self, GtkSnapshot *snapshot)
{
  graphene_rect_t r;
  int size = self->grid == CAM_GRID_4X4 ? 4 : 3;
  double unit = self->grid == CAM_GRID_GOLDEN ? 2 + CAM_GOLDEN_RATIO : size;
  GdkRGBA color = CAM_CA (CAM_C_GRID);
  /* Paint strokeWidth 2: device pixels */
  double px = 2.0 / neo_device_scale (GTK_WIDGET (self));

  if (self->grid == CAM_GRID_OFF)
    return;
  cam_viewfinder_get_picture (self, &r);
  for (int i = size - 1; i >= 1; i--)
    {
      double position = self->grid == CAM_GRID_GOLDEN && i == 2 ? 1 + CAM_GOLDEN_RATIO : i;
      double x = r.origin.x + r.size.width / unit * position, y = r.origin.y + r.size.height / unit * position;

      line (snapshot, x, r.origin.y, x, r.origin.y + r.size.height, px, &color);
      line (snapshot, r.origin.x, y, r.origin.x + r.size.width, y, px, &color);
    }
}

static void
draw_leveler (CamViewfinder *self, GtkSnapshot *snapshot)
{
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = gtk_widget_get_height (GTK_WIDGET (self));
  double m = MIN (w, h), cx = w / 2, cy = h / 2;
  double o = self->tilt, px = 4.0 / neo_device_scale (GTK_WIDGET (self));
  int within = (int) o % 90;
  gboolean level = within < CAM_LEVEL_ZONE_DEG || 90 - within < CAM_LEVEL_ZONE_DEG;
  gboolean landscape = (o >= 45 && o < 135) || (o >= 225 && o < 315);
  double xl = m * (landscape ? CAM_LEVEL_BASE_Y_FRAC : CAM_LEVEL_BASE_X_FRAC);
  double yl = m * (landscape ? CAM_LEVEL_BASE_X_FRAC : CAM_LEVEL_BASE_Y_FRAC);
  GdkRGBA base = level ? CAM_C (CAM_C_YELLOW) : CAM_CA (CAM_C_GRID), plain = CAM_CA (CAM_C_GRID);

  if (!self->leveler || isnan (o))
    return;
  line (snapshot, cx - xl, cy, cx + xl, cy, px, &base);
  line (snapshot, cx, cy - yl, cx, cy + yl, px, &base);
  if (!level)
    {
      double radians = -o / 180 * G_PI, len = round (m * CAM_LEVEL_LINE_FRAC);

      line (snapshot, cx - cos (radians) * len, cy - sin (radians) * len, cx + cos (radians) * len,
            cy + sin (radians) * len, px, &plain);
    }
}

static void
cam_viewfinder_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamViewfinder *self = CAM_VIEWFINDER (widget);
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  GdkRGBA black = CAM_C (0);

  gtk_snapshot_append_color (snapshot, &black, &box);
  gtk_snapshot_push_clip (snapshot, &box);
  if (self->frozen)
    {
      /* RenderEffect.createBlurEffect (25 px) */
      gtk_snapshot_push_blur (snapshot, CAM_BLUR_RADIUS);
      gtk_snapshot_append_node (snapshot, self->frozen);
      gtk_snapshot_pop (snapshot);
    }
  else
    {
      draw_preview (self, snapshot, self->paintable);
      /* the grid is at alpha 0 while the camera rebinds */
      draw_grid (self, snapshot);
    }
  draw_leveler (self, snapshot);
  if (self->focusing || neo_anim_is_running (self->focus_anim) || self->focus_hide)
    {
      double r = CAM_FOCUS_RING / 2.0 - self->focus_pad;
      g_autoptr(GskPathBuilder) builder = gsk_path_builder_new ();
      g_autoptr(GskPath) path = NULL;
      g_autoptr(GskStroke) stroke = gsk_stroke_new (1);
      GdkRGBA white = CAM_C (0xFFFFFF);

      /* camera_focus_circle: a 1 dp white oval stroke inside the 75 dp view, less its padding. Over it, once the
       * search is over: yellow for a lens that found its peak, red for one that did not */
      if (!self->focusing)
        white = self->focus_ok ? CAM_C (CAM_C_YELLOW) : CAM_C (CAM_C_REC_RED);
      gsk_path_builder_add_circle (builder, &GRAPHENE_POINT_INIT (self->focus_x, self->focus_y), r - 0.5);
      path = gsk_path_builder_to_path (builder);
      gtk_snapshot_append_stroke (snapshot, path, stroke, &white);
      if (self->focusing)
        {
          /* the search is running: a quarter of a circle goes round the ring */
          g_autoptr(GskPathBuilder) arc = gsk_path_builder_new ();
          g_autoptr(GskPath) arc_path = NULL;
          g_autoptr(GskStroke) thick = gsk_stroke_new (3);

          for (int i = 0; i <= CAM_FOCUS_ARC_STEPS; i++)
            {
              double a = (self->focus_angle + 90.0 * i / CAM_FOCUS_ARC_STEPS) * G_PI / 180;
              double x = self->focus_x + (r - 0.5) * cos (a), y = self->focus_y + (r - 0.5) * sin (a);

              if (i == 0)
                gsk_path_builder_move_to (arc, x, y);
              else
                gsk_path_builder_line_to (arc, x, y);
            }
          arc_path = gsk_path_builder_to_path (arc);
          gtk_snapshot_append_stroke (snapshot, arc_path, thick, &white);
        }
    }
  if (self->blink > 0 && self->blink < 1)
    {
      /* ValueAnimator.ofInt (0, 255, 0): up and down over the one 300 ms AD run */
      GdkRGBA shade = CAM_C (0);

      shade.alpha = 1 - fabs (2 * self->blink - 1);
      gtk_snapshot_append_color (snapshot, &shade, &box);
    }
  gtk_snapshot_pop (snapshot);
}

/* ---- touch ----------------------------------------------------------------------------------------------------- */

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CamViewfinder *self)
{
  self->drag_dx = self->drag_dy = 0;
  self->dragged = FALSE;
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CamViewfinder *self)
{
  self->drag_dx = dx;
  self->drag_dy = dy;
  if (hypot (dx, dy) > TOUCH_SLOP)
    self->dragged = TRUE;
}

static void
on_swipe (GtkGestureSwipe *swipe, double vx, double vy, CamViewfinder *self)
{
  /* GestureDetector.onFling, then Aperture's |dx| > 75 dp; a pinch blocks it (the guard is the window's) */
  if (self->pinching || fabs (vx) < CAM_FLING_MIN_VELOCITY || fabs (self->drag_dx) <= CAM_FLING_MIN
      || fabs (self->drag_dx) < fabs (self->drag_dy))
    return;
  /* a fling to the left is the next mode */
  g_signal_emit (self, signals[SIG_FLING], 0, self->drag_dx < 0 ? 1 : -1);
}

static void
on_click_released (GtkGestureClick *click, int n, double x, double y, CamViewfinder *self)
{
  if (!self->dragged && !self->pinching)
    g_signal_emit (self, signals[SIG_TAPPED], 0, x, y);
}

static void
on_zoom_begin (GtkGesture *gesture, GdkEventSequence *sequence, CamViewfinder *self)
{
  self->pinching = TRUE;
  self->dragged = TRUE;
  g_signal_emit (self, signals[SIG_PINCH_BEGIN], 0);
}

static void
on_zoom_scale (GtkGestureZoom *gesture, double scale, CamViewfinder *self)
{
  g_signal_emit (self, signals[SIG_PINCH], 0, scale);
}

static void
on_zoom_end (GtkGesture *gesture, GdkEventSequence *sequence, CamViewfinder *self)
{
  self->pinching = FALSE;
  g_signal_emit (self, signals[SIG_PINCH_END], 0);
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
on_focus_anim (NeoAnim *anim, double value, gpointer data)
{
  CAM_VIEWFINDER (data)->focus_pad = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
on_blink_anim (NeoAnim *anim, double value, gpointer data)
{
  CAM_VIEWFINDER (data)->blink = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
on_invalidate (GdkPaintable *paintable, CamViewfinder *self)
{
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cam_viewfinder_dispose (GObject *object)
{
  CamViewfinder *self = CAM_VIEWFINDER (object);

  if (self->paintable)
    g_signal_handlers_disconnect_by_data (self->paintable, self);
  g_clear_object (&self->paintable);
  g_clear_pointer (&self->frozen, gsk_render_node_unref);
  g_clear_pointer (&self->focus_anim, neo_anim_free);
  g_clear_pointer (&self->blink_anim, neo_anim_free);
  g_clear_handle_id (&self->focus_hide, g_source_remove);
  G_OBJECT_CLASS (cam_viewfinder_parent_class)->dispose (object);
}

static void
cam_viewfinder_class_init (CamViewfinderClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GType type = G_TYPE_FROM_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_viewfinder_dispose;
  widget_class->snapshot = cam_viewfinder_snapshot;
  signals[SIG_TAPPED] = g_signal_new ("tapped", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                                      G_TYPE_DOUBLE, G_TYPE_DOUBLE);
  signals[SIG_FLING] = g_signal_new ("fling", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                                     G_TYPE_INT);
  signals[SIG_PINCH_BEGIN] = g_signal_new ("pinch-begin", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                           0);
  signals[SIG_PINCH] = g_signal_new ("pinch", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                                     G_TYPE_DOUBLE);
  signals[SIG_PINCH_END] = g_signal_new ("pinch-end", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
cam_viewfinder_init (CamViewfinder *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkGesture *swipe = gtk_gesture_swipe_new ();
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *zoom = gtk_gesture_zoom_new ();

  self->zoom = 1;
  self->tilt = NAN;
  self->focus_anim = neo_anim_new (GTK_WIDGET (self), on_focus_anim, self);
  self->blink_anim = neo_anim_new (GTK_WIDGET (self), on_blink_anim, self);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  g_signal_connect (swipe, "swipe", G_CALLBACK (on_swipe), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (swipe));
  g_signal_connect (click, "released", G_CALLBACK (on_click_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  g_signal_connect (zoom, "begin", G_CALLBACK (on_zoom_begin), self);
  g_signal_connect (zoom, "scale-changed", G_CALLBACK (on_zoom_scale), self);
  g_signal_connect (zoom, "end", G_CALLBACK (on_zoom_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (zoom));
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
cam_viewfinder_new (GdkPaintable *paintable)
{
  CamViewfinder *self = g_object_new (CAM_TYPE_VIEWFINDER, NULL);

  if (paintable)
    {
      self->paintable = g_object_ref (paintable);
      g_signal_connect (paintable, "invalidate-contents", G_CALLBACK (on_invalidate), self);
      g_signal_connect (paintable, "invalidate-size", G_CALLBACK (on_invalidate), self);
    }
  return GTK_WIDGET (self);
}

void
cam_viewfinder_set_geometry (CamViewfinder *self, int orientation, gboolean mirror, gboolean cover, int width,
                             int height)
{
  self->orientation = ((orientation % 360) + 360) % 360;
  self->mirror = mirror;
  self->cover = cover;
  self->frame_w = width;
  self->frame_h = height;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_viewfinder_set_zoom (CamViewfinder *self, double zoom)
{
  self->zoom = MAX (zoom, 1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_viewfinder_freeze (CamViewfinder *self)
{
  g_autoptr(GdkPaintable) frame = NULL;
  GtkSnapshot *snapshot;

  if (self->frozen || !self->paintable)
    return;
  /* this very frame, as it shows now: the stream behind it is about to change */
  frame = gdk_paintable_get_current_image (self->paintable);
  snapshot = gtk_snapshot_new ();
  draw_preview (self, snapshot, frame);
  self->frozen = gtk_snapshot_free_to_node (snapshot);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_viewfinder_thaw (CamViewfinder *self)
{
  g_clear_pointer (&self->frozen, gsk_render_node_unref);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
cam_viewfinder_is_frozen (CamViewfinder *self)
{
  return self->frozen != NULL;
}

void
cam_viewfinder_set_grid (CamViewfinder *self, CamGrid grid)
{
  self->grid = grid;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_viewfinder_set_leveler (CamViewfinder *self, gboolean shown, double tilt)
{
  self->leveler = shown;
  self->tilt = tilt;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_focus_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CamViewfinder *self = CAM_VIEWFINDER (widget);

  if (!self->focusing)
    {
      self->focus_tick = 0;
      return G_SOURCE_REMOVE;
    }
  self->focus_angle = fmod (gdk_frame_clock_get_frame_time (clock) / 1000.0 * 360.0 / CAM_MS_FOCUS_SPIN, 360.0);
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

void
cam_viewfinder_focus (CamViewfinder *self, double x, double y)
{
  g_clear_handle_id (&self->focus_hide, g_source_remove);
  self->focus_x = x;
  self->focus_y = y;
  if (!self->focusing)
    {
      self->focusing = TRUE;
      /* padding 0 → 8 dp, 300 ms AD */
      neo_anim_start (self->focus_anim, 0, CAM_FOCUS_RING_INSET, CAM_MS_DEFAULT, 0, SPONGY_ACCELERATE_DECELERATE);
    }
  if (!self->focus_tick)
    self->focus_tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_focus_tick, NULL, NULL);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
on_focus_hide (gpointer data)
{
  CamViewfinder *self = data;

  self->focus_hide = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_REMOVE;
}

void
cam_viewfinder_focus_done (CamViewfinder *self, gboolean ok)
{
  if (!self->focusing)
    return;
  self->focusing = FALSE;
  self->focus_ok = ok;
  neo_anim_start (self->focus_anim, self->focus_pad, 0, CAM_MS_DEFAULT, 0, SPONGY_ACCELERATE_DECELERATE);
  g_clear_handle_id (&self->focus_hide, g_source_remove);
  self->focus_hide = g_timeout_add (CAM_MS_DEFAULT + (ok ? CAM_MS_FOCUS_HIDE : CAM_MS_FOCUS_FAILED_HIDE), on_focus_hide,
                                    self);
}

gboolean
cam_viewfinder_to_frame (CamViewfinder *self, double x, double y, double *frame_x, double *frame_y)
{
  graphene_rect_t rect, visible;
  double scale, sx, sy, th = self->orientation * G_PI / 180, ux, uy;
  int pw = self->paintable ? gdk_paintable_get_intrinsic_width (self->paintable) : 0;
  int ph = self->paintable ? gdk_paintable_get_intrinsic_height (self->paintable) : 0;

  if (pw <= 0 || ph <= 0)
    pw = self->frame_w, ph = self->frame_h;
  if (pw <= 0 || ph <= 0)
    return FALSE;
  picture_rect (self, &rect, NULL);
  cam_viewfinder_get_picture (self, &visible);
  /* the inverse of draw_preview: its scale, then undo the turn and the mirror about the visible part's middle */
  scale = self->orientation % 180 ? MIN (rect.size.width / ph, rect.size.height / pw)
                                  : MIN (rect.size.width / pw, rect.size.height / ph);
  sx = (self->mirror ? -1 : 1) * scale * self->zoom;
  sy = scale * self->zoom;
  x = (x - (visible.origin.x + visible.size.width / 2)) / sx;
  y = (y - (visible.origin.y + visible.size.height / 2)) / sy;
  ux = x * cos (th) + y * sin (th) + pw / 2.0;
  uy = -x * sin (th) + y * cos (th) + ph / 2.0;
  *frame_x = ux / pw;
  *frame_y = uy / ph;
  return *frame_x >= 0 && *frame_x <= 1 && *frame_y >= 0 && *frame_y <= 1;
}

void
cam_viewfinder_get_centre (CamViewfinder *self, double *x, double *y)
{
  graphene_rect_t visible;

  cam_viewfinder_get_picture (self, &visible);
  *x = visible.origin.x + visible.size.width / 2;
  *y = visible.origin.y + visible.size.height / 2;
}

void
cam_viewfinder_blink (CamViewfinder *self)
{
  neo_anim_start (self->blink_anim, 0, 1, CAM_MS_BLINK, 0, SPONGY_ACCELERATE_DECELERATE);
}
