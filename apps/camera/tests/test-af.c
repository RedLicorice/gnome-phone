/* The autofocus search on synthetic sharpness curves (noise, plateau, two peaks, nothing to focus on), the sharpness
 * measure, the config from the schema's "af-*" keys (GSettings' memory backend; CAM_SCHEMA_DIR is the directory with the
 * schema) and the lens lookup in a fake devicetree. */
#include "cam-af.h"
#include "cam-lens.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>

static GSettings *settings;
static CamAfConfig config;

typedef double (*Curve) (int position);

static double noise_pct;      /* ± this fraction of the value, from a fixed sequence */
static guint32 seed;

static double
noisy (double value)
{
  seed = seed * 1664525u + 1013904223u;
  return value * (1 + noise_pct * ((seed >> 8) / (double) (1 << 24) * 2 - 1));
}

/* a peak of @height over a floor of 0.02, @width counts wide (sigma) */
static double
bump (int p, double centre, double width, double height)
{
  return 0.02 + height * exp (-pow ((p - centre) / width, 2) / 2);
}

static double peak_700 (int p) { return bump (p, 700, 60, 4); }
static double peak_far (int p) { return bump (p, 1023, 60, 4); }
static double peak_near (int p) { return bump (p, 384, 60, 4); }
static double plateau (int p) { return p >= 600 && p <= 700 ? 3 : 0.02 + 3 * exp (-pow (MIN (abs (p - 600), abs (p - 700)) / 40.0, 2)); }
static double weak_then_strong (int p) { return bump (p, 450, 40, 0.03) + bump (p, 900, 50, 4) - 0.02; }
static double strong_then_weak (int p) { return bump (p, 560, 40, 4) + bump (p, 960, 50, 3) - 0.02; }
static double blank (int p) { return 0.5 + 0.004 * sin (p); }
static double dark (int p) { return 0.00001 + 0.00001 * sin (p); }

static CamAfResult
run (Curve curve, int *log, int *log_n)
{
  CamAfSearch *search = cam_af_search_new (&config, config.pos_min, config.pos_max);
  CamAfResult result;
  int position, n = 0;

  seed = 12345;
  while (cam_af_search_next (search, &position))
    {
      g_assert_cmpint (position, >=, config.pos_min);
      g_assert_cmpint (position, <=, config.pos_max);
      if (log)
        log[n] = position;
      n++;
      cam_af_search_feed (search, noisy (curve (position)));
    }
  cam_af_search_result (search, &result);
  if (log_n)
    *log_n = n;
  cam_af_search_free (search);
  return result;
}

static void
test_single_peak (void)
{
  noise_pct = 0;
  CamAfResult r = run (peak_700, NULL, NULL);

  g_assert_true (r.ok);
  g_assert_cmpint (abs (r.position - 700), <=, 12);
  g_assert_cmpfloat (r.contrast, >, 50);
}

static void
test_peaks_at_the_ends (void)
{
  noise_pct = 0;
  g_assert_cmpint (abs (run (peak_far, NULL, NULL).position - 1023), <=, 15);
  g_assert_cmpint (abs (run (peak_near, NULL, NULL).position - 384), <=, 15);
}

static void
test_noise (void)
{
  noise_pct = 0.08;
  for (int i = 0; i < 20; i++)
    {
      CamAfResult r;

      noise_pct = 0.08;
      r = run (peak_700, NULL, NULL);
      g_assert_true (r.ok);
      g_assert_cmpint (abs (r.position - 700), <=, 45);
      seed += i;      /* another noise sequence each time */
    }
}

static void
test_plateau_takes_the_middle (void)
{
  noise_pct = 0;
  CamAfResult r = run (plateau, NULL, NULL);

  g_assert_true (r.ok);
  g_assert_cmpint (abs (r.position - 650), <=, 40);
}

static void
test_two_peaks (void)
{
  CamAfResult r;

  noise_pct = 0;
  /* a weak bump before the real peak does not end the sweep (it is not 6× over the worst score, as a clear peak is) */
  r = run (weak_then_strong, NULL, NULL);
  g_assert_true (r.ok);
  g_assert_cmpint (abs (r.position - 900), <=, 20);
  /* a clear peak first ends the sweep: the one beyond it is not looked for (documented limit) */
  r = run (strong_then_weak, NULL, NULL);
  g_assert_true (r.ok);
  g_assert_cmpint (abs (r.position - 560), <=, 20);
}

static void
test_stops_early (void)
{
  int n;

  noise_pct = 0;
  run (peak_700 /* 5th of 8 coarse positions */, NULL, &n);
  /* the sweep stops after two scores fall; coarse 8 + refine 4 would be 12 */
  g_assert_cmpint (n, <, config.coarse_steps + 2 * config.refine_levels);
  run (peak_near, NULL, &n);
  g_assert_cmpint (n, <=, 3 + 2 * config.refine_levels);
}

static void
test_nothing_to_focus_on (void)
{
  CamAfResult r;

  noise_pct = 0.01;
  r = run (blank, NULL, NULL);
  g_assert_false (r.ok);
  g_assert_cmpint (r.reason, ==, CAM_AF_REASON_LOW_CONTRAST);
  noise_pct = 0;
  r = run (dark, NULL, NULL);
  g_assert_false (r.ok);
  g_assert_cmpint (r.reason, ==, CAM_AF_REASON_FLAT);
}

static void
test_positions_stay_in_range_and_are_new (void)
{
  int log[CAM_AF_MAX_POINTS], n;

  noise_pct = 0;
  run (peak_far, log, &n);
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      g_assert_cmpint (log[i], !=, log[j]);
}

static void
test_sharpness_measure (void)
{
  guint8 sharp[32 * 32], soft[32 * 32], bright[32 * 32];

  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      {
        int stripe = (x / 2) % 2;

        sharp[y * 32 + x] = stripe ? 200 : 40;
        soft[y * 32 + x] = 120 + (stripe ? 5 : -5);
        bright[y * 32 + x] = sharp[y * 32 + x] / 2;
      }
  g_assert_cmpfloat (cam_af_tile_sharpness (sharp, 32, 1, 32, 0), >, 20 * cam_af_tile_sharpness (soft, 32, 1, 32, 0));
  /* half the light, the same picture: the same sharpness (within the +1 that keeps a black frame finite) */
  g_assert_cmpfloat (fabs (cam_af_tile_sharpness (bright, 32, 1, 32, 0) / cam_af_tile_sharpness (sharp, 32, 1, 32, 0) - 1),
                     <, 0.05);
  /* a dark tile (noise over a tiny mean) is not sharp: it scores 0 whatever its gradient */
  {
    guint8 dark_noise[32 * 32];

    for (int i = 0; i < 32 * 32; i++)
      dark_noise[i] = (i * 7) % 5;
    g_assert_cmpfloat (cam_af_tile_sharpness (dark_noise, 32, 1, 32, 12), ==, 0);
    g_assert_cmpfloat (cam_af_tile_sharpness (dark_noise, 32, 1, 32, 0), >, 0);
  }
  /* a pixel stride: every 4th byte of an RGBx-like row */
  {
    guint8 packed[32 * 32 * 4] = { 0 };

    for (int i = 0; i < 32 * 32; i++)
      packed[i * 4 + 1] = sharp[i];
    g_assert_cmpfloat (cam_af_tile_sharpness (packed + 1, 32 * 4, 4, 32, 0), ==, cam_af_tile_sharpness (sharp, 32, 1, 32, 0));
  }
}

static void
test_roi_rect (void)
{
  int x, y, side;

  cam_af_roi_rect (1280, 720, 0.3, 0.5, 0.5, &x, &y, &side);
  g_assert_cmpint (side, ==, 216);
  g_assert_cmpint (x, ==, 640 - 108);
  g_assert_cmpint (y, ==, 360 - 108);
  /* a tap at the corner keeps the region inside the frame */
  cam_af_roi_rect (1280, 720, 0.3, 0.0, 1.0, &x, &y, &side);
  g_assert_cmpint (x, ==, 0);
  g_assert_cmpint (y, ==, 720 - 216);
  cam_af_roi_rect (1280, 720, 0.3, 1.5, -0.5, &x, &y, &side);
  g_assert_cmpint (x, ==, 1280 - 216);
  g_assert_cmpint (y, ==, 0);
}

static void
test_config (void)
{
  CamAfConfig c;
  g_autoptr(GError) error = NULL;

  g_assert_true (cam_af_config_load (&c, settings, &error));
  g_assert_no_error (error);
  g_assert_cmpint (c.pos_min, ==, 384);
  g_assert_cmpint (c.pos_max, ==, 1023);
  g_assert_cmpint (c.coarse_steps, ==, 8);
  g_assert_cmpfloat (c.roi, ==, 0.3);
  g_assert_cmpstr (c.dt_root, ==, "/sys/firmware/devicetree/base");
  cam_af_config_clear (&c);
  /* a bad combination is refused, whatever the schema's ranges allow key by key */
  g_settings_set_int (settings, "af-pos-min", 900);
  g_settings_set_int (settings, "af-pos-max", 800);
  g_assert_false (cam_af_config_load (&c, settings, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_settings_reset (settings, "af-pos-min");
  g_settings_reset (settings, "af-pos-max");
  /* the schema keeps each number in its range */
  {
    g_autoptr(GSettingsSchema) schema = NULL;
    g_autoptr(GSettingsSchemaKey) key = NULL;

    g_object_get (settings, "settings-schema", &schema, NULL);
    key = g_settings_schema_get_key (schema, "af-coarse-steps");
    g_assert_false (g_settings_schema_key_range_check (key, g_variant_new_int32 (2)));
    g_assert_true (g_settings_schema_key_range_check (key, g_variant_new_int32 (8)));
  }
}

static void
touch (const char *dir, const char *name, const char *bytes, gsize n)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *parent = g_path_get_dirname (path);

  g_mkdir_with_parents (parent, 0755);
  g_assert_true (g_file_set_contents (path, bytes, n, NULL));
}

static void
test_lens_lookup (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("cam-af-XXXXXX", &error);
  g_autofree char *dt = g_build_filename (root, "dt", NULL);
  g_autofree char *cls = g_build_filename (root, "class", NULL);
  g_autofree char *found = NULL;
  CamAfConfig c = config;
  const char handle[] = { 0, 0, 0, 0xa5 }, other[] = { 0, 0, 0, 0x11 };

  c.dt_root = dt;
  c.subdev_class = cls;
  c.dev_dir = (char *) "/dev";
  /* camera@1a names its lens; camera@10 does not. Two subdevs: the lens is the one whose phandle matches */
  touch (dt, "soc/i2c/camera@1a/lens-focus", handle, 4);
  touch (dt, "soc/i2c/camera@10/name", "x", 1);
  touch (cls, "v4l-subdev17/device/of_node/phandle", other, 4);
  touch (cls, "v4l-subdev18/device/of_node/phandle", handle, 4);
  touch (cls, "video0/name", "x", 1);
  found = cam_lens_find (&c, "/base/soc/i2c/camera@1a");
  g_assert_cmpstr (found, ==, "/dev/v4l-subdev18");
  g_assert_null (cam_lens_find (&c, "/base/soc/i2c/camera@10"));
  g_assert_null (cam_lens_find (&c, "/base/soc/i2c/camera@99"));
  /* a lens found but not openable says why */
  g_assert_null (cam_lens_open (&c, "/base/soc/i2c/camera@10", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);
  g_assert_null (cam_lens_open (&c, "/base/soc/i2c/camera@1a", &error));
  g_assert_nonnull (error);
  {
    g_autofree char *cmd = g_strdup_printf ("rm -rf %s", root);

    g_assert_cmpint (system (cmd), ==, 0);
  }
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp ("cam-af-schema-XXXXXX", &error);
  g_autofree char *src = g_build_filename (g_getenv ("CAM_SCHEMA_DIR"), "de.yesman.Camera.gschema.xml", NULL);
  g_autofree char *dst = g_build_filename (dir, "de.yesman.Camera.gschema.xml", NULL);
  const char *compile[] = { "glib-compile-schemas", dir, NULL };
  g_autoptr(GFile) from = g_file_new_for_path (src);
  g_autoptr(GFile) to = g_file_new_for_path (dst);
  int status;

  g_test_init (&argc, &argv, NULL);
  g_assert_no_error (error);
  g_assert_true (g_file_copy (from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error));
  g_assert_true (g_spawn_sync (NULL, (char **) compile, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_cmpint (status, ==, 0);
  g_setenv ("GSETTINGS_SCHEMA_DIR", dir, TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  settings = g_settings_new ("de.yesman.Camera");
  g_assert_true (cam_af_config_load (&config, settings, &error));
  g_test_add_func ("/af/single-peak", test_single_peak);
  g_test_add_func ("/af/peaks-at-the-ends", test_peaks_at_the_ends);
  g_test_add_func ("/af/noise", test_noise);
  g_test_add_func ("/af/plateau", test_plateau_takes_the_middle);
  g_test_add_func ("/af/two-peaks", test_two_peaks);
  g_test_add_func ("/af/early-stop", test_stops_early);
  g_test_add_func ("/af/nothing-to-focus-on", test_nothing_to_focus_on);
  g_test_add_func ("/af/positions", test_positions_stay_in_range_and_are_new);
  g_test_add_func ("/af/sharpness", test_sharpness_measure);
  g_test_add_func ("/af/roi", test_roi_rect);
  g_test_add_func ("/af/config", test_config);
  g_test_add_func ("/af/lens-lookup", test_lens_lookup);
  status = g_test_run ();
  g_object_unref (settings);
  cam_af_config_clear (&config);
  g_remove (dst);
  g_autofree char *compiled = g_build_filename (dir, "gschemas.compiled", NULL);
  g_remove (compiled);
  g_rmdir (dir);
  return status;
}
