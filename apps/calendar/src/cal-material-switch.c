/* CalMaterialSwitch (cal-material-switch.h): libneo's NeoSwitch without icons, over the app's palette. */
#include "cal-material-switch.h"
#include "cal-colors.h"

#include <neo.h>

#define TRACK_W        52
#define TRACK_H        32
#define TRACK_OUTLINE  2
#define THUMB          24
#define THUMB_OFF      16      /* MaterialSwitch's unchecked thumb without an icon */
#define THUMB_INSET    4
#define TARGET_H       48
#define TOGGLE_MS      250

struct _CalMaterialSwitch {
  GtkWidget parent;
  gboolean active;
  double position;              /* 0 off … 1 on */
  NeoAnim *slide;
};

G_DEFINE_FINAL_TYPE (CalMaterialSwitch, cal_material_switch, GTK_TYPE_WIDGET)

enum { PROP_0, PROP_ACTIVE, N_PROPS };
static GParamSpec *props[N_PROPS];

static void
fill_round (GtkSnapshot *snapshot, double x, double y, double w, double h, const GdkRGBA *color)
{
  GskRoundedRect rect;

  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (x, y, w, h), h / 2.0);
  gtk_snapshot_push_rounded_clip (snapshot, &rect);
  gtk_snapshot_append_color (snapshot, color, &rect.bounds);
  gtk_snapshot_pop (snapshot);
}

static void
cal_material_switch_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalMaterialSwitch *self = CAL_MATERIAL_SWITCH (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  double x = (w - TRACK_W) / 2, y = (h - TRACK_H) / 2;
  float t = self->position;
  GdkRGBA track_off = cal_argb (CAL_SURFACE_CONTAINER_HIGHEST), track_on = cal_argb (CAL_PRIMARY);
  GdkRGBA outline_off = cal_argb (CAL_OUTLINE), outline_on = cal_argb (CAL_PRIMARY);
  GdkRGBA thumb_off = cal_argb (CAL_OUTLINE), thumb_on = cal_argb (CAL_ON_PRIMARY);
  GdkRGBA track, outline, thumb;
  double size, travel, cx, cy;

  outline_on.alpha = 0;
  track = neo_rgba_mix (&track_off, &track_on, t);
  outline = neo_rgba_mix (&outline_off, &outline_on, t);
  thumb = neo_rgba_mix (&thumb_off, &thumb_on, t);
  fill_round (snapshot, x, y, TRACK_W, TRACK_H, &track);
  if (outline.alpha > 0)
    {
      GskRoundedRect rect;
      float widths[4] = { TRACK_OUTLINE, TRACK_OUTLINE, TRACK_OUTLINE, TRACK_OUTLINE };
      GdkRGBA colors[4] = { outline, outline, outline, outline };

      gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (x, y, TRACK_W, TRACK_H), TRACK_H / 2.0);
      gtk_snapshot_append_border (snapshot, &rect, widths, colors);
    }
  /* the thumb's centre slides between the centres of its 24 dp places, growing from 16 dp to 24 */
  size = THUMB_OFF + (THUMB - THUMB_OFF) * t;
  travel = TRACK_W - 2 * THUMB_INSET - THUMB;
  cx = x + THUMB_INSET + THUMB / 2.0 + travel * t;
  cy = y + TRACK_H / 2.0;
  fill_round (snapshot, cx - size / 2, cy - size / 2, size, size, &thumb);
}

static void
cal_material_switch_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                             int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? TRACK_W : TARGET_H;
}

static void
on_slide (NeoAnim *anim, double value, gpointer data)
{
  CalMaterialSwitch *self = data;

  self->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_released (GtkGestureClick *click, int n_press, double x, double y, CalMaterialSwitch *self)
{
  cal_material_switch_set_active (self, !self->active);
}

static void
cal_material_switch_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    g_value_set_boolean (value, CAL_MATERIAL_SWITCH (object)->active);
}

static void
cal_material_switch_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == PROP_ACTIVE)
    cal_material_switch_set_active (CAL_MATERIAL_SWITCH (object), g_value_get_boolean (value));
}

static void
cal_material_switch_dispose (GObject *object)
{
  g_clear_pointer (&CAL_MATERIAL_SWITCH (object)->slide, neo_anim_free);
  G_OBJECT_CLASS (cal_material_switch_parent_class)->dispose (object);
}

/* the tap as an action, so assistive technologies can toggle it too (AT-SPI lists a widget's actions) */
static void
switch_toggle (GtkWidget *widget, const char *name, GVariant *parameter)
{
  CalMaterialSwitch *self = CAL_MATERIAL_SWITCH (widget);

  cal_material_switch_set_active (self, !self->active);
}

static void
cal_material_switch_class_init (CalMaterialSwitchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = cal_material_switch_get_property;
  object_class->set_property = cal_material_switch_set_property;
  object_class->dispose = cal_material_switch_dispose;
  widget_class->snapshot = cal_material_switch_snapshot;
  widget_class->measure = cal_material_switch_measure;
  gtk_widget_class_set_css_name (widget_class, "neo-switch");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_SWITCH);
  gtk_widget_class_install_action (widget_class, "switch.toggle", NULL, switch_toggle);
  props[PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
cal_material_switch_init (CalMaterialSwitch *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->slide = neo_anim_new (GTK_WIDGET (self), on_slide, self);
  /* a pressable around it leaves its taps to it */
  g_object_set_data (G_OBJECT (self), "neo-tappable", GINT_TO_POINTER (TRUE));
  g_signal_connect (click, "released", G_CALLBACK (on_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
cal_material_switch_new (void)
{
  return g_object_new (CAL_TYPE_MATERIAL_SWITCH, NULL);
}

gboolean
cal_material_switch_get_active (CalMaterialSwitch *self)
{
  return self->active;
}

void
cal_material_switch_set_active (CalMaterialSwitch *self, gboolean active)
{
  active = !!active;
  if (active == self->active)
    return;
  self->active = active;
  /* from where the thumb is, accelerate-decelerate (SwitchCompat) */
  neo_anim_start (self->slide, self->position, active, TOGGLE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_CHECKED,
                               active ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
}

/* --- CalCheckBox */

#define BOX 18
#define BOX_STROKE 2
#define BOX_TARGET 48

struct _CalCheckBox {
  GtkWidget parent;
  gboolean active;
};

G_DEFINE_FINAL_TYPE (CalCheckBox, cal_check_box, GTK_TYPE_WIDGET)

enum { BOX_PROP_0, BOX_PROP_ACTIVE, BOX_N_PROPS };
static GParamSpec *box_props[BOX_N_PROPS];

static void
cal_check_box_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalCheckBox *self = CAL_CHECK_BOX (widget);
  double x = (gtk_widget_get_width (widget) - BOX) / 2.0, y = (gtk_widget_get_height (widget) - BOX) / 2.0;
  GskRoundedRect rect;
  float alpha = gtk_widget_get_sensitive (widget) ? 1 : 0.38f;

  gsk_rounded_rect_init_from_rect (&rect, &GRAPHENE_RECT_INIT (x, y, BOX, BOX), 2);
  if (self->active)
    {
      GdkRGBA fill = cal_argb (CAL_PRIMARY), mark = cal_argb (CAL_ON_PRIMARY);
      g_autoptr(GskPathBuilder) builder = gsk_path_builder_new ();
      g_autoptr(GskPath) path = NULL;
      g_autoptr(GskStroke) stroke = gsk_stroke_new (2);

      fill.alpha *= alpha;
      gtk_snapshot_push_rounded_clip (snapshot, &rect);
      gtk_snapshot_append_color (snapshot, &fill, &rect.bounds);
      gtk_snapshot_pop (snapshot);
      /* ic_mtrl_checked_circle's tick in the 18 dp box */
      gsk_path_builder_move_to (builder, x + 4, y + 9.2);
      gsk_path_builder_line_to (builder, x + 7.4, y + 12.6);
      gsk_path_builder_line_to (builder, x + 14, y + 6);
      path = gsk_path_builder_free_to_path (g_steal_pointer (&builder));
      gtk_snapshot_append_stroke (snapshot, path, stroke, &mark);
    }
  else
    {
      GdkRGBA outline = cal_argb (CAL_ON_SURFACE_VARIANT);
      float widths[4] = { BOX_STROKE, BOX_STROKE, BOX_STROKE, BOX_STROKE };
      GdkRGBA colors[4];

      outline.alpha *= alpha;
      for (int i = 0; i < 4; i++)
        colors[i] = outline;
      gtk_snapshot_append_border (snapshot, &rect, widths, colors);
    }
}

static void
cal_check_box_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = BOX_TARGET;
}

static void
on_box_released (GtkGestureClick *click, int n_press, double x, double y, CalCheckBox *self)
{
  cal_check_box_set_active (self, !self->active);
}

static void
cal_check_box_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == BOX_PROP_ACTIVE)
    g_value_set_boolean (value, CAL_CHECK_BOX (object)->active);
}

static void
cal_check_box_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == BOX_PROP_ACTIVE)
    cal_check_box_set_active (CAL_CHECK_BOX (object), g_value_get_boolean (value));
}

static void
box_toggle (GtkWidget *widget, const char *name, GVariant *parameter)
{
  CalCheckBox *self = CAL_CHECK_BOX (widget);

  cal_check_box_set_active (self, !self->active);
}

static void
cal_check_box_class_init (CalCheckBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = cal_check_box_get_property;
  object_class->set_property = cal_check_box_set_property;
  widget_class->snapshot = cal_check_box_snapshot;
  widget_class->measure = cal_check_box_measure;
  gtk_widget_class_set_css_name (widget_class, "cal-check-box");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_CHECKBOX);
  gtk_widget_class_install_action (widget_class, "checkbox.toggle", NULL, box_toggle);
  box_props[BOX_PROP_ACTIVE] = g_param_spec_boolean ("active", NULL, NULL, FALSE,
                                                     G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY
                                                     | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, BOX_N_PROPS, box_props);
}

static void
cal_check_box_init (CalCheckBox *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  g_object_set_data (G_OBJECT (self), "neo-tappable", GINT_TO_POINTER (TRUE));
  g_signal_connect (click, "released", G_CALLBACK (on_box_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
cal_check_box_new (void)
{
  return g_object_new (CAL_TYPE_CHECK_BOX, NULL);
}

gboolean
cal_check_box_get_active (CalCheckBox *self)
{
  return self->active;
}

void
cal_check_box_set_active (CalCheckBox *self, gboolean active)
{
  active = !!active;
  if (active == self->active)
    return;
  self->active = active;
  gtk_accessible_update_state (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_STATE_CHECKED,
                               active ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_object_notify_by_pspec (G_OBJECT (self), box_props[BOX_PROP_ACTIVE]);
}
