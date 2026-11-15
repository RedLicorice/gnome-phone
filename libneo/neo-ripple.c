/* neo-ripple: see neo-ripple.h. */
#include "neo-ripple.h"
#include "neo-style.h"

#include <math.h>
#include <string.h>
#include <spongy.h>

#define ENTER_MS 450.0          /* RippleAnimationSession.ENTER_ANIM_DURATION */
#define EXIT_MS  375.0          /* EXIT_ANIM_DURATION */

static double
clamp01 (double v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* where a wave is at `now`: its progress 0..1, or -1 once it has finished */
static double
wave_progress (const NeoRippleWave *wave, gint64 now)
{
  double since_press = (now - wave->pressed_us) / 1000.0;
  double enter = 0.5 * spongy_curve (SPONGY_FAST_OUT_SLOW_IN, clamp01 (since_press / ENTER_MS));
  if (!wave->released_us)
    return enter;
  /* the exit waits for the enter to finish (RippleAnimationSession.computeDelay) */
  gint64 exit_start = MAX (wave->released_us, wave->pressed_us + (gint64) (ENTER_MS * 1000));
  if (now < exit_start)
    return enter;
  double t = (now - exit_start) / (EXIT_MS * 1000.0);
  return t >= 1 ? -1 : 0.5 + 0.5 * t;
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  NeoRipple *ripple = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  gboolean alive = FALSE;
  for (int i = 0; i < NEO_RIPPLE_WAVES; i++) {
    NeoRippleWave *wave = &ripple->waves[i];
    if (!wave->pressed_us)
      continue;
    if (wave_progress (wave, now) < 0)
      wave->pressed_us = 0;
    else
      alive = TRUE;
  }
  gtk_widget_queue_draw (widget);
  if (alive)
    return G_SOURCE_CONTINUE;
  ripple->tick = 0;
  return G_SOURCE_REMOVE;
}

void
neo_ripple_init (NeoRipple *ripple, GtkWidget *widget)
{
  *ripple = (NeoRipple) { .widget = widget };
}

void
neo_ripple_clear (NeoRipple *ripple)
{
  if (ripple->tick)
    gtk_widget_remove_tick_callback (ripple->widget, ripple->tick);
  ripple->tick = 0;
  memset (ripple->waves, 0, sizeof ripple->waves);
}

static gint64
frame_time (NeoRipple *ripple)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (ripple->widget);
  return clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
}

void
neo_ripple_press (NeoRipple *ripple, double x, double y)
{
  /* a new finger lets go of the old wave and starts its own (RippleDrawable.tryRippleEnter) */
  neo_ripple_release (ripple);
  NeoRippleWave *slot = &ripple->waves[0];
  for (int i = 0; i < NEO_RIPPLE_WAVES; i++) {
    if (!ripple->waves[i].pressed_us) {
      slot = &ripple->waves[i];
      break;
    }
    if (ripple->waves[i].pressed_us < slot->pressed_us)
      slot = &ripple->waves[i];       /* all busy: the oldest makes way */
  }
  *slot = (NeoRippleWave) { .x = x, .y = y, .pressed_us = frame_time (ripple) };
  if (!ripple->tick && gtk_widget_get_mapped (ripple->widget))
    ripple->tick = gtk_widget_add_tick_callback (ripple->widget, on_tick, ripple, NULL);
  gtk_widget_queue_draw (ripple->widget);
}

void
neo_ripple_release (NeoRipple *ripple)
{
  gint64 now = frame_time (ripple);
  for (int i = 0; i < NEO_RIPPLE_WAVES; i++)
    if (ripple->waves[i].pressed_us && !ripple->waves[i].released_us)
      ripple->waves[i].released_us = now;
}

gboolean
neo_ripple_is_active (NeoRipple *ripple)
{
  for (int i = 0; i < NEO_RIPPLE_WAVES; i++)
    if (ripple->waves[i].pressed_us)
      return TRUE;
  return FALSE;
}

/* RippleShader's softCircle (blur 1): 1 - smoothstep (0.5, 1.5, d / r), as gradient stops from the centre to 1.5 r */
#define EDGE_STOPS 9

void
neo_ripple_snapshot (NeoRipple *ripple, GtkSnapshot *snapshot, const graphene_rect_t *bounds, const GdkRGBA *color)
{
  if (!ripple->tick)
    return;
  gint64 now = frame_time (ripple);
  double w = bounds->size.width, h = bounds->size.height;
  double max_radius = sqrt (w * w / 4 + h * h / 4) * 2.3;   /* RippleShader.setRadius: radius * 2.3f */
  double cx = bounds->origin.x + w / 2, cy = bounds->origin.y + h / 2;

  for (int i = 0; i < NEO_RIPPLE_WAVES; i++) {
    const NeoRippleWave *wave = &ripple->waves[i];
    if (!wave->pressed_us)
      continue;
    double p = wave_progress (wave, now);
    if (p <= 0)
      continue;
    double alpha = MIN (clamp01 (p / 0.13), 1 - clamp01 ((p - 0.4) / 0.6));
    double radius = max_radius * p;
    if (alpha <= 0 || radius < 0.5)
      continue;
    double centre_t = clamp01 (p * 2);
    double x = bounds->origin.x + wave->x + (cx - bounds->origin.x - wave->x) * centre_t;
    double y = bounds->origin.y + wave->y + (cy - bounds->origin.y - wave->y) * centre_t;

    GskColorStop stops[EDGE_STOPS + 1];
    stops[0] = (GskColorStop) { 0, *color };
    stops[0].color.alpha *= alpha;
    for (int s = 0; s < EDGE_STOPS; s++) {
      double d = 0.5 + s / (double) (EDGE_STOPS - 1);       /* 0.5 .. 1.5, in radii */
      double e = (d - 0.5);                                  /* smoothstep (0.5, 1.5, d) */
      double fade = 1 - e * e * (3 - 2 * e);
      stops[s + 1] = (GskColorStop) { d / 1.5, *color };
      stops[s + 1].color.alpha *= alpha * fade;
    }
    gtk_snapshot_append_radial_gradient (snapshot, bounds, &GRAPHENE_POINT_INIT (x, y),
                                         radius * 1.5, radius * 1.5, 0, 1, stops, EDGE_STOPS + 1);
  }
}

GdkRGBA
neo_ripple_default_color (void)
{
  /* white on the dark surfaces; a light look's ripple is ink, or it would not show on its pale cards */
  if (neo_theme_is_day ())
    return (GdkRGBA) { 0x2b / 255.f, 0x2c / 255.f, 0x31 / 255.f, 0.12f };
  return (GdkRGBA) { 1, 1, 1, 0.2f / 2 };
}
