/* Who may call a method: the one rule of the phone (Minerva's docs/who-may-call.md), as busowner.ForCallers does it
 * in Go and minerva_busowner.callers in Python. The programs of the method's group in the root-owned callers file; the
 * caller is the same user and runs one of them, told by the program file's device and inode (never its path or a bus
 * name), with the process pinned by its pidfd so that a reused pid cannot pass. Anything missing fails closed. */

#include "msg-callers.h"

#include <gio/gunixfdlist.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BUS_DAEMON "org.freedesktop.DBus"
#define PROGRAMS_KEY "programs"

/* the file and its folder are root's and only root writes them */
static gboolean
root_only (const char *path, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (path);
  const char *paths[] = { dir, path };

  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      struct stat st;

      if (stat (paths[i], &st) != 0 || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s is not root's alone", paths[i]);
          return FALSE;
        }
    }
  return TRUE;
}

static GStrv
programs_of (const char *path, const char *group, GError **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_auto(GStrv) programs = NULL;

  if (!root_only (path, error) || !g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, error))
    return NULL;
  programs = g_key_file_get_string_list (file, group, PROGRAMS_KEY, NULL, NULL);
  if (!programs || !programs[0])
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s [%s] lists no program", path, group);
      return NULL;
    }
  return g_steal_pointer (&programs);
}

gboolean
msg_callers_check (GDBusConnection *bus, const char *sender, const char *path, const char *group, GError **error)
{
  g_auto(GStrv) programs = programs_of (path, group, error);
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GVariant) reply = NULL, creds = NULL;
  g_autofree char *exe = NULL;
  guint32 uid = G_MAXUINT32, pid = 0;
  gint32 handle = -1;
  int pidfd = -1;
  struct stat seen;
  gboolean trusted = FALSE;

  if (!programs)
    return FALSE;
  reply = g_dbus_connection_call_with_unix_fd_list_sync (bus, BUS_DAEMON, "/org/freedesktop/DBus", BUS_DAEMON,
                                                         "GetConnectionCredentials", g_variant_new ("(s)", sender),
                                                         G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
                                                         NULL, &fds, NULL, error);
  if (!reply)
    return FALSE;
  creds = g_variant_get_child_value (reply, 0);
  if (!g_variant_lookup (creds, "UnixUserID", "u", &uid) || !g_variant_lookup (creds, "ProcessID", "u", &pid) ||
      !g_variant_lookup (creds, "ProcessFD", "h", &handle) || !fds ||
      (pidfd = g_unix_fd_list_get (fds, handle, NULL)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s: the bus gave no user, process or pidfd",
                   sender);
      return FALSE;
    }
  exe = g_strdup_printf ("/proc/%u/exe", pid);
  if (uid == getuid () && stat (exe, &seen) == 0)
    for (guint i = 0; programs[i] && !trusted; i++)
      {
        struct stat program;

        trusted = stat (programs[i], &program) == 0 && program.st_dev == seen.st_dev && program.st_ino == seen.st_ino;
      }
  /* the process the pidfd names is still the one we looked at */
  if (trusted && syscall (SYS_pidfd_send_signal, pidfd, 0, NULL, 0) != 0)
    trusted = FALSE;
  close (pidfd);
  if (!trusted)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s (process %u) is not a caller of [%s]", sender,
                 pid, group);
  return trusted;
}
