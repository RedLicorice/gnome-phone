/* cam-window: the camera screen (CameraActivity and CameraViewModel, spec/camera.md §1-§19) and its Settings page.
 *
 * The window is full screen and black; the screen is chained bottom-up from the window's bottom edge (§2): the mode
 * selector (80 dp), the primary bar (gallery, shutter, flip or pause; 91 dp), the secondary bar (pro button, lens
 * buttons, flash; 72 dp with its divider) and the viewfinder over the rest, with the pro row sliding up over the
 * viewfinder's bottom. One camera stream runs while the screen shows; it stops when the window is hidden or
 * Settings is open. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWindow *cam_window_new        (GtkApplication *app);
/* the shell's "volume-key" action: +1 up, -1 down */
void       cam_window_volume_key (GtkWindow *window, int direction);

/* `neo-camera --scan`: the next window starts in SCAN, and an open one switches to it */
void       cam_window_set_start_scan (gboolean scan);
void       cam_window_show_scan      (GtkWindow *window);

/* argv of the process, for starting it afresh (cam_pipeline_needs_fresh_process) */
void       cam_window_remember_argv (char **argv);

G_END_DECLS
