/* A row swiped away sideways (msg-swipe.h). */
#include "msg-swipe.h"

#include <math.h>
#include <neo.h>

#define SLOP 8                  /* ViewConfiguration's paging touch slop, dp */
#define CANCEL_RATIO 1.2        /* a drag with |dy| > 1.2 |dx| is the list's */
#define COMMIT 0.4              /* of the width */
#define FLING_DISTANCE 0.05     /* of the width, before a fling counts */
#define FLING_MIN 50            /* dp/s: ViewConfiguration.MINIMUM_FLING_VELOCITY */
#define FLING_MAX 4000          /* px/s, max_swipe_velocity_px_per_second, at 3 px per dp */
#define DURATION_MS 300         /* swipe_duration_ms */
#define ICON_PADDING 16
#define VELOCITY_WINDOW_US (100 * 1000)

struct _MsgSwipe {
  GtkWidget parent;
  GtkWidget *background, *row;
  GtkWidget *start_icon, *end_icon;
  NeoAnim *settle;
  double dx;
  gboolean enabled, swiping, committing;
  /* the last positions, for the speed it was let go at */
  double sample_x[2];
  gint64 sample_us[2];
};

G_DEFINE_FINAL_TYPE (MsgSwipe, msg_swipe, GTK_TYPE_WIDGET)

enum { SWIPED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
set_dx (MsgSwipe *self, double dx)
{
  self->dx = dx;
  gtk_widget_set_child_visible (self->background, dx != 0);
  /* only the icon the row uncovers: the start one going right, the end one going left */
  gtk_widget_set_visible (self->start_icon, dx > 0);
  gtk_widget_set_visible (self->end_icon, dx < 0);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_settle (NeoAnim *anim, double value, gpointer data)
{
  MsgSwipe *self = data;

  set_dx (self, value);
  if (!neo_anim_is_running (anim) && self->committing)
    {
      self->committing = FALSE;
      g_signal_emit (self, signals[SWIPED], 0, self->dx > 0);
    }
}

static void
add_sample (MsgSwipe *self, double x)
{
  self->sample_x[0] = self->sample_x[1];
  self->sample_us[0] = self->sample_us[1];
  self->sample_x[1] = x;
  self->sample_us[1] = g_get_monotonic_time ();
}

/* px/s over the last move, 0 when the finger had stopped */
static double
velocity (MsgSwipe *self)
{
  gint64 dt = self->sample_us[1] - self->sample_us[0];

  if (dt <= 0 || g_get_monotonic_time () - self->sample_us[1] > VELOCITY_WINDOW_US)
    return 0;
  return CLAMP ((self->sample_x[1] - self->sample_x[0]) * G_USEC_PER_SEC / dt, -FLING_MAX / 3.0, FLING_MAX / 3.0);
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, MsgSwipe *self)
{
  if (!self->enabled || self->committing)
    {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  neo_anim_stop (self->settle);
  self->swiping = self->dx != 0;          /* caught while settling: it is the finger's again */
  self->sample_us[0] = self->sample_us[1] = 0;
  add_sample (self, self->dx);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, MsgSwipe *self)
{
  if (!self->swiping)
    {
      if (fabs (dy) > SLOP && fabs (dy) > CANCEL_RATIO * fabs (dx))
        {
          gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
          return;
        }
      if (fabs (dx) <= SLOP)
        return;
      /* it is a swipe: the row's press and the list's scroll let go of this touch */
      self->swiping = TRUE;
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    }
  add_sample (self, dx);
  set_dx (self, dx);
}

static void
settle (MsgSwipe *self, double v)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  gboolean flung = fabs (v) > FLING_MIN && fabs (self->dx) > FLING_DISTANCE * width && (v > 0) == (self->dx > 0);
  double to = 0;
  guint duration = DURATION_MS;

  self->swiping = FALSE;
  if (width > 0 && (fabs (self->dx) > COMMIT * width || flung))
    {
      to = self->dx > 0 ? width : -width;
      self->committing = TRUE;
      /* as fast as it was flung, at most the 300 ms */
      if (flung)
        duration = MIN (DURATION_MS, (guint) (fabs (to - self->dx) / fabs (v) * 1000));
    }
  neo_anim_start (self->settle, self->dx, to, MAX (duration, 1), 0, SPONGY_FAST_OUT_SLOW_IN);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, MsgSwipe *self)
{
  if (self->swiping)
    settle (self, velocity (self));
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, MsgSwipe *self)
{
  if (self->swiping)
    settle (self, 0);
}

void
msg_swipe_set_enabled (MsgSwipe *self, gboolean enabled)
{
  self->enabled = enabled;
  if (!enabled && self->dx != 0 && !self->committing)
    settle (self, 0);
}

static void
msg_swipe_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                   int *min_baseline, int *nat_baseline)
{
  gtk_widget_measure (MSG_SWIPE (widget)->row, orientation, for_size, min, nat, NULL, NULL);
}

/* the row's: its wrapping snippet is as tall as the width it gets (height-for-width); a constant size would be
 * measured once, at the natural width, and the row's last line cut */
static GtkSizeRequestMode
msg_swipe_get_request_mode (GtkWidget *widget)
{
  return gtk_widget_get_request_mode (MSG_SWIPE (widget)->row);
}

static void
msg_swipe_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  MsgSwipe *self = MSG_SWIPE (widget);

  gtk_widget_allocate (self->background, width, height, -1, NULL);
  gtk_widget_allocate (self->row, width, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (round (self->dx), 0)));
}

static void
msg_swipe_dispose (GObject *object)
{
  MsgSwipe *self = MSG_SWIPE (object);

  g_clear_pointer (&self->settle, neo_anim_free);
  g_clear_pointer (&self->background, gtk_widget_unparent);
  g_clear_pointer (&self->row, gtk_widget_unparent);
  G_OBJECT_CLASS (msg_swipe_parent_class)->dispose (object);
}

static void
msg_swipe_class_init (MsgSwipeClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = msg_swipe_dispose;
  widget_class->measure = msg_swipe_measure;
  widget_class->get_request_mode = msg_swipe_get_request_mode;
  widget_class->size_allocate = msg_swipe_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "msg-swipe");
  signals[SWIPED] = g_signal_new ("swiped", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
msg_swipe_init (MsgSwipe *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->enabled = TRUE;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  self->settle = neo_anim_new (GTK_WIDGET (self), on_settle, self);
}

static GtkWidget *
icon (const char *name)
{
  GtkWidget *image = gtk_image_new_from_icon_name (name);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  return image;
}

GtkWidget *
msg_swipe_new (GtkWidget *row, const char *start_icon, const char *end_icon)
{
  MsgSwipe *self = g_object_new (MSG_TYPE_SWIPE, NULL);
  GtkWidget *start = icon (start_icon), *end = icon (end_icon);

  /* crossSwipeBackground: window_background, 16 dp in, the icon at each end */
  self->background = gtk_center_box_new ();
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (self->background), start);
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (self->background), end);
  gtk_widget_set_margin_start (start, ICON_PADDING);
  gtk_widget_set_margin_end (end, ICON_PADDING);
  gtk_widget_add_css_class (self->background, "msg-swipe-background");
  gtk_widget_set_parent (self->background, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->background, FALSE);
  self->start_icon = start;
  self->end_icon = end;
  self->row = row;
  gtk_widget_set_parent (row, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}
