/* The argv a restarted Camera gets: a plain launch, whatever the first process was started with. */
#include "cam-restart.h"

static void
test_plain_launch (void)
{
  g_auto(GStrv) argv = cam_restart_argv ("/usr/bin/neo-camera");

  g_assert_cmpuint (g_strv_length (argv), ==, 1);
  g_assert_cmpstr (argv[0], ==, "/usr/bin/neo-camera");
  /* a service-mode start (D-Bus activation) never shows a window */
  g_assert_false (g_strv_contains ((const char *const *) argv, "--gapplication-service"));
  g_assert_false (g_strv_contains ((const char *const *) argv, "--scan"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/restart/plain-launch", test_plain_launch);
  return g_test_run ();
}
