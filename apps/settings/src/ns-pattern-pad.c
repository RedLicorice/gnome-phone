/*
 * The unlock pattern's pad: the finger joins the dots, the pattern is the dots in order as digits (the account's
 * password, the same the lock screen checks). The dots, their hit test, the digits and the drawing are
 * libminerva-pattern's (Minerva's minerva-pattern, opened at run time: the one implementation the lock screen
 * and its PIN sheets share); this is only its GTK 4 widget. "started" when a finger goes down, "finished" when
 * it is lifted after joining at least one dot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ns-pattern-pad.h"

#include <gmodule.h>

#define PATTERN_LIBRARY "libminerva-pattern.so.0"

enum {
  STARTED,
  FINISHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

typedef struct _MinervaPattern MinervaPattern;

static struct {
  gboolean          tried;
  gboolean          ok;
  MinervaPattern *(*new) (void);
  void            (*free) (MinervaPattern *);
  void            (*press) (MinervaPattern *, double, double, double, double);
  void            (*drag) (MinervaPattern *, double, double, double, double);
  void            (*release) (MinervaPattern *);
  void            (*clear) (MinervaPattern *);
  void            (*set_error) (MinervaPattern *, int);
  const char     *(*get_digits) (MinervaPattern *);
  int             (*complete) (MinervaPattern *);
  void            (*paint) (MinervaPattern *, cairo_t *, double, double, const double[4]);
} lib;

struct _NsPatternPad {
  GtkDrawingArea  parent;

  MinervaPattern *pattern;
};

G_DEFINE_FINAL_TYPE (NsPatternPad, ns_pattern_pad, GTK_TYPE_DRAWING_AREA)


static gboolean
load_library (void)
{
  GModule *module;

  if (lib.tried)
    return lib.ok;
  lib.tried = TRUE;
  module = g_module_open (PATTERN_LIBRARY, G_MODULE_BIND_LOCAL);
  if (module == NULL) {
    g_message ("no unlock pattern: %s", g_module_error ());
    return FALSE;
  }
  lib.ok = g_module_symbol (module, "minerva_pattern_new", (gpointer *) &lib.new) &&
           g_module_symbol (module, "minerva_pattern_free", (gpointer *) &lib.free) &&
           g_module_symbol (module, "minerva_pattern_press", (gpointer *) &lib.press) &&
           g_module_symbol (module, "minerva_pattern_drag", (gpointer *) &lib.drag) &&
           g_module_symbol (module, "minerva_pattern_release", (gpointer *) &lib.release) &&
           g_module_symbol (module, "minerva_pattern_clear", (gpointer *) &lib.clear) &&
           g_module_symbol (module, "minerva_pattern_set_error", (gpointer *) &lib.set_error) &&
           g_module_symbol (module, "minerva_pattern_get_digits", (gpointer *) &lib.get_digits) &&
           g_module_symbol (module, "minerva_pattern_complete", (gpointer *) &lib.complete) &&
           g_module_symbol (module, "minerva_pattern_paint", (gpointer *) &lib.paint);
  if (!lib.ok)
    g_message ("no unlock pattern: %s lacks a symbol", PATTERN_LIBRARY);
  g_module_make_resident (module);   /* never unloaded under a live pad */
  return lib.ok;
}


static void
draw_func (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  NsPatternPad *self = NS_PATTERN_PAD (area);
  GdkRGBA fg;
  double rgba[4];

  gtk_widget_get_color (GTK_WIDGET (area), &fg);
  rgba[0] = fg.red;
  rgba[1] = fg.green;
  rgba[2] = fg.blue;
  rgba[3] = fg.alpha;
  lib.paint (self->pattern, cr, width, height, rgba);
}


static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, NsPatternPad *self)
{
  /* the touch is the pattern's: a dialog's own drag (a bottom sheet swipes down to close) would end it */
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  lib.press (self->pattern, gtk_widget_get_width (GTK_WIDGET (self)), gtk_widget_get_height (GTK_WIDGET (self)), x, y);
  g_signal_emit (self, signals[STARTED], 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, NsPatternPad *self)
{
  double x, y;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  lib.drag (self->pattern, gtk_widget_get_width (GTK_WIDGET (self)), gtk_widget_get_height (GTK_WIDGET (self)),
            x + dx, y + dy);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, NsPatternPad *self)
{
  lib.release (self->pattern);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (*lib.get_digits (self->pattern) != '\0')
    g_signal_emit (self, signals[FINISHED], 0);
}


static void
ns_pattern_pad_finalize (GObject *object)
{
  NsPatternPad *self = NS_PATTERN_PAD (object);

  g_clear_pointer (&self->pattern, lib.free);
  G_OBJECT_CLASS (ns_pattern_pad_parent_class)->finalize (object);
}


static void
ns_pattern_pad_class_init (NsPatternPadClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = ns_pattern_pad_finalize;

  /* a finger went down: a new pattern starts */
  signals[STARTED] = g_signal_new ("started", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /* the finger was lifted after joining a dot: ns_pattern_pad_get_digits, ns_pattern_pad_complete */
  signals[FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void
ns_pattern_pad_init (NsPatternPad *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->pattern = lib.new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), draw_func, NULL, NULL);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}


/* whether a pattern can be drawn: the library opens and reads its look (pattern.conf) */
gboolean
ns_pattern_pad_available (void)
{
  static int available = -1;

  if (available < 0) {
    MinervaPattern *probe = load_library () ? lib.new () : NULL;

    available = probe != NULL;
    if (probe)
      lib.free (probe);
  }
  return available;
}


/* only when ns_pattern_pad_available () */
GtkWidget *
ns_pattern_pad_new (void)
{
  g_return_val_if_fail (ns_pattern_pad_available (), NULL);

  return g_object_new (NS_TYPE_PATTERN_PAD, NULL);
}


/* the joined dots in order, as digits; "" with none */
const char *
ns_pattern_pad_get_digits (NsPatternPad *self)
{
  g_return_val_if_fail (NS_IS_PATTERN_PAD (self), "");

  return lib.get_digits (self->pattern);
}


/* whether enough dots are joined to be a pattern (pattern.conf [rules] min_dots) */
gboolean
ns_pattern_pad_complete (NsPatternPad *self)
{
  g_return_val_if_fail (NS_IS_PATTERN_PAD (self), FALSE);

  return lib.complete (self->pattern);
}


/* a wrong or too short pattern: drawn in the error colour until the next stroke */
void
ns_pattern_pad_set_error (NsPatternPad *self, gboolean error)
{
  g_return_if_fail (NS_IS_PATTERN_PAD (self));

  lib.set_error (self->pattern, error);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
ns_pattern_pad_clear (NsPatternPad *self)
{
  g_return_if_fail (NS_IS_PATTERN_PAD (self));

  lib.clear (self->pattern);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
