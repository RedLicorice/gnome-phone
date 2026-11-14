/* The per-camera tuning in de.yesman.Camera "tuning" (cam_tone_*), on GSettings' memory backend. CAM_SCHEMA_DIR is the
 * directory with the schema; it is compiled into a temporary one. */
#include "cam-tone.h"

#include <glib/gstdio.h>
#include <stdlib.h>

#define FRONT "/base/soc@0/cci@ac4a000/i2c-bus@1/camera@10"
#define MAIN  "/base/soc@0/cci@ac4a000/i2c-bus@0/camera@1a"

static GSettings *settings;

static void
test_seed (void)
{
  CamTone tone = CAM_TONE_NEUTRAL;

  g_assert_true (cam_tone_load (settings, FRONT, &tone));
  g_assert_cmpfloat (tone.gamma, ==, 1.8);
  g_assert_cmpfloat (tone.contrast, ==, 1.3);
  g_assert_cmpfloat (tone.saturation, ==, 1.0);
  g_assert_false (cam_tone_load (settings, MAIN, &tone));
  g_assert_false (cam_tone_load (settings, NULL, &tone));
  g_assert_true (cam_tone_load_default (settings, FRONT, &tone));
  g_assert_false (cam_tone_load_default (settings, MAIN, &tone));
}

static void
test_store (void)
{
  CamTone main_tone = { 2.0, 0.8, 1.5 }, tone = CAM_TONE_NEUTRAL, front = { 1.0, 1.0, 0.5 };

  cam_tone_store (settings, MAIN, &main_tone);
  g_assert_true (cam_tone_load (settings, MAIN, &tone));
  g_assert_cmpfloat (tone.gamma, ==, 2.0);
  g_assert_cmpfloat (tone.contrast, ==, 0.8);
  g_assert_cmpfloat (tone.saturation, ==, 1.5);
  /* the seeded entry is kept by a store for another camera */
  g_assert_true (cam_tone_load (settings, FRONT, &tone));
  g_assert_cmpfloat (tone.gamma, ==, 1.8);
  /* a store replaces its own camera's entry, and the schema's seed does not move */
  cam_tone_store (settings, FRONT, &front);
  g_assert_true (cam_tone_load (settings, FRONT, &tone));
  g_assert_cmpfloat (tone.saturation, ==, 0.5);
  g_assert_true (cam_tone_load_default (settings, FRONT, &tone));
  g_assert_cmpfloat (tone.gamma, ==, 1.8);
  g_assert_cmpfloat (tone.saturation, ==, 1.0);
}

static void
test_clamp (void)
{
  CamTone wild = { 9, -3, 50 }, tone = CAM_TONE_NEUTRAL;

  cam_tone_clamp (&wild);
  g_assert_cmpfloat (wild.gamma, ==, CAM_GAMMA_MAX);
  g_assert_cmpfloat (wild.contrast, ==, CAM_CONTRAST_MIN);
  g_assert_cmpfloat (wild.saturation, ==, CAM_SATURATION_MAX);
  /* whatever was stored loads inside the ranges the sliders have */
  wild = (CamTone) { 0.1, 7, -1 };
  cam_tone_store (settings, "/other", &wild);
  g_assert_true (cam_tone_load (settings, "/other", &tone));
  g_assert_cmpfloat (tone.gamma, ==, CAM_GAMMA_MIN);
  g_assert_cmpfloat (tone.contrast, ==, CAM_CONTRAST_MAX);
  g_assert_cmpfloat (tone.saturation, ==, CAM_SATURATION_MIN);
}

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp ("cam-tone-XXXXXX", &error);
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
  g_test_add_func ("/tone/seed", test_seed);
  g_test_add_func ("/tone/store", test_store);
  g_test_add_func ("/tone/clamp", test_clamp);
  status = g_test_run ();
  g_object_unref (settings);
  g_remove (dst);
  g_autofree char *compiled = g_build_filename (dir, "gschemas.compiled", NULL);
  g_remove (compiled);
  g_rmdir (dir);
  return status;
}
