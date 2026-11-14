/* cam-tone: the picture tuning of one camera (Soft ISP Gamma, Contrast, Saturation) as de.yesman.Camera "tuning"
 * keeps it: a{s(ddd)}, libcamera camera id → gamma, contrast, saturation. Minerva's eyes read the same key
 * (minerva_eyes/camera.py). Only the controls that were seen to change the picture are offered (the ROM's soft-ISP AGC
 * ignores exposure-value). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  double gamma, contrast, saturation;
} CamTone;

#define CAM_TONE_NEUTRAL ((CamTone) { 1.0, 1.0, 1.0 })
#define CAM_GAMMA_MIN        1.0
#define CAM_GAMMA_MAX        3.0
#define CAM_CONTRAST_MIN     0.5
#define CAM_CONTRAST_MAX     2.0
#define CAM_SATURATION_MIN   0.0
#define CAM_SATURATION_MAX   2.0

/* Clamps into the slider ranges. */
void     cam_tone_clamp     (CamTone *tone);
/* The value stored for the camera: FALSE if there is none (leave the camera as libcamera has it). */
gboolean cam_tone_load      (GSettings *settings, const char *camera_id, CamTone *tone);
/* The schema's seed for the camera, FALSE if it has none. */
gboolean cam_tone_load_default (GSettings *settings, const char *camera_id, CamTone *tone);
/* Stores it for the camera, the other cameras' entries kept. */
void     cam_tone_store     (GSettings *settings, const char *camera_id, const CamTone *tone);

G_END_DECLS
