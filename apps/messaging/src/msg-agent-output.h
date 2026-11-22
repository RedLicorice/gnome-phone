/* Where the agents' audio plays (Minerva's docs/agent-output.md): agent-output.conf in its layers. */
#pragma once

#include <gst/gst.h>

/* The stream properties of one agent stream, read now: media.role always, target.object when the owner chose a sink
 * other than the loudspeaker. WirePlumber routes on them; NULL when the file names no role. */
GstStructure *msg_agent_output_props (void);
