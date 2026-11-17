/* Autofocus on the real camera, without the window: libcamerasrc into a fakesink, cam-focus on its frames.
 *   live-af [camera-id] [auto|tap|manual] [x y]        run it on the phone, with the camera free; G_MESSAGES_DEBUG=all shows
 *                                                      every position and score
 * Searches once (at the middle of the frame, or at x y, fractions of it), prints the state changes with the time
 * since the first frame and the lens position, then repeats the search twice more (tap-like) and quits. Meant for
 * tuning the "af-*" keys and for checking the lens after a kernel change. */
#include "cam-devices.h"
#include "cam-focus.h"
#include "cam-pipeline.h"

#include <stdlib.h>

static GMainLoop *loop;
static gint64 t0;
static int searches_left = 3;
static double ax = 0.5, ay = 0.5;

static const char *const states[] = { "idle", "searching", "focused", "failed" };
static const char *const reasons[] = { "", "flat", "low-contrast", "timeout" };

static void
again (gpointer focus)
{
  cam_focus_at (focus, ax, ay);
}

static void
on_state (CamFocus *focus, int state, int reason, gpointer data)
{
  g_print ("%6.2f s  %s %s  lens %.0f %%\n", (g_get_monotonic_time () - t0) / 1e6, states[state], reasons[reason],
           cam_focus_get_position (focus) * 100);
  if (state == CAM_FOCUS_FOCUSED || state == CAM_FOCUS_FAILED)
    {
      if (--searches_left <= 0)
        g_main_loop_quit (loop);
      else
        g_timeout_add_once (500, again, focus);
    }
}

static gboolean
on_first_frame (gpointer data)
{
  t0 = g_get_monotonic_time ();
  cam_focus_streaming (data, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean seen;

static GstPadProbeReturn
on_frame (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  if (g_atomic_int_compare_and_exchange (&seen, FALSE, TRUE))
    g_idle_add (on_first_frame, data);
  cam_focus_frame (data, GST_PAD_PROBE_INFO_BUFFER (info), pad);
  return GST_PAD_PROBE_OK;
}

static gboolean
on_timeout (gpointer data)
{
  g_printerr ("live-af: no result after 60 s\n");
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  g_autoptr(GSettings) settings = g_settings_new ("de.yesman.Camera");
  g_autoptr(GPtrArray) cameras = NULL;
  g_autofree char *desc = NULL, *id = NULL;
  g_autoptr(GError) error = NULL;
  GstElement *pipeline, *caps;
  g_autoptr(GstPad) pad = NULL;
  CamFocus *focus;
  const char *mode = argc > 2 ? argv[2] : "auto";

  gst_init (&argc, &argv);
  if (argc > 1)
    id = g_strdup (argv[1]);
  else
    {
      cameras = cam_devices_enumerate ();
      CamCamera *main_camera = cam_devices_main_for (cameras, CAM_FACING_BACK);

      id = main_camera ? g_strdup (main_camera->id) : NULL;
    }
  if (!id)
    {
      g_printerr ("live-af: no camera\n");
      return 1;
    }
  if (argc > 4)
    {
      ax = g_ascii_strtod (argv[3], NULL);
      ay = g_ascii_strtod (argv[4], NULL);
    }
  focus = cam_focus_new (settings);
  cam_focus_set_mode (focus, g_str_equal (mode, "manual") ? CAM_FOCUS_MANUAL
                             : g_str_equal (mode, "tap") ? CAM_FOCUS_TAP : CAM_FOCUS_AUTO);
  cam_focus_set_camera (focus, id);
  desc = g_strdup_printf ("libcamerasrc camera-name=\"%s\" %s ! capsfilter name=caps ! fakesink sync=false", id,
                          cam_focus_has_lens (focus, id) ? "af-mode=manual" : "");
  pipeline = gst_parse_launch (desc, &error);
  if (!pipeline)
    {
      g_printerr ("live-af: %s\n", error->message);
      return 1;
    }
  caps = gst_bin_get_by_name (GST_BIN (pipeline), "caps");
  pad = gst_element_get_static_pad (caps, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, on_frame, focus, NULL);
  g_signal_connect (focus, "state-changed", G_CALLBACK (on_state), NULL);
  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add_seconds (60, on_timeout, NULL);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  g_print ("live-af: %s, mode %s, lens %s\n", id, mode, cam_focus_has_lens (focus, id) ? "yes" : "none");
  g_main_loop_run (loop);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_object_unref (focus);
  return 0;
}
