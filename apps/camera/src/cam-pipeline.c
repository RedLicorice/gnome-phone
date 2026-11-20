/* cam-pipeline: see cam-pipeline.h. */
#include "cam-pipeline.h"
#include "cam-focus.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <math.h>
#include <string.h>

#define FIRST_FRAME_TIMEOUT_S  5
#define AF_MODE_MANUAL         0        /* libcamera's AfMode: its own autofocus stays still (cam-focus.h drives the lens) */

/* the pause before each retry of an open: CAMSS on the Poco keeps a sensor link busy for a few seconds after a
 * close (24.9); any pipeline handler may have such a moment */
static const guint backoff_ms[] = { 0, 250, 500, 1000, 2000, 4000 };

struct _CamPipeline {
  GObject       parent;

  GstElement   *pipeline, *src, *caps, *view, *scan_valve, *rec_valve, *rec_crop, *rec_caps, *rec_flip, *rec_sink;
  GdkPaintable *paintable;
  GstClock     *clock;
  GThreadPool  *worker;

  GMutex        lock;           /* the request, and what the worker and the streaming threads tell each other */
  GCond         cond;
  gboolean      want_active;
  char         *want_camera;
  int           want_w, want_h;
  guint         generation;     /* +1 per request */
  gboolean      force_reopen;
  char         *last_error;
  gboolean      got_frame;
  guint         first_pending;  /* the generation whose first frame the probe looks out for, 0 none */
  CamFrameFunc  rec_func;
  gpointer      rec_data;
  gboolean      tone_wanted, tone_applied;   /* the Soft ISP's picture tuning, and whether it was ever sent */
  double        tone[3];                     /* gamma, contrast, saturation */

  /* the worker's own */
  char         *cur_camera;
  int           cur_w, cur_h;
  gboolean      cur_playing;
  gboolean      in_transition;  /* errors now are the worker's to handle (atomic) */

  /* the main thread's */
  gboolean      streaming;
  int           width, height;
  gint          grab_pending;   /* atomic */
  GSList       *grabs;          /* (CamGrabFunc, data) pairs waiting */
  gboolean      can_scan, scanning, scan_hold;
  double        zoom;
  CamFocus     *focus;          /* the lens' autofocus, if the window gave one (cam_pipeline_set_focus) */
  gboolean      grab_wait;      /* a photo waits for the search that is running */
  gboolean      want_manual_af; /* the camera asked for has a lens: libcamera's autofocus is told to stay still (lock) */
};

G_DEFINE_FINAL_TYPE (CamPipeline, cam_pipeline, G_TYPE_OBJECT)

enum { SIG_STREAMING, SIG_ERROR, SIG_BARCODE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- messages from other threads to the main loop -------------------------------------------------------------- */

typedef struct {
  CamPipeline *self;
  int          kind;
  guint        generation;
  char        *text;
  GstSample   *sample;
} Note;

enum { NOTE_STREAMING, NOTE_FAILED, NOTE_DIED, NOTE_BARCODE, NOTE_GRAB };

static gboolean on_note (gpointer data);

static void
post (CamPipeline *self, int kind, guint generation, const char *text, GstSample *sample)
{
  Note *note = g_new0 (Note, 1);

  note->self = g_object_ref (self);
  note->kind = kind;
  note->generation = generation;
  note->text = g_strdup (text);
  note->sample = sample;
  g_main_context_invoke (NULL, on_note, note);
}

/* ---- the worker ------------------------------------------------------------------------------------------------ */

static void
set_state_wait (CamPipeline *self, GstState state)
{
  if (gst_element_set_state (self->pipeline, state) == GST_STATE_CHANGE_ASYNC)
    gst_element_get_state (self->pipeline, NULL, NULL, 5 * GST_SECOND);
}

/* libcamerasrc's controls are properties only since libcamera 0.4, and the test source has none */
static gboolean
src_has (CamPipeline *self, const char *property)
{
  return self->src && g_object_class_find_property (G_OBJECT_GET_CLASS (self->src), property);
}

/* The tuning goes to the live source (the controls are controllable properties: they act on the next requests, the
 * stream goes on). Without a tuning wanted the properties are left alone, unless one was sent before: they stay on
 * the element across cameras, so that is when the neutral values go out. */
static void
apply_tone (CamPipeline *self)
{
  const char *const names[] = { "gamma", "contrast", "saturation" };
  double values[3] = { 1, 1, 1 };
  gboolean wanted;

  g_mutex_lock (&self->lock);
  wanted = self->tone_wanted;
  if (wanted)
    memcpy (values, self->tone, sizeof values);
  if (!wanted && !self->tone_applied)
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  self->tone_applied = wanted;
  g_mutex_unlock (&self->lock);
  for (int i = 0; i < 3; i++)
    if (src_has (self, names[i]))
      g_object_set (self->src, names[i], (float) values[i], NULL);
}

void
cam_pipeline_set_tone (CamPipeline *self, gboolean enabled, double gamma, double contrast, double saturation)
{
  g_mutex_lock (&self->lock);
  self->tone_wanted = enabled;
  self->tone[0] = gamma;
  self->tone[1] = contrast;
  self->tone[2] = saturation;
  g_mutex_unlock (&self->lock);
  apply_tone (self);
}

static void
apply_caps (CamPipeline *self, int width, int height)
{
  g_autoptr(GstCaps) caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, width, "height", G_TYPE_INT,
                                                 height, NULL);

  g_object_set (self->caps, "caps", caps, NULL);
}

static gboolean
is_busy_error (const char *text)
{
  return text && (strstr (text, "in use") || strstr (text, "busy") || strstr (text, "acquire")
                  || strstr (text, "Busy"));
}

static void
run_request (gpointer job, gpointer data)
{
  CamPipeline *self = data;
  g_autofree char *camera = NULL;
  g_autofree char *error = NULL;
  gboolean active, reopen, name_set = FALSE, ok = FALSE;
  int width, height;
  guint generation;

  g_mutex_lock (&self->lock);
  active = self->want_active;
  camera = g_strdup (self->want_camera);
  width = self->want_w;
  height = self->want_h;
  generation = self->generation;
  if (self->force_reopen)
    self->cur_playing = FALSE;
  self->force_reopen = FALSE;
  g_mutex_unlock (&self->lock);

  if (!active || !camera)
    {
      if (self->cur_playing)
        g_message ("camera: closing %s", self->cur_camera);
      set_state_wait (self, GST_STATE_NULL);
      self->cur_playing = FALSE;
      return;
    }
  reopen = !self->cur_playing || g_strcmp0 (self->cur_camera, camera);
  if (!reopen && self->cur_w == width && self->cur_h == height)
    return;

  g_atomic_int_set (&self->in_transition, TRUE);
  for (guint attempt = 0; attempt < G_N_ELEMENTS (backoff_ms); attempt++)
    {
      gint64 deadline;
      gboolean manual_af;

      if (backoff_ms[attempt])
        {
          g_usleep (backoff_ms[attempt] * 1000);
          /* a newer request is queued behind this one: it does the rest */
          g_mutex_lock (&self->lock);
          gboolean stale = self->generation != generation;
          g_mutex_unlock (&self->lock);
          if (stale)
            break;
        }
      g_mutex_lock (&self->lock);
      g_clear_pointer (&self->last_error, g_free);
      self->got_frame = FALSE;
      g_mutex_unlock (&self->lock);

      if (reopen)
        {
          set_state_wait (self, GST_STATE_NULL);
          /* the element no longer plays what cur_camera says: a request that is stale before this one is over must
           * not take it for running */
          self->cur_playing = FALSE;
          /* the name goes to a fresh source: set again, libcamerasrc looks the camera up once more and fails
           * ("Could not find a camera named") while the camera it opened still lives in the element */
          if (!name_set && src_has (self, "camera-name"))
            {
              g_object_set (self->src, "camera-name", camera, NULL);
              name_set = TRUE;
            }
        }
      else
        set_state_wait (self, GST_STATE_READY);
      apply_caps (self, width, height);
      apply_tone (self);
      /* libcamera's soft ISP has an autofocus of its own, continuous by default and slow: for a camera whose lens
       * cam-focus.h drives it is told to leave the lens alone */
      g_mutex_lock (&self->lock);
      manual_af = self->want_manual_af;
      g_mutex_unlock (&self->lock);
      if (manual_af && src_has (self, "af-mode"))
        g_object_set (self->src, "af-mode", AF_MODE_MANUAL, NULL);

      g_mutex_lock (&self->lock);
      self->first_pending = generation;
      g_mutex_unlock (&self->lock);
      if (gst_element_set_state (self->pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE)
        {
          /* a live source plays at once; whether the camera really runs shows with its first frame or an error */
          deadline = g_get_monotonic_time () + FIRST_FRAME_TIMEOUT_S * G_TIME_SPAN_SECOND;
          g_mutex_lock (&self->lock);
          while (!self->got_frame && !self->last_error && self->generation == generation)
            if (!g_cond_wait_until (&self->cond, &self->lock, deadline))
              break;
          ok = self->got_frame && !self->last_error;
          g_mutex_unlock (&self->lock);
        }
      if (ok)
        break;

      g_mutex_lock (&self->lock);
      g_free (error);
      error = g_strdup (self->last_error ? self->last_error : "the camera gave no frame");
      self->first_pending = 0;
      g_mutex_unlock (&self->lock);
      g_message ("camera: opening %s at %dx%d (attempt %u): %s", camera, width, height, attempt + 1, error);
      set_state_wait (self, GST_STATE_NULL);
      self->cur_playing = FALSE;
      reopen = TRUE;
    }
  g_atomic_int_set (&self->in_transition, FALSE);

  if (ok)
    {
      g_free (self->cur_camera);
      self->cur_camera = g_strdup (camera);
      self->cur_w = width;
      self->cur_h = height;
      self->cur_playing = TRUE;
      return;
    }
  g_mutex_lock (&self->lock);
  gboolean stale = self->generation != generation;
  g_mutex_unlock (&self->lock);
  if (!stale)
    post (self, NOTE_FAILED, generation, error, NULL);
}

static void on_focus_state (CamFocus *focus, int state, int reason, CamPipeline *self);

static void
request (CamPipeline *self)
{
  g_thread_pool_push (self->worker, GUINT_TO_POINTER (1), NULL);
}

/* ---- the streaming threads ------------------------------------------------------------------------------------- */

static GstPadProbeReturn
on_frame (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  CamPipeline *self = data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  guint first;

  g_mutex_lock (&self->lock);
  first = self->first_pending;
  self->first_pending = 0;
  if (first)
    {
      self->got_frame = TRUE;
      g_cond_broadcast (&self->cond);
    }
  g_mutex_unlock (&self->lock);
  if (first)
    post (self, NOTE_STREAMING, first, NULL, NULL);

  if (self->focus)
    cam_focus_frame (self->focus, buffer, pad);
  if (g_atomic_int_compare_and_exchange (&self->grab_pending, TRUE, FALSE))
    {
      /* a copy: the camera's buffers go back to it at once (libcamera has a handful) */
      g_autoptr(GstCaps) caps = gst_pad_get_current_caps (pad);
      g_autoptr(GstBuffer) copy = gst_buffer_copy_deep (buffer);

      post (self, NOTE_GRAB, 0, NULL, gst_sample_new (copy, caps, NULL, NULL));
    }
  return GST_PAD_PROBE_OK;
}

static GstFlowReturn
on_record_sample (GstAppSink *sink, gpointer data)
{
  CamPipeline *self = data;
  g_autoptr(GstSample) sample = gst_app_sink_pull_sample (sink);

  if (!sample)
    return GST_FLOW_EOS;
  g_mutex_lock (&self->lock);
  if (self->rec_func)
    self->rec_func (sample, self->rec_data);
  g_mutex_unlock (&self->lock);
  return GST_FLOW_OK;
}

static GstBusSyncReply
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  CamPipeline *self = data;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_ERROR:
      {
        g_autoptr(GError) error = NULL;
        g_autofree char *debug = NULL;
        g_autofree char *text = NULL;

        gst_message_parse_error (message, &error, &debug);
        text = g_strdup_printf ("%s (%s)", error->message, GST_OBJECT_NAME (GST_MESSAGE_SRC (message)));
        g_mutex_lock (&self->lock);
        g_free (self->last_error);
        self->last_error = g_strdup (text);
        g_cond_broadcast (&self->cond);
        g_mutex_unlock (&self->lock);
        /* while streaming it is the main loop's to recover from; during a change, the worker's */
        if (!g_atomic_int_get (&self->in_transition))
          post (self, NOTE_DIED, 0, text, NULL);
        break;
      }
    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (message);
        const char *symbol = s && gst_structure_has_name (s, "barcode") ? gst_structure_get_string (s, "symbol") : NULL;

        if (symbol)
          post (self, NOTE_BARCODE, 0, symbol, NULL);
        break;
      }
    default:
      break;
    }
  return GST_BUS_DROP;
}

/* ---- the main loop's side -------------------------------------------------------------------------------------- */

static void
read_size (CamPipeline *self)
{
  g_autoptr(GstPad) pad = gst_element_get_static_pad (self->caps, "src");
  g_autoptr(GstCaps) caps = gst_pad_get_current_caps (pad);
  GstVideoInfo info;

  if (caps && gst_video_info_from_caps (&info, caps))
    {
      self->width = GST_VIDEO_INFO_WIDTH (&info);
      self->height = GST_VIDEO_INFO_HEIGHT (&info);
    }
}

static gboolean
on_note (gpointer data)
{
  Note *note = data;
  CamPipeline *self = note->self;

  switch (note->kind)
    {
    case NOTE_STREAMING:
      {
        g_mutex_lock (&self->lock);
        gboolean current = note->generation == self->generation && self->want_active;
        g_mutex_unlock (&self->lock);
        if (!current)
          break;
        self->streaming = TRUE;
        read_size (self);
        if (self->focus)
          cam_focus_streaming (self->focus, TRUE);
        g_signal_emit (self, signals[SIG_STREAMING], 0);
        break;
      }
    case NOTE_FAILED:
      g_signal_emit (self, signals[SIG_ERROR], 0, is_busy_error (note->text) ? CAM_PIPELINE_ERROR_BUSY
                                                                            : CAM_PIPELINE_ERROR_STREAM, note->text);
      break;
    case NOTE_DIED:
      if (!self->streaming)
        break;
      g_warning ("camera: the stream stopped: %s", note->text);
      self->streaming = FALSE;
      if (self->focus)
        cam_focus_streaming (self->focus, FALSE);
      g_mutex_lock (&self->lock);
      self->force_reopen = TRUE;
      self->generation++;
      g_mutex_unlock (&self->lock);
      request (self);
      g_signal_emit (self, signals[SIG_ERROR], 0, CAM_PIPELINE_ERROR_RECOVERABLE, note->text);
      break;
    case NOTE_BARCODE:
      if (self->scanning && !self->scan_hold)
        {
          /* one result, then nothing until the sheet is gone (QrImageAnalyzer) */
          self->scan_hold = TRUE;
          g_object_set (self->scan_valve, "drop", TRUE, NULL);
          g_signal_emit (self, signals[SIG_BARCODE], 0, note->text);
        }
      break;
    case NOTE_GRAB:
      {
        GSList *grabs = self->grabs;

        self->grabs = NULL;
        for (GSList *l = grabs; l; l = l->next->next)
          ((CamGrabFunc) l->data) (note->sample ? gst_sample_ref (note->sample) : NULL, l->next->data);
        g_slist_free (grabs);
        break;
      }
    }
  g_clear_pointer (&note->sample, gst_sample_unref);
  g_free (note->text);
  g_object_unref (note->self);
  g_free (note);
  return G_SOURCE_REMOVE;
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
cam_pipeline_dispose (GObject *object)
{
  CamPipeline *self = CAM_PIPELINE (object);

  if (self->worker)
    {
      cam_pipeline_stop (self);
      /* waits for the worker to finish the queue: the camera is closed when this returns */
      g_thread_pool_free (self->worker, FALSE, TRUE);
      self->worker = NULL;
    }
  if (self->pipeline)
    {
      g_autoptr(GstBus) bus = gst_element_get_bus (self->pipeline);

      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      gst_bus_set_sync_handler (bus, NULL, NULL, NULL);
    }
  g_clear_object (&self->paintable);
  g_clear_object (&self->focus);
  g_clear_object (&self->pipeline);
  g_clear_object (&self->clock);
  G_OBJECT_CLASS (cam_pipeline_parent_class)->dispose (object);
}

static void
cam_pipeline_finalize (GObject *object)
{
  CamPipeline *self = CAM_PIPELINE (object);

  g_free (self->want_camera);
  g_free (self->cur_camera);
  g_free (self->last_error);
  g_slist_free (self->grabs);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (cam_pipeline_parent_class)->finalize (object);
}

static void
cam_pipeline_class_init (CamPipelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cam_pipeline_dispose;
  object_class->finalize = cam_pipeline_finalize;
  signals[SIG_STREAMING] = g_signal_new ("streaming", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                         NULL, G_TYPE_NONE, 0);
  signals[SIG_ERROR] = g_signal_new ("error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
  signals[SIG_BARCODE] = g_signal_new ("barcode", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
cam_pipeline_init (CamPipeline *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  self->zoom = 1;
}

static gboolean
has_element (const char *name)
{
  g_autoptr(GstElementFactory) factory = gst_element_factory_find (name);

  return factory != NULL;
}

CamPipeline *
cam_pipeline_new (GError **error)
{
  /* NEO_CAMERA_TEST_SOURCE: a gst-launch fragment giving raw video in place of libcamerasrc (tests on a machine
   * whose camera must stay off; cam-devices then lists two test cameras), e.g. "videotestsrc is-live=true" */
  const char *test = g_getenv ("NEO_CAMERA_TEST_SOURCE");
  static const char *const needed[] = {
    "gtk4paintablesink", "tee", "queue", "valve", "videoconvertscale", "videocrop", "videoflip",
    "appsink", "fakesink", NULL,
  };
  g_autoptr(CamPipeline) self = NULL;
  g_autoptr(GstPad) pad = NULL;
  g_autoptr(GstBus) bus = NULL;
  gboolean scan = has_element ("zxing");
  g_autofree char *launch = NULL;

  if (!test && !has_element ("libcamerasrc"))
    {
      g_set_error (error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN, "GStreamer has no libcamerasrc");
      return NULL;
    }
  for (int i = 0; needed[i]; i++)
    if (!has_element (needed[i]))
      {
        g_set_error (error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN, "GStreamer has no %s", needed[i]);
        return NULL;
      }
  self = g_object_new (CAM_TYPE_PIPELINE, NULL);
  self->can_scan = scan;
  launch = g_strdup_printf (
    "%s ! capsfilter name=caps ! tee name=t "
    "t. ! queue max-size-buffers=1 leaky=downstream ! gtk4paintablesink name=view sync=false "
    "t. ! queue max-size-buffers=1 leaky=downstream ! valve name=scan drop=true "
    "   ! videoconvertscale ! video/x-raw,format=I420,width=800 ! %s ! fakesink sync=false async=false "
    "t. ! queue max-size-buffers=3 leaky=downstream ! valve name=rec drop=true ! videocrop name=crop "
    "   ! videoconvertscale n-threads=2 ! capsfilter name=reccaps ! videoflip name=flip "
    "   ! appsink name=recsink sync=false async=false max-buffers=4 drop=true",
    test ? test : "libcamerasrc name=src", scan ? "zxing try-rotate=true" : "identity name=noscan");
  self->pipeline = gst_parse_launch_full (launch, NULL, GST_PARSE_FLAG_FATAL_ERRORS, error);
  if (!self->pipeline)
    return NULL;
  gst_object_ref_sink (self->pipeline);
  self->src = gst_bin_get_by_name (GST_BIN (self->pipeline), "src");
  self->caps = gst_bin_get_by_name (GST_BIN (self->pipeline), "caps");
  self->view = gst_bin_get_by_name (GST_BIN (self->pipeline), "view");
  self->scan_valve = gst_bin_get_by_name (GST_BIN (self->pipeline), "scan");
  self->rec_valve = gst_bin_get_by_name (GST_BIN (self->pipeline), "rec");
  self->rec_crop = gst_bin_get_by_name (GST_BIN (self->pipeline), "crop");
  self->rec_caps = gst_bin_get_by_name (GST_BIN (self->pipeline), "reccaps");
  self->rec_flip = gst_bin_get_by_name (GST_BIN (self->pipeline), "flip");
  self->rec_sink = gst_bin_get_by_name (GST_BIN (self->pipeline), "recsink");
  /* the bin holds them; these are borrowed */
  if (self->src)
    gst_object_unref (self->src);
  gst_object_unref (self->caps);
  gst_object_unref (self->view);
  gst_object_unref (self->scan_valve);
  gst_object_unref (self->rec_valve);
  gst_object_unref (self->rec_crop);
  gst_object_unref (self->rec_caps);
  gst_object_unref (self->rec_flip);
  gst_object_unref (self->rec_sink);

  /* on the main thread, as gtk4paintablesink wants */
  g_object_get (self->view, "paintable", &self->paintable, NULL);
  self->clock = gst_system_clock_obtain ();
  gst_pipeline_use_clock (GST_PIPELINE (self->pipeline), self->clock);

  pad = gst_element_get_static_pad (self->caps, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, on_frame, self, NULL);
  gst_app_sink_set_callbacks (GST_APP_SINK (self->rec_sink),
                              &(GstAppSinkCallbacks) { .new_sample = on_record_sample }, self, NULL);
  bus = gst_element_get_bus (self->pipeline);
  gst_bus_set_sync_handler (bus, on_bus, self, NULL);
  self->worker = g_thread_pool_new (run_request, self, 1, FALSE, NULL);
  return g_steal_pointer (&self);
}

GdkPaintable *
cam_pipeline_get_paintable (CamPipeline *self)
{
  return self->paintable;
}

void
cam_pipeline_start (CamPipeline *self, const char *camera_id, int width, int height)
{
  g_mutex_lock (&self->lock);
  if (self->want_active && !g_strcmp0 (self->want_camera, camera_id) && self->want_w == width
      && self->want_h == height)
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  self->want_active = TRUE;
  g_free (self->want_camera);
  self->want_camera = g_strdup (camera_id);
  self->want_w = width;
  self->want_h = height;
  self->want_manual_af = self->focus && cam_focus_has_lens (self->focus, camera_id);
  self->generation++;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  self->streaming = FALSE;
  if (self->focus)
    {
      cam_focus_streaming (self->focus, FALSE);
      cam_focus_set_camera (self->focus, camera_id);
    }
  request (self);
}

void
cam_pipeline_stop (CamPipeline *self)
{
  g_mutex_lock (&self->lock);
  gboolean was = self->want_active;
  self->want_active = FALSE;
  self->generation++;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  self->streaming = FALSE;
  self->grab_wait = FALSE;
  if (self->focus)
    {
      cam_focus_streaming (self->focus, FALSE);
      cam_focus_set_camera (self->focus, NULL);
    }
  if (was)
    request (self);
}

gboolean
cam_pipeline_is_streaming (CamPipeline *self)
{
  return self->streaming;
}

void
cam_pipeline_get_size (CamPipeline *self, int *width, int *height)
{
  *width = self->streaming ? self->width : 0;
  *height = self->streaming ? self->height : 0;
}

/* a photo taken while the lens is still searching would be blurred: the grab is armed when the search is over (the
 * search ends by itself within the autofocus timeout) */
static void
on_focus_state (CamFocus *focus, int state, int reason, CamPipeline *self)
{
  if (self->grab_wait && state != CAM_FOCUS_SEARCHING)
    {
      self->grab_wait = FALSE;
      g_atomic_int_set (&self->grab_pending, TRUE);
    }
}

void
cam_pipeline_grab (CamPipeline *self, CamGrabFunc done, gpointer data)
{
  if (!self->streaming)
    {
      done (NULL, data);
      return;
    }
  self->grabs = g_slist_append (g_slist_append (self->grabs, done), data);
  if (self->focus && cam_focus_busy (self->focus))
    self->grab_wait = TRUE;
  else
    g_atomic_int_set (&self->grab_pending, TRUE);
}

void
cam_pipeline_set_scanning (CamPipeline *self, gboolean scanning)
{
  self->scanning = scanning;
  self->scan_hold = FALSE;
  g_object_set (self->scan_valve, "drop", !scanning, NULL);
}

gboolean
cam_pipeline_can_scan (CamPipeline *self)
{
  return self->can_scan;
}

void
cam_pipeline_scan_resume (CamPipeline *self)
{
  self->scan_hold = FALSE;
  g_object_set (self->scan_valve, "drop", !self->scanning, NULL);
}

static void
apply_crop (CamPipeline *self)
{
  int w = self->width, h = self->height;
  /* even pixels: the subsampled formats behind it want them */
  int x = w > 0 ? ((int) (w * (1 - 1 / self->zoom) / 2)) & ~1 : 0;
  int y = h > 0 ? ((int) (h * (1 - 1 / self->zoom) / 2)) & ~1 : 0;

  g_object_set (self->rec_crop, "left", x, "right", x, "top", y, "bottom", y, NULL);
}

void
cam_pipeline_set_recording (CamPipeline *self, CamFrameFunc func, gpointer data, const char *format,
                            gboolean mirror, int rotation)
{
  if (func)
    {
      g_autoptr(GstCaps) caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, format, "width",
                                                     G_TYPE_INT, self->width, "height", G_TYPE_INT, self->height,
                                                     NULL);
      /* a left-right mirror of the upright picture is a left-right flip of a frame shown as it is, a top-bottom
       * one of a frame shown turned a quarter (GstVideoOrientationMethod: 0 identity, 4 horiz, 5 vert) */
      int direction = !mirror ? 0 : rotation % 180 == 0 ? 4 : 5;

      g_object_set (self->rec_caps, "caps", caps, NULL);
      g_object_set (self->rec_flip, "video-direction", direction, NULL);
      apply_crop (self);
    }
  g_mutex_lock (&self->lock);
  self->rec_func = func;
  self->rec_data = data;
  g_mutex_unlock (&self->lock);
  g_object_set (self->rec_valve, "drop", func == NULL, NULL);
}

void
cam_pipeline_set_zoom (CamPipeline *self, double zoom)
{
  self->zoom = MAX (zoom, 1);
  apply_crop (self);
}

void
cam_pipeline_set_focus (CamPipeline *self, CamFocus *focus)
{
  g_clear_object (&self->focus);
  self->focus = focus ? g_object_ref (focus) : NULL;
  if (self->focus)
    g_signal_connect_object (self->focus, "state-changed", G_CALLBACK (on_focus_state), self, 0);
}

CamFocus *
cam_pipeline_get_focus (CamPipeline *self)
{
  return self->focus;
}

GstClock *
cam_pipeline_get_clock (CamPipeline *self)
{
  return self->clock;
}

GstClockTime
cam_pipeline_get_base_time (CamPipeline *self)
{
  return gst_element_get_base_time (self->pipeline);
}
