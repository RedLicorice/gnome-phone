/* cam-tune: the sheet that sets a camera's picture tuning (cam-tone.h). It is not modal: it sits over the lower
 * viewfinder, so the picture shows the change while a slider moves. */
#pragma once

#include "cam-tone.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAM_TYPE_TUNE_PANEL (cam_tune_panel_get_type ())
G_DECLARE_FINAL_TYPE (CamTunePanel, cam_tune_panel, CAM, TUNE_PANEL, GtkBox)

GtkWidget *cam_tune_panel_new       (void);
/* the camera's name over the sliders, and where they stand */
void       cam_tune_panel_set       (CamTunePanel *self, const char *camera_name, const CamTone *tone);
void       cam_tune_panel_get       (CamTunePanel *self, CamTone *tone);

/* Signals: "changed" (): a slider was moved (read it with cam_tune_panel_get); "reset" (): the Reset button;
 * "done" (): the Done button */

G_END_DECLS
