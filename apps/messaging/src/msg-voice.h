/* A voice note or an audio file in its bubble: play and pause, its waveform filled as it plays, and its length
 * (then the time left). One note plays at a time, on the phone's sound output. */
#pragma once

#include <gtk/gtk.h>

#include "sms-store.h"

G_BEGIN_DECLS

/* the player of @part; @fetch (void (*) (GtkWidget *player)) is called when it is played and its media is not on the
 * phone */
GtkWidget *msg_voice_new (const SmsPart *part, GCallback fetch);
/* plays the note of @widget, pauses it, or plays it on; asks for its media when it is not on the phone */
void       msg_voice_toggle (GtkWidget *widget);

G_END_DECLS
