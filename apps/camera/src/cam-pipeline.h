/* cam-pipeline: the one camera stream of the app, and everything that reads from it.
 *
 *   libcamerasrc ! capsfilter ! tee
 *     ! queue leaky ! gtk4paintablesink                         the viewfinder (cam_pipeline_get_paintable)
 *     ! queue leaky ! valve ! videoconvertscale ! zxing ! fakesink   SCAN mode: "barcode"
 *     ! queue leaky ! valve ! videocrop ! videoconvertscale ! videoflip ! appsink   a recording's frames
 *
 * plus a probe on the stream that copies one frame when a photo asks for it (cam_pipeline_grab): a photo is the
 * stream's current frame, so the shutter is instant and the viewfinder never freezes.
 *
 * libcamera gives one stream per camera and libcamerasrc must not be made twice in a process (a second one crashes
 * in ControlList::merge even after the first is gone), so the element lives as long as the app: another camera is
 * NULL → camera-name → PLAYING, another size READY → caps → PLAYING. The state changes block (a camera takes a
 * few hundred ms to open), so they run on a thread of their own, in order, always towards the latest request;
 * an open that fails is tried again with a growing pause (a sensor link still busy from the last close), and
 * "error" tells the window when it gives up. */
#pragma once

#include <gdk/gdk.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
  CAM_PIPELINE_ERROR_BUSY,          /* another process has the camera */
  CAM_PIPELINE_ERROR_STREAM,        /* it would not configure or start */
  CAM_PIPELINE_ERROR_RECOVERABLE,   /* it stopped while streaming; it is being reopened */
} CamPipelineError;

#define CAM_TYPE_PIPELINE (cam_pipeline_get_type ())
G_DECLARE_FINAL_TYPE (CamPipeline, cam_pipeline, CAM, PIPELINE, GObject)

/* NULL if GStreamer lacks an element it needs (the message says which) */
CamPipeline  *cam_pipeline_new            (GError **error);
GdkPaintable *cam_pipeline_get_paintable  (CamPipeline *self);

/* Streams `camera_id` at width × height (the source's own frame rate), or keeps streaming if it does already.
 * "streaming" follows with the first frame. */
void          cam_pipeline_start          (CamPipeline *self, const char *camera_id, int width, int height);
/* to NULL: the camera is let go (window hidden, settings open, suspend) */
void          cam_pipeline_stop           (CamPipeline *self);
/* TRUE from the first frame of the current configuration until the next change */
gboolean      cam_pipeline_is_streaming   (CamPipeline *self);
/* the stream's size and format now (0 / NULL before the first frame) */
void          cam_pipeline_get_size       (CamPipeline *self, int *width, int *height);

/* The next frame, copied, on the main loop: `done` gets a GstSample (transfer full), or NULL if the stream is not
 * running. */
typedef void (*CamGrabFunc) (GstSample *frame, gpointer data);
void          cam_pipeline_grab           (CamPipeline *self, CamGrabFunc done, gpointer data);

/* SCAN mode's decoder on or off; "barcode" (text) once per code until cam_pipeline_scan_resume */
void          cam_pipeline_set_scanning   (CamPipeline *self, gboolean scanning);
/* whether GStreamer has a barcode reader (zxing) for SCAN */
gboolean      cam_pipeline_can_scan       (CamPipeline *self);
void          cam_pipeline_scan_resume    (CamPipeline *self);

/* A recording's frames: each one cropped to the centre 1/zoom (digital zoom), scaled back to the stream's size,
 * in `format`, mirrored left to right in the picture's own upright frame if `mirror` (with the picture turned by
 * `rotation`), and handed to `func` on the streaming thread. NULL func stops it. */
typedef void (*CamFrameFunc) (GstSample *frame, gpointer data);
void          cam_pipeline_set_recording  (CamPipeline *self, CamFrameFunc func, gpointer data, const char *format,
                                           gboolean mirror, int rotation);
void          cam_pipeline_set_zoom       (CamPipeline *self, double zoom);

/* Autofocus, where the camera has it (libcamera's AfMode answers continuous): known a moment after "streaming",
 * then "notify::has-af". cam_pipeline_refocus restarts the search; "focused" when the lens has settled. */
gboolean      cam_pipeline_has_af         (CamPipeline *self);
void          cam_pipeline_refocus        (CamPipeline *self);

/* The picture tuning of the camera in use (libcamera's Gamma, Contrast, Saturation), live and kept across restarts and
 * camera changes. @enabled FALSE: none wanted (the camera stays as libcamera has it, after a tuned one it is
 * neutral). */
void          cam_pipeline_set_tone       (CamPipeline *self, gboolean enabled, double gamma, double contrast,
                                           double saturation);

/* the clock both this pipeline and a recorder run on, and this one's base time (to turn a frame's time into the
 * recorder's) */
GstClock     *cam_pipeline_get_clock      (CamPipeline *self);
GstClockTime  cam_pipeline_get_base_time  (CamPipeline *self);

/* Signals: "streaming" (), "error" (int CamPipelineError, string detail), "barcode" (string text),
 * "focused" (); property "has-af" */

G_END_DECLS
