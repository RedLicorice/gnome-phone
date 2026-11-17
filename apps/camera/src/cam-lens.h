/* cam-lens: the focus actuator of a camera, through its V4L2 subdev (V4L2_CID_FOCUS_ABSOLUTE).
 *
 * The sensor's devicetree node names its lens in "lens-focus" (a phandle); the lens' I2C device is the subdev's. So
 * a camera is matched to its lens by libcamera's camera id (a devicetree path) and the kernel's own links, with no
 * list of phones here. A camera without "lens-focus" has none: fixed focus.
 *
 * The coil's I2C side is powered by the sensor: the lens answers only while the camera streams. And the driver lets
 * the coil rest (back to its zero) when the last holder closes the subdev, so a CamLens stays open as long as the
 * camera runs. Used from one thread only. */
#pragma once

#include "cam-af.h"

G_BEGIN_DECLS

typedef struct _CamLens CamLens;

/* the subdev node of the camera's lens (g_free), NULL for a camera without one. Nothing is opened. */
char    *cam_lens_find   (const CamAfConfig *config, const char *camera_id);
/* opens it and reads its range; NULL with @error if it cannot be opened (permissions) or has no focus control */
CamLens *cam_lens_open   (const CamAfConfig *config, const char *camera_id, GError **error);
void     cam_lens_range  (CamLens *lens, int *min, int *max);
gboolean cam_lens_set    (CamLens *lens, int position, GError **error);
void     cam_lens_free   (CamLens *lens);

G_END_DECLS
