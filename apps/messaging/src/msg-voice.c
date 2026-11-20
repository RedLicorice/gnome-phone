/* A voice note's player (msg-voice.h). One playbin for the whole app: a note that starts stops the one playing. The
 * waveform and the time are redrawn on the frame clock only while a note plays: a still bubble costs nothing. */
#include "msg-voice.h"

#include <gst/gst.h>

/* a bar fills this share of its step of the waveform: the rest is the gap to the next */
#define BAR_SHARE 0.6

typedef struct
{
  GtkWidget *box, *button, *wave, *time;   /* not references: the data is the box's own */
  char *path;
  guint duration;               /* ms, as the message says */
  GBytes *waveform;
  GCallback fetch;
  guint tick;
  gboolean paused;
} Voice;

static GstElement *player;
static Voice *current;          /* the note in the player, playing or paused */

static char *
clock_text (gint64 ms)
{
  gint64 s = MAX (ms, 0) / 1000;

  return g_strdup_printf ("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, s / 60, s % 60);
}

/* how far the current note is: 0..1, and its time left (ms) */
static double
progress (Voice *voice, gint64 *left)
{
  gint64 position = 0, length = (gint64) voice->duration * GST_MSECOND;

  if (voice == current && player)
    {
      gst_element_query_position (player, GST_FORMAT_TIME, &position);
      if (length <= 0)
        gst_element_query_duration (player, GST_FORMAT_TIME, &length);
    }
  if (left)
    *left = (length - position) / GST_MSECOND;
  return length > 0 ? CLAMP ((double) position / length, 0, 1) : 0;
}

static void
show_time (Voice *voice)
{
  gint64 left = voice->duration;
  g_autofree char *text = NULL;

  if (voice == current)
    progress (voice, &left);
  text = clock_text (left);
  gtk_label_set_text (GTK_LABEL (voice->time), text);
}

static void
draw_wave (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Voice *voice = data;
  gsize n = 0;
  const guint8 *level = voice->waveform ? g_bytes_get_data (voice->waveform, &n) : NULL;
  double done = progress (voice, NULL), step;
  GdkRGBA color;

  gtk_widget_get_color (GTK_WIDGET (area), &color);
  if (!n)
    {
      /* no waveform: a line, filled as it plays */
      cairo_set_source_rgba (cr, color.red, color.green, color.blue, color.alpha / 3);
      cairo_rectangle (cr, 0, height / 2.0 - 1, width, 2);
      cairo_fill (cr);
      cairo_set_source_rgba (cr, color.red, color.green, color.blue, color.alpha);
      cairo_rectangle (cr, 0, height / 2.0 - 1, width * done, 2);
      cairo_fill (cr);
      return;
    }
  step = (double) width / n;
  for (gsize i = 0; i < n; i++)
    {
      /* WhatsApp's levels are 0..100 */
      double h = MAX (2, height * MIN (level[i], 100) / 100.0);
      double played = (i + 1) * step <= width * done;

      cairo_set_source_rgba (cr, color.red, color.green, color.blue, played ? color.alpha : color.alpha / 3);
      cairo_rectangle (cr, i * step, (height - h) / 2, step * BAR_SHARE, h);
      cairo_fill (cr);
    }
}

static void
set_icon (Voice *voice, gboolean playing)
{
  gtk_button_set_icon_name (GTK_BUTTON (voice->button),
                            playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}

static gboolean
on_tick (GtkWidget *wave, GdkFrameClock *clock, gpointer data)
{
  Voice *voice = data;

  gtk_widget_queue_draw (voice->wave);
  show_time (voice);
  return G_SOURCE_CONTINUE;
}

static void
stop_ticking (Voice *voice)
{
  if (voice->tick)
    gtk_widget_remove_tick_callback (voice->wave, voice->tick);
  voice->tick = 0;
}

/* the player let go of the current note: back to its start, its length shown */
static void
stop (void)
{
  Voice *voice = current;

  current = NULL;
  if (player)
    gst_element_set_state (player, GST_STATE_NULL);
  if (!voice)
    return;
  stop_ticking (voice);
  voice->paused = FALSE;
  set_icon (voice, FALSE);
  gtk_widget_queue_draw (voice->wave);
  show_time (voice);
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      g_autoptr(GError) error = NULL;

      gst_message_parse_error (message, &error, NULL);
      g_warning ("messaging: a voice note did not play: %s", error->message);
    }
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    stop ();
  return G_SOURCE_CONTINUE;
}

static gboolean
make_player (void)
{
  g_autoptr(GstBus) bus = NULL;

  if (player)
    return TRUE;
  if (!gst_is_initialized ())
    gst_init (NULL, NULL);
  if (!(player = gst_element_factory_make ("playbin", NULL)))
    return FALSE;
  gst_util_set_object_arg (G_OBJECT (player), "flags", "audio");
  bus = gst_element_get_bus (player);
  gst_bus_add_watch (bus, on_bus, NULL);
  return TRUE;
}

void
msg_voice_toggle (GtkWidget *widget)
{
  Voice *voice = g_object_get_data (G_OBJECT (widget), "msg-voice");
  g_autofree char *uri = NULL;

  if (!voice->path || !g_file_test (voice->path, G_FILE_TEST_IS_REGULAR))
    {
      if (voice->fetch)
        ((void (*) (GtkWidget *)) voice->fetch) (voice->box);
      return;
    }
  if (voice == current)
    {
      voice->paused = !voice->paused;
      gst_element_set_state (player, voice->paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
      set_icon (voice, !voice->paused);
      if (voice->paused)
        stop_ticking (voice);
      else
        voice->tick = gtk_widget_add_tick_callback (voice->wave, on_tick, voice, NULL);
      return;
    }
  stop ();
  if (!make_player () || !(uri = g_filename_to_uri (voice->path, NULL, NULL)))
    return;
  current = voice;
  g_object_set (player, "uri", uri, NULL);
  gst_element_set_state (player, GST_STATE_PLAYING);
  set_icon (voice, TRUE);
  voice->tick = gtk_widget_add_tick_callback (voice->wave, on_tick, voice, NULL);
}

static void
voice_free (Voice *voice)
{
  /* its widgets are going: the player is let go without them */
  if (voice == current)
    {
      current = NULL;
      gst_element_set_state (player, GST_STATE_NULL);
    }
  g_free (voice->path);
  g_clear_pointer (&voice->waveform, g_bytes_unref);
  g_free (voice);
}

GtkWidget *
msg_voice_new (const SmsPart *part, GCallback fetch)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  Voice *voice = g_new0 (Voice, 1);

  voice->path = g_strdup (part->uri);
  voice->duration = part->duration;
  voice->waveform = part->waveform ? g_bytes_ref (part->waveform) : NULL;
  voice->fetch = fetch;
  voice->box = box;
  voice->button = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_add_css_class (voice->button, "msg-voice-play");
  /* the bubble's tap plays and pauses (msg_voice_toggle): the button only shows which */
  gtk_widget_set_can_target (voice->button, FALSE);
  gtk_widget_set_valign (voice->button, GTK_ALIGN_CENTER);
  voice->wave = gtk_drawing_area_new ();
  gtk_widget_add_css_class (voice->wave, "msg-voice-wave");
  gtk_widget_set_hexpand (voice->wave, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (voice->wave), draw_wave, voice, NULL);
  voice->time = gtk_label_new (NULL);
  gtk_widget_add_css_class (voice->time, "msg-voice-time");
  show_time (voice);
  gtk_box_append (GTK_BOX (box), voice->button);
  gtk_box_append (GTK_BOX (box), voice->wave);
  gtk_box_append (GTK_BOX (box), voice->time);
  gtk_widget_add_css_class (box, "msg-voice");
  g_object_set_data_full (G_OBJECT (box), "msg-voice", voice, (GDestroyNotify) voice_free);
  return box;
}
