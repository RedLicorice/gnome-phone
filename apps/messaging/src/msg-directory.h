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

#define MSG_TYPE_DIRECTORY (msg_directory_get_type ())
G_DECLARE_FINAL_TYPE (MsgDirectory, msg_directory, MSG, DIRECTORY, GObject)

/* the process's one directory; the first call starts reading */
MsgDirectory *msg_directory_get_default (void);

/* the provider's name for @destination (``whatsapp:+39...``), or NULL; @push says the sender chose it */
const char *msg_directory_name (MsgDirectory *self, const char *destination, gboolean *push);

G_END_DECLS
