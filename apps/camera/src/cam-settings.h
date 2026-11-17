/* cam-settings: the Settings screen (§20; SettingsActivity.kt, R/xml/root_preferences.xml,
 * gestures_preferences.xml), a page of the camera's own window.
 *
 * Only the rows this build can back (§20.3): Bright screen (if the shell can hold the brightness), Save location
 * data (if GeoClue is there), Shutter sound, Leveler (if the phone has a raw accelerometer), Gestures (if the shell
 * hands the app its volume keys), Focus (if the camera in use has a lens), Mirror front camera (if there is a front camera), Enable video mirroring (if a
 * video can be recorded). The rest of Aperture's rows are left out, not shown disabled. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  CAM_SETTINGS_LOCATION = 1 << 0,
  CAM_SETTINGS_LEVELER  = 1 << 1,
  CAM_SETTINGS_GESTURES = 1 << 2,
  CAM_SETTINGS_FRONT    = 1 << 3,
  CAM_SETTINGS_VIDEO    = 1 << 4,
  CAM_SETTINGS_BRIGHT   = 1 << 5,
  CAM_SETTINGS_FOCUS    = 1 << 6,
} CamSettingsRows;

#define CAM_TYPE_SETTINGS (cam_settings_get_type ())
G_DECLARE_FINAL_TYPE (CamSettings, cam_settings, CAM, SETTINGS, GtkWidget)

GtkWidget *cam_settings_new   (GSettings *settings, CamSettingsRows rows);
/* the rows to show (what the phone turned out to have) */
void       cam_settings_set_rows (CamSettings *self, CamSettingsRows rows);
/* back to the first page, scrolled to the top */
void       cam_settings_reset (CamSettings *self);

/* Signals: "closed" (): Navigate up, or Back on the first page */

G_END_DECLS
