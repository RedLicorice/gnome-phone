/* NeoNavBar: see neo-nav-bar.h. Geometry measured on still:clock-alarm (spec/clock.md §1.2): the bar y 2040…2280,
 * the pill 192 x 96 px at y 2076…2172, the icon y 2088…2160, the label y 2184…2226 (px at 3 per dp). */
#include "neo-nav-bar.h"
#include "neo-anim.h"
#include "neo-ripple.h"
#include "neo-style.h"

#define BAR_H          80
#define PILL_W         64
#define PILL_H         32
#define PILL_TOP       12
#define ICON           24
#define LABEL_TOP      48
#define INDICATOR_MS   500    /* MDC NavigationBarItemView: motionDurationLong2, emphasized */
/* the rail (Material 3's NavigationRail, a landscape window): the same items stacked down the start edge in a column
 * BAR_H wide; a row is the indicator and the label under it (M3 rail: 56 dp destinations, 4 dp from the indicator to
 * the label), so a phone's landscape height holds four of them under an app bar */
#define RAIL_TOP       8
#define RAIL_ROW       64
#define RAIL_PILL_TOP  4
#define RAIL_LABEL_TOP 40

typedef struct {
  GtkWidget *icon, *label;
  double     shown;            /* the pill: 0 hidden .. 1 shown */
  NeoAnim   *indicator;
  NeoRipple  ripple;
  NeoNavBar *bar;
} Item;

struct _NeoNavBar {
  GtkWidget  parent;
  GPtrArray *items;
  int        selected;
  int        pressed;          /* the item under the finger, or -1 */
  gboolean   rail;             /* stacked down the side (neo_nav_bar_set_rail) */
};

G_DEFINE_FINAL_TYPE (NeoNavBar, neo_nav_bar, GTK_TYPE_WIDGET)

enum { SELECTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
item_free (gpointer data)
{
  Item *item = data;
  neo_anim_free (item->indicator);
  neo_ripple_clear (&item->ripple);
  gtk_widget_unparent (item->icon);
  gtk_widget_unparent (item->label);
  g_free (item);
}

static double
column_width (NeoNavBar *self)
{
  return self->items->len ? gtk_widget_get_width (GTK_WIDGET (self)) / (double) self->items->len : 0;
}

/* the top-left corner of an item's cell: a column of the bar, a row of the rail */
static graphene_point_t
cell_origin (NeoNavBar *self, guint index)
{
  return self->rail ? GRAPHENE_POINT_INIT (0, RAIL_TOP + RAIL_ROW * index) : GRAPHENE_POINT_INIT (column_width (self) * index, 0);
}

static double
cell_width (NeoNavBar *self)
{
  return self->rail ? BAR_H : column_width (self);
}

static graphene_rect_t
pill_rect (NeoNavBar *self, guint index)
{
  graphene_point_t o = cell_origin (self, index);
  return GRAPHENE_RECT_INIT (o.x + (cell_width (self) - PILL_W) / 2, o.y + (self->rail ? RAIL_PILL_TOP : PILL_TOP), PILL_W, PILL_H);
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

static void
neo_nav_bar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                     int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  NeoNavBar *self = NEO_NAV_BAR (widget);
  if (self->rail)
    *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? BAR_H : RAIL_TOP + (int) self->items->len * RAIL_ROW;
  else
    *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? BAR_H : (int) self->items->len * PILL_W;
}

static void
neo_nav_bar_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NeoNavBar *self = NEO_NAV_BAR (widget);
  double col = cell_width (self);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    graphene_point_t o = cell_origin (self, i);
    gtk_widget_allocate (item->icon, ICON, ICON, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (o.x + (col - ICON) / 2,
                                                                              o.y + (self->rail ? RAIL_PILL_TOP : PILL_TOP) + (PILL_H - ICON) / 2)));
    int label_h, unused;
    gtk_widget_measure (item->label, GTK_ORIENTATION_VERTICAL, (int) col, &label_h, &unused, NULL, NULL);
    gtk_widget_allocate (item->label, (int) col, label_h, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (o.x, o.y + (self->rail ? RAIL_LABEL_TOP : LABEL_TOP))));
  }
}

static void
neo_nav_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NeoNavBar *self = NEO_NAV_BAR (widget);
  GdkRGBA bar = neo_color (NEO_SURFACE_CONTAINER_HIGHEST);
  gtk_snapshot_append_color (snapshot, &bar,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));

  GdkRGBA pill = neo_color (NEO_SECONDARY_CONTAINER);
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    graphene_rect_t rect = pill_rect (self, i);
    /* MDC's ActiveIndicatorTransform: the pill widens from 40 % and fades in over the first fifth */
    if (item->shown > 0) {
      double scale_x = 0.4 + 0.6 * item->shown;
      graphene_rect_t r = rect;
      r.size.width = PILL_W * scale_x;
      r.origin.x += (PILL_W - r.size.width) / 2;
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &r, PILL_H / 2.0);
      GdkRGBA c = pill;
      c.alpha = MIN (1, item->shown * 5);
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      gtk_snapshot_append_color (snapshot, &c, &r);
      gtk_snapshot_pop (snapshot);
    }
    /* the ripple plays in the pill's shape (M3 navigation items have their state layer on the indicator) */
    if (neo_ripple_is_active (&item->ripple)) {
      GskRoundedRect rounded;
      gsk_rounded_rect_init_from_rect (&rounded, &rect, PILL_H / 2.0);
      GdkRGBA ripple = neo_ripple_default_color ();
      gtk_snapshot_push_rounded_clip (snapshot, &rounded);
      neo_ripple_snapshot (&item->ripple, snapshot, &rect, &ripple);
      gtk_snapshot_pop (snapshot);
    }
    gtk_widget_snapshot_child (widget, item->icon, snapshot);
    gtk_widget_snapshot_child (widget, item->label, snapshot);
  }
}

/* ---- selection ------------------------------------------------------------------------------------------------- */

static void
on_indicator (NeoAnim *anim, double value, gpointer data)
{
  Item *item = data;
  item->shown = value;
  gtk_widget_queue_draw (GTK_WIDGET (item->bar));
}

static void
select_item (NeoNavBar *self, int index, gboolean animate)
{
  for (guint i = 0; i < self->items->len; i++) {
    Item *item = g_ptr_array_index (self->items, i);
    gboolean on = (int) i == index;
    if (on) {
      gtk_widget_add_css_class (item->icon, "selected");
      gtk_widget_add_css_class (item->label, "selected");
    } else {
      gtk_widget_remove_css_class (item->icon, "selected");
      gtk_widget_remove_css_class (item->label, "selected");
    }
    if (animate && on && (int) i != self->selected)
      neo_anim_start (item->indicator, item->shown, 1, INDICATOR_MS, 0, SPONGY_EMPHASIZED);
    else if (!animate || !on) {
      /* the old pill goes at once: only the new one animates */
      neo_anim_stop (item->indicator);
      item->shown = on;
    }
  }
  self->selected = index;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static int
item_at (NeoNavBar *self, double x, double y)
{
  double col = column_width (self);
  int index = self->rail ? (y >= RAIL_TOP ? (int) ((y - RAIL_TOP) / RAIL_ROW) : -1) : col > 0 ? (int) (x / col) : -1;
  return index >= 0 && index < (int) self->items->len ? index : -1;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, NeoNavBar *self)
{
  self->pressed = item_at (self, x, y);
  if (self->pressed < 0)
    return;
  Item *item = g_ptr_array_index (self->items, self->pressed);
  graphene_rect_t pill = pill_rect (self, self->pressed);
  neo_ripple_press (&item->ripple, x - pill.origin.x, y - pill.origin.y);
}

static void
release (NeoNavBar *self)
{
  if (self->pressed >= 0)
    neo_ripple_release (&((Item *) g_ptr_array_index (self->items, self->pressed))->ripple);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, NeoNavBar *self)
{
  int index = self->pressed;
  release (self);
  self->pressed = -1;
  if (index < 0 || index == self->selected)
    return;
  select_item (self, index, TRUE);
  g_signal_emit (self, signals[SELECTED], 0, index);
}

static void
on_stopped (GtkGestureClick *click, NeoNavBar *self)
{
  release (self);
  self->pressed = -1;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
neo_nav_bar_dispose (GObject *object)
{
  NeoNavBar *self = NEO_NAV_BAR (object);
  g_clear_pointer (&self->items, g_ptr_array_unref);
  G_OBJECT_CLASS (neo_nav_bar_parent_class)->dispose (object);
}

static void
neo_nav_bar_class_init (NeoNavBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = neo_nav_bar_dispose;
  widget_class->measure = neo_nav_bar_measure;
  widget_class->size_allocate = neo_nav_bar_size_allocate;
  widget_class->snapshot = neo_nav_bar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "neo-nav-bar");

  signals[SELECTED] = g_signal_new ("selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
neo_nav_bar_init (NeoNavBar *self)
{
  self->items = g_ptr_array_new_with_free_func (item_free);
  self->selected = -1;
  self->pressed = -1;
  gtk_widget_add_css_class (GTK_WIDGET (self), "neo-nav-bar");

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
neo_nav_bar_new (void)
{
  return g_object_new (NEO_TYPE_NAV_BAR, NULL);
}

int
neo_nav_bar_append (NeoNavBar *self, const char *icon_name, const char *label)
{
  Item *item = g_new0 (Item, 1);
  item->bar = self;
  item->icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (item->icon), ICON);
  gtk_widget_add_css_class (item->icon, "neo-nav-item");
  item->label = gtk_label_new (label);
  gtk_label_set_ellipsize (GTK_LABEL (item->label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (item->label, "neo-nav-item");
  gtk_widget_add_css_class (item->label, "neo-nav-label");
  gtk_widget_set_parent (item->icon, GTK_WIDGET (self));
  gtk_widget_set_parent (item->label, GTK_WIDGET (self));
  item->indicator = neo_anim_new (GTK_WIDGET (self), on_indicator, item);
  neo_ripple_init (&item->ripple, GTK_WIDGET (self));
  g_ptr_array_add (self->items, item);
  if (self->selected < 0)
    select_item (self, 0, FALSE);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  return self->items->len - 1;
}

void
neo_nav_bar_set_selected (NeoNavBar *self, int index)
{
  select_item (self, index, FALSE);
}

int
neo_nav_bar_get_selected (NeoNavBar *self)
{
  return self->selected;
}

void
neo_nav_bar_set_rail (NeoNavBar *self, gboolean rail)
{
  if (self->rail == !!rail)
    return;
  self->rail = !!rail;
  if (rail)
    gtk_widget_add_css_class (GTK_WIDGET (self), "neo-nav-rail");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "neo-nav-rail");
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
