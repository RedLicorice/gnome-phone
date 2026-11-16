/* cam-restart: how the camera app starts itself again (cam-window.c restart_app) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The argv of the new process: @exe alone. The first process may have been started by D-Bus activation
 * ("--gapplication-service": no window until someone calls Activate, nobody does) or with "--scan" (which would
 * force SCAN over the mode the settings hold), and the new process must come up as a plain launch that shows its
 * window in the mode, the camera and the tuning the settings name. Free with g_strfreev. */
char **cam_restart_argv (const char *exe);

G_END_DECLS
