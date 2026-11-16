/* NeoDialog: see neo-dialog.h. The motion is frameworks/base core/res/res/anim/popup_enter_material.xml and
 * popup_exit_material.xml; the scrim is the Material themes' backgroundDimAmount, 0.6 (spec/clock.md §12.5). */
#include "neo-dialog.h"
#include "neo-anim.h"
#include "neo-pressable.h"
#include "neo-style.h"

#define SIDE_INSET   24       /* MaterialAlertDialog's background insets: the card is the screen minus 2 x 24 dp */
#define MAX_WIDTH    560      /* M3 dialogs stop growing there */
#define ENTER_MS     220
#define ENTER_RISE   20
#define EXIT_MS      150
#define EXIT_RISE    10

struct _NeoDialog {
  GtkWidget  parent;
  GtkWidget *card, *title, *message, *content, *buttons;
  double     shown;           /* the window animation: 0 .. 1 */
  double     rise;            /* dp the card is below (positive) or above (negative) its place */
  NeoAnim   *anim;
  GtkEventController *keys;   /* the window's, while the dialog is up */
  gboolean   closing;
  gboolean   flush;
};

G_DEFINE_FINAL_TYPE (NeoDialog, neo_dialog, GTK_TYPE_WIDGET)

enum { RESPONSE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- layout: the scrim fills the window, the card is centred in it ---------------------------------------------- */

static int
card_width (int width)
{
  return MIN (width - 2 * SIDE_INSET, MAX_WIDTH);
}

static void
neo_dialog_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                    int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;     /* it takes whatever the overlay gives it: the whole window */
}

static void
neo_dialog_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NeoDialog *self = NEO_DIALOG (widget);
  int min_w, nat_w, h, min_h;
  gtk_widget_measure (self->card, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
  /* an alert dialog spans the window less its insets; one that holds a picker wraps it (TimePickerDialog) */
  int w = self->flush ? MIN (nat_w, width) : MAX (card_width (width), min_w);
  /* its content's natural height (a list wraps its rows, as a dialog's WRAP_CONTENT window does), up to the window
   * less its insets: a scrolled list then scrolls */
  gtk_widget_measure (self->card, GTK_ORIENTATION_VERTICAL, w, &min_h, &h, NULL, NULL);
  h = MAX (MIN (h, height - 2 * SIDE_INSET), MIN (min_h, height - 2 * SIDE_INSET));
  gtk_widget_allocate (self->card, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - w) / 2.0, (height - h) / 2.0)));
}

static void
neo_dialog_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NeoDialog *self = NEO_DIALOG (widget);
  GdkRGBA scrim = { 0, 0, 0, 0.6f };
  /* the dim fades with the window (WindowManager animates the dim layer with the dialog's alpha) */
  gtk_snapshot_push_opacity (snapshot, self->shown);
  gtk_snapshot_append_color (snapshot, &scrim,
                             &GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget), gtk_widget_get_height (widget)));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, self->rise));
  gtk_widget_snapshot_child (widget, self->card, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

/* ---- the window animation -------------------------------------------------------------------------------------- */

static gboolean on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state,
                        NeoDialog *self);

/* Escape and Back belong to the dialog while it is up, wherever the focus is: the key controller sits on the window
 * (not on the dialog, which never takes the focus) and runs in the bubble phase, so a popover inside the dialog and
 * the text in it still see the key first. */
static void
detach_keys (NeoDialog *self)
{
  GtkRoot *root = NULL;

  if (!self->keys)
    return;
  root = gtk_widget_get_root (GTK_WIDGET (self));
  if (root)
    gtk_widget_remove_controller (GTK_WIDGET (root), self->keys);
  self->keys = NULL;   /* the widget owned it: removing it destroyed it */
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  NeoDialog *self = data;
  self->shown = value;
  self->rise = self->closing ? -EXIT_RISE * (1 - value) : ENTER_RISE * (1 - value);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !neo_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
    detach_keys (self);
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

void
neo_dialog_respond (NeoDialog *self, int response)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  g_object_ref (self);
  g_signal_emit (self, signals[RESPONSE], 0, response);
  /* popup_exit_material: alpha 1 -> 0 and translateY 0 -> -10 dp over 150 ms, standard_accelerate */
  neo_anim_start_cubic (self->anim, 1, 0, EXIT_MS, 0, 0.3f, 0, 1, 1);
  g_object_unref (self);
}

void
neo_dialog_present (NeoDialog *self, GtkWidget *parent)
{
  GtkOverlay *overlay = neo_window_get_overlay (parent);
  GtkRoot *root = NULL;
  g_return_if_fail (overlay != NULL);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
  /* the scrim covers the whole window and takes every touch outside the card */
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  /* popup_enter_material: alpha 0 -> 1 and translateY 20 dp -> 0 over 220 ms, standard (0.2, 0, 0, 1) */
  neo_anim_start_cubic (self->anim, 0, 1, ENTER_MS, 0, 0.2f, 0, 0, 1);
  /* the first field takes the keyboard focus (and on the phone opens the keyboard), as the label dialog's does */
  gtk_widget_child_focus (self->card, GTK_DIR_TAB_FORWARD);
  root = gtk_widget_get_root (GTK_WIDGET (self));
  if (root && !self->keys)
    {
      self->keys = gtk_event_controller_key_new ();
      gtk_event_controller_set_propagation_phase (self->keys, GTK_PHASE_BUBBLE);
      g_signal_connect (self->keys, "key-pressed", G_CALLBACK (on_key), self);
      gtk_widget_add_controller (GTK_WIDGET (root), self->keys);
    }
}

static void
on_scrim_pressed (GtkGestureClick *click, int n_press, double x, double y, NeoDialog *self)
{
  graphene_rect_t card;
  if (gtk_widget_compute_bounds (self->card, GTK_WIDGET (self), &card) &&
      graphene_rect_contains_point (&card, &GRAPHENE_POINT_INIT (x, y)))
    return;
  /* setCanceledOnTouchOutside: a touch outside the card cancels */
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  neo_dialog_respond (self, NEO_RESPONSE_CANCEL);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, NeoDialog *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  neo_dialog_respond (self, NEO_RESPONSE_CANCEL);
  return TRUE;
}

static void
on_button (NeoPressable *button, NeoDialog *self)
{
  neo_dialog_respond (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "neo-response")));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
neo_dialog_dispose (GObject *object)
{
  NeoDialog *self = NEO_DIALOG (object);
  detach_keys (self);
  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->card, gtk_widget_unparent);
  G_OBJECT_CLASS (neo_dialog_parent_class)->dispose (object);
}

static void
neo_dialog_class_init (NeoDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = neo_dialog_dispose;
  widget_class->measure = neo_dialog_measure;
  widget_class->size_allocate = neo_dialog_size_allocate;
  widget_class->snapshot = neo_dialog_snapshot;
  gtk_widget_class_set_css_name (widget_class, "neo-dialog-scrim");
  /* a dialog to the shell's back gesture too (a11y.js), which then closes it with Escape instead of leaving the app */
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);

  signals[RESPONSE] = g_signal_new ("response", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
neo_dialog_init (NeoDialog *self)
{
  self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->card, "neo-dialog");
  gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_parent (self->card, GTK_WIDGET (self));

  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_label_set_wrap (GTK_LABEL (self->title), TRUE);
  gtk_widget_add_css_class (self->title, "neo-dialog-title");
  gtk_widget_set_visible (self->title, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->title);

  self->message = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->message), 0);
  gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
  gtk_widget_add_css_class (self->message, "neo-dialog-message");
  gtk_widget_set_visible (self->message, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->message);

  self->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (self->buttons, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->buttons, "neo-dialog-buttons");
  /* shown with its first button: a dialog without any (a list to pick from) has no button row's space */
  gtk_widget_set_visible (self->buttons, FALSE);
  gtk_box_append (GTK_BOX (self->card), self->buttons);

  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_scrim_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}

NeoDialog *
neo_dialog_new (const char *title)
{
  NeoDialog *self = g_object_new (NEO_TYPE_DIALOG, NULL);
  if (title) {
    gtk_label_set_text (GTK_LABEL (self->title), title);
    gtk_widget_set_visible (self->title, TRUE);
  }
  return self;
}

void
neo_dialog_set_message (NeoDialog *self, const char *message)
{
  gtk_label_set_text (GTK_LABEL (self->message), message);
  gtk_widget_set_visible (self->message, message != NULL);
}

void
neo_dialog_set_content (NeoDialog *self, GtkWidget *content)
{
  if (self->content)
    gtk_box_remove (GTK_BOX (self->card), self->content);
  self->content = content;
  if (content)
    gtk_box_insert_child_after (GTK_BOX (self->card), content, self->message);
}

GtkWidget *
neo_dialog_add_button (NeoDialog *self, const char *label, int response)
{
  GtkWidget *button = neo_text_button_new (label);
  g_object_set_data (G_OBJECT (button), "neo-response", GINT_TO_POINTER (response));
  g_signal_connect (button, "clicked", G_CALLBACK (on_button), self);
  gtk_box_append (GTK_BOX (self->buttons), button);
  gtk_widget_set_visible (self->buttons, TRUE);
  return button;
}

void
neo_dialog_set_flush (NeoDialog *self, gboolean flush)
{
  self->flush = flush;
  if (flush)
    gtk_widget_add_css_class (self->card, "flush");
  else
    gtk_widget_remove_css_class (self->card, "flush");
}
