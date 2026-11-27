/* The voice enrollment dialog of Minerva > Voce: her face, what she makes of every attempt, Annulla in every state. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* called once the dialog has closed (the page reads the voiceprint's state again) */
typedef void (*NsVoiceEnrollDone) (gpointer data);

/* conf: the voiceprint's "dialog" of minerva-config's status (texts, faces, start and cancel commands) */
void ns_voice_enroll_open (GtkWidget *parent, JsonObject *conf, NsVoiceEnrollDone done, gpointer data);
