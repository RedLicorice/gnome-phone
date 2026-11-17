/* cam-lens: see cam-lens.h. */
#include "cam-lens.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct _CamLens {
  int fd;
  int min, max;
};

static gboolean
same_bytes (const char *path_a, const char *path_b)
{
  g_autofree char *a = NULL, *b = NULL;
  gsize la, lb;

  return g_file_get_contents (path_a, &a, &la, NULL) && g_file_get_contents (path_b, &b, &lb, NULL) && la == lb
         && la > 0 && memcmp (a, b, la) == 0;
}

char *
cam_lens_find (const CamAfConfig *config, const char *camera_id)
{
  const char *relative = g_str_has_prefix (camera_id, "/base/") ? camera_id + strlen ("/base/") : camera_id;
  g_autofree char *phandle = NULL;
  g_autoptr(GDir) dir = NULL;
  const char *name;

  while (*relative == '/')
    relative++;
  phandle = g_build_filename (config->dt_root, relative, "lens-focus", NULL);
  if (!g_file_test (phandle, G_FILE_TEST_EXISTS) || !(dir = g_dir_open (config->subdev_class, 0, NULL)))
    return NULL;
  while ((name = g_dir_read_name (dir)))
    {
      g_autofree char *theirs = NULL;

      if (!g_str_has_prefix (name, "v4l-subdev"))
        continue;
      theirs = g_build_filename (config->subdev_class, name, "device", "of_node", "phandle", NULL);
      if (same_bytes (phandle, theirs))
        return g_build_filename (config->dev_dir, name, NULL);
    }
  return NULL;
}

CamLens *
cam_lens_open (const CamAfConfig *config, const char *camera_id, GError **error)
{
  g_autofree char *path = cam_lens_find (config, camera_id);
  struct v4l2_queryctrl query = { .id = V4L2_CID_FOCUS_ABSOLUTE };
  CamLens *lens;
  int fd;

  if (!path)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s has no focus lens", camera_id);
      return NULL;
    }
  fd = open (path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", path, g_strerror (errno));
      return NULL;
    }
  if (ioctl (fd, VIDIOC_QUERYCTRL, &query) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s has no focus control: %s", path,
                   g_strerror (errno));
      close (fd);
      return NULL;
    }
  lens = g_new0 (CamLens, 1);
  lens->fd = fd;
  lens->min = query.minimum;
  lens->max = query.maximum;
  return lens;
}

void
cam_lens_range (CamLens *lens, int *min, int *max)
{
  *min = lens->min;
  *max = lens->max;
}

gboolean
cam_lens_set (CamLens *lens, int position, GError **error)
{
  struct v4l2_control control = { .id = V4L2_CID_FOCUS_ABSOLUTE, .value = CLAMP (position, lens->min, lens->max) };

  if (ioctl (lens->fd, VIDIOC_S_CTRL, &control) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "lens: %s", g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

void
cam_lens_free (CamLens *lens)
{
  if (!lens)
    return;
  close (lens->fd);
  g_free (lens);
}
