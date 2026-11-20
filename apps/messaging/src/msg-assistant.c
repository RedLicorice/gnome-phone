/* msg-assistant.c: who this node's assistant is, read once from ASSISTANT_FILE (meson option assistant-file), or
 * from MINERVA_ASSISTANT_CONF when set (the same variable as Minerva's readers).
 */
#include "msg-assistant.h"
#include "sms-agent.h"
#include "sms-provider.h"

#define ASSISTANT_ENV "MINERVA_ASSISTANT_CONF"
#define ASSISTANT_GROUP "assistant"

static char *name = NULL;
static char *destination = NULL;

static void
load (void)
{
  static gsize done = 0;

  if (g_once_init_enter (&done))
    {
      const char *set = g_getenv (ASSISTANT_ENV);
      const char *path = set && *set ? set : ASSISTANT_FILE;
      g_autoptr(GKeyFile) file = sms_provider_load_ini (path);
      g_autofree char *value = file ? g_key_file_get_string (file, ASSISTANT_GROUP, "name", NULL) : NULL;

      if (value && *g_strstrip (value))
        {
          name = g_strdup (value);
          destination = g_strconcat (SMS_AGENT_PREFIX, name, NULL);
        }
      else
        g_warning ("no [%s] name in %s: the agents have no assistant of this node", ASSISTANT_GROUP, path);
      g_once_init_leave (&done, 1);
    }
}

const char *
msg_assistant_name (void)
{
  load ();
  return name;
}

const char *
msg_assistant_destination (void)
{
  load ();
  return destination;
}
