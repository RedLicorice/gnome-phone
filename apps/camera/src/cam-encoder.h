/* cam-encoder: which H.264 encoder records the videos, and which sizes it holds at 30 fps.
 *
 * Aperture's video quality list is what the camera and its encoder can record (CameraX's QualitySelector). Here the
 * encoders are GStreamer's: a phone's hardware one (V4L2 or VA) if it works, else x264 or OpenH264 in software.
 * Whether a hardware encoder works, and how fast one is, is a property of the phone's kernel and CPU that no
 * element reports, so it is measured: each candidate encodes 120 frames of a test picture at 1080p and at 720p in a
 * child process (`neo-camera --probe-encoder`, so a driver that crashes takes only the child with it), a size that
 * falls short once is measured again (a hardware encoder's first session is slow), and a size counts when it encodes
 * at least CAM_ENCODER_MIN_FPS within twice the bitrate asked for (an encoder that ignores its rate control fills a
 * phone at 150 Mbit/s). A result with an encoder is kept in the user's cache, keyed by the kernel and GStreamer
 * versions, so it runs once; a run where every candidate failed is not kept (measured again at the next start). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define CAM_ENCODER_MIN_FPS 40.0    /* a third over 30 fps: the camera and the preview run beside it */

typedef struct {
  char     *element;                /* "v4l2h264enc", "x264enc", … or NULL: no encoder can record */
  gboolean  hd;                     /* 1280 × 720 at 30 fps */
  gboolean  fhd;                    /* 1920 × 1080 at 30 fps */
} CamEncoder;

/* the cached answer at once, or NULL until the probe has run */
const CamEncoder *cam_encoder_get   (void);
/* measures in the background if there is no answer yet; `done` runs on the main loop when there is one */
void              cam_encoder_probe (GCallback done, gpointer data);

/* the encoder chain for a bin: "<encoder with its properties> ! h264parse", for `bitrate` bits/s */
char             *cam_encoder_launch (const char *element, int bitrate);
/* the raw format it wants: NV12, or I420 for OpenH264 */
const char       *cam_encoder_format (const char *element);

/* the child's side: `neo-camera --probe-encoder ELEMENT` prints "fps1080 fps720" and exits */
int               cam_encoder_probe_main (const char *element);

G_END_DECLS
