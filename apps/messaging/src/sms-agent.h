/* Agents as destinations: a conversation whose only destination is "agent:<name>" is with an agent (an opencode
 * session behind minerva-chat-bridge), not with a phone. Its messages never go to the modem: the service hands them
 * to the bridge (Outgoing, Delivered) and takes the agent's replies from it (Inject). Everywhere it is named by what
 * follows "agent:". */
#pragma once

#include <glib.h>
#include <string.h>

#define SMS_AGENT_PREFIX "agent:"

/* the agent's name ("Minerva" for "agent:Minerva"), or NULL for a destination that is not an agent's */
static inline const char *
sms_agent_name (const char *destination)
{
  return destination && g_str_has_prefix (destination, SMS_AGENT_PREFIX) && destination[strlen (SMS_AGENT_PREFIX)]
         ? destination + strlen (SMS_AGENT_PREFIX) : NULL;
}

/* a conversation with an agent alone: an agent among others is a group, sent as SMS */
static inline gboolean
sms_agent_is_conversation (const char * const *destinations)
{
  return destinations && destinations[0] && !destinations[1] && sms_agent_name (destinations[0]);
}
