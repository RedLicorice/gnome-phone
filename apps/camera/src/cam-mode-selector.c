/* cam-mode-selector: see cam-mode-selector.h. */
#include "cam-mode-selector.h"
#include "cam-constants.h"

#include <glib/gi18n.h>
#include <neo.h>

struct _CamModeSelector {
  GtkWidget  parent;
  GtkWidget *buttons[CAM_N_MODES];
  GtkWidget *labels[CAM_N_MODES];
  GtkWidget *duration;
  CamMode    mode;
  gboolean   available[CAM_N_MODES];
  gboolean   recording;
  gboolean   vertical;          /* a column beside a landscape preview, the buttons stacked */
  double     pill_x, pill_w;    /* the highlight along the band (x, or y in a column) and its length there;
                                 * pill_x < 0 until laid out */
  NeoAnim   *x_anim, *w_anim;
  gboolean   pending;           /* the pill moves once the buttons have their places */
};

G_DEFINE_FINAL_TYPE (CamModeSelector, cam_mode_selector, GTK_TYPE_WIDGET)

enum { SIG_MODE_CHOSEN, N_SIGNALS };
static guint signals[N_SIGNALS];

static int
n_available (CamModeSelector *self)
{
  int n = 0;

  for (int i = 0; i < CAM_N_MODES; i++)
    n += self->available[i];
  return n;
}

static void
cam_mode_selector_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                           int *min_base, int *nat_base)
{
  CamModeSelector *self = CAM_MODE_SELECTOR (widget);

  if (self->vertical)
    *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? CAM_MODE_COLUMN_W
                                                            : n_available (self) * CAM_MODE_BUTTON_H;
  else
    *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? n_available (self) * CAM_MODE_BUTTON_W
                                                            : CAM_MODE_SELECTOR_H;
}

/* where a mode's pill starts along the band: its x in the bar, its y in the column (the 48 dp buttons stacked,
 * centred, each pill 4 dp into its button as in the bar) */
static double
button_x (GtkWidget *widget, CamMode mode)
{
  CamModeSelector *self = CAM_MODE_SELECTOR (widget);
  int before = 0;

  for (int i = 0; i < (int) mode; i++)
    before += self->available[i];
  if (self->vertical)
    return (gtk_widget_get_height (widget) - n_available (self) * CAM_MODE_BUTTON_H) / 2 + before * CAM_MODE_BUTTON_H
           + (CAM_MODE_BUTTON_H - CAM_MODE_PILL_H) / 2;
  return (gtk_widget_get_width (widget) - n_available (self) * CAM_MODE_BUTTON_W) / 2 + before * CAM_MODE_BUTTON_W;
}

/* the pill's length along the band */
static double
pill_length (CamModeSelector *self)
{
  return self->vertical ? CAM_MODE_PILL_H : CAM_MODE_BUTTON_W;
}

static void
cam_mode_selector_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CamModeSelector *self = CAM_MODE_SELECTOR (widget);
  /* MaterialButton's 4 dp insets: the 48 dp button draws (and ripples) in its middle 40 dp */
  int y = CAM_MODE_PAD + (CAM_MODE_BUTTON_H - CAM_MODE_PILL_H) / 2;
  int x = (width - CAM_MODE_BUTTON_W) / 2;

  for (int i = 0; i < CAM_N_MODES; i++)
    gtk_widget_size_allocate (self->buttons[i],
                              self->vertical
                                ? &(GtkAllocation) { x, (int) button_x (widget, i), CAM_MODE_BUTTON_W, CAM_MODE_PILL_H }
                                : &(GtkAllocation) { (int) button_x (widget, i), y, CAM_MODE_BUTTON_W, CAM_MODE_PILL_H },
                              -1);
  gtk_widget_size_allocate (self->duration, &(GtkAllocation) { x, self->vertical ? (height - CAM_MODE_PILL_H) / 2 : y,
                                                                CAM_MODE_BUTTON_W, CAM_MODE_PILL_H }, -1);
  if (self->pill_x < 0 || self->pending)
    {
      self->pending = FALSE;
      self->pill_x = button_x (widget, self->mode);
      self->pill_w = pill_length (self);
    }
}

static void
cam_mode_selector_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamModeSelector *self = CAM_MODE_SELECTOR (widget);

  if (!self->recording && self->pill_x >= 0)
    {
      GskRoundedRect pill;
      GdkRGBA fill = neo_color (NEO_SECONDARY_CONTAINER);

      if (self->vertical)
        gsk_rounded_rect_init_from_rect (&pill, &GRAPHENE_RECT_INIT ((gtk_widget_get_width (widget)
                                                                      - CAM_MODE_BUTTON_W) / 2, self->pill_x,
                                                                     CAM_MODE_BUTTON_W, self->pill_w),
                                         CAM_MODE_PILL_H / 2.0);
      else
        gsk_rounded_rect_init_from_rect (&pill, &GRAPHENE_RECT_INIT (self->pill_x, CAM_MODE_PAD + 4, self->pill_w,
                                                                     CAM_MODE_PILL_H), CAM_MODE_PILL_H / 2.0);
      gtk_snapshot_push_rounded_clip (snapshot, &pill);
      gtk_snapshot_append_color (snapshot, &fill, &pill.bounds);
      gtk_snapshot_pop (snapshot);
    }
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_snapshot_child (widget, child, snapshot);
}

static void
on_x (NeoAnim *anim, double value, gpointer data)
{
  CAM_MODE_SELECTOR (data)->pill_x = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
on_w (NeoAnim *anim, double value, gpointer data)
{
  CAM_MODE_SELECTOR (data)->pill_w = value;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
on_button (NeoPressable *button, CamModeSelector *self)
{
  for (int i = 0; i < CAM_N_MODES; i++)
    if (GTK_WIDGET (button) == self->buttons[i])
      g_signal_emit (self, signals[SIG_MODE_CHOSEN], 0, i);
}

static void
cam_mode_selector_dispose (GObject *object)
{
  CamModeSelector *self = CAM_MODE_SELECTOR (object);
  GtkWidget *child;

  g_clear_pointer (&self->x_anim, neo_anim_free);
  g_clear_pointer (&self->w_anim, neo_anim_free);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (cam_mode_selector_parent_class)->dispose (object);
}

static void
cam_mode_selector_class_init (CamModeSelectorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_mode_selector_dispose;
  widget_class->measure = cam_mode_selector_measure;
  widget_class->size_allocate = cam_mode_selector_size_allocate;
  widget_class->snapshot = cam_mode_selector_snapshot;
  signals[SIG_MODE_CHOSEN] = g_signal_new ("mode-chosen", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
cam_mode_selector_init (CamModeSelector *self)
{
  /* camera_mode_photo / _video / _qr, upper-cased by textAllCaps */
  const char *names[CAM_N_MODES] = { C_("mode", "PHOTO"), C_("mode", "VIDEO"), C_("mode", "SCAN"), C_("mode", "NIGHT") };

  self->pill_x = -1;
  for (int i = 0; i < CAM_N_MODES; i++)
    {
      self->available[i] = TRUE;
      self->labels[i] = gtk_label_new (names[i]);
      gtk_widget_add_css_class (self->labels[i], "cam-mode-label");
      self->buttons[i] = neo_pressable_new (self->labels[i]);
      neo_pressable_set_ripple (NEO_PRESSABLE (self->buttons[i]), NEO_RIPPLE_BOUNDED, CAM_MODE_PILL_H / 2.0);
      gtk_widget_set_parent (self->buttons[i], GTK_WIDGET (self));
      g_signal_connect (self->buttons[i], "clicked", G_CALLBACK (on_button), self);
    }
  self->duration = gtk_label_new ("00:00:00");
  gtk_widget_add_css_class (self->duration, "cam-duration");
  gtk_widget_set_visible (self->duration, FALSE);
  gtk_widget_set_parent (self->duration, GTK_WIDGET (self));
  self->x_anim = neo_anim_new (GTK_WIDGET (self), on_x, self);
  self->w_anim = neo_anim_new (GTK_WIDGET (self), on_w, self);
  cam_mode_selector_set_mode (self, CAM_MODE_PHOTO, FALSE);
}

GtkWidget *
cam_mode_selector_new (void)
{
  return g_object_new (CAM_TYPE_MODE_SELECTOR, NULL);
}

void
cam_mode_selector_set_mode (CamModeSelector *self, CamMode mode, gboolean animate)
{
  GtkWidget *widget = GTK_WIDGET (self);

  self->mode = mode;
  for (int i = 0; i < CAM_N_MODES; i++)
    {
      /* isEnabled = mode != current: the chosen one is white and does not take a tap */
      gtk_widget_set_sensitive (self->buttons[i], i != (int) mode);
      if (i == (int) mode)
        gtk_widget_add_css_class (self->labels[i], "selected");
      else
        gtk_widget_remove_css_class (self->labels[i], "selected");
    }
  if (animate && gtk_widget_get_mapped (widget) && self->pill_x >= 0)
    {
      /* two ValueAnimators, 300 ms AD */
      neo_anim_start (self->x_anim, self->pill_x, button_x (widget, mode), CAM_MS_DEFAULT, 0,
                      SPONGY_ACCELERATE_DECELERATE);
      neo_anim_start (self->w_anim, self->pill_w, pill_length (self), CAM_MS_DEFAULT, 0, SPONGY_ACCELERATE_DECELERATE);
    }
  else
    {
      neo_anim_stop (self->x_anim);
      neo_anim_stop (self->w_anim);
      self->pending = TRUE;
      gtk_widget_queue_allocate (widget);
    }
}

void
cam_mode_selector_set_recording (CamModeSelector *self, gboolean recording)
{
  self->recording = recording;
  /* invisible, keeping their places */
  for (int i = 0; i < CAM_N_MODES; i++)
    {
      gtk_widget_set_opacity (self->buttons[i], recording ? 0 : 1);
      gtk_widget_set_can_target (self->buttons[i], !recording);
    }
  gtk_widget_set_visible (self->duration, recording);
  if (recording)
    cam_mode_selector_set_duration (self, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
cam_mode_selector_set_duration (CamModeSelector *self, gint64 microseconds)
{
  gint64 s = microseconds / G_USEC_PER_SEC;
  g_autofree char *text = g_strdup_printf ("%02d:%02d:%02d", (int) (s / 3600), (int) (s / 60 % 60), (int) (s % 60));

  gtk_label_set_text (GTK_LABEL (self->duration), text);
}

void
cam_mode_selector_set_available (CamModeSelector *self, CamMode mode, gboolean available)
{
  self->available[mode] = available;
  gtk_widget_set_visible (self->buttons[mode], available);
  self->pending = TRUE;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

gboolean
cam_mode_selector_is_available (CamModeSelector *self, CamMode mode)
{
  return self->available[mode];
}

void
cam_mode_selector_set_vertical (CamModeSelector *self, gboolean vertical)
{
  if (vertical == self->vertical)
    return;
  self->vertical = vertical;
  neo_anim_stop (self->x_anim);
  neo_anim_stop (self->w_anim);
  /* no resize queued: the parent sets this while it allocates, and allocates the selector at its new size next */
  self->pending = TRUE;
}
