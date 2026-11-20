/* msg-assistant.h: who this node's assistant is (Minerva's assistant/assistant.conf, installed as
 * /etc/minerva/assistant.conf), read once: a new name shows when the app starts again.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* her name ([assistant] name), or NULL when the file cannot be read */
const char *msg_assistant_name (void);
/* her conversation (agent:<name>), or NULL */
const char *msg_assistant_destination (void);

G_END_DECLS
