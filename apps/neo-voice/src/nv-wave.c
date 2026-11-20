/* The clip's waveform: see nv-wave.h. */
#include "nv-wave.h"

#include <neo.h>
#include <math.h>
#include <string.h>

typedef struct {
  gint16        *pcm;
  gsize          samples;
  guint          rate;
  int            trim_a, trim_b;   /* ms */
  int            word_a, word_b;   /* ms, b <= 0: none */
  int            playhead;         /* ms, < 0: none */
  int            grab;             /* px around a handle that grabs it */
  int            dragging;         /* 0 none, 1 start, 2 end */
  NvWaveChanged  changed;
  gpointer       changed_data;
} Wave;

static Wave *
wave_of (GtkWidget *widget)
{
  return g_object_get_data (G_OBJECT (widget), "nv-wave");
}

static void
wave_free (Wave *w)
{
  g_free (w->pcm);
  g_free (w);
}

static guint32
le32 (const guint8 *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (guint32) p[3] << 24;
}

/* the PCM of a RIFF/WAVE: walks the chunks for fmt and data (16-bit mono is what the voice service writes) */
static gboolean
parse_wav (GBytes *wav, Wave *w)
{
  gsize size = 0;
  const guint8 *d = wav ? g_bytes_get_data (wav, &size) : NULL;
  gsize at = 12;
  guint bits = 0, channels = 0;

  if (!d || size < 12 || memcmp (d, "RIFF", 4) || memcmp (d + 8, "WAVE", 4))
    return FALSE;
  while (at + 8 <= size)
    {
      guint32 len = le32 (d + at + 4);

      if (!memcmp (d + at, "fmt ", 4) && at + 8 + 16 <= size)
        {
          channels = d[at + 10] | d[at + 11] << 8;
          w->rate = le32 (d + at + 12);
          bits = d[at + 22] | d[at + 23] << 8;
        }
      else if (!memcmp (d + at, "data", 4))
        {
          gsize n = MIN ((gsize) len, size - at - 8) / 2;

          if (bits != 16 || channels != 1 || !w->rate)
            return FALSE;
          w->pcm = g_memdup2 (d + at + 8, n * 2);
          w->samples = n;
          return TRUE;
        }
      at += 8 + len + (len & 1);
    }
  return FALSE;
}

int
nv_wave_length_ms (GtkWidget *widget)
{
  Wave *w = wave_of (widget);

  return w->rate ? (int) (w->samples * 1000 / w->rate) : 0;
}

static double
x_of (GtkWidget *widget, int ms)
{
  int len = nv_wave_length_ms (widget);

  return len ? (double) ms * gtk_widget_get_width (widget) / len : 0;
}

static int
ms_of (GtkWidget *widget, double x)
{
  int width = gtk_widget_get_width (widget);

  return width ? (int) CLAMP (x * nv_wave_length_ms (widget) / width, 0, nv_wave_length_ms (widget)) : 0;
}

static void
draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  GtkWidget *widget = GTK_WIDGET (area);
  Wave *w = wave_of (widget);
  GdkRGBA bg = neo_color (NEO_SURFACE_CONTAINER_HIGHEST), line = neo_color (NEO_PRIMARY);
  GdkRGBA dim = neo_color_alpha (NEO_ON_SURFACE_VARIANT, 0.45), word = neo_color_alpha (NEO_TERTIARY, 0.18);
  GdkRGBA handle = neo_color (NEO_ON_SURFACE), head = neo_color (NEO_ERROR);
  double mid = height / 2.0, a = 0, b = 0;

  gdk_cairo_set_source_rgba (cr, &bg);
  cairo_paint (cr);
  if (!w->samples)
    return;
  if (w->word_b > 0)
    {
      gdk_cairo_set_source_rgba (cr, &word);
      cairo_rectangle (cr, x_of (widget, w->word_a), 0, x_of (widget, w->word_b) - x_of (widget, w->word_a), height);
      cairo_fill (cr);
    }
  a = x_of (widget, w->trim_a);
  b = x_of (widget, w->trim_b);
  for (int x = 0; x < width; x++)   /* one peak per pixel column */
    {
      gsize from = (gsize) x * w->samples / width, to = (gsize) (x + 1) * w->samples / width;
      int peak = 0;

      for (gsize i = from; i < to && i < w->samples; i++)
        peak = MAX (peak, ABS ((int) w->pcm[i]));
      gdk_cairo_set_source_rgba (cr, x >= a && x <= b ? &line : &dim);
      cairo_rectangle (cr, x, mid - mid * peak / 32768.0, 1, MAX (1.0, 2 * mid * peak / 32768.0));
      cairo_fill (cr);
    }
  gdk_cairo_set_source_rgba (cr, &handle);
  for (int i = 0; i < 2; i++)
    {
      double x = i ? b : a;

      cairo_rectangle (cr, x - 1.5, 0, 3, height);
      cairo_fill (cr);
      cairo_arc (cr, x, i ? height - 8 : 8, 7, 0, 2 * G_PI);
      cairo_fill (cr);
    }
  if (w->playhead >= 0)
    {
      gdk_cairo_set_source_rgba (cr, &head);
      cairo_rectangle (cr, x_of (widget, w->playhead) - 1, 0, 2, height);
      cairo_fill (cr);
    }
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, gpointer data)
{
  GtkWidget *widget = data;
  Wave *w = wave_of (widget);
  double da = fabs (x - x_of (widget, w->trim_a)), db = fabs (x - x_of (widget, w->trim_b));

  w->dragging = MIN (da, db) > w->grab ? 0 : (da <= db ? 1 : 2);
  if (!w->dragging)
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
  else
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
  GtkWidget *widget = data;
  Wave *w = wave_of (widget);
  double sx = 0, sy = 0;
  int ms = 0;

  if (!w->dragging || !gtk_gesture_drag_get_start_point (drag, &sx, &sy))
    return;
  ms = ms_of (widget, sx + dx);
  if (w->dragging == 1)
    w->trim_a = MIN (ms, w->trim_b - 1);
  else
    w->trim_b = MAX (ms, w->trim_a + 1);
  gtk_widget_queue_draw (widget);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
  Wave *w = wave_of (data);

  if (w->dragging && w->changed)
    w->changed (w->changed_data);
  w->dragging = 0;
}

GtkWidget *
nv_wave_new (int height, int grab_px)
{
  GtkWidget *area = gtk_drawing_area_new ();
  Wave *w = g_new0 (Wave, 1);
  GtkGesture *drag = gtk_gesture_drag_new ();

  w->grab = grab_px;
  w->playhead = -1;
  g_object_set_data_full (G_OBJECT (area), "nv-wave", w, (GDestroyNotify) wave_free);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), height);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw, NULL, NULL);
  gtk_widget_add_css_class (area, "nv-wave");
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), area);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), area);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), area);
  gtk_widget_add_controller (area, GTK_EVENT_CONTROLLER (drag));
  return area;
}

void
nv_wave_set (GtkWidget *widget, GBytes *wav, int trim_start, int trim_end, int word_start, int word_end)
{
  Wave *w = wave_of (widget);

  g_clear_pointer (&w->pcm, g_free);
  w->samples = 0;
  w->rate = 0;
  w->playhead = -1;
  if (wav)
    parse_wav (wav, w);
  w->word_a = word_start;
  w->word_b = word_end;
  if (trim_end > 0 && trim_end <= nv_wave_length_ms (widget) && trim_start < trim_end)
    {
      w->trim_a = MAX (0, trim_start);
      w->trim_b = trim_end;
    }
  else
    nv_wave_reset_trim (widget);
  gtk_widget_queue_draw (widget);
}

void
nv_wave_get_trim (GtkWidget *widget, int *start_ms, int *end_ms)
{
  Wave *w = wave_of (widget);

  *start_ms = w->trim_a;
  *end_ms = w->trim_b;
}

void
nv_wave_reset_trim (GtkWidget *widget)
{
  Wave *w = wave_of (widget);

  w->trim_a = 0;
  w->trim_b = nv_wave_length_ms (widget);
  gtk_widget_queue_draw (widget);
}

void
nv_wave_set_playhead (GtkWidget *widget, int ms)
{
  wave_of (widget)->playhead = ms;
  gtk_widget_queue_draw (widget);
}

void
nv_wave_set_changed (GtkWidget *widget, NvWaveChanged cb, gpointer data)
{
  Wave *w = wave_of (widget);

  w->changed = cb;
  w->changed_data = data;
}
