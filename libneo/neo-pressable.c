/* NeoPressable: see neo-pressable.h. */
#include "neo-pressable.h"
#include "neo-ripple.h"
#include "neo-style.h"

#include <math.h>
#include <spongy-gtk.h>

#define TAP_TIMEOUT_MS        100      /* ViewConfiguration.TAP_TIMEOUT */
#define LONG_PRESS_TIMEOUT_MS 400      /* ViewConfiguration.DEFAULT_LONG_PRESS_TIMEOUT */

struct _NeoPressable {
  GtkWidget       parent;
  GtkWidget      *child;

  NeoRipple       ripple;
  NeoRippleShape  shape;
  double          radius;
  GdkRGBA         color;
  GdkRGBA         fill;               /* drawn in the shape, under the ripple; alpha 0 for none */
  gboolean        elevated;
  double          scale;

  gboolean        down;               /* a press that may still become a click */
  double          down_x, down_y;
  guint           tap_timeout;        /* the ripple is waiting to start */
  guint           long_press_timeout;
  gboolean        long_pressed;       /* this press was a long press: its release is not a click */
};

G_DEFINE_FINAL_TYPE (NeoPressable, neo_pressable, GTK_TYPE_WIDGET)

enum { CLICKED, LONG_PRESSED, ACTIVATE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- the press ------------------------------------------------------------------------------------------------- */

static void
clear_timeouts (NeoPressable *self)
{
  g_clear_handle_id (&self->tap_timeout, g_source_remove);
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
}

static gboolean
in_scroll_view (GtkWidget *widget)
{
  for (GtkWidget *w = gtk_widget_get_parent (widget); w; w = gtk_widget_get_parent (w))
    if (SPONGY_IS_SCROLL_VIEW (w) || GTK_IS_SCROLLED_WINDOW (w))
      return TRUE;
  return FALSE;
}

static gboolean
on_tap_timeout (gpointer data)
{
  NeoPressable *self = data;
  self->tap_timeout = 0;
  if (self->shape != NEO_RIPPLE_NONE)
    neo_ripple_press (&self->ripple, self->down_x, self->down_y);
  return G_SOURCE_REMOVE;
}

static gboolean
on_long_press_timeout (gpointer data)
{
  NeoPressable *self = data;
  self->long_press_timeout = 0;
  self->long_pressed = TRUE;
  g_signal_emit (self, signals[LONG_PRESSED], 0);
  return G_SOURCE_REMOVE;
}

/* a tap on something inside that takes taps itself (a switch in a card, a button in a row) is its, not ours: the
 * child's gesture runs first but does not claim the touch, so that a drag from it can still scroll the list */
gboolean
neo_pick_is_tappable (GtkWidget *widget, double x, double y)
{
  GtkWidget *picked = gtk_widget_pick (widget, x, y, GTK_PICK_DEFAULT);
  for (GtkWidget *w = picked; w && w != widget; w = gtk_widget_get_parent (w))
    if (g_object_get_data (G_OBJECT (w), "neo-tappable"))
      return TRUE;
  return FALSE;
}

static void
on_pressed (GtkGestureClick *click, int n_press, double x, double y, NeoPressable *self)
{
  if (self->scale <= 0 || neo_pick_is_tappable (GTK_WIDGET (self), x, y)) {
    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  clear_timeouts (self);
  self->down = TRUE;
  self->long_pressed = FALSE;
  self->down_x = x;
  self->down_y = y;
  if (in_scroll_view (GTK_WIDGET (self)))
    self->tap_timeout = g_timeout_add (TAP_TIMEOUT_MS, on_tap_timeout, self);
  else
    on_tap_timeout (self);
  if (g_signal_has_handler_pending (self, signals[LONG_PRESSED], 0, FALSE))
    self->long_press_timeout = g_timeout_add (LONG_PRESS_TIMEOUT_MS, on_long_press_timeout, self);
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, NeoPressable *self)
{
  if (!self->down)
    return;
  self->down = FALSE;
  /* a tap quicker than the tap timeout: Android sets the pressed state now, so the ripple still plays */
  if (self->tap_timeout) {
    g_clear_handle_id (&self->tap_timeout, g_source_remove);
    on_tap_timeout (self);
  }
  g_clear_handle_id (&self->long_press_timeout, g_source_remove);
  neo_ripple_release (&self->ripple);
  if (!self->long_pressed)
    g_signal_emit (self, signals[CLICKED], 0);
}

/* the press is lost: the finger moved away (on_stopped, on_update) or a scroll view took the touch ("cancel") */
static void
on_lost (GtkGesture *gesture, gpointer unused, NeoPressable *self)
{
  if (!self->down)
    return;
  self->down = FALSE;
  clear_timeouts (self);
  neo_ripple_release (&self->ripple);
}

/* the finger is up, or further from where it went down than a click allows (gtk-double-click-distance, the
 * distance GtkGestureClick itself stops at) */
static gboolean
moved_away (NeoPressable *self, GtkGesture *gesture)
{
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  int distance = 5;
  double x, y;

  if (!gtk_gesture_get_point (gesture, sequence, &x, &y))
    return TRUE;
  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-double-click-distance", &distance, NULL);
  return hypot (x - self->down_x, y - self->down_y) > distance;
}

/* GtkGestureClick stops for two reasons: the finger moved away, or its double-click time (400 ms) ran out with the
 * finger still down. Only the first loses the press: the second is a press held, which goes on to be a long press
 * (whose 400 ms the click's timer would otherwise always beat) */
static void
on_stopped (GtkGestureClick *click, NeoPressable *self)
{
  if (moved_away (self, GTK_GESTURE (click)))
    on_lost (GTK_GESTURE (click), NULL, self);
}

/* once the click has stopped on its timer it says nothing more, so a finger moving away after that is seen here */
static void
on_update (GtkGesture *gesture, GdkEventSequence *sequence, NeoPressable *self)
{
  if (self->down && moved_away (self, gesture))
    on_lost (gesture, NULL, self);
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

/* the widget's shape: its box with rounded corners, or a circle around its centre */
static GskRoundedRect
shape_rect (NeoPressable *self, double w, double h)
{
  GskRoundedRect rect;
  if (self->shape == NEO_RIPPLE_CIRCLE) {
    double r = self->radius;
    gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (w / 2 - r, h / 2 - r, 2 * r, 2 * r), r);
  } else {
    gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (0, 0, w, h), self->radius);
  }
  return rect;
}

static void
neo_pressable_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NeoPressable *self = NEO_PRESSABLE (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  if (self->scale <= 0)
    return;

  /* everything below scales with the button: the shadow and the fill too, which is why they are drawn here
   * rather than by CSS (GTK draws a CSS background before this, outside the scale) */
  gtk_snapshot_save (snapshot);
  if (self->scale != 1) {
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (w / 2, h / 2));
    gtk_snapshot_scale (snapshot, self->scale, self->scale);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-w / 2, -h / 2));
  }

  GskRoundedRect shape = shape_rect (self, w, h);
  if (self->elevated) {
    /* Material 3 elevation level 3 (a FAB's 6 dp): a key shadow and an ambient one */
    gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.15f }, 0, 4, 3, 8);
    gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.3f }, 0, 1, 0, 3);
  }
  gboolean ripple = self->shape != NEO_RIPPLE_NONE && neo_ripple_is_active (&self->ripple);
  if (self->fill.alpha > 0 || ripple) {
    gtk_snapshot_push_rounded_clip (snapshot, &shape);
    if (self->fill.alpha > 0)
    {
      GdkRGBA fill = neo_theme_map_rgba (&self->fill);   /* an app's own colour, in the Renaissance look */
      gtk_snapshot_append_color (snapshot, &fill, &shape.bounds);
    }
    if (ripple)
      /* the wave grows over the widget's box, from where the finger is, whatever its clip */
      neo_ripple_snapshot (&self->ripple, snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h), &self->color);
    gtk_snapshot_pop (snapshot);
  }

  if (self->child)
    gtk_widget_snapshot_child (widget, self->child, snapshot);
  gtk_snapshot_restore (snapshot);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
neo_pressable_dispose (GObject *object)
{
  NeoPressable *self = NEO_PRESSABLE (object);
  clear_timeouts (self);
  neo_ripple_clear (&self->ripple);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (neo_pressable_parent_class)->dispose (object);
}

static void
neo_pressable_unmap (GtkWidget *widget)
{
  NeoPressable *self = NEO_PRESSABLE (widget);
  self->down = FALSE;
  clear_timeouts (self);
  neo_ripple_clear (&self->ripple);
  GTK_WIDGET_CLASS (neo_pressable_parent_class)->unmap (widget);
}

/* "activate": a click that came from elsewhere than a finger, such as the accessibility bus's DoAction (the shell's
 * back gesture presses a "Back" button that way) */
static void
on_activate (NeoPressable *self)
{
  g_signal_emit (self, signals[CLICKED], 0);
}

static void
on_click_action (GtkWidget *widget, const char *action, GVariant *parameter)
{
  g_signal_emit (widget, signals[CLICKED], 0);
}

static void
on_long_press_action (GtkWidget *widget, const char *action, GVariant *parameter)
{
  g_signal_emit (widget, signals[LONG_PRESSED], 0);
}

static void
neo_pressable_class_init (NeoPressableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = neo_pressable_dispose;
  widget_class->snapshot = neo_pressable_snapshot;
  widget_class->unmap = neo_pressable_unmap;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "neo-pressable");

  signals[CLICKED] = g_signal_new ("clicked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                        NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[ACTIVATE] = g_signal_new_class_handler ("activate", G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, G_CALLBACK (on_activate),
                                                  NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_set_activate_signal (widget_class, signals[ACTIVATE]);
  /* GTK offers a custom widget's class actions as its AT-SPI actions, the last installed first: 0 clicks it (what
   * the back gesture's DoAction (0) does), 1 long-presses it (TalkBack's long-click) */
  gtk_widget_class_install_action (widget_class, "pressable.long-press", NULL, on_long_press_action);
  gtk_widget_class_install_action (widget_class, "pressable.click", NULL, on_click_action);
  /* a button to assistive technology: its label is read, and it can be pressed (GENERIC would drop the label) */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
}

static void
neo_pressable_init (NeoPressable *self)
{
  self->scale = 1;
  self->color = neo_ripple_default_color ();
  g_object_set_data (G_OBJECT (self), "neo-tappable", GINT_TO_POINTER (TRUE));
  neo_ripple_init (&self->ripple, GTK_WIDGET (self));

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_stopped), self);
  g_signal_connect (click, "update", G_CALLBACK (on_update), self);
  g_signal_connect (click, "cancel", G_CALLBACK (on_lost), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
neo_pressable_new (GtkWidget *child)
{
  NeoPressable *self = g_object_new (NEO_TYPE_PRESSABLE, NULL);
  neo_pressable_set_child (self, child);
  return GTK_WIDGET (self);
}

void
neo_pressable_set_child (NeoPressable *self, GtkWidget *child)
{
  g_clear_pointer (&self->child, gtk_widget_unparent);
  self->child = child;
  if (child)
    gtk_widget_set_parent (child, GTK_WIDGET (self));
}

GtkWidget *
neo_pressable_get_child (NeoPressable *self)
{
  return self->child;
}

void
neo_pressable_set_ripple (NeoPressable *self, NeoRippleShape shape, double radius)
{
  self->shape = shape;
  self->radius = radius;
}

void
neo_pressable_set_ripple_color (NeoPressable *self, const GdkRGBA *color)
{
  self->color = *color;
}

void
neo_pressable_set_fill (NeoPressable *self, const GdkRGBA *fill, gboolean elevated)
{
  self->fill = *fill;
  self->elevated = elevated;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
neo_pressable_set_scale (NeoPressable *self, double scale)
{
  self->scale = scale;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
neo_pressable_get_scale (NeoPressable *self)
{
  return self->scale;
}

/* ---- ready-made buttons ---------------------------------------------------------------------------------------- */

GtkWidget *
neo_text_button_new (const char *label)
{
  GtkWidget *text = gtk_label_new (label);
  /* a button is a text view too: its 14 sp line, font padding and all, centred in the button */
  neo_label_set_font_padding (text, 14);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  GtkWidget *button = neo_pressable_new (text);
  gtk_widget_add_css_class (button, "neo-text-button");
  /* M3 text buttons are 40 dp pills inside a 48 dp target */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_BOUNDED, 20);
  return button;
}

GtkWidget *
neo_icon_button_new (const char *icon_name)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  GtkWidget *button = neo_pressable_new (image);
  gtk_widget_add_css_class (button, "neo-icon-button");
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, 20);
  return button;
}

GtkWidget *
neo_fab_new (const char *icon_name, gboolean large)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  GtkWidget *fab = neo_pressable_new (image);
  gtk_widget_add_css_class (fab, large ? "neo-fab" : "neo-fab-small");
  gtk_widget_set_halign (fab, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (fab, GTK_ALIGN_CENTER);
  GdkRGBA fill = neo_color (large ? NEO_PRIMARY : NEO_SECONDARY);
  neo_pressable_set_fill (NEO_PRESSABLE (fab), &fill, TRUE);
  neo_pressable_set_ripple (NEO_PRESSABLE (fab), NEO_RIPPLE_CIRCLE, large ? 44 : 28);
  return fab;
}

void
neo_fab_set_icon (NeoPressable *fab, const char *icon_name)
{
  gtk_image_set_from_icon_name (GTK_IMAGE (fab->child), icon_name);
}
