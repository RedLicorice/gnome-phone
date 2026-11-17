/* cam-focus: the autofocus of the camera in use, on a camera that has a lens actuator (cam-lens.h): tap to focus on a
 * region, automatic focus that searches again when the scene changes, a manual position.
 *
 * The search is cam-af.h's (a coarse sweep, then a refinement); this object drives it. The lens moves on a thread of
 * its own (an I2C write may block for a few ms), each position is measured on a frame that was exposed after the
 * lens had settled (the probe is on the stream: cam_focus_frame), and the state goes to the UI as it changes: the
 * focus ring shows "searching" until the lens stands where the peak is.
 *
 * Modes (the "af-mode" setting): AUTO searches when the first frame is there and again when the sharpness of the
 * middle of the frame fell for a while; TAP searches only at start and when asked (cam_focus_at); MANUAL puts the
 * lens where cam_focus_set_position says and never searches. libcamera's own autofocus is switched off by the
 * pipeline (af-mode manual) for a camera this has a lens for, so the lens has one driver. */
#pragma once

#include "cam-af.h"

#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
  CAM_FOCUS_AUTO,
  CAM_FOCUS_TAP,
  CAM_FOCUS_MANUAL,
} CamFocusMode;

typedef enum {
  CAM_FOCUS_IDLE,
  CAM_FOCUS_SEARCHING,
  CAM_FOCUS_FOCUSED,
  CAM_FOCUS_FAILED,          /* no clear peak (cam_focus_get_reason says why): the lens is back where it last worked */
} CamFocusState;

#define CAM_TYPE_FOCUS (cam_focus_get_type ())
G_DECLARE_FINAL_TYPE (CamFocus, cam_focus, CAM, FOCUS, GObject)

CamFocus     *cam_focus_new           (GSettings *settings);

/* TRUE if libcamera's camera @camera_id has a lens (main thread; reads the kernel's devicetree) */
gboolean      cam_focus_has_lens      (CamFocus *self, const char *camera_id);
/* the camera that will stream (NULL: none, the lens is let go) */
void          cam_focus_set_camera    (CamFocus *self, const char *camera_id);
/* "streaming" from the pipeline: the first frame is there / the stream is gone */
void          cam_focus_streaming     (CamFocus *self, gboolean streaming);
/* each frame, on the streaming thread */
void          cam_focus_frame         (CamFocus *self, GstBuffer *buffer, GstPad *pad);

/* TRUE while the camera in use has an open lens: the property "available" */
gboolean      cam_focus_available     (CamFocus *self);
void          cam_focus_set_mode      (CamFocus *self, CamFocusMode mode);
CamFocusMode  cam_focus_get_mode      (CamFocus *self);
/* a search with its region centred on (x, y), fractions of the frame as the sensor sends it (0 … 1); AUTO and TAP */
void          cam_focus_at            (CamFocus *self, double x, double y);
/* MANUAL: the lens at @fraction (0 … 1) of its sweep */
void          cam_focus_set_position  (CamFocus *self, double fraction);
double        cam_focus_get_position  (CamFocus *self);
CamFocusState cam_focus_get_state     (CamFocus *self);
CamAfReason   cam_focus_get_reason    (CamFocus *self);
/* TRUE while a search runs (a photo waits for it) */
gboolean      cam_focus_busy          (CamFocus *self);
/* TRUE: no automatic searching (a recording: the picture must not breathe); a tap still searches */
void          cam_focus_set_hold      (CamFocus *self, gboolean hold);

/* Signals: "state-changed" (int CamFocusState, int CamAfReason); property "available" */

G_END_DECLS
