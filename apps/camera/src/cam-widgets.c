/* cam-widgets: see cam-widgets.h. Geometry and timing are spec/camera.md's (cam-constants.h). */
#include "cam-widgets.h"
#include "cam-constants.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

#define FAMILY "Neo Roboto, Roboto, sans-serif"

/* a tap: down and up on the widget, "clicked" */
static void
on_click_released (GtkGestureClick *gesture, int n, double x, double y, gpointer data)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  if (gtk_widget_contains (widget, x, y))
    g_signal_emit_by_name (widget, "clicked");
}

static void
add_click (GtkWidget *widget)
{
  GtkGesture *click = gtk_gesture_click_new ();

  g_signal_connect (click, "released", G_CALLBACK (on_click_released), NULL);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (click));
}

static guint
new_clicked_signal (GType type)
{
  return g_signal_new ("clicked", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
on_click_action (GtkWidget *widget, const char *action, GVariant *parameter)
{
  if (gtk_widget_is_sensitive (widget))
    g_signal_emit_by_name (widget, "clicked");
}

/* a button to assistive technology, which can press it: GTK offers a widget's class actions as its AT-SPI actions
 * (as NeoPressable does) */
static void
install_button_role (GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action (widget_class, "cam.click", NULL, on_click_action);
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
}

static PangoLayout *
make_layout (GtkWidget *widget, const char *text, double size, int weight)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);
  PangoFontDescription *font = pango_font_description_from_string (FAMILY);

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, weight);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  return layout;
}

static void
fill_circle (GtkSnapshot *snapshot, double cx, double cy, double r, const GdkRGBA *color)
{
  GskRoundedRect round;

  if (r <= 0)
    return;
  gsk_rounded_rect_init_from_rect (&round, &GRAPHENE_RECT_INIT (cx - r, cy - r, 2 * r, 2 * r), r);
  gtk_snapshot_push_rounded_clip (snapshot, &round);
  gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT (cx - r, cy - r, 2 * r, 2 * r));
  gtk_snapshot_pop (snapshot);
}

/* ---- CamRotator ------------------------------------------------------------------------------------------------ */

struct _CamRotator {
  GtkWidget  parent;
  GtkWidget *child;
  double     angle;
  NeoAnim   *anim;
};

G_DEFINE_FINAL_TYPE (CamRotator, cam_rotator, GTK_TYPE_WIDGET)

static void
cam_rotator_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                     int *nat_base)
{
  CamRotator *self = CAM_ROTATOR (widget);

  if (self->child)
    gtk_widget_measure (self->child, orientation, for_size, min, nat, NULL, NULL);
}

static void
cam_rotator_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CamRotator *self = CAM_ROTATOR (widget);
  GskTransform *transform = NULL;

  if (!self->child)
    return;
  /* about the centre; the child's touches turn with it */
  transform = gsk_transform_translate (transform, &GRAPHENE_POINT_INIT (width / 2.0, height / 2.0));
  transform = gsk_transform_rotate (transform, self->angle);
  transform = gsk_transform_translate (transform, &GRAPHENE_POINT_INIT (-width / 2.0, -height / 2.0));
  gtk_widget_allocate (self->child, width, height, -1, transform);
}

static void
on_rotator_anim (NeoAnim *anim, double value, gpointer data)
{
  CamRotator *self = data;

  self->angle = value;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
cam_rotator_dispose (GObject *object)
{
  CamRotator *self = CAM_ROTATOR (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (cam_rotator_parent_class)->dispose (object);
}

static void
cam_rotator_class_init (CamRotatorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_rotator_dispose;
  widget_class->measure = cam_rotator_measure;
  widget_class->size_allocate = cam_rotator_size_allocate;
}

static void
cam_rotator_init (CamRotator *self)
{
  self->anim = neo_anim_new (GTK_WIDGET (self), on_rotator_anim, self);
}

GtkWidget *
cam_rotator_new (GtkWidget *child)
{
  CamRotator *self = g_object_new (CAM_TYPE_ROTATOR, NULL);

  self->child = child;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

GtkWidget *
cam_rotator_get_child (CamRotator *self)
{
  return self->child;
}

void
cam_rotator_set_angle (CamRotator *self, double angle, gboolean animate)
{
  double from = neo_anim_is_running (self->anim) ? neo_anim_get_to (self->anim) : self->angle;
  /* the shortest way round (Rotation.getDifference), from where it is going */
  double diff = fmod (fmod (angle - from, 360) + 540, 360) - 180;

  if (fabs (diff) < 0.01)
    return;
  if (!animate)
    {
      neo_anim_stop (self->anim);
      self->angle = from + diff;
      gtk_widget_queue_allocate (GTK_WIDGET (self));
      return;
    }
  neo_anim_start (self->anim, self->angle, from + diff, CAM_MS_ROTATE, 0, SPONGY_ACCELERATE_DECELERATE);
}

/* ---- CamIcon --------------------------------------------------------------------------------------------------- */

struct _CamIcon {
  GtkWidget  parent;
  char      *name;
  double     size;
  gboolean   has_color;
  GdkRGBA    color;
};

G_DEFINE_FINAL_TYPE (CamIcon, cam_icon, GTK_TYPE_WIDGET)

static void
cam_icon_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                  int *nat_base)
{
  *min = *nat = (int) ceil (CAM_ICON (widget)->size);
}

static void
cam_icon_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamIcon *self = CAM_ICON (widget);
  GdkRGBA color = self->has_color ? self->color : gtk_widget_is_sensitive (widget) ? CAM_C (0xFFFFFF)
                                                                                     : CAM_C (CAM_C_DARK_GREY);

  if (self->name)
    neo_snapshot_icon (snapshot, widget, self->name, (gtk_widget_get_width (widget) - self->size) / 2,
                       (gtk_widget_get_height (widget) - self->size) / 2, self->size, &color);
}

static void
cam_icon_finalize (GObject *object)
{
  g_free (CAM_ICON (object)->name);
  G_OBJECT_CLASS (cam_icon_parent_class)->finalize (object);
}

static void
cam_icon_class_init (CamIconClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = cam_icon_finalize;
  widget_class->measure = cam_icon_measure;
  widget_class->snapshot = cam_icon_snapshot;
}

static void
cam_icon_init (CamIcon *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
cam_icon_new (const char *icon_name, double size)
{
  CamIcon *self = g_object_new (CAM_TYPE_ICON, NULL);

  self->name = g_strdup (icon_name);
  self->size = size;
  return GTK_WIDGET (self);
}

void
cam_icon_set_icon (CamIcon *self, const char *icon_name)
{
  if (!g_strcmp0 (icon_name, self->name))
    return;
  g_free (self->name);
  self->name = g_strdup (icon_name);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_icon_set_color (CamIcon *self, const GdkRGBA *color)
{
  self->has_color = color != NULL;
  if (color)
    self->color = *color;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- CamShutter ------------------------------------------------------------------------------------------------ */

struct _CamShutter {
  GtkWidget  parent;
  double     cap_scale;         /* the capture disc: 1 photo, 0.3125 video's dot, 0.0625 recording */
  double     cap_white;         /* its colour, secondaryContainer 0 … white 1 */
  double     cap_alpha;
  double     square;            /* the record disc, circle 0 … 18 dp square 1 */
  double     pulse;             /* photo capture, 0 … 1 */
  NeoAnim   *scale_anim, *white_anim, *alpha_anim, *square_anim, *pulse_anim;
};

G_DEFINE_FINAL_TYPE (CamShutter, cam_shutter, GTK_TYPE_WIDGET)

static guint shutter_clicked;

static void
cam_shutter_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                     int *nat_base)
{
  *min = *nat = CAM_SHUTTER_VIEW;
}

static void
cam_shutter_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamShutter *self = CAM_SHUTTER (widget);
  double cx = gtk_widget_get_width (widget) / 2.0, cy = gtk_widget_get_height (widget) / 2.0;
  /* avd_photo_capture: both discs to 0.625 and back */
  double pulse = 1 - (1 - CAM_PHOTO_PULSE_SCALE) * self->pulse;
  GdkRGBA white = CAM_C (0xFFFFFF), backplate = CAM_C (CAM_C_GRAY_10), red = CAM_C (CAM_C_REC_RED);
  GdkRGBA container = neo_color (NEO_SECONDARY_CONTAINER);
  GdkRGBA cap = neo_rgba_mix (&container, &white, self->cap_white);
  double half = (32 + (9 - 32) * self->square) * pulse, radius = 32 * (1 - self->square) * pulse;
  GskRoundedRect record;

  /* ic_shutter_base: the backplate's 4-unit white stroke around its gray_10 fill (r 32 … 36), the vector unscaled */
  fill_circle (snapshot, cx, cy, 36, &white);
  fill_circle (snapshot, cx, cy, 32, &backplate);
  /* the record disc, morphing into the 18 × 18 square of avd_video_start */
  gsk_rounded_rect_init_from_rect (&record, &GRAPHENE_RECT_INIT (cx - half, cy - half, 2 * half, 2 * half), radius);
  gtk_snapshot_push_rounded_clip (snapshot, &record);
  gtk_snapshot_append_color (snapshot, &red, &record.bounds);
  gtk_snapshot_pop (snapshot);
  cap.alpha *= self->cap_alpha;
  fill_circle (snapshot, cx, cy, 32 * self->cap_scale * pulse, &cap);
}

#define SHUTTER_SETTER(field)                                                                                       \
  static void on_shutter_##field (NeoAnim *anim, double value, gpointer data)                                     \
  {                                                                                                                 \
    CAM_SHUTTER (data)->field = value;                                                                              \
    gtk_widget_queue_draw (GTK_WIDGET (data));                                                                      \
  }
SHUTTER_SETTER (cap_scale)
SHUTTER_SETTER (cap_white)
SHUTTER_SETTER (cap_alpha)
SHUTTER_SETTER (square)

static void
on_shutter_pulse (NeoAnim *anim, double value, gpointer data)
{
  /* 100 ms out, then the same 100 ms reversed (repeatMode reverse plays the curve backwards) */
  CAM_SHUTTER (data)->pulse = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, value <= 1 ? value : 2 - value);
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
cam_shutter_dispose (GObject *object)
{
  CamShutter *self = CAM_SHUTTER (object);

  g_clear_pointer (&self->scale_anim, neo_anim_free);
  g_clear_pointer (&self->white_anim, neo_anim_free);
  g_clear_pointer (&self->alpha_anim, neo_anim_free);
  g_clear_pointer (&self->square_anim, neo_anim_free);
  g_clear_pointer (&self->pulse_anim, neo_anim_free);
  G_OBJECT_CLASS (cam_shutter_parent_class)->dispose (object);
}

static void
cam_shutter_class_init (CamShutterClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_shutter_dispose;
  widget_class->measure = cam_shutter_measure;
  widget_class->snapshot = cam_shutter_snapshot;
  shutter_clicked = new_clicked_signal (CAM_TYPE_SHUTTER);
  install_button_role (widget_class);
}

static void
cam_shutter_init (CamShutter *self)
{
  self->cap_scale = 1;
  self->cap_alpha = 1;
  self->scale_anim = neo_anim_new (GTK_WIDGET (self), on_shutter_cap_scale, self);
  self->white_anim = neo_anim_new (GTK_WIDGET (self), on_shutter_cap_white, self);
  self->alpha_anim = neo_anim_new (GTK_WIDGET (self), on_shutter_cap_alpha, self);
  self->square_anim = neo_anim_new (GTK_WIDGET (self), on_shutter_square, self);
  self->pulse_anim = neo_anim_new (GTK_WIDGET (self), on_shutter_pulse, self);
  add_click (GTK_WIDGET (self));
}

GtkWidget *
cam_shutter_new (void)
{
  GtkWidget *self = g_object_new (CAM_TYPE_SHUTTER, NULL);

  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Shutter"), -1);
  return self;
}

static void
shutter_to (NeoAnim *anim, double *field, double to, guint ms, SpongyCurve curve, gboolean animate, GtkWidget *w)
{
  if (animate && gtk_widget_get_mapped (w))
    neo_anim_start (anim, *field, to, ms, 0, curve);
  else
    {
      neo_anim_stop (anim);
      *field = to;
      gtk_widget_queue_draw (w);
    }
}

void
cam_shutter_set_video (CamShutter *self, gboolean video, gboolean animate)
{
  GtkWidget *w = GTK_WIDGET (self);

  /* avd_mode_photo_video: scale 300 ms FOSI, colour 300 ms AD */
  shutter_to (self->scale_anim, &self->cap_scale, video ? CAM_VIDEO_DOT_SCALE : 1, CAM_MS_SHUTTER_MORPH,
              SPONGY_FAST_OUT_SLOW_IN, animate, w);
  shutter_to (self->white_anim, &self->cap_white, video ? 1 : 0, CAM_MS_SHUTTER_MORPH, SPONGY_ACCELERATE_DECELERATE,
              animate, w);
  shutter_to (self->alpha_anim, &self->cap_alpha, 1, CAM_MS_SHUTTER_MORPH, SPONGY_FAST_OUT_SLOW_IN, animate, w);
  shutter_to (self->square_anim, &self->square, 0, CAM_MS_SHUTTER_MORPH, SPONGY_FAST_OUT_SLOW_IN, animate, w);
}

void
cam_shutter_set_recording (CamShutter *self, gboolean recording, gboolean animate)
{
  GtkWidget *w = GTK_WIDGET (self);

  /* avd_video_start / avd_video_end: 300 ms FOSI each */
  shutter_to (self->scale_anim, &self->cap_scale, recording ? CAM_VIDEO_DOT_END_SCALE : CAM_VIDEO_DOT_SCALE,
              CAM_MS_SHUTTER_MORPH, SPONGY_FAST_OUT_SLOW_IN, animate, w);
  shutter_to (self->alpha_anim, &self->cap_alpha, recording ? 0 : 1, CAM_MS_SHUTTER_MORPH, SPONGY_FAST_OUT_SLOW_IN,
              animate, w);
  shutter_to (self->square_anim, &self->square, recording ? 1 : 0, CAM_MS_SHUTTER_MORPH, SPONGY_FAST_OUT_SLOW_IN,
              animate, w);
}

void
cam_shutter_pulse (CamShutter *self)
{
  neo_anim_start (self->pulse_anim, 0, 2, 2 * CAM_MS_PHOTO_PULSE, 0, SPONGY_LINEAR);
}

/* ---- CamFlip --------------------------------------------------------------------------------------------------- */

/* R/drawable/ic_flip_camera.xml, viewport 32 */
static const char flip_arrow_1[] =
  "M0 16C0 15.3278 0.0423017 14.6599 0.125431 14C0.567068 10.494 2.16104 7.21154 4.68629 4.68629C7.21155 2.16104 "
  "10.494 0.567068 14 0.125431C14.6599 0.0423017 15.3278 0 16 0H24L16 8V2.01934C12.2921 2.01934 8.73606 3.4923 "
  "6.11418 6.11418C3.4923 8.73606 2.01934 12.2921 2.01934 16H0Z";
static const char flip_arrow_2[] =
  "M29.9807 16C29.9807 17.836 29.619 19.654 28.9164 21.3502C28.2139 23.0464 27.184 24.5876 25.8858 25.8858C24.5876 "
  "27.184 23.0464 28.2139 21.3502 28.9164C19.654 29.619 17.836 29.9807 16 29.9807L16 24L8 32H16C16.67 32 17.338 "
  "31.9579 18 31.8745C19.4139 31.6964 20.8008 31.3297 22.1229 30.7821C24.0641 29.978 25.828 28.7994 27.3137 "
  "27.3137C28.7994 25.828 29.978 24.0641 30.7821 22.1229C31.3297 20.8008 31.6964 19.4139 31.8745 18C31.9579 17.338 "
  "32 16.67 32 16H29.9807Z";

struct _CamFlip {
  GtkWidget  parent;
  GskPath   *arrows;
  GskPath   *ring;
  double     angle;
  NeoAnim   *anim;
};

G_DEFINE_FINAL_TYPE (CamFlip, cam_flip, GTK_TYPE_WIDGET)

static guint flip_clicked;

static void
cam_flip_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                  int *nat_base)
{
  *min = *nat = CAM_FLIP_SIZE;
}

static void
cam_flip_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamFlip *self = CAM_FLIP (widget);
  GdkRGBA white = CAM_C (0xFFFFFF), primary = neo_color (NEO_PRIMARY);
  g_autoptr(GskStroke) stroke = gsk_stroke_new (2);

  /* scaleType center: the 32 dp drawable unscaled in the 60 dp button; the group turns about (16, 16) */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (gtk_widget_get_width (widget) / 2.0,
                                                          gtk_widget_get_height (widget) / 2.0));
  gtk_snapshot_rotate (snapshot, self->angle);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-16, -16));
  gtk_snapshot_append_fill (snapshot, self->arrows, GSK_FILL_RULE_EVEN_ODD, &white);
  gtk_snapshot_append_stroke (snapshot, self->ring, stroke, &primary);
  gtk_snapshot_restore (snapshot);
}

static void
on_flip_anim (NeoAnim *anim, double value, gpointer data)
{
  CamFlip *self = data;

  /* the AVD starts from 0 on every flip; the arrows look the same at −180° */
  self->angle = neo_anim_is_running (anim) ? value : 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cam_flip_dispose (GObject *object)
{
  CamFlip *self = CAM_FLIP (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->arrows, gsk_path_unref);
  g_clear_pointer (&self->ring, gsk_path_unref);
  G_OBJECT_CLASS (cam_flip_parent_class)->dispose (object);
}

static void
cam_flip_class_init (CamFlipClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_flip_dispose;
  widget_class->measure = cam_flip_measure;
  widget_class->snapshot = cam_flip_snapshot;
  flip_clicked = new_clicked_signal (CAM_TYPE_FLIP);
  install_button_role (widget_class);
}

static void
cam_flip_init (CamFlip *self)
{
  g_autofree char *arrows = g_strconcat (flip_arrow_1, " ", flip_arrow_2, NULL);
  GskPathBuilder *ring = gsk_path_builder_new ();

  self->arrows = gsk_path_parse (arrows);
  gsk_path_builder_add_circle (ring, &GRAPHENE_POINT_INIT (16, 16), 3);
  self->ring = gsk_path_builder_free_to_path (ring);
  self->anim = neo_anim_new (GTK_WIDGET (self), on_flip_anim, self);
  add_click (GTK_WIDGET (self));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Flip camera"), -1);
}

GtkWidget *
cam_flip_new (void)
{
  return g_object_new (CAM_TYPE_FLIP, NULL);
}

void
cam_flip_spin (CamFlip *self)
{
  neo_anim_start (self->anim, 0, -180, CAM_MS_FLIP, 0, SPONGY_ACCELERATE_DECELERATE);
}

/* ---- CamPause -------------------------------------------------------------------------------------------------- */

struct _CamPause {
  GtkWidget  parent;
  gboolean   paused;
  double     fill;              /* the backplate, gray_10 0 … gray_60 1 */
  double     bars, play;        /* how present each glyph is, 0 … 1 */
  NeoAnim   *fill_anim, *bars_anim, *play_anim;
  GskPath   *bars_path, *play_path;
};

G_DEFINE_FINAL_TYPE (CamPause, cam_pause, GTK_TYPE_WIDGET)

static guint pause_clicked;

static void
cam_pause_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                   int *nat_base)
{
  *min = *nat = CAM_PAUSE_SIZE;
}

/* a group of the AVD: scaled about (16, 16), then moved by `dx` (VectorDrawable's order) */
static void
pause_glyph (GtkSnapshot *snapshot, GskPath *path, double presence, double dx, const GdkRGBA *color)
{
  double scale = 0.5 + 0.5 * presence;
  GdkRGBA c = *color;

  if (presence <= 0)
    return;
  c.alpha *= presence;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (16 + dx * (1 - presence), 16));
  gtk_snapshot_scale (snapshot, scale, scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-16, -16));
  gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, &c);
  gtk_snapshot_restore (snapshot);
}

static void
cam_pause_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamPause *self = CAM_PAUSE (widget);
  GdkRGBA white = CAM_C (0xFFFFFF), gray10 = CAM_C (CAM_C_GRAY_10), gray60 = CAM_C (CAM_C_GRAY_60);
  GdkRGBA plate = neo_rgba_mix (&gray10, &gray60, self->fill);

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((gtk_widget_get_width (widget) - 32) / 2.0,
                                                          (gtk_widget_get_height (widget) - 32) / 2.0));
  /* the backplate r 15 with its 2-unit white stroke: white to r 16, the fill to r 14 */
  fill_circle (snapshot, 16, 16, 16, &white);
  fill_circle (snapshot, 16, 16, 14, &plate);
  /* the bars leave to −8, the triangle comes from +8 (and back) */
  pause_glyph (snapshot, self->bars_path, self->bars, -8, &gray60);
  pause_glyph (snapshot, self->play_path, self->play, 8, &white);
  gtk_snapshot_restore (snapshot);
}

#define PAUSE_SETTER(field)                                                                                         \
  static void on_pause_##field (NeoAnim *anim, double value, gpointer data)                                       \
  {                                                                                                                 \
    CAM_PAUSE (data)->field = value;                                                                                \
    gtk_widget_queue_draw (GTK_WIDGET (data));                                                                      \
  }
PAUSE_SETTER (fill)
PAUSE_SETTER (bars)
PAUSE_SETTER (play)

static void
cam_pause_dispose (GObject *object)
{
  CamPause *self = CAM_PAUSE (object);

  g_clear_pointer (&self->fill_anim, neo_anim_free);
  g_clear_pointer (&self->bars_anim, neo_anim_free);
  g_clear_pointer (&self->play_anim, neo_anim_free);
  g_clear_pointer (&self->bars_path, gsk_path_unref);
  g_clear_pointer (&self->play_path, gsk_path_unref);
  G_OBJECT_CLASS (cam_pause_parent_class)->dispose (object);
}

static void
cam_pause_class_init (CamPauseClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_pause_dispose;
  widget_class->measure = cam_pause_measure;
  widget_class->snapshot = cam_pause_snapshot;
  pause_clicked = new_clicked_signal (CAM_TYPE_PAUSE);
  install_button_role (widget_class);
}

static void
cam_pause_init (CamPause *self)
{
  self->bars = 1;
  self->bars_path = gsk_path_parse ("M 14 9 L 10 9 L 10 23 L 14 23 L 14 9 Z M 22 9 L 18 9 L 18 23 L 22 23 L 22 9 Z");
  self->play_path = gsk_path_parse ("M 11.5 9 L 22.5 16 L 11.5 23 L 11.5 9 Z");
  self->fill_anim = neo_anim_new (GTK_WIDGET (self), on_pause_fill, self);
  self->bars_anim = neo_anim_new (GTK_WIDGET (self), on_pause_bars, self);
  self->play_anim = neo_anim_new (GTK_WIDGET (self), on_pause_play, self);
  add_click (GTK_WIDGET (self));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Pause/resume video recording"), -1);
}

GtkWidget *
cam_pause_new (void)
{
  return g_object_new (CAM_TYPE_PAUSE, NULL);
}

void
cam_pause_set_paused (CamPause *self, gboolean paused, gboolean animate)
{
  NeoAnim *out = paused ? self->bars_anim : self->play_anim, *in = paused ? self->play_anim : self->bars_anim;
  double *out_v = paused ? &self->bars : &self->play, *in_v = paused ? &self->play : &self->bars;

  self->paused = paused;
  if (!animate || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      neo_anim_stop (self->fill_anim);
      neo_anim_stop (out);
      neo_anim_stop (in);
      self->fill = paused;
      *out_v = 0;
      *in_v = 1;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }
  /* avd_video_recording_pause / _resume: the fill 200 ms LIN after 100 ms, the glyph going 200 ms ACC, the glyph
   * coming 200 ms DEC after 100 ms */
  neo_anim_start (self->fill_anim, self->fill, paused, CAM_MS_PAUSE_MORPH, 100, SPONGY_LINEAR);
  neo_anim_start (out, *out_v, 0, CAM_MS_PAUSE_MORPH, 0, SPONGY_ACCELERATE);
  neo_anim_start (in, *in_v, 1, CAM_MS_PAUSE_MORPH, 100, SPONGY_DECELERATE);
}

/* ---- CamGallery ------------------------------------------------------------------------------------------------ */

struct _CamGallery {
  GtkWidget   parent;
  GdkTexture *texture, *old;
  double      fade;             /* the new thumbnail over the old one, 0 … 1 */
  NeoAnim    *anim;
};

G_DEFINE_FINAL_TYPE (CamGallery, cam_gallery, GTK_TYPE_WIDGET)

static guint gallery_clicked;

static void
cam_gallery_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                     int *nat_base)
{
  *min = *nat = CAM_GALLERY_SIZE;
}

static void
gallery_layer (GtkSnapshot *snapshot, GtkWidget *widget, GdkTexture *texture, double alpha)
{
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget));

  if (alpha <= 0)
    return;
  gtk_snapshot_push_opacity (snapshot, alpha);
  if (texture)
    /* centerCrop: the thumbnail is square already */
    gtk_snapshot_append_scaled_texture (snapshot, texture, GSK_SCALING_FILTER_TRILINEAR, &box);
  else
    {
      GdkRGBA tint = neo_color (NEO_ON_SECONDARY_CONTAINER);
      /* the 60 dp ImageView at scale 0.5 */
      neo_snapshot_icon (snapshot, widget, "cam-image-symbolic", (box.size.width - CAM_GALLERY_ICON) / 2,
                         (box.size.height - CAM_GALLERY_ICON) / 2, CAM_GALLERY_ICON, &tint);
    }
  gtk_snapshot_pop (snapshot);
}

static void
cam_gallery_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamGallery *self = CAM_GALLERY (widget);
  GskRoundedRect card;
  GdkRGBA fill = neo_color (NEO_SECONDARY_CONTAINER);

  gsk_rounded_rect_init_from_rect (&card, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                               gtk_widget_get_height (widget)), CAM_GALLERY_RADIUS);
  gtk_snapshot_push_rounded_clip (snapshot, &card);
  gtk_snapshot_append_color (snapshot, &fill, &card.bounds);
  if (self->fade < 1)
    gallery_layer (snapshot, widget, self->old, 1);
  gallery_layer (snapshot, widget, self->texture, self->fade);
  gtk_snapshot_pop (snapshot);
}

static void
on_gallery_fade (NeoAnim *anim, double value, gpointer data)
{
  CamGallery *self = data;

  self->fade = value;
  if (!neo_anim_is_running (anim))
    g_clear_object (&self->old);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cam_gallery_dispose (GObject *object)
{
  CamGallery *self = CAM_GALLERY (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_object (&self->texture);
  g_clear_object (&self->old);
  G_OBJECT_CLASS (cam_gallery_parent_class)->dispose (object);
}

static void
cam_gallery_class_init (CamGalleryClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_gallery_dispose;
  widget_class->measure = cam_gallery_measure;
  widget_class->snapshot = cam_gallery_snapshot;
  gallery_clicked = new_clicked_signal (CAM_TYPE_GALLERY);
  install_button_role (widget_class);
}

static void
cam_gallery_init (CamGallery *self)
{
  self->fade = 1;
  self->anim = neo_anim_new (GTK_WIDGET (self), on_gallery_fade, self);
  add_click (GTK_WIDGET (self));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Gallery"), -1);
}

GtkWidget *
cam_gallery_new (void)
{
  return g_object_new (CAM_TYPE_GALLERY, NULL);
}

void
cam_gallery_set_thumbnail (CamGallery *self, GdkTexture *texture)
{
  if (texture == self->texture)
    return;
  g_clear_object (&self->old);
  self->old = self->texture;
  self->texture = texture ? g_object_ref (texture) : NULL;
  /* Coil's crossfade, 200 ms; the placeholder comes back at once */
  if (texture && gtk_widget_get_mapped (GTK_WIDGET (self)))
    neo_anim_start (self->anim, 0, 1, CAM_MS_THUMB_CROSSFADE, 0, SPONGY_ACCELERATE_DECELERATE);
  else
    {
      neo_anim_stop (self->anim);
      self->fade = 1;
      g_clear_object (&self->old);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- CamLens --------------------------------------------------------------------------------------------------- */

struct _CamLens {
  GtkWidget  parent;
  char      *text;
  gboolean   selected;
};

G_DEFINE_FINAL_TYPE (CamLens, cam_lens, GTK_TYPE_WIDGET)

static guint lens_clicked;

/* the pill's width: the 32 dp circle, wider for a label that does not fit ("Macro") */
static double
lens_pill_width (GtkWidget *widget, const char *text)
{
  g_autoptr(PangoLayout) layout = make_layout (widget, text, CAM_DP_LENS_TEXT, 800);
  int w;

  pango_layout_get_pixel_size (layout, &w, NULL);
  return MAX (CAM_LENS_BUTTON, w + 2 * CAM_LENS_PAD);
}

static void
cam_lens_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                  int *nat_base)
{
  /* 32 dp and LensSelectorLayout's 5 px margins: 35⅓, rounded up; the pill is drawn centred at its exact size */
  double pill = orientation == GTK_ORIENTATION_HORIZONTAL ? lens_pill_width (widget, CAM_LENS (widget)->text)
                                                          : CAM_LENS_BUTTON;

  *min = *nat = (int) ceil (pill + 2 * CAM_LENS_MARGIN);
}

static void
cam_lens_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamLens *self = CAM_LENS (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  g_autoptr(PangoLayout) layout = make_layout (widget, self->text, CAM_DP_LENS_TEXT, 800);
  GdkRGBA white = CAM_C (0xFFFFFF);
  PangoRectangle logical;

  if (self->selected)
    {
      GskRoundedRect pill;
      GdkRGBA fill = neo_color (NEO_SECONDARY_CONTAINER);

      double pw = lens_pill_width (widget, self->text);

      gsk_rounded_rect_init_from_rect (&pill, &GRAPHENE_RECT_INIT ((w - pw) / 2, (h - CAM_LENS_BUTTON) / 2, pw,
                                                                   CAM_LENS_BUTTON),
                                       CAM_LENS_RADIUS);
      gtk_snapshot_push_rounded_clip (snapshot, &pill);
      gtk_snapshot_append_color (snapshot, &fill, &pill.bounds);
      gtk_snapshot_pop (snapshot);
    }
  pango_layout_get_extents (layout, NULL, &logical);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((w - logical.width / (double) PANGO_SCALE) / 2,
                                                          (h - logical.height / (double) PANGO_SCALE) / 2));
  gtk_snapshot_append_layout (snapshot, layout, &white);
  gtk_snapshot_restore (snapshot);
}

static void
cam_lens_finalize (GObject *object)
{
  g_free (CAM_LENS (object)->text);
  G_OBJECT_CLASS (cam_lens_parent_class)->finalize (object);
}

static void
cam_lens_class_init (CamLensClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = cam_lens_finalize;
  widget_class->measure = cam_lens_measure;
  widget_class->snapshot = cam_lens_snapshot;
  lens_clicked = new_clicked_signal (CAM_TYPE_LENS);
  install_button_role (widget_class);
}

static void
cam_lens_init (CamLens *self)
{
  add_click (GTK_WIDGET (self));
}

GtkWidget *
cam_lens_new (const char *text, gboolean selected)
{
  CamLens *self = g_object_new (CAM_TYPE_LENS, NULL);

  self->text = g_strdup (text);
  self->selected = selected;
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
  return GTK_WIDGET (self);
}

/* ---- CamCountdown ---------------------------------------------------------------------------------------------- */

struct _CamCountdown {
  GtkWidget  parent;
  int        remaining;
  double     t;                 /* the digit's fade, 0 … 1 */
  gboolean   showing;
  guint      tick;
  double     angle;
  NeoAnim   *anim, *turn;
};

G_DEFINE_FINAL_TYPE (CamCountdown, cam_countdown, GTK_TYPE_WIDGET)

static guint countdown_finished;

static void
cam_countdown_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamCountdown *self = CAM_COUNTDOWN (widget);
  g_autofree char *text = NULL;
  g_autoptr(PangoLayout) layout = NULL;
  PangoRectangle logical;
  double scale = 1 + (CAM_COUNTDOWN_END_SCALE - 1) * self->t;
  GdkRGBA white = CAM_C (0xFFFFFF);

  if (!self->showing || self->t >= 1)
    return;
  text = g_strdup_printf ("%d", self->remaining);
  /* 125 sp sans-serif, 400 + Bold text = 700 */
  layout = make_layout (widget, text, CAM_SP_COUNTDOWN, 700);
  pango_layout_get_extents (layout, NULL, &logical);
  white.alpha = 1 - self->t;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (gtk_widget_get_width (widget) / 2.0,
                                                          gtk_widget_get_height (widget) / 2.0));
  gtk_snapshot_rotate (snapshot, self->angle);
  gtk_snapshot_scale (snapshot, scale, scale);
  /* the TextView's box has Roboto's font padding (yMax over the ascent on top, yMin under the descent below, as
   * neo_label_set_font_padding): centring that box puts the digit 0.05 em lower than centring Pango's */
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-logical.width / (2.0 * PANGO_SCALE),
                                                          -logical.height / (2.0 * PANGO_SCALE)
                                                          + CAM_SP_COUNTDOWN * ((2163 - 1900) - (555 - 500))
                                                            / (2.0 * 2048)));
  gtk_snapshot_append_layout (snapshot, layout, &white);
  gtk_snapshot_restore (snapshot);
}

static void
on_countdown_anim (NeoAnim *anim, double value, gpointer data)
{
  CAM_COUNTDOWN (data)->t = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
on_countdown_turn (NeoAnim *anim, double value, gpointer data)
{
  CAM_COUNTDOWN (data)->angle = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
show_digit (CamCountdown *self)
{
  self->showing = TRUE;
  /* scale 1 → 2.5 and alpha 1 → 0, 800 ms AD; the next second 1000 ms on, so a 200 ms gap */
  neo_anim_start (self->anim, 0, 1, CAM_MS_COUNTDOWN_FADE, 0, SPONGY_ACCELERATE_DECELERATE);
}

static gboolean
on_countdown_tick (gpointer data)
{
  CamCountdown *self = data;

  self->remaining--;
  if (self->remaining <= 0)
    {
      self->tick = 0;
      self->showing = FALSE;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      g_signal_emit (self, countdown_finished, 0);
      return G_SOURCE_REMOVE;
    }
  show_digit (self);
  return G_SOURCE_CONTINUE;
}

static void
cam_countdown_dispose (GObject *object)
{
  CamCountdown *self = CAM_COUNTDOWN (object);

  g_clear_handle_id (&self->tick, g_source_remove);
  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->turn, neo_anim_free);
  G_OBJECT_CLASS (cam_countdown_parent_class)->dispose (object);
}

static void
cam_countdown_class_init (CamCountdownClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cam_countdown_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = cam_countdown_snapshot;
  countdown_finished = g_signal_new ("finished", CAM_TYPE_COUNTDOWN, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}

static void
cam_countdown_init (CamCountdown *self)
{
  self->anim = neo_anim_new (GTK_WIDGET (self), on_countdown_anim, self);
  self->turn = neo_anim_new (GTK_WIDGET (self), on_countdown_turn, self);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
cam_countdown_new (void)
{
  return g_object_new (CAM_TYPE_COUNTDOWN, NULL);
}

void
cam_countdown_start (CamCountdown *self, int seconds)
{
  cam_countdown_cancel (self);
  self->remaining = seconds;
  show_digit (self);
  self->tick = g_timeout_add (CAM_MS_COUNTDOWN_TICK, on_countdown_tick, self);
}

void
cam_countdown_cancel (CamCountdown *self)
{
  g_clear_handle_id (&self->tick, g_source_remove);
  neo_anim_stop (self->anim);
  self->showing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
cam_countdown_is_running (CamCountdown *self)
{
  return self->tick != 0;
}

void
cam_countdown_set_angle (CamCountdown *self, double angle, gboolean animate)
{
  double diff = fmod (fmod (angle - self->angle, 360) + 540, 360) - 180;

  if (animate)
    neo_anim_start (self->turn, self->angle, self->angle + diff, CAM_MS_ROTATE, 0, SPONGY_ACCELERATE_DECELERATE);
  else
    {
      self->angle += diff;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* ---- CamZoomSlider --------------------------------------------------------------------------------------------- */

struct _CamZoomSlider {
  GtkWidget  parent;
  double     progress;
  char      *text;
  double     angle;
};

G_DEFINE_FINAL_TYPE (CamZoomSlider, cam_zoom_slider, GTK_TYPE_WIDGET)

static guint zoom_changed;

static void
cam_zoom_slider_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                         int *min_base, int *nat_base)
{
  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? CAM_ZOOM_SLIDER_W : CAM_ZOOM_SLIDER_H;
}

static void
cam_zoom_slider_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamZoomSlider *self = CAM_ZOOM_SLIDER (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  /* HorizontalSlider: a track h/5 tall from h/2 to w − h/2, rounded; the thumb r = h/2.15 */
  double th = h / 5, left = h / 2, right = w - h / 2, r = h / 2.15;
  double cx = left + (right - left) * self->progress, cy = h / 2;
  GskRoundedRect track, thumb;
  GdkRGBA track_color = CAM_CA (CAM_C_ZOOM_TRACK), black = CAM_C (0), white = CAM_C (0xFFFFFF);
  g_autoptr(PangoLayout) layout = NULL;
  PangoRectangle ink;

  gsk_rounded_rect_init_from_rect (&track, &GRAPHENE_RECT_INIT (left, (h - th) / 2, right - left, th), th / 2);
  /* setShadowLayer (1 px, black) */
  gtk_snapshot_append_outset_shadow (snapshot, &track, &black, 0, 0, 0, 1 / 3.0);
  gtk_snapshot_push_rounded_clip (snapshot, &track);
  gtk_snapshot_append_color (snapshot, &track_color, &track.bounds);
  gtk_snapshot_pop (snapshot);

  /* the thumb turns with the phone about its centre */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy));
  gtk_snapshot_rotate (snapshot, self->angle);
  gsk_rounded_rect_init_from_rect (&thumb, &GRAPHENE_RECT_INIT (-r, -r, 2 * r, 2 * r), r);
  gtk_snapshot_append_outset_shadow (snapshot, &thumb, &black, 0, 0, 0, 1);
  gtk_snapshot_push_rounded_clip (snapshot, &thumb);
  gtk_snapshot_append_color (snapshot, &white, &thumb.bounds);
  gtk_snapshot_pop (snapshot);
  if (self->text)
    {
      /* Paint text, not a TextView: 400, no Bold-text boost; centred on its ink (getTextBounds) */
      layout = make_layout (widget, self->text, CAM_DP_SLIDER_TEXT, 400);
      pango_layout_get_pixel_extents (layout, &ink, NULL);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-ink.x - ink.width / 2.0, -ink.y - ink.height / 2.0));
      gtk_snapshot_append_layout (snapshot, layout, &black);
    }
  gtk_snapshot_restore (snapshot);
}

static void
zoom_slider_touch (CamZoomSlider *self, double x)
{
  double w = gtk_widget_get_width (GTK_WIDGET (self));

  /* Slider.onTouchEvent: x over the whole width, not the track */
  self->progress = CLAMP (x, 0, w) / MAX (w, 1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, zoom_changed, 0, self->progress);
}

static void
on_zoom_drag_begin (GtkGestureDrag *drag, double x, double y, gpointer data)
{
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  zoom_slider_touch (data, x);
}

static void
on_zoom_drag_update (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
  double x, y;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  zoom_slider_touch (data, x + dx);
}

static void
cam_zoom_slider_finalize (GObject *object)
{
  g_free (CAM_ZOOM_SLIDER (object)->text);
  G_OBJECT_CLASS (cam_zoom_slider_parent_class)->finalize (object);
}

static void
cam_zoom_slider_class_init (CamZoomSliderClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = cam_zoom_slider_finalize;
  widget_class->measure = cam_zoom_slider_measure;
  widget_class->snapshot = cam_zoom_slider_snapshot;
  zoom_changed = g_signal_new ("changed", CAM_TYPE_ZOOM_SLIDER, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                               1, G_TYPE_DOUBLE);
}

static void
cam_zoom_slider_init (CamZoomSlider *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_zoom_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_zoom_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
cam_zoom_slider_new (void)
{
  return g_object_new (CAM_TYPE_ZOOM_SLIDER, NULL);
}

void
cam_zoom_slider_set (CamZoomSlider *self, double progress, const char *text)
{
  self->progress = CLAMP (progress, 0, 1);
  g_free (self->text);
  self->text = g_strdup (text);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_zoom_slider_set_angle (CamZoomSlider *self, double angle)
{
  self->angle = angle;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- CamIsland ------------------------------------------------------------------------------------------------- */

struct _CamIsland {
  GtkWidget  parent;
  guint      items;
  int        orientation;
  double     alpha;
  NeoAnim   *fade;
};

G_DEFINE_FINAL_TYPE (CamIsland, cam_island, GTK_TYPE_WIDGET)

static int
island_count (guint items)
{
  int n = 0;

  for (guint i = 0; i < 5; i++)
    n += (items >> i) & 1;
  return n;
}

static void
cam_island_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                    int *nat_base)
{
  CamIsland *self = CAM_ISLAND (widget);

  /* 8 dp either side, a 28 dp cell per icon (24 + 2 + 2), at least 36 dp; 36 dp tall */
  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? MAX (CAM_ISLAND_H, 16 + 28 * island_count (self->items))
                                                          : CAM_ISLAND_H;
}

static void
cam_island_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamIsland *self = CAM_ISLAND (widget);
  static const char *const icons[] = { "cam-thermostat-symbolic", "cam-thermostat-symbolic",
                                       "cam-battery-1-bar-symbolic", "cam-battery-charging-20-symbolic",
                                       "cam-mic-off-symbolic" };
  GdkRGBA card = neo_color (NEO_SURFACE), on = neo_color (NEO_ON_SURFACE), error = neo_color (NEO_ERROR);
  GskRoundedRect box;
  double x = 8 + 2;

  if (self->alpha <= 0)
    return;
  gtk_snapshot_push_opacity (snapshot, self->alpha);
  gsk_rounded_rect_init_from_rect (&box, &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                              gtk_widget_get_height (widget)), CAM_ISLAND_RADIUS);
  gtk_snapshot_push_rounded_clip (snapshot, &box);
  gtk_snapshot_append_color (snapshot, &card, &box.bounds);
  for (int i = 0; i < 5; i++)
    if (self->items & (1u << i))
      {
        /* thermal "moderate/severe" in onSurface, everything worse and the battery in colorError */
        const GdkRGBA *tint = i == CAM_ISLAND_THERMAL || i == CAM_ISLAND_MIC_OFF ? &on : &error;

        neo_snapshot_icon (snapshot, widget, icons[i], x, (CAM_ISLAND_H - 24) / 2.0, 24, tint);
        x += 28;
      }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_island_fade (NeoAnim *anim, double value, gpointer data)
{
  CamIsland *self = data;

  self->alpha = value;
  gtk_widget_set_visible (GTK_WIDGET (self), value > 0 || self->items);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cam_island_dispose (GObject *object)
{
  g_clear_pointer (&CAM_ISLAND (object)->fade, neo_anim_free);
  G_OBJECT_CLASS (cam_island_parent_class)->dispose (object);
}

static void
cam_island_class_init (CamIslandClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cam_island_dispose;
  GTK_WIDGET_CLASS (klass)->measure = cam_island_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = cam_island_snapshot;
}

static void
cam_island_init (CamIsland *self)
{
  self->fade = neo_anim_new (GTK_WIDGET (self), on_island_fade, self);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
cam_island_new (void)
{
  return g_object_new (CAM_TYPE_ISLAND, NULL);
}

void
cam_island_set_items (CamIsland *self, guint items)
{
  gboolean was = self->items != 0;

  if (items == self->items)
    return;
  self->items = items;
  gtk_widget_queue_resize (GTK_WIDGET (self));
  /* fades in with its first item and out with its last, config_shortAnimTime */
  if (!was && items)
    {
      gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
      neo_anim_start (self->fade, self->alpha, 1, CAM_MS_ISLAND_FADE, 0, SPONGY_ACCELERATE_DECELERATE);
    }
  else if (was && !items)
    neo_anim_start (self->fade, self->alpha, 0, CAM_MS_ISLAND_FADE, 0, SPONGY_ACCELERATE_DECELERATE);
}

void
cam_island_set_orientation (CamIsland *self, int orientation)
{
  self->orientation = orientation;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

int
cam_island_get_orientation (CamIsland *self)
{
  return self->orientation;
}

/* ---- CamToast -------------------------------------------------------------------------------------------------- */

struct _CamToast {
  GtkWidget  parent;
  GtkWidget *box, *label;
  NeoAnim   *fade;
  guint      timer;
};

G_DEFINE_FINAL_TYPE (CamToast, cam_toast, GTK_TYPE_WIDGET)

static void
cam_toast_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CamToast *self = CAM_TOAST (widget);
  int min, nat, h;
  int w;

  /* centred, 20 dp from either side at most, its bottom 48 dp over the screen's (still:camera-toast-…) */
  gtk_widget_measure (self->box, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  w = MIN (nat, width - 40);
  w = MAX (w, min);
  gtk_widget_measure (self->box, GTK_ORIENTATION_VERTICAL, w, &min, &h, NULL, NULL);
  gtk_widget_size_allocate (self->box, &(GtkAllocation) { (width - w) / 2, height - 48 - h, w, h }, -1);
}

static void
cam_toast_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                   int *nat_base)
{
  *min = *nat = 0;
}

static void
on_toast_fade (NeoAnim *anim, double value, gpointer data)
{
  CamToast *self = data;

  gtk_widget_set_opacity (self->box, value);
  gtk_widget_set_visible (GTK_WIDGET (self), value > 0);
}

static gboolean
on_toast_timeout (gpointer data)
{
  CamToast *self = data;

  self->timer = 0;
  neo_anim_start (self->fade, gtk_widget_get_opacity (self->box), 0, 250, 0, SPONGY_ACCELERATE_DECELERATE);
  return G_SOURCE_REMOVE;
}

static void
cam_toast_dispose (GObject *object)
{
  CamToast *self = CAM_TOAST (object);

  g_clear_handle_id (&self->timer, g_source_remove);
  g_clear_pointer (&self->fade, neo_anim_free);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (cam_toast_parent_class)->dispose (object);
}

static void
cam_toast_class_init (CamToastClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cam_toast_dispose;
  GTK_WIDGET_CLASS (klass)->measure = cam_toast_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = cam_toast_size_allocate;
}

static void
cam_toast_init (CamToast *self)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("de.yesman.Camera");

  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->box, "cam-toast");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "cam-toast-icon");
  gtk_box_append (GTK_BOX (self->box), icon);
  self->label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->label), TRUE);
  /* a TextView's width: the text on one line, wrapped only where the toast's widest is too narrow for it */
  gtk_label_set_natural_wrap_mode (GTK_LABEL (self->label), GTK_NATURAL_WRAP_NONE);
  gtk_widget_set_hexpand (self->label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->label), 0);
  gtk_widget_add_css_class (self->label, "cam-toast-text");
  gtk_box_append (GTK_BOX (self->box), self->label);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_set_opacity (self->box, 0);
  self->fade = neo_anim_new (GTK_WIDGET (self), on_toast_fade, self);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

GtkWidget *
cam_toast_new (void)
{
  return g_object_new (CAM_TYPE_TOAST, NULL);
}

void
cam_toast_show (CamToast *self, const char *text)
{
  gtk_label_set_text (GTK_LABEL (self->label), text);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  neo_anim_start (self->fade, gtk_widget_get_opacity (self->box), 1, 250, 0, SPONGY_ACCELERATE_DECELERATE);
  g_clear_handle_id (&self->timer, g_source_remove);
  /* Toast.LENGTH_SHORT */
  self->timer = g_timeout_add (CAM_MS_TOAST, on_toast_timeout, self);
}
