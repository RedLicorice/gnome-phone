/* Who a destination belongs to. A conversation is addressed by a destination string, and a destination that
 * starts with a provider's address (``whatsapp:+39333...``, ``telegram:8675309``, ``agent:Minerva``) is that
 * provider's, not the modem's: it never touches SMS and its messages arrive and leave through the provider's
 * process.
 *
 * The addresses are data: ``~/.config/minerva/messaging.conf`` (``[agent]``, ``[whatsapp]``, ... each with an
 * ``address``), written at install from the shipped file. Nothing here names a provider. When the file is
 * missing the registry is empty and only the agent's built-in prefix keeps working, which is what a phone
 * without the providers does today.
 *
 * The file is read once and read again when it changes, so adding a provider needs no restart. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the destination's provider: its id, or NULL when no registered address fits */
char *sms_provider_owner (const char *destination);

/* the part of the destination after the provider's address, or NULL */
char *sms_provider_chat (const char *destination);

/* TRUE when the destination belongs to a provider at all */
gboolean sms_provider_is (const char *destination);

/* a provider's keys, from its section: the label to show, and how to treat a name it sends */
char *sms_provider_label (const char *provider_id);
gboolean sms_provider_flag (const char *provider_id, const char *key);

/* every section of the file that declares an address, as provider ids (the caller frees) */
GStrv sms_provider_ids (void);

/* the file being read (``MINERVA_MESSAGING_CONF``, else the phone's config dir) */
const char *sms_provider_conf_path (void);

/* forget what was read: the next call reads the file again */
void sms_provider_reload (void);

G_END_DECLS
