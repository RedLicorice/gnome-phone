/* cam-encoder: see cam-encoder.h. */
#include "cam-encoder.h"

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

/* the candidates, best first: the stateful V4L2 hardware encoders (Qualcomm Venus, Samsung MFC, …), VA-API, then
 * software */
static const char *const candidates[] = {
  "v4l2h264enc", "vah264enc", "vah264lpenc", "vaapih264enc", "x264enc", "openh264enc", NULL,
};

#define PROBE_TIMEOUT_S 45      /* up to four runs of 120 frames: a slow software encoder takes 7 s a run */

static CamEncoder answer;
static gboolean answered;

const char *
cam_encoder_format (const char *element)
{
  return !g_strcmp0 (element, "openh264enc") ? "I420" : "NV12";
}

char *
cam_encoder_launch (const char *element, int bitrate)
{
  if (!g_strcmp0 (element, "v4l2h264enc"))
    /* the V4L2 codec controls; the driver's default rate (1 Mbit/s on Venus) is far below a camera's */
    return g_strdup_printf ("v4l2h264enc extra-controls=\"controls,video_bitrate=%d,video_gop_size=30\" ! h264parse",
                            bitrate);
  if (!g_strcmp0 (element, "vah264enc") || !g_strcmp0 (element, "vah264lpenc"))
    return g_strdup_printf ("%s bitrate=%d key-int-max=30 ! h264parse", element, bitrate / 1000);
  if (!g_strcmp0 (element, "vaapih264enc"))
    return g_strdup_printf ("vaapih264enc bitrate=%d keyframe-period=30 ! h264parse", bitrate / 1000);
  if (!g_strcmp0 (element, "x264enc"))
    return g_strdup_printf ("x264enc speed-preset=ultrafast tune=zerolatency bitrate=%d key-int-max=30 ! h264parse",
                            bitrate / 1000);
  if (!g_strcmp0 (element, "openh264enc"))
    /* one thread: its slice threads were erratic on the Poco (1080p 45 → 49 fps, 720p 89 → 27) */
    return g_strdup_printf ("openh264enc bitrate=%d gop-size=30 ! h264parse", bitrate);
  return g_strdup_printf ("%s ! h264parse", element);
}

/* ---- the cache ------------------------------------------------------------------------------------------------- */

static char *
cache_key (void)
{
  struct utsname u;

  uname (&u);
  return g_strdup_printf ("%s %s", u.release, gst_version_string ());
}

static char *
cache_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "neo-camera", "encoder.ini", NULL);
}

static gboolean
load_cache (void)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *path = cache_path ();
  g_autofree char *key = cache_key ();
  g_autofree char *saved = NULL;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
    return FALSE;
  saved = g_key_file_get_string (file, "encoder", "key", NULL);
  if (g_strcmp0 (saved, key))
    return FALSE;
  answer.element = g_key_file_get_string (file, "encoder", "element", NULL);
  if (answer.element && !*answer.element)
    g_clear_pointer (&answer.element, g_free);
  answer.hd = g_key_file_get_boolean (file, "encoder", "hd", NULL);
  answer.fhd = g_key_file_get_boolean (file, "encoder", "fhd", NULL);
  answered = TRUE;
  return TRUE;
}

static void
save_cache (const char *measures)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *path = cache_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *key = cache_key ();

  g_key_file_set_comment (file, NULL, NULL, " neo-camera: the video encoder, measured once (cam-encoder.h)", NULL);
  g_key_file_set_string (file, "encoder", "key", key);
  g_key_file_set_string (file, "encoder", "element", answer.element ? answer.element : "");
  g_key_file_set_boolean (file, "encoder", "hd", answer.hd);
  g_key_file_set_boolean (file, "encoder", "fhd", answer.fhd);
  g_key_file_set_string (file, "encoder", "measured", measures);
  g_mkdir_with_parents (dir, 0700);
  g_key_file_save_to_file (file, path, NULL);
}

const CamEncoder *
cam_encoder_get (void)
{
  if (!answered)
    load_cache ();
  return answered ? &answer : NULL;
}

/* ---- the probe, parent side: one child per candidate, in order ------------------------------------------------- */

typedef struct {
  int           next;           /* the next candidate */
  GSubprocess  *child;
  guint         timeout;
  GString      *measures;       /* for the cache file: what each candidate did */
  char         *first_hd;       /* the first candidate that held 720p */
  GPtrArray    *waiters;        /* GCallback, data, GCallback, data, … */
} Probe;

static Probe *probe;

static void probe_next (void);

static void
probe_finish (void)
{
  if (!answer.element && probe->first_hd)
    {
      answer.element = g_strdup (probe->first_hd);
      answer.hd = TRUE;
    }
  answered = TRUE;
  /* only an encoder that works is kept: a run where every candidate failed (a busy boot, a package being
   * installed) is measured again at the next start instead of leaving the phone without video for good */
  if (answer.element)
    save_cache (probe->measures->str);
  g_message ("camera: video encoder %s (720p %s, 1080p %s; %s)", answer.element ? answer.element : "none",
             answer.hd ? "yes" : "no", answer.fhd ? "yes" : "no", probe->measures->str);
  for (guint i = 0; i + 1 < probe->waiters->len; i += 2)
    ((void (*) (gpointer)) probe->waiters->pdata[i]) (probe->waiters->pdata[i + 1]);
  g_ptr_array_unref (probe->waiters);
  g_string_free (probe->measures, TRUE);
  g_free (probe->first_hd);
  g_clear_pointer (&probe, g_free);
}

static void
on_child_done (GObject *object, GAsyncResult *result, gpointer data)
{
  const char *element = data;
  g_autoptr(GSubprocess) child = G_SUBPROCESS (object);
  g_autofree char *out = NULL;
  g_autofree char *err = NULL;
  double fhd = 0, hd = 0;
  gboolean ok;

  if (probe->timeout)
    g_source_remove (probe->timeout);
  probe->timeout = 0;
  probe->child = NULL;
  ok = g_subprocess_communicate_utf8_finish (child, result, &out, &err, NULL)
       && g_subprocess_get_if_exited (child) && g_subprocess_get_exit_status (child) == 0 && out
       && sscanf (out, "%lf %lf", &fhd, &hd) == 2;
  if (out)
    g_strstrip (out);
  if (!ok)
    {
      /* why it failed: how the child ended and the last line it wrote to stderr */
      const char *last = err ? strrchr (g_strstrip (err), '\n') : NULL;

      if (g_subprocess_get_if_signaled (child))
        g_message ("camera: the %s probe failed: signal %d: %s", element, g_subprocess_get_term_sig (child),
                   last ? last + 1 : err && *err ? err : "no output");
      else
        g_message ("camera: the %s probe failed: exit status %d: %s", element,
                   g_subprocess_get_if_exited (child) ? g_subprocess_get_exit_status (child) : -1,
                   last ? last + 1 : err && *err ? err : "no output");
    }
  g_string_append_printf (probe->measures, "%s%s %s", probe->measures->len ? ", " : "", element,
                          ok ? out : "failed");
  if (ok && fhd >= CAM_ENCODER_MIN_FPS)
    {
      answer.element = g_strdup (element);
      answer.fhd = TRUE;
      answer.hd = hd >= CAM_ENCODER_MIN_FPS;
      probe_finish ();
      return;
    }
  if (ok && hd >= CAM_ENCODER_MIN_FPS && !probe->first_hd)
    probe->first_hd = g_strdup (element);
  probe_next ();
}

static gboolean
on_child_timeout (gpointer data)
{
  probe->timeout = 0;
  /* a hung driver: the child goes, and the candidate counts as failed */
  if (probe->child)
    g_subprocess_force_exit (probe->child);
  return G_SOURCE_REMOVE;
}

static void
probe_next (void)
{
  while (candidates[probe->next])
    {
      const char *element = candidates[probe->next++];
      g_autoptr(GstElementFactory) factory = gst_element_factory_find (element);
      g_autoptr(GError) error = NULL;
      GSubprocess *child;

      if (!factory)
        continue;
      child = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
                                "/proc/self/exe", "--probe-encoder", element, NULL);
      if (!child)
        {
          g_warning ("camera: cannot probe %s: %s", element, error->message);
          continue;
        }
      probe->child = child;
      probe->timeout = g_timeout_add_seconds (PROBE_TIMEOUT_S, on_child_timeout, NULL);
      g_subprocess_communicate_utf8_async (child, NULL, NULL, on_child_done, (gpointer) element);
      return;
    }
  probe_finish ();
}

void
cam_encoder_probe (GCallback done, gpointer data)
{
  if (cam_encoder_get ())
    {
      if (done)
        ((void (*) (gpointer)) done) (data);
      return;
    }
  if (!probe)
    {
      probe = g_new0 (Probe, 1);
      probe->measures = g_string_new (NULL);
      probe->waiters = g_ptr_array_new ();
    }
  if (done)
    {
      g_ptr_array_add (probe->waiters, done);
      g_ptr_array_add (probe->waiters, data);
    }
  if (!probe->child && !probe->timeout && probe->next == 0)
    probe_next ();
}

/* ---- the probe, child side ------------------------------------------------------------------------------------- */

typedef struct {
  gint64 first, last;
  int    frames;
  gint64 bytes;
} Count;

/* an encoder that writes more than this many times the bitrate it was asked for does not keep to it: the Poco's Venus
 * writes 40 Mbit/s of the test picture and 150 of a dark room for 16, whatever its rate and QP controls say */
#define BITRATE_TOLERANCE 2

static GstPadProbeReturn
on_encoded (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  Count *count = data;
  gint64 now = g_get_monotonic_time ();

  if (!count->frames++)
    count->first = now;
  count->last = now;
  count->bytes += gst_buffer_get_size (GST_PAD_PROBE_INFO_BUFFER (info));
  return GST_PAD_PROBE_OK;
}

/* frames a second, 0 if it fails or does not keep to the bitrate; *kbps what it wrote of the 30 fps picture */
static double
measure (const char *element, int width, int height, int *kbps)
{
  int bitrate = width >= 1920 ? 16000000 : 8000000;
  g_autofree char *encoder = cam_encoder_launch (element, bitrate);
  g_autofree char *launch = g_strdup_printf ("videotestsrc num-buffers=120 ! video/x-raw,format=%s,width=%d,height=%d,"
                                             "framerate=30/1 ! %s ! fakesink name=sink sync=false",
                                             cam_encoder_format (element), width, height, encoder);
  g_autoptr(GError) error = NULL;
  GstElement *pipeline = gst_parse_launch (launch, &error);
  g_autoptr(GstBus) bus = NULL;
  g_autoptr(GstMessage) message = NULL;
  g_autoptr(GstPad) pad = NULL;
  GstElement *sink;
  Count count = { 0 };
  double fps = 0;

  if (!pipeline)
    return 0;
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_object_unref (sink);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, on_encoded, &count, NULL);
  bus = gst_element_get_bus (pipeline);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  message = gst_bus_timed_pop_filtered (bus, 15 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  /* counted from the first encoded frame: a hardware encoder's start-up is not its speed */
  if (message && GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS && count.frames > 30 && count.last > count.first)
    {
      fps = (count.frames - 1) / ((count.last - count.first) / 1e6);
      *kbps = (int) (count.bytes * 8 * 30 / count.frames / 1000);
      if (*kbps > BITRATE_TOLERANCE * bitrate / 1000)
        fps = 0;
    }
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  return fps;
}

int
cam_encoder_probe_main (const char *element)
{
  double fhd, hd;
  int fhd_kbps = 0, hd_kbps = 0;

  gst_init (NULL, NULL);
  /* a hardware encoder's first session after a boot or a resume can run at a quarter of its speed (the Poco's Venus:
   * 17.6 fps, then 72.6), and the answer is kept: a size that falls short is measured once more */
  fhd = measure (element, 1920, 1080, &fhd_kbps);
  if (fhd < CAM_ENCODER_MIN_FPS)
    fhd = MAX (fhd, measure (element, 1920, 1080, &fhd_kbps));
  hd = measure (element, 1280, 720, &hd_kbps);
  if (hd < CAM_ENCODER_MIN_FPS)
    hd = MAX (hd, measure (element, 1280, 720, &hd_kbps));
  /* "fps1080 fps720 (kbit/s1080 kbit/s720)": the parent reads the first two, the cache keeps it all */
  printf ("%.1f %.1f (%d %d kbit/s)\n", fhd, hd, fhd_kbps, hd_kbps);
  /* the numbers say what failed; a crash or a hang is what the parent counts as the child failing */
  return 0;
}
