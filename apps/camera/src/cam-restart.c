#include "cam-restart.h"

char **
cam_restart_argv (const char *exe)
{
  char **argv = g_new0 (char *, 2);

  argv[0] = g_strdup (exe);
  return argv;
}
