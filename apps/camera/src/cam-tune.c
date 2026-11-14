/* cam-tune: see cam-tune.h. */
#include "cam-tune.h"
#include "cam-constants.h"
#include "cam-widgets.h"

#include <glib/gi18n.h>
#include <math.h>

/* ---- the sheet ------------------------------------------------------------------------------------------------- */

enum { ROW_GAMMA, ROW_CONTRAST, ROW_SATURATION, ROWS };

struct _CamTunePanel {
  GtkBox     parent;
  GtkWidget *title;
  GtkWidget *sliders[ROWS];
  CamTone    tone;
};

G_DEFINE_FINAL_TYPE (CamTunePanel, cam_tune_panel, GTK_TYPE_BOX)

static guint changed_signal, reset_signal, done_signal;

static const struct {
  const char *label;
  double      min, max;
} rows[ROWS] = {
  { N_("Gamma"), CAM_GAMMA_MIN, CAM_GAMMA_MAX },
  { N_("Contrast"), CAM_CONTRAST_MIN, CAM_CONTRAST_MAX },
  { N_("Saturation"), CAM_SATURATION_MIN, CAM_SATURATION_MAX },
};

static double *
tone_field (CamTunePanel *self, int row)
{
  return row == ROW_GAMMA ? &self->tone.gamma : row == ROW_CONTRAST ? &self->tone.contrast : &self->tone.saturation;
}

static void
show_row (CamTunePanel *self, int row)
{
  double value = *tone_field (self, row);
  g_autofree char *text = g_strdup_printf ("%.2f", value);

  cam_zoom_slider_set (CAM_ZOOM_SLIDER (self->sliders[row]), (value - rows[row].min) / (rows[row].max - rows[row].min),
                       text);
}

static void
on_slider (CamZoomSlider *slider, double progress, CamTunePanel *self)
{
  int row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (slider), "row"));
  /* steps of 0.05, as the thumb shows them */
  double value = round ((rows[row].min + progress * (rows[row].max - rows[row].min)) * 20) / 20;

  *tone_field (self, row) = CLAMP (value, rows[row].min, rows[row].max);
  show_row (self, row);
  g_signal_emit (self, changed_signal, 0);
}

static void
on_reset (GtkButton *button, CamTunePanel *self)
{
  g_signal_emit (self, reset_signal, 0);
}

static void
on_done (GtkButton *button, CamTunePanel *self)
{
  g_signal_emit (self, done_signal, 0);
}

void
cam_tune_panel_set (CamTunePanel *self, const char *camera_name, const CamTone *tone)
{
  g_autofree char *title = g_strdup_printf (_("Tune · %s"), camera_name ? camera_name : "");

  self->tone = *tone;
  cam_tone_clamp (&self->tone);
  gtk_label_set_text (GTK_LABEL (self->title), title);
  for (int i = 0; i < ROWS; i++)
    show_row (self, i);
}

void
cam_tune_panel_get (CamTunePanel *self, CamTone *tone)
{
  *tone = self->tone;
}

static void
cam_tune_panel_class_init (CamTunePanelClass *klass)
{
  changed_signal = g_signal_new ("changed", CAM_TYPE_TUNE_PANEL, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  reset_signal = g_signal_new ("reset", CAM_TYPE_TUNE_PANEL, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  done_signal = g_signal_new ("done", CAM_TYPE_TUNE_PANEL, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
cam_tune_panel_init (CamTunePanel *self)
{
  GtkWidget *buttons, *reset, *done, *spacer;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "cam-tune");
  self->tone = CAM_TONE_NEUTRAL;
  self->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->title), 0);
  gtk_widget_add_css_class (self->title, "cam-tune-title");
  gtk_box_append (GTK_BOX (self), self->title);
  for (int i = 0; i < ROWS; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *label = gtk_label_new (_(rows[i].label));

      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_widget_set_size_request (label, CAM_TUNE_LABEL_W, -1);
      gtk_widget_add_css_class (label, "cam-tune-label");
      gtk_box_append (GTK_BOX (row), label);
      self->sliders[i] = cam_zoom_slider_new ();
      g_object_set_data (G_OBJECT (self->sliders[i]), "row", GINT_TO_POINTER (i));
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->sliders[i]), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _(rows[i].label), -1);
      g_signal_connect (self->sliders[i], "changed", G_CALLBACK (on_slider), self);
      gtk_box_append (GTK_BOX (row), self->sliders[i]);
      gtk_box_append (GTK_BOX (self), row);
    }
  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  reset = gtk_button_new_with_label (_("Reset"));
  done = gtk_button_new_with_label (_("Done"));
  gtk_widget_add_css_class (reset, "cam-tune-button");
  gtk_widget_add_css_class (done, "cam-tune-button");
  g_signal_connect (reset, "clicked", G_CALLBACK (on_reset), self);
  g_signal_connect (done, "clicked", G_CALLBACK (on_done), self);
  gtk_box_append (GTK_BOX (buttons), spacer);
  gtk_box_append (GTK_BOX (buttons), reset);
  gtk_box_append (GTK_BOX (buttons), done);
  gtk_box_append (GTK_BOX (self), buttons);
}

GtkWidget *
cam_tune_panel_new (void)
{
  return g_object_new (CAM_TYPE_TUNE_PANEL, NULL);
}
