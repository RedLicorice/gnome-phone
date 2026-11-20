/* A voice note held to the ear plays from the earpiece (messaging.conf [media] earpiece_by_proximity): while a note
 * plays, the proximity sensor (iio-sensor-proxy) is claimed; near, and with no call, the sound goes to the earpiece
 * the way the dialer's calls do (callaudiod's call mode, speaker off); away, or the note over, back to the speaker.
 * A call that comes meanwhile takes the sound back at once and stops the note. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* a note started playing: @stop (void (*) (gpointer data)) stops it when a call comes */
void msg_earpiece_start (GCallback stop, gpointer data);
/* the note paused or ended: the speaker again, the sensor let go */
void msg_earpiece_stop (void);

G_END_DECLS
