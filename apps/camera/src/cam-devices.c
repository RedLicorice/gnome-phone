/* cam-devices: see cam-devices.h. */
#include "cam-devices.h"

#include <gio/gio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void
camera_free (CamCamera *camera)
{
  g_free (camera->id);
  g_free (camera->model);
  gst_clear_caps (&camera->caps);
  g_free (camera);
}

/* properties are in the device's "camera-properties" structure as api.libcamera.<Property> */
static int
prop_int (const GstStructure *props, const char *name, int fallback)
{
  int value;

  return props && gst_structure_get_int (props, name, &value) ? value : fallback;
}

static void
prop_size (const GstStructure *props, const char *name, int *w, int *h)
{
  const GValue *value = props ? gst_structure_get_value (props, name) : NULL;

  *w = *h = 0;
  /* an array of two ints (libcamera's Size), as GstValueArray */
  if (value && GST_VALUE_HOLDS_ARRAY (value) && gst_value_array_get_size (value) == 2)
    {
      *w = g_value_get_int (gst_value_array_get_value (value, 0));
      *h = g_value_get_int (gst_value_array_get_value (value, 1));
    }
}

/* NEO_CAMERA_TEST_SOURCE (cam-pipeline.c): a back camera turned 90° and a front one turned 270°, as on most phones,
 * with the sizes a phone's camera lists */
static void
add_test_cameras (GPtrArray *cameras)
{
  static const char *const sizes =
    "video/x-raw,format=BGRx,width=640,height=480; video/x-raw,format=BGRx,width=1280,height=720;"
    "video/x-raw,format=BGRx,width=1600,height=1200; video/x-raw,format=BGRx,width=1920,height=1080;"
    "video/x-raw,format=BGRx,width=2592,height=1944";

  for (int i = 0; i < 2; i++)
    {
      CamCamera *camera = g_new0 (CamCamera, 1);

      camera->id = g_strdup (i ? "test-front" : "test-back");
      camera->model = g_strdup ("test");
      camera->facing = i ? CAM_FACING_FRONT : CAM_FACING_BACK;
      camera->orientation = i ? 270 : 90;
      camera->pixels_w = 2592;
      camera->pixels_h = 1944;
      camera->caps = gst_caps_from_string (sizes);
      camera->flash = !i && cam_flash_led () != NULL;
      g_ptr_array_add (cameras, camera);
    }
}

GPtrArray *
cam_devices_enumerate (void)
{
  GPtrArray *cameras = g_ptr_array_new_with_free_func ((GDestroyNotify) camera_free);
  GstDeviceProvider *provider;
  GList *devices;

  if (g_getenv ("NEO_CAMERA_TEST_SOURCE"))
    {
      add_test_cameras (cameras);
      return cameras;
    }
  provider = gst_device_provider_factory_get_by_name ("libcameraprovider");
  if (!provider)
    {
      g_warning ("camera: no libcamera device provider (gstreamer1.0-libcamera)");
      return cameras;
    }
  /* get_devices probes once and lets go of libcamera again: the one libcamerasrc of the app opens it after this */
  devices = gst_device_provider_get_devices (provider);
  for (GList *l = devices; l; l = l->next)
    {
      GstDevice *device = l->data;
      g_autoptr(GstStructure) props = gst_device_get_properties (device);
      CamCamera *camera = g_new0 (CamCamera, 1);
      const char *location = props ? gst_structure_get_string (props, "api.libcamera.Location") : NULL;
      const char *model = props ? gst_structure_get_string (props, "api.libcamera.Model") : NULL;
      /* the display name is the camera's id (gstlibcameraprovider.cpp gives "name" and "display-name" both
       * camera->id(); its "name" is write-only) */
      g_autofree char *name = gst_device_get_display_name (device);
      int rotation;

      camera->id = g_strdup (name);
      camera->model = g_strdup (model);
      camera->facing = !g_strcmp0 (location, "CameraLocationFront") ? CAM_FACING_FRONT
                     : !g_strcmp0 (location, "CameraLocationExternal") ? CAM_FACING_EXTERNAL : CAM_FACING_BACK;
      /* libcamera counts its Rotation anticlockwise; Android's sensor orientation is the clockwise turn that makes
       * the frame upright (libcamera's own Android HAL: orientation_ = (360 - rotation) % 360) */
      rotation = prop_int (props, "api.libcamera.Rotation", 0);
      camera->orientation = ((360 - rotation) % 360 + 360) % 360;
      prop_size (props, "api.libcamera.PixelArraySize", &camera->pixels_w, &camera->pixels_h);
      camera->caps = gst_device_get_caps (device);
      camera->flash = camera->facing == CAM_FACING_BACK && cam_flash_led () != NULL;
      if (camera->id)
        g_ptr_array_add (cameras, camera);
      else
        camera_free (camera);
    }
  g_list_free_full (devices, gst_object_unref);
  gst_object_unref (provider);
  return cameras;
}

CamCamera *
cam_devices_main_for (GPtrArray *cameras, CamFacing facing)
{
  CamCamera *best = NULL;

  for (guint i = 0; i < cameras->len; i++)
    {
      CamCamera *camera = cameras->pdata[i];

      if (camera->facing != facing)
        continue;
      /* the first one wins a tie, libcamera's order */
      if (!best || (gint64) camera->pixels_w * camera->pixels_h > (gint64) best->pixels_w * best->pixels_h)
        best = camera;
    }
  return best;
}

/* ---- sizes ---------------------------------------------------------------------------------------------------- */

static gboolean
is_rgb_format (const GstStructure *s)
{
  const char *format = gst_structure_get_string (s, "format");

  /* what the soft ISP gives (RGBA/RGBx/BGRA/BGRx); a list or no format at all is fine too */
  return !format || g_str_has_prefix (format, "RGB") || g_str_has_prefix (format, "BGR")
         || g_str_has_prefix (format, "xRGB") || g_str_has_prefix (format, "xBGR");
}

static gboolean
range_contains (const GValue *value, int v)
{
  if (G_VALUE_HOLDS_INT (value))
    return g_value_get_int (value) == v;
  if (GST_VALUE_HOLDS_INT_RANGE (value))
    {
      int min = gst_value_get_int_range_min (value), max = gst_value_get_int_range_max (value);
      int step = gst_value_get_int_range_step (value);
      return v >= min && v <= max && (step <= 1 || (v - min) % step == 0);
    }
  return FALSE;
}

gboolean
cam_camera_has_size (CamCamera *camera, int width, int height)
{
  for (guint i = 0; camera->caps && i < gst_caps_get_size (camera->caps); i++)
    {
      const GstStructure *s = gst_caps_get_structure (camera->caps, i);
      const GValue *w = gst_structure_get_value (s, "width"), *h = gst_structure_get_value (s, "height");

      if (w && h && is_rgb_format (s) && range_contains (w, width) && range_contains (h, height))
        return TRUE;
    }
  return FALSE;
}

gboolean
cam_camera_pick_size (CamCamera *camera, int aw, int ah, int pixels, int *width, int *height)
{
  double best_score = G_MAXDOUBLE;
  int best_w = 0, best_h = 0;

  /* the exact sizes libcamera lists first: they are the ones every pipeline handler means */
  for (guint i = 0; camera->caps && i < gst_caps_get_size (camera->caps); i++)
    {
      const GstStructure *s = gst_caps_get_structure (camera->caps, i);
      int w, h;

      if (!is_rgb_format (s) || !gst_structure_get_int (s, "width", &w) || !gst_structure_get_int (s, "height", &h))
        continue;
      if ((gint64) w * ah != (gint64) h * aw)
        continue;
      /* libcamera lists 1400×1050 for every camera, and the soft ISP's GPU debayer then fails ("debayerGPU failed", no
       * frame, and libcamera asserts when the stream is stopped, taking the app with it); every size it does debayer
       * has a width that is a multiple of 16. The 1.9 MP sensors of the macro and depth cameras list nothing above
       * 1400×1050, so they come out at 1152×864. */
      if (w % 16)
        continue;
      /* nearest to the target in the ratio of areas, either way */
      double score = fabs (log ((double) w * h / pixels));
      if (score < best_score)
        best_score = score, best_w = w, best_h = h;
    }
  /* else a size of that aspect inside a range */
  if (!best_w)
    for (guint i = 0; camera->caps && i < gst_caps_get_size (camera->caps); i++)
      {
        const GstStructure *s = gst_caps_get_structure (camera->caps, i);
        const GValue *wv = gst_structure_get_value (s, "width"), *hv = gst_structure_get_value (s, "height");

        if (!is_rgb_format (s) || !wv || !hv || !GST_VALUE_HOLDS_INT_RANGE (wv) || !GST_VALUE_HOLDS_INT_RANGE (hv))
          continue;
        /* a 16 × 16 multiple of the aspect fits every step libcamera uses (4 across, 2 down); shrink until it fits */
        int max_w = gst_value_get_int_range_max (wv), max_h = gst_value_get_int_range_max (hv);
        int unit = 16, k = (int) floor (sqrt ((double) pixels / (aw * ah)) / unit);
        for (; k > 0; k--)
          {
            int w = aw * unit * k, h = ah * unit * k;
            if (w <= max_w && h <= max_h && range_contains (wv, w) && range_contains (hv, h))
              {
                best_w = w, best_h = h;
                break;
              }
          }
        if (best_w)
          break;
      }
  if (!best_w)
    return FALSE;
  *width = best_w;
  *height = best_h;
  return TRUE;
}

/* ---- the flash LED -------------------------------------------------------------------------------------------- */

/* The LED is the one the kernel names for the job ("devicename:colour:function", the function "torch" or "flash"),
 * found as gmobile, Phosh and the shell's torch tile (extension/state.js findTorch) find it. It is lit through logind,
 * which lets the active session set its seat's LEDs, so the app needs no udev rule and no write access to sysfs. The
 * torch (and the TORCH mode) is a quarter of the LED's range, as the shell's tile lights it, and never below what
 * gmobile's hwdb says still shows; the flash for a photo is all of it, for the second it is on. */
#define LED_TORCH_SHARE 0.25

static char *led_name;
static int   led_max, led_torch;

/* a property udev gave the device (its database, /run/udev/data/+<subsystem>:<sysname>, "E:KEY=value" lines, what
 * libudev's udev_device_get_property_value reads), or NULL */
static char *
udev_property (const char *subsystem, const char *sysname, const char *key)
{
  g_autofree char *path = g_strdup_printf ("/run/udev/data/+%s:%s", subsystem, sysname);
  g_autofree char *data = NULL;
  g_autofree char *prefix = g_strdup_printf ("E:%s=", key);
  g_auto(GStrv) lines = NULL;

  if (!g_file_get_contents (path, &data, NULL, NULL))
    return NULL;
  lines = g_strsplit (data, "\n", -1);
  for (char **l = lines; *l; l++)
    if (g_str_has_prefix (*l, prefix))
      return g_strdup (*l + strlen (prefix));
  return NULL;
}

static int
read_int (const char *dir, const char *name)
{
  g_autofree char *path = g_build_filename ("/sys/class/leds", dir, name, NULL);
  g_autofree char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return -1;
  return atoi (text);
}

const char *
cam_flash_led (void)
{
  static const char *const functions[] = { "torch", "flash" };
  static gboolean looked;

  if (looked)
    return led_name;
  looked = TRUE;
  /* the shell's order: a torch function first, then a flash one */
  for (guint f = 0; f < G_N_ELEMENTS (functions) && !led_name; f++)
    {
      g_autoptr(GDir) dir = g_dir_open ("/sys/class/leds", 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          const char *function = strrchr (name, ':');
          g_autofree char *min_text = NULL;
          int max, min;

          if (!function || !g_str_equal (function + 1, functions[f]) || (max = read_int (name, "max_brightness")) <= 0)
            continue;
          min_text = udev_property ("leds", name, "GM_TORCH_MIN_BRIGHTNESS");
          min = min_text ? atoi (min_text) : 0;
          led_name = g_strdup (name);
          led_max = max;
          led_torch = MIN (max, MAX (MAX (min, 1), (int) round (max * LED_TORCH_SHARE)));
          g_message ("camera: flash LED %s (torch %d, flash %d of %d)", name, led_torch, led_max, led_max);
          break;
        }
    }
  return led_name;
}

gboolean
cam_flash_led_is_lit (void)
{
  return led_name && read_int (led_name, "brightness") > 0;
}

static GDBusConnection *bus;

static GDBusConnection *
system_bus (void)
{
  static gboolean tried;

  if (!tried)
    {
      g_autoptr(GError) error = NULL;

      tried = TRUE;
      bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
      if (!bus)
        g_warning ("camera: the flash LED: no system bus: %s", error->message);
    }
  return bus;
}

static int
led_level (CamLed led)
{
  return led == CAM_LED_FLASH ? led_max : led == CAM_LED_TORCH ? led_torch : 0;
}

/* what the LED does, read back from sysfs: what was asked for is not what happened when logind says no */
static void
check_led (CamLed led, GError *error)
{
  gboolean lit = cam_flash_led_is_lit ();

  if (error)
    g_warning ("camera: the flash LED (%s): %s", led_name, error->message);
  if (lit != (led != CAM_LED_OFF))
    g_warning ("camera: the flash LED (%s) is %s, asked for %d", led_name, lit ? "lit" : "dark", led_level (led));
}

static void
on_led_set (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);

  check_led (GPOINTER_TO_INT (data), error);
}

static gboolean
led_call (CamLed led, gboolean wait)
{
  GDBusConnection *bus;
  GVariant *args;

  if (!cam_flash_led ())
    return FALSE;
  /* tests: everything up to the LED, which stays dark; turning it off still goes to logind, and lights nothing */
  if (led != CAM_LED_OFF && g_strcmp0 (g_getenv ("NEO_CAMERA_NO_LED"), "1") == 0)
    {
      g_message ("camera: flash LED %s to %d (NEO_CAMERA_NO_LED: not lit)", led_name, led_level (led));
      return FALSE;
    }
  if (!(bus = system_bus ()))
    return FALSE;
  args = g_variant_new ("(ssu)", "leds", led_name, (guint32) led_level (led));
  if (wait)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync (bus, "org.freedesktop.login1",
                                                               "/org/freedesktop/login1/session/auto",
                                                               "org.freedesktop.login1.Session", "SetBrightness",
                                                               args, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
      check_led (led, error);
    }
  else
    g_dbus_connection_call (bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                            "org.freedesktop.login1.Session", "SetBrightness", args, NULL, G_DBUS_CALL_FLAGS_NONE,
                            2000, NULL, on_led_set, GINT_TO_POINTER (led));
  return TRUE;
}

void
cam_flash_led_set (CamLed led)
{
  led_call (led, FALSE);
}

void
cam_flash_led_set_sync (CamLed led)
{
  led_call (led, TRUE);
}

void
cam_flash_led_flush (void)
{
  if (bus)
    g_dbus_connection_flush_sync (bus, NULL, NULL);
}

/* ---- the phone's name ----------------------------------------------------------------------------------------- */

static char *
read_line (const char *path)
{
  g_autofree char *text = NULL;
  gsize length;

  if (!g_file_get_contents (path, &text, &length, NULL))
    return NULL;
  /* devicetree strings end in NUL, sysfs ones in a newline */
  text[strcspn (text, "\n")] = 0;
  g_strstrip (text);
  return *text && g_ascii_strcasecmp (text, "Unknown") && g_ascii_strcasecmp (text, "Default string")
         ? g_steal_pointer (&text) : NULL;
}

static char *make, *model;

static void
read_names (void)
{
  static gboolean done;

  if (done)
    return;
  done = TRUE;
  model = read_line ("/sys/class/dmi/id/product_name");
  if (!model)
    model = read_line ("/proc/device-tree/model");
  make = read_line ("/sys/class/dmi/id/sys_vendor");
  /* no vendor: the model's first word ("Xiaomi POCO X3 NFC") */
  if (!make && model)
    make = g_strndup (model, strcspn (model, " "));
  /* a vendor id in lower case ("xiaomi") as Android's Build.MANUFACTURER spells it ("Xiaomi") */
  if (make && g_ascii_islower (make[0]))
    make[0] = g_ascii_toupper (make[0]);
  if (!make)
    make = g_strdup ("");
  if (!model)
    model = g_strdup ("");
}

const char *
cam_device_make (void)
{
  read_names ();
  return make;
}

const char *
cam_device_model (void)
{
  read_names ();
  return model;
}
