/* A message's bubble (msg-bubble.h). */
#include "msg-bubble.h"

#include <math.h>

#define RADIUS 28               /* message_bubble_*_no_arrow */
#define SNAP 20                 /* message_bubble_width_snap */
#define MIN_HEIGHT 48

struct _MsgBubble {
  GtkWidget parent;
  GtkWidget *child;
  GdkRGBA color;
  int max_width;
};

G_DEFINE_FINAL_TYPE (MsgBubble, msg_bubble, GTK_TYPE_WIDGET)

void
msg_bubble_set_color (MsgBubble *self, const GdkRGBA *color)
{
  self->color = *color;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
msg_bubble_set_max_width (MsgBubble *self, int max_width)
{
  if (self->max_width == max_width)
    return;
  self->max_width = max_width;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
msg_bubble_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                    int *min_baseline, int *nat_baseline)
{
  MsgBubble *self = MSG_BUBBLE (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      int child_min, child_nat;

      /* Height-for-width only: the width does not depend on a height. A box that allocates the bubble asks for its
       * width at the row's height; passing that height on makes every wrapping label search the narrowest width
       * that fits it (GtkLabel's width-for-height, a full Pango relayout per step), about 30 relayouts per message */
      gtk_widget_measure (self->child, orientation, -1, &child_min, &child_nat, NULL, NULL);
      /* MessageBubbleBackground.onMeasure: the width rounded up to the snap, within the most it may take */
      child_nat = (int) ceil (child_nat / (double) SNAP) * SNAP;
      if (self->max_width > 0)
        child_nat = MIN (child_nat, self->max_width);
      *min = MIN (child_min, child_nat);
      *nat = MAX (*min, child_nat);
    }
  else
    {
      gtk_widget_measure (self->child, orientation, for_size, min, nat, NULL, NULL);
      *min = MAX (*min, MIN_HEIGHT);
      *nat = MAX (*nat, MIN_HEIGHT);
    }
}

/* the text wraps: its height follows the width it gets */
static GtkSizeRequestMode
msg_bubble_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
msg_bubble_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (MSG_BUBBLE (widget)->child, width, height, -1, NULL);
}

static void
msg_bubble_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  MsgBubble *self = MSG_BUBBLE (widget);
  GskRoundedRect shape;

  gsk_rounded_rect_init_from_rect (&shape, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                                gtk_widget_get_height (widget)), RADIUS);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &self->color, &shape.bounds);
  gtk_snapshot_pop (snapshot);
  gtk_widget_snapshot_child (widget, self->child, snapshot);
}

static void
msg_bubble_dispose (GObject *object)
{
  g_clear_pointer (&MSG_BUBBLE (object)->child, gtk_widget_unparent);
  G_OBJECT_CLASS (msg_bubble_parent_class)->dispose (object);
}

static void
msg_bubble_class_init (MsgBubbleClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = msg_bubble_dispose;
  widget_class->get_request_mode = msg_bubble_get_request_mode;
  widget_class->measure = msg_bubble_measure;
  widget_class->size_allocate = msg_bubble_size_allocate;
  widget_class->snapshot = msg_bubble_snapshot;
  gtk_widget_class_set_css_name (widget_class, "msg-bubble");
}

static void
msg_bubble_init (MsgBubble *self)
{
}

GtkWidget *
msg_bubble_new (GtkWidget *child)
{
  MsgBubble *self = g_object_new (MSG_TYPE_BUBBLE, NULL);

  self->child = child;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}
