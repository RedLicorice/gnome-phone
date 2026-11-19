#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* TRUE when @sender (a unique name on @bus) may call the method of @group in the root-owned callers file at @path
   (docs/who-may-call.md of Minerva): the same user, running one of the group's programs. FALSE with the reason
   otherwise, and for a file that root does not alone own */
gboolean msg_callers_check (GDBusConnection *bus, const char *sender, const char *path, const char *group,
                            GError **error);

G_END_DECLS
