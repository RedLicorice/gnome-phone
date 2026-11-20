/* The clip's waveform with two trim handles (a drag near a handle moves it), the wake word's span shaded and the
 * playhead. Non destructive: the handles are only numbers until the owner saves the trim. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*NvWaveChanged) (gpointer data);

GtkWidget *nv_wave_new          (int height, int grab_px);
/* a 16-bit mono WAV (NULL: nothing to draw); trim and word in ms, end <= 0: none */
void       nv_wave_set          (GtkWidget *wave, GBytes *wav, int trim_start, int trim_end, int word_start,
                                 int word_end);
int        nv_wave_length_ms    (GtkWidget *wave);
void       nv_wave_get_trim     (GtkWidget *wave, int *start_ms, int *end_ms);
void       nv_wave_reset_trim   (GtkWidget *wave);
void       nv_wave_set_playhead (GtkWidget *wave, int ms);
void       nv_wave_set_changed  (GtkWidget *wave, NvWaveChanged cb, gpointer data);

G_END_DECLS
