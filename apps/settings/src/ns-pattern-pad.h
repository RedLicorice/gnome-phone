/*
 * The unlock pattern's pad: libminerva-pattern (Minerva's minerva-pattern) in a GTK 4 drawing area.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NS_TYPE_PATTERN_PAD (ns_pattern_pad_get_type ())

G_DECLARE_FINAL_TYPE (NsPatternPad, ns_pattern_pad, NS, PATTERN_PAD, GtkDrawingArea)

gboolean    ns_pattern_pad_available  (void);
GtkWidget  *ns_pattern_pad_new        (void);
const char *ns_pattern_pad_get_digits (NsPatternPad *self);
gboolean    ns_pattern_pad_complete   (NsPatternPad *self);
void        ns_pattern_pad_set_error  (NsPatternPad *self, gboolean error);
void        ns_pattern_pad_clear      (NsPatternPad *self);

G_END_DECLS
