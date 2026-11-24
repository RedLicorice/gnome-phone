/* The names the providers give their chats: what each provider daemon's own directory says a chat is called (the
 * owner's address book on that network, a group's subject, or the name the sender chose for itself). Messages
 * titles a thread with it when no contact matches (msg-people.c), as the provider's keys in messaging.conf allow
 * (``directory-names``, ``push-name-format``).
 *
 * Read from each provider's ``Chats`` method over the session bus, without blocking: at start, when a provider
 * appears on the bus, and when it signals ``ChatsChanged`` (a chat got a name, a group was renamed). "changed" is
 * emitted after each read, so the screens read the names again. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the provider daemons' bus protocol (messaging/internal/dbus/names.go): bus name org.minerva.Provider.<id>, object
 * /org/minerva/Provider/<e> and interface org.minerva.Provider.<e>, <e> the id escaped as an object path element
 * (g_dbus_escape_object_path: a "-" is not allowed there); every method answers one JSON string */
#define MSG_PROVIDER_NAME_PREFIX "org.minerva.Provider."
#define MSG_PROVIDER_PATH_PREFIX "/org/minerva/Provider/"

/* the bus name, object path and interface of provider @id (each newly allocated) */
void msg_provider_bus (const char *id, char **name, char **path, char **iface);

#define MSG_TYPE_DIRECTORY (msg_directory_get_type ())
G_DECLARE_FINAL_TYPE (MsgDirectory, msg_directory, MSG, DIRECTORY, GObject)

/* the process's one directory; the first call starts reading */
MsgDirectory *msg_directory_get_default (void);

/* the provider's name for @destination (``<channel>:<id>``), or NULL; @push says the sender chose it */
const char *msg_directory_name (MsgDirectory *self, const char *destination, gboolean *push);

G_END_DECLS
