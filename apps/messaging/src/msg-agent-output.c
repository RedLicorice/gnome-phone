/* Where the agents' audio plays (msg-agent-output.h). A voice note is agent audio: it plays where the owner's picker
 * "Uscita agente" says, as her voice does. agent-output.conf, read in its layers for every note (the choice changes
 * while the app runs): [stream] role, [output] target, and [targets] for what "speaker" and "earpiece" are as a
 * target (empty: none, the role's sink, the loudspeaker). Nothing is routed here: WirePlumber does it, falling back
 * to the loudspeaker while the chosen sink is away. */
#include "msg-agent-output.h"

#include <string.h>

#include "minerva-layers.h"

#define CONF_NAME "agent-output.conf"

static char *
value (GKeyFile *conf, const char *section, const char *key)
{
  char *text = g_key_file_get_value (conf, section, key, NULL);
  char *semi;

  if (!text)
    return NULL;
  semi = strchr (text, ';');
  if (semi)
    *semi = '\0';
  return g_strstrip (text);
}

GstStructure *
msg_agent_output_props (void)
{
  g_auto(GStrv) files = minerva_layers_files (CONF_NAME, TRUE);
  g_autoptr(GKeyFile) conf = files ? minerva_layers_load ((const char *const *) files) : NULL;
  g_autofree char *role = conf ? value (conf, "stream", "role") : NULL;
  g_autofree char *choice = NULL;
  g_autofree char *target = NULL;
  GstStructure *props;

  if (!role || !*role)
    return NULL;
  props = gst_structure_new ("props", "media.role", G_TYPE_STRING, role, NULL);
  choice = value (conf, "output", "target");
  target = choice && g_key_file_has_key (conf, "targets", choice, NULL) ? value (conf, "targets", choice)
                                                                      : g_strdup (choice);
  if (target && *target)
    gst_structure_set (props, "target.object", G_TYPE_STRING, target, NULL);
  return props;
}
