/* cam-mode-selector: PHOTO VIDEO SCAN, or the recording's duration (§3, §11; CameraModeSelectorLayout.kt,
 * L/camera_mode_selector_layout.xml).
 *
 * Three 88 × 48 dp buttons centred as a group, 16 dp under the band's top; the highlight pill (secondaryContainer,
 * 40 dp tall, fully rounded) slides to the chosen one in 300 ms AD; the chosen one's label is white and it cannot
 * be tapped, the others are #444444. While recording the buttons and the pill go invisible and the red duration pill
 * shows in the middle. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  CAM_MODE_PHOTO,
  CAM_MODE_VIDEO,
  CAM_MODE_SCAN,
  CAM_MODE_NIGHT,      /* a burst merged into one picture (Minerva's burst CLI) */
  CAM_N_MODES,
} CamMode;

#define CAM_TYPE_MODE_SELECTOR (cam_mode_selector_get_type ())
G_DECLARE_FINAL_TYPE (CamModeSelector, cam_mode_selector, CAM, MODE_SELECTOR, GtkWidget)

GtkWidget *cam_mode_selector_new           (void);
void       cam_mode_selector_set_mode      (CamModeSelector *self, CamMode mode, gboolean animate);
/* the duration pill instead of the buttons */
void       cam_mode_selector_set_recording (CamModeSelector *self, gboolean recording);
void       cam_mode_selector_set_duration  (CamModeSelector *self, gint64 microseconds);
/* a mode this phone cannot back (SCAN without GStreamer's zxing) has no button; the others centre as a group */
void       cam_mode_selector_set_available (CamModeSelector *self, CamMode mode, gboolean available);
gboolean   cam_mode_selector_is_available  (CamModeSelector *self, CamMode mode);
/* a column (CAM_MODE_COLUMN_W wide) beside a landscape preview instead of the bar under a portrait one; it takes
 * effect at the selector's next allocation, which the caller makes */
void       cam_mode_selector_set_vertical  (CamModeSelector *self, gboolean vertical);

/* Signals: "mode-chosen" (int CamMode) */

G_END_DECLS
