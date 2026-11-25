/* Neo Settings' building blocks (see ns-ui.h). */
#include "ns-ui.h"
#include <string.h>
#include "ns-look.h"

#include <math.h>

#ifdef HAVE_SPONGY
#include <spongy-gtk.h>
#endif

static AdwNavigationView *nav;
static gint64 tapped;          /* when a row was tapped: NS_TIMING logs how long its page took to build */
static gboolean root_next;     /* ns_open_root is running: the next page pushed replaces the stack */
static gboolean root_pushed;   /* and it did push one */

void
ns_ui_init (AdwNavigationView *n)
{
  nav = n;
}

void
ns_push (AdwNavigationPage *page)
{
  if (tapped && g_getenv ("NS_TIMING"))
    g_message ("%s: built in %.1f ms", adw_navigation_page_get_title (page),
               (g_get_monotonic_time () - tapped) / 1000.0);
  tapped = 0;
  if (root_next) {
    root_next = FALSE;
    root_pushed = TRUE;
    adw_navigation_view_replace (nav, &page, 1);
    return;
  }
  adw_navigation_view_push (nav, page);
}

gboolean
ns_open_root (NsActivate cb, GtkWidget *row, gpointer data)
{
  root_next = TRUE;
  root_pushed = FALSE;
  cb (row, data);
  /* a page that pushed nothing (it opened a dialog): the next push is an ordinary one */
  root_next = FALSE;
  return root_pushed;
}

GtkWidget *
ns_scroller_new (GtkWidget *child)
{
#ifdef HAVE_SPONGY
  GtkWidget *sc = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (sc), child);
#else
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), child);
#endif
  return sc;
}

/* ---- the page's title: big in the page, small in the bar ----
 *
 * Android's Settings pages (SettingsLib's CollapsingToolbarBaseActivity: Material's CollapsingToolbarLayout with
 * titleCollapseMode "fade", scroll|exitUntilCollapsed|snap) open with the title big, above the first card, and
 * only the back arrow in the bar. Scrolled, the big title goes up with the cards and fades out; once it is under
 * the bar, the bar's own small title fades in. Measured frame by frame on the Galaxy S10e, over the 168 dp it
 * takes the big title's block to go under the bar:
 *   the big title   fully there until a quarter of the way, gone at 62.5 %
 *   the small title  appears at 62.5 %, fully there at the end
 * (Material's fade mode: the big title fades from fadeModeStartFraction, here 0.25, to halfway between that and
 * the end; the small title over the rest.) The bar keeps its colour all along.
 *
 * Here the big title is the first thing in the scrolled column, so it moves with the content for nothing; only
 * the two opacities follow the scroll offset, which redraws but lays nothing out again. It comes back only with
 * the top of the page, as Android's does. A finger let go with the block part way under the bar finishes the
 * move (Android's "snap"): to the closer end, once the scrolling has stopped.
 *
 * Every page can scroll its title all the way under the bar, however short it is: the page below the title is
 * made at least as tall as the scroller's view, as Android's content view is as tall as the screen below the
 * collapsed bar (ScrollingViewBehavior). Without that, a page of a few rows (Software information) ran out of
 * scrolling with the block a quarter of the way still to go, and the bar's title stayed a third faded in. */

#define TITLE_FADE_START   0.25
#define TITLE_FADE_SWITCH  0.625   /* TITLE_FADE_START + (1 - TITLE_FADE_START) / 2 */

typedef struct {
  GtkWidget     *block, *small;   /* the big title's block (first in the column), the bar's title: held */
  GtkWidget     *body;            /* the page under it, at least as tall as the view: held */
  GtkWidget     *scroller;
  GtkAdjustment *adj;
  AdwAnimation  *snap;
  guint          settle;          /* waits for the scrolling to stop, then snaps */
  guint          fill;            /* sets the body's height after a layout */
  int            touches;         /* fingers (or buttons) down on the page */
} Title;

static void
title_free (Title *t)
{
  g_clear_handle_id (&t->settle, g_source_remove);
  g_clear_handle_id (&t->fill, g_source_remove);
  if (t->snap)
    adw_animation_pause (t->snap);
  g_clear_object (&t->snap);
  g_signal_handlers_disconnect_by_data (t->adj, t);
  g_object_unref (t->adj);
  g_object_unref (t->block);
  g_object_unref (t->small);
  g_object_unref (t->body);
  g_free (t);
}

/* how far the block can go: its own height, from the top of the column */
static double
title_range (Title *t)
{
  graphene_rect_t b;
  if (!gtk_widget_compute_bounds (t->block, gtk_widget_get_parent (t->block), &b))
    return 0;
  return b.origin.y + b.size.height;
}

/* 0 up to `from`, 1 from `to`, a straight line between */
static double
ramp (double from, double to, double x)
{
  return x <= from ? 0 : x >= to ? 1 : (x - from) / (to - from);
}

static void
title_fade (Title *t)
{
  /* the page is going: the scroller lets go of its column first, and moves its adjustment after */
  if (!gtk_widget_get_parent (t->block))
    return;
  double range = title_range (t);
  double f = range > 0 ? gtk_adjustment_get_value (t->adj) / range : 0;
  gtk_widget_set_opacity (gtk_widget_get_first_child (t->block), 1 - ramp (TITLE_FADE_START, TITLE_FADE_SWITCH, f));
  gtk_widget_set_opacity (t->small, ramp (TITLE_FADE_SWITCH, 1, f));
}

static gboolean
title_snap (gpointer data)
{
  Title *t = data;
  t->settle = 0;
  if (!gtk_widget_get_parent (t->block))
    return G_SOURCE_REMOVE;
  double range = title_range (t), at = gtk_adjustment_get_value (t->adj);
  if (t->touches || range <= 0 || at <= 0 || at >= range)
    return G_SOURCE_REMOVE;
  double to = at < range / 2 ? 0 : range;
  /* AppBarLayout's timing: 150 ms, and up to 150 more for a longer way; slowing down at the end */
  guint ms = (guint) (150 * (1 + fabs (to - at) / range));
  if (t->snap)
    adw_animation_pause (t->snap);
  g_clear_object (&t->snap);
  t->snap = adw_timed_animation_new (t->scroller, at, to, ms,
                                     adw_property_animation_target_new (G_OBJECT (t->adj), "value"));
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (t->snap), ADW_EASE_OUT_CUBIC);
  adw_animation_play (t->snap);
  return G_SOURCE_REMOVE;
}

/* scrolling stopped when the offset has not moved for a moment with no finger down */
static void
title_settle_later (Title *t)
{
  g_clear_handle_id (&t->settle, g_source_remove);
  if (!t->touches)
    t->settle = g_timeout_add (120, title_snap, t);
}

static void
title_scrolled (GtkAdjustment *adj, Title *t)
{
  title_fade (t);
  /* the snap's own moves are not a scroll to wait for */
  if (!t->snap || adw_animation_get_state (t->snap) != ADW_ANIMATION_PLAYING)
    title_settle_later (t);
}

/* The view's height is the adjustment's page size, which the scroller sets while it lays the page out; the body
 * takes it after that layout, not in the middle of it. It changes when the window does (the keyboard, a turn), and
 * the height set is the same again after the next layout, so this settles at once. */
static gboolean
title_fill (gpointer data)
{
  Title *t = data;
  t->fill = 0;
  if (!gtk_widget_get_parent (t->body))
    return G_SOURCE_REMOVE;
  int view = (int) ceil (gtk_adjustment_get_page_size (t->adj)), now;
  gtk_widget_get_size_request (t->body, NULL, &now);
  if (view > 0 && view != now)
    gtk_widget_set_size_request (t->body, -1, view);
  return G_SOURCE_REMOVE;
}

static void
title_resized (GtkAdjustment *adj, Title *t)
{
  if (!t->fill)
    t->fill = g_idle_add (title_fill, t);
}

/* a finger down stops a snap, and holds the next one off until it is lifted */
static gboolean
title_touch (GtkEventControllerLegacy *c, GdkEvent *e, Title *t)
{
  switch (gdk_event_get_event_type (e)) {
  case GDK_BUTTON_PRESS:
  case GDK_TOUCH_BEGIN:
    t->touches++;
    if (t->snap)
      adw_animation_pause (t->snap);
    g_clear_handle_id (&t->settle, g_source_remove);
    break;
  case GDK_BUTTON_RELEASE:
  case GDK_TOUCH_END:
  case GDK_TOUCH_CANCEL:
    t->touches = MAX (t->touches - 1, 0);
    title_settle_later (t);
    break;
  default:
    break;
  }
  return GDK_EVENT_PROPAGATE;
}

static GtkAdjustment *
scroller_vadjustment (GtkWidget *sc)
{
#ifdef HAVE_SPONGY
  return spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (sc));
#else
  return gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sc));
#endif
}

static AdwNavigationPage *
page_with (const char *title, GtkWidget *child)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *bar = adw_header_bar_new ();
  /* One UI: the title next to the back arrow, not centred */
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (bar), gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  Title *t = g_new0 (Title, 1);
  t->small = gtk_label_new (title);
  gtk_widget_add_css_class (t->small, "ns-head");
  gtk_label_set_ellipsize (GTK_LABEL (t->small), PANGO_ELLIPSIZE_END);
  gtk_widget_set_opacity (t->small, 0);
  adw_header_bar_pack_start (ADW_HEADER_BAR (bar), t->small);
  gtk_widget_add_css_class (bar, "flat");
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), bar);

  /* the column: the big title's block, then the page */
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  t->block = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (t->block, "ns-big-title-block");
  GtkWidget *big = gtk_label_new (title);
  gtk_label_set_wrap (GTK_LABEL (big), TRUE);
  gtk_label_set_lines (GTK_LABEL (big), 3);
  gtk_label_set_ellipsize (GTK_LABEL (big), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (big), 0);
  gtk_widget_add_css_class (big, "ns-big-title");
  gtk_box_append (GTK_BOX (t->block), big);
  gtk_box_append (GTK_BOX (col), t->block);
  /* the page at the top of a box of its own, which is made as tall as the view (title_fill) */
  t->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (t->body), child);
  gtk_box_append (GTK_BOX (col), t->body);

  t->scroller = ns_scroller_new (col);
  /* held: the snap's animation holds it too, and may let go of it after the scroller has gone */
  t->adj = g_object_ref (scroller_vadjustment (t->scroller));
  g_object_ref (t->block);
  g_object_ref (t->small);
  g_object_ref (t->body);
  g_object_set_data_full (G_OBJECT (t->scroller), "ns-title", t, (GDestroyNotify) title_free);
  g_signal_connect (t->adj, "value-changed", G_CALLBACK (title_scrolled), t);
  g_signal_connect (t->adj, "changed", G_CALLBACK (title_resized), t);
  GtkEventController *touch = gtk_event_controller_legacy_new ();
  gtk_event_controller_set_propagation_phase (touch, GTK_PHASE_CAPTURE);
  g_signal_connect (touch, "event", G_CALLBACK (title_touch), t);
  gtk_widget_add_controller (t->scroller, touch);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), t->scroller);

  AdwNavigationPage *page = adw_navigation_page_new (tv, title);
  gtk_widget_add_css_class (GTK_WIDGET (page), "ns-root");
  return page;
}

AdwNavigationPage *
ns_page_new (const char *title, GtkWidget **content)
{
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (col, "ns-column");
  *content = col;
  return page_with (title, col);
}

AdwNavigationPage *
ns_page_new_list (const char *title, GtkWidget *list)
{
  gtk_widget_add_css_class (list, "ns-list");
  return page_with (title, list);
}

/* a page's title after it is made, for the pages whose texts arrive with the data: the big one on the page and the
 * bar's small one follow the navigation page's */
void
ns_page_set_title (AdwNavigationPage *page, const char *title)
{
  adw_navigation_page_set_title (page, title);
  GtkWidget *tv = adw_navigation_page_get_child (page);
  GtkWidget *sc = tv && ADW_IS_TOOLBAR_VIEW (tv) ? adw_toolbar_view_get_content (ADW_TOOLBAR_VIEW (tv)) : NULL;
  Title *t = sc ? g_object_get_data (G_OBJECT (sc), "ns-title") : NULL;
  if (!t)
    return;
  gtk_label_set_text (GTK_LABEL (t->small), title);
  GtkWidget *big = t->block ? gtk_widget_get_first_child (t->block) : NULL;
  if (big)
    gtk_label_set_text (GTK_LABEL (big), title);
}

/* ---- groups and rows ---- */

static void
inset_lines (GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
  if (!before || !gtk_widget_get_visible (GTK_WIDGET (before))) {
    gtk_list_box_row_set_header (row, NULL);
    return;
  }
  GtkWidget *sep = gtk_list_box_row_get_header (row);
  if (sep)
    return;
  sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  /* from where the text starts: past the icon, if the row has one */
  gtk_widget_add_css_class (sep, g_object_get_data (G_OBJECT (row), "ns-icon") ? "ns-inset-icon" : "ns-inset");
  gtk_list_box_row_set_header (row, sep);
}

static void
activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  NsActivate cb = g_object_get_data (G_OBJECT (row), "ns-cb");
  tapped = g_get_monotonic_time ();
  if (cb)
    cb (GTK_WIDGET (row), g_object_get_data (G_OBJECT (row), "ns-data"));
}

GtkWidget *
ns_group (GtkWidget *content, const char *title)
{
  if (title) {
    GtkWidget *l = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_widget_add_css_class (l, "ns-group-title");
    gtk_box_append (GTK_BOX (content), l);
  }
  GtkWidget *box = gtk_list_box_new ();
  if (title)
    g_object_set_data (G_OBJECT (box), "ns-group-title", gtk_widget_get_last_child (content));
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box), GTK_SELECTION_NONE);
  gtk_list_box_set_header_func (GTK_LIST_BOX (box), inset_lines, NULL, NULL);
  gtk_widget_set_overflow (box, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (box, "ns-card");
  g_signal_connect (box, "row-activated", G_CALLBACK (activated), NULL);
  gtk_box_append (GTK_BOX (content), box);
  return box;
}

GtkWidget *
ns_row (GtkWidget *group, const char *icon, const char *color, const char *title, const char *subtitle)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_widget_add_css_class (row, "ns-row");
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
  if (icon) {
    GtkWidget *bubble = gtk_image_new ();
    ns_look_set_icon (GTK_IMAGE (bubble), icon);   /* the icon theme's, or the Renaissance look's bare glyph */
    gtk_widget_set_valign (bubble, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (bubble, "ns-icon");
    g_autofree char *cls = g_strconcat ("ns-", color ? color : "blue", NULL);
    gtk_widget_add_css_class (bubble, cls);
    gtk_box_append (GTK_BOX (h), bubble);
    g_object_set_data (G_OBJECT (row), "ns-icon", bubble);
  }
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (v, TRUE);
  gtk_widget_set_valign (v, GTK_ALIGN_CENTER);
  GtkWidget *t = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (t), 0);
  gtk_label_set_wrap (GTK_LABEL (t), TRUE);
  gtk_widget_add_css_class (t, "ns-title");
  gtk_box_append (GTK_BOX (v), t);
  g_object_set_data (G_OBJECT (row), "ns-title-label", t);
  GtkWidget *s = gtk_label_new (subtitle);
  gtk_label_set_xalign (GTK_LABEL (s), 0);
  gtk_label_set_wrap (GTK_LABEL (s), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (s), PANGO_WRAP_WORD_CHAR);   /* a value without spaces (a list) breaks too */
  gtk_widget_add_css_class (s, "ns-sub");
  gtk_widget_set_visible (s, subtitle && *subtitle);
  gtk_box_append (GTK_BOX (v), s);
  gtk_box_append (GTK_BOX (h), v);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), h);
  g_object_set_data (G_OBJECT (row), "ns-box", h);
  g_object_set_data (G_OBJECT (row), "ns-subtitle", s);
  g_object_set_data_full (G_OBJECT (row), "ns-text", g_strconcat (title, " ", subtitle ? subtitle : "", NULL), g_free);
  gtk_list_box_append (GTK_LIST_BOX (group), row);
  return row;
}

void
ns_row_on_activate (GtkWidget *row, NsActivate cb, gpointer data)
{
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data (G_OBJECT (row), "ns-cb", cb);
  g_object_set_data (G_OBJECT (row), "ns-data", data);
}

void
ns_row_suffix (GtkWidget *row, GtkWidget *suffix)
{
  gtk_widget_set_valign (suffix, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (g_object_get_data (G_OBJECT (row), "ns-box")), suffix);
}

void
ns_row_set_subtitle (GtkWidget *row, const char *subtitle)
{
  GtkWidget *s = g_object_get_data (G_OBJECT (row), "ns-subtitle");
  gtk_label_set_text (GTK_LABEL (s), subtitle ? subtitle : "");
  gtk_widget_set_visible (s, subtitle && *subtitle);
}

static void
toggle_switch (GtkWidget *row, gpointer sw)
{
  gtk_switch_set_active (sw, !gtk_switch_get_active (sw));
}

GtkWidget *
ns_row_switch (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key)
{
  GtkWidget *row = ns_row (group, NULL, NULL, title, subtitle);
  GtkWidget *sw = gtk_switch_new ();
  if (s)
    g_settings_bind (s, key, sw, "active", G_SETTINGS_BIND_DEFAULT);
  ns_row_suffix (row, sw);
  ns_row_on_activate (row, toggle_switch, sw);
  return row;
}

GtkWidget *
ns_row_widget (GtkWidget *group, const char *title, GtkWidget *child)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_widget_add_css_class (row, "ns-row");
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  if (title) {
    GtkWidget *t = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (t), 0);
    gtk_widget_add_css_class (t, "ns-title");
    gtk_box_append (GTK_BOX (v), t);
  }
  gtk_box_append (GTK_BOX (v), child);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), v);
  g_object_set_data_full (G_OBJECT (row), "ns-text", g_strdup (title ? title : ""), g_free);
  gtk_list_box_append (GTK_LIST_BOX (group), row);
  return row;
}

/* ---- a stepper for an integer key ---- */

typedef struct {
  GSettings *s;
  char      *key;
  int        lo, hi;
  GtkWidget *value, *minus, *plus;
} Spin;

static void
spin_show (Spin *sp)
{
  int v = g_settings_get_int (sp->s, sp->key);
  g_autofree char *t = g_strdup_printf ("%d", v);
  gtk_label_set_text (GTK_LABEL (sp->value), t);
  gtk_widget_set_sensitive (sp->minus, v > sp->lo);
  gtk_widget_set_sensitive (sp->plus, v < sp->hi);
}

static void
spin_step (GtkButton *b, Spin *sp)
{
  int d = GTK_WIDGET (b) == sp->plus ? 1 : -1;
  g_settings_set_int (sp->s, sp->key, CLAMP (g_settings_get_int (sp->s, sp->key) + d, sp->lo, sp->hi));
}

static void
spin_changed (GSettings *s, const char *key, Spin *sp)
{
  spin_show (sp);
}

static void
spin_free (Spin *sp)
{
  g_signal_handlers_disconnect_by_data (sp->s, sp);
  g_free (sp->key);
  g_free (sp);
}

GtkWidget *
ns_row_spin (GtkWidget *group, const char *title, GSettings *s, const char *key, int lo, int hi)
{
  GtkWidget *row = ns_row (group, NULL, NULL, title, NULL);
  Spin *sp = g_new0 (Spin, 1);
  sp->s = s;
  sp->key = g_strdup (key);
  sp->lo = lo;
  sp->hi = hi;
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  sp->minus = gtk_button_new_from_icon_name ("list-remove-symbolic");
  sp->plus = gtk_button_new_from_icon_name ("list-add-symbolic");
  sp->value = gtk_label_new ("");
  gtk_label_set_width_chars (GTK_LABEL (sp->value), 2);
  gtk_widget_add_css_class (sp->value, "ns-title");
  for (GtkWidget **b = (GtkWidget *[]) { sp->minus, sp->plus, NULL }; *b; b++) {
    gtk_widget_add_css_class (*b, "circular");
    gtk_widget_add_css_class (*b, "flat");
    g_signal_connect (*b, "clicked", G_CALLBACK (spin_step), sp);
  }
  gtk_box_append (GTK_BOX (h), sp->minus);
  gtk_box_append (GTK_BOX (h), sp->value);
  gtk_box_append (GTK_BOX (h), sp->plus);
  ns_row_suffix (row, h);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (spin_changed), sp);
  g_object_set_data_full (G_OBJECT (row), "ns-own", sp, (GDestroyNotify) spin_free);
  spin_show (sp);
  return row;
}

/* ---- a choice from a list ---- */

typedef struct {
  GSettings          *s;
  char               *key;
  const char *const  *values, *const *labels;
  GtkWidget          *row;
  gboolean            font;   /* the values are font families: every label is drawn in its own family */
} Choice;

#define FONT_SAMPLE "The quick brown fox 0123"

/* `label` drawn in `family` (a family, or a CSS style chain "A, B, serif"; Pango takes both) */
static void
font_preview (GtkWidget *label, const char *family)
{
  PangoAttrList *attrs = pango_attr_list_new ();
  if (family && *family)
    pango_attr_list_insert (attrs, pango_attr_family_new (family));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
}

static const char *
choice_label (Choice *c)
{
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  for (guint i = 0; c->values[i]; i++)
    if (!g_strcmp0 (c->values[i], v))
      return c->labels[i];
  return "";
}

/* the row's line under its title: the choice, and for a font a sample in that font */
static void
choice_show (Choice *c)
{
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  if (!c->font) {
    ns_row_set_subtitle (c->row, choice_label (c));
    return;
  }
  g_autofree char *t = g_strdup_printf ("%s • %s", choice_label (c), FONT_SAMPLE);
  ns_row_set_subtitle (c->row, t);
  font_preview (g_object_get_data (G_OBJECT (c->row), "ns-subtitle"), v);
}

static void
choice_picked (GtkWidget *row, gpointer data)
{
  Choice *c = g_object_get_data (G_OBJECT (row), "ns-choice");
  g_settings_set_string (c->s, c->key, data);
  choice_show (c);
  GtkWidget *box = gtk_widget_get_parent (row);
  for (GtkWidget *r = gtk_widget_get_first_child (box); r; r = gtk_widget_get_next_sibling (r)) {
    GtkWidget *mark = g_object_get_data (G_OBJECT (r), "ns-mark");
    if (mark)
      gtk_widget_set_opacity (mark, r == row ? 1 : 0);
  }
}

static void
choice_open (GtkWidget *row, gpointer data)
{
  Choice *c = data;
  GtkWidget *content;
  const char *title = g_object_get_data (G_OBJECT (row), "ns-title");
  AdwNavigationPage *page = ns_page_new (title, &content);
  GtkWidget *g = ns_group (content, NULL);
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  for (guint i = 0; c->values[i]; i++) {
    GtkWidget *r = ns_row (g, NULL, NULL, c->labels[i], c->font ? FONT_SAMPLE : NULL);
    if (c->font) {
      font_preview (g_object_get_data (G_OBJECT (r), "ns-title-label"), c->values[i]);
      font_preview (g_object_get_data (G_OBJECT (r), "ns-subtitle"), c->values[i]);
    }
    GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_add_css_class (mark, "ns-accent");
    gtk_widget_set_opacity (mark, !g_strcmp0 (v, c->values[i]));
    ns_row_suffix (r, mark);
    g_object_set_data (G_OBJECT (r), "ns-mark", mark);
    g_object_set_data (G_OBJECT (r), "ns-choice", c);
    ns_row_on_activate (r, choice_picked, (gpointer) c->values[i]);
  }
  ns_push (page);
}

static void
choice_free (Choice *c)
{
  g_free (c->key);
  g_free (c);
}

static GtkWidget *
choice_new (GtkWidget *group, const char *title, GSettings *s, const char *key, const char *const *values,
            const char *const *labels, gboolean font)
{
  Choice *c = g_new0 (Choice, 1);
  c->font = font;
  c->s = s;
  c->key = g_strdup (key);
  c->values = values;
  c->labels = labels;
  c->row = ns_row (group, NULL, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (c->row), "ns-title", g_strdup (title), g_free);
  choice_show (c);
  ns_row_on_activate (c->row, choice_open, c);
  g_object_set_data_full (G_OBJECT (c->row), "ns-own", c, (GDestroyNotify) choice_free);
  return c->row;
}

GtkWidget *
ns_row_choice (GtkWidget *group, const char *title, GSettings *s, const char *key,
               const char *const *values, const char *const *labels)
{
  return choice_new (group, title, s, key, values, labels, FALSE);
}

GtkWidget *
ns_row_choice_font (GtkWidget *group, const char *title, GSettings *s, const char *key,
                    const char *const *values, const char *const *labels)
{
  return choice_new (group, title, s, key, values, labels, TRUE);
}

GtkWidget *
ns_row_toggle (GtkWidget *group, const char *icon, const char *color, const char *title, const char *subtitle)
{
  GtkWidget *r = ns_row (group, icon, color, title, subtitle);
  GtkWidget *sw = gtk_switch_new ();
  ns_row_suffix (r, sw);
  ns_row_on_activate (r, toggle_switch, sw);
  return r;
}

void
ns_switch_quiet (GtkWidget *sw, gboolean on)
{
  g_object_set_data (G_OBJECT (sw), "ns-syncing", GINT_TO_POINTER (1));
  gtk_switch_set_active (GTK_SWITCH (sw), on);
  g_object_set_data (G_OBJECT (sw), "ns-syncing", NULL);
}

gboolean
ns_switch_syncing (GtkWidget *sw)
{
  return g_object_get_data (G_OBJECT (sw), "ns-syncing") != NULL;
}

GtkWidget *
ns_row_get_switch (GtkWidget *row)
{
  return gtk_widget_get_last_child (g_object_get_data (G_OBJECT (row), "ns-box"));
}

GtkWidget *
ns_row_app (GtkWidget *group, GAppInfo *app, const char *subtitle)
{
  GtkWidget *row = ns_row (group, NULL, NULL, g_app_info_get_display_name (app), subtitle);
  GIcon *gi = g_app_info_get_icon (app);
  GtkWidget *img = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 40);
  gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
  gtk_box_prepend (GTK_BOX (g_object_get_data (G_OBJECT (row), "ns-box")), img);
  g_object_set_data (G_OBJECT (row), "ns-icon", img);
  return row;
}

GtkWidget *
ns_row_info (GtkWidget *group, const char *title, const char *value)
{
  GtkWidget *r = ns_row (group, NULL, NULL, title, value && *value ? value : "–");
  gtk_widget_set_focusable (r, FALSE);
  return r;
}

GtkWidget *
ns_row_button (GtkWidget *group, const char *title, gboolean destructive, NsActivate cb, gpointer data)
{
  GtkWidget *r = ns_row (group, NULL, NULL, title, NULL);
  gtk_widget_add_css_class (g_object_get_data (G_OBJECT (r), "ns-title-label"), destructive ? "ns-danger" : "ns-accent");
  ns_row_on_activate (r, cb, data);
  return r;
}

GtkWidget *
ns_row_scale (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
              GtkWidget **scale)
{
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  if (icon)
    gtk_box_append (GTK_BOX (h), gtk_image_new_from_icon_name (icon));
  GtkWidget *sc = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
  gtk_scale_set_draw_value (GTK_SCALE (sc), FALSE);
  gtk_widget_set_hexpand (sc, TRUE);
  ns_scale_guard (sc);
  gtk_box_append (GTK_BOX (h), sc);
  *scale = sc;
  return ns_row_widget (group, title, h);
}

/* ---- one flag of a flags key ---- */

typedef struct {
  GSettings *s;
  char      *key, *flag;
  GtkWidget *sw;
  gboolean   syncing;
} Flag;

static void
flag_show (GSettings *s, const char *key, Flag *f)
{
  g_auto (GStrv) v = g_settings_get_strv (f->s, f->key);
  f->syncing = TRUE;
  gtk_switch_set_active (GTK_SWITCH (f->sw), g_strv_contains ((const char *const *) v, f->flag));
  f->syncing = FALSE;
}

static void
flag_toggled (GtkSwitch *sw, GParamSpec *ps, Flag *f)
{
  if (f->syncing)
    return;
  g_auto (GStrv) v = g_settings_get_strv (f->s, f->key);
  GStrvBuilder *b = g_strv_builder_new ();
  for (guint i = 0; v[i]; i++)
    if (g_strcmp0 (v[i], f->flag))
      g_strv_builder_add (b, v[i]);
  if (gtk_switch_get_active (sw))
    g_strv_builder_add (b, f->flag);
  g_auto (GStrv) nv = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_settings_set_strv (f->s, f->key, (const char *const *) nv);
}

static void
flag_free (Flag *f)
{
  g_signal_handlers_disconnect_by_data (f->s, f);
  g_free (f->key);
  g_free (f->flag);
  g_free (f);
}

GtkWidget *
ns_row_flag (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key,
             const char *flag)
{
  GtkWidget *row = ns_row_switch (group, title, subtitle, NULL, NULL);
  Flag *f = g_new0 (Flag, 1);
  f->s = s;
  f->key = g_strdup (key);
  f->flag = g_strdup (flag);
  f->sw = ns_row_get_switch (row);
  flag_show (s, key, f);
  g_signal_connect (f->sw, "notify::active", G_CALLBACK (flag_toggled), f);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (flag_show), f);
  g_object_set_data_full (G_OBJECT (row), "ns-own", f, (GDestroyNotify) flag_free);
  return row;
}

/* ---- a choice by position ---- */

typedef struct {
  char     **labels;
  char     **reasons;   /* NULL, or one per label: non-empty = not pickable, said under it */
  guint      selected;
  NsPicked   picked;
  gpointer   data;
  GtkWidget *row;
} Pick;

static void
mark_only (GtkWidget *row)
{
  GtkWidget *box = gtk_widget_get_parent (row);
  for (GtkWidget *r = gtk_widget_get_first_child (box); r; r = gtk_widget_get_next_sibling (r)) {
    GtkWidget *mark = g_object_get_data (G_OBJECT (r), "ns-mark");
    if (mark)
      gtk_widget_set_opacity (mark, r == row ? 1 : 0);
  }
}

static GtkWidget *
mark_row (GtkWidget *g, const char *label, gboolean on)
{
  GtkWidget *r = ns_row (g, NULL, NULL, label, NULL);
  GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (mark, "ns-accent");
  gtk_widget_set_opacity (mark, on);
  ns_row_suffix (r, mark);
  g_object_set_data (G_OBJECT (r), "ns-mark", mark);
  return r;
}

static void
pick_picked (GtkWidget *row, gpointer index)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");
  mark_only (row);
  ns_row_pick_select (p->row, GPOINTER_TO_UINT (index));
  if (p->picked)
    p->picked (p->row, GPOINTER_TO_UINT (index), p->data);
}

static void
pick_open (GtkWidget *row, gpointer data)
{
  Pick *p = data;
  GtkWidget *content;
  AdwNavigationPage *page = ns_page_new (g_object_get_data (G_OBJECT (row), "ns-title"), &content);
  GtkWidget *g = ns_group (content, NULL);
  for (guint i = 0; p->labels[i]; i++) {
    GtkWidget *r = mark_row (g, p->labels[i], i == p->selected);
    const char *reason = p->reasons && i < g_strv_length (p->reasons) ? p->reasons[i] : NULL;
    if (reason && *reason) {
      ns_row_set_subtitle (r, reason);
      gtk_widget_set_sensitive (r, FALSE);
      continue;
    }
    g_object_set_data (G_OBJECT (r), "ns-pick", p);
    ns_row_on_activate (r, pick_picked, GUINT_TO_POINTER (i));
  }
  ns_push (page);
}

static void
pick_free (Pick *p)
{
  g_strfreev (p->labels);
  g_strfreev (p->reasons);
  g_free (p);
}

GtkWidget *
ns_row_pick (GtkWidget *group, const char *title, const char *const *labels, guint selected, NsPicked picked,
             gpointer data)
{
  Pick *p = g_new0 (Pick, 1);
  p->labels = g_strdupv ((char **) labels);
  p->picked = picked;
  p->data = data;
  p->row = ns_row (group, NULL, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (p->row), "ns-title", g_strdup (title), g_free);
  g_object_set_data (G_OBJECT (p->row), "ns-pick", p);
  ns_row_pick_select (p->row, selected);
  ns_row_on_activate (p->row, pick_open, p);
  g_object_set_data_full (G_OBJECT (p->row), "ns-own", p, (GDestroyNotify) pick_free);
  return p->row;
}

void
ns_row_pick_select (GtkWidget *row, guint selected)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");
  p->selected = selected;
  ns_row_set_subtitle (row, selected < g_strv_length (p->labels) ? p->labels[selected] : NULL);
}

void
ns_row_pick_set_reasons (GtkWidget *row, const char *const *reasons)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");
  g_strfreev (p->reasons);
  p->reasons = g_strdupv ((char **) reasons);
}

/* ---- a typed value ---- */

typedef struct {
  GSettings *s;
  char      *key;
  GtkWidget *row;
} Entry;

static gboolean
entry_is_int (Entry *e)
{
  g_autoptr (GVariant) v = g_settings_get_value (e->s, e->key);
  return g_variant_is_of_type (v, G_VARIANT_TYPE_INT32);
}

static char *
entry_text (Entry *e)
{
  if (entry_is_int (e))
    return g_strdup_printf ("%d", g_settings_get_int (e->s, e->key));
  return g_settings_get_string (e->s, e->key);
}

static void
entry_show (GSettings *s, const char *key, Entry *e)
{
  g_autofree char *t = entry_text (e);
  ns_row_set_subtitle (e->row, *t ? t : "Not set");
}

static void
entry_done (const char *text, gpointer data)
{
  Entry *e = data;
  if (!text)
    return;
  if (entry_is_int (e))
    g_settings_set_int (e->s, e->key, atoi (text));
  else
    g_settings_set_string (e->s, e->key, text);
}

static void
entry_open (GtkWidget *row, gpointer data)
{
  Entry *e = data;
  g_autofree char *t = entry_text (e);
  ns_ask (row, g_object_get_data (G_OBJECT (row), "ns-title"), NULL, t, FALSE, "Save", entry_done, e);
}

static void
entry_free (Entry *e)
{
  g_signal_handlers_disconnect_by_data (e->s, e);
  g_free (e->key);
  g_free (e);
}

GtkWidget *
ns_row_entry (GtkWidget *group, const char *title, GSettings *s, const char *key)
{
  Entry *e = g_new0 (Entry, 1);
  e->s = s;
  e->key = g_strdup (key);
  e->row = ns_row (group, NULL, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (e->row), "ns-title", g_strdup (title), g_free);
  entry_show (s, key, e);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (entry_show), e);
  ns_row_on_activate (e->row, entry_open, e);
  g_object_set_data_full (G_OBJECT (e->row), "ns-own", e, (GDestroyNotify) entry_free);
  return e->row;
}

/* ---- texts of the rows and dialogs below: English, unless a caller set its own on an ancestor (ns_texts_set) ---- */

#define NS_TEXT_NOT_SET "ns-text-not-set"
#define NS_TEXT_SAVE    "ns-text-save"
#define NS_TEXT_CANCEL  "ns-text-cancel"

void
ns_texts_set (GtkWidget *scope, const char *not_set, const char *save, const char *cancel)
{
  const char *names[] = { NS_TEXT_NOT_SET, NS_TEXT_SAVE, NS_TEXT_CANCEL };
  const char *texts[] = { not_set, save, cancel };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    g_object_set_data_full (G_OBJECT (scope), names[i], texts[i] && *texts[i] ? g_strdup (texts[i]) : NULL, g_free);
}

/* the text `name` of the nearest ancestor of `w` (itself included) that has one, else `fallback` */
static const char *
ns_text (GtkWidget *w, const char *name, const char *fallback)
{
  for (; w; w = gtk_widget_get_parent (w)) {
    const char *t = g_object_get_data (G_OBJECT (w), name);
    if (t)
      return t;
  }
  return fallback;
}

/* a value of the caller's own (a form field): the row shows it, a tap asks for a new one */
typedef struct {
  GtkWidget *row;
  char      *title, *value;
  gboolean   secret;
  NsEntered  changed;
  gpointer   data;
} Value;

static void
value_show (Value *v)
{
  ns_row_set_subtitle (v->row, !v->value || !*v->value ? ns_text (v->row, NS_TEXT_NOT_SET, "Not set")
                               : v->secret ? "••••••••" : v->value);
}

static void
value_done (const char *text, gpointer data)
{
  Value *v = data;
  if (!text)
    return;
  g_free (v->value);
  v->value = g_strstrip (g_strdup (text));
  value_show (v);
  if (v->changed)
    v->changed (v->value, v->data);
}

static void
value_open (GtkWidget *row, gpointer data)
{
  Value *v = data;
  ns_ask (row, v->title, NULL, v->value, v->secret, ns_text (row, NS_TEXT_SAVE, "Save"), value_done, v);
}

static void
value_free (Value *v)
{
  g_free (v->title);
  g_free (v->value);
  g_free (v);
}

GtkWidget *
ns_row_value (GtkWidget *group, const char *title, const char *value, gboolean secret, NsEntered changed,
              gpointer data)
{
  Value *v = g_new0 (Value, 1);
  v->row = ns_row (group, NULL, NULL, title, NULL);
  v->title = g_strdup (title);
  v->value = g_strdup (value);
  v->secret = secret;
  v->changed = changed;
  v->data = data;
  value_show (v);
  ns_row_on_activate (v->row, value_open, v);
  g_object_set_data_full (G_OBJECT (v->row), "ns-value", v, (GDestroyNotify) value_free);
  return v->row;
}

const char *
ns_row_value_get (GtkWidget *row)
{
  Value *v = g_object_get_data (G_OBJECT (row), "ns-value");
  return v && v->value ? v->value : "";
}

void
ns_row_value_set (GtkWidget *row, const char *value)
{
  Value *v = g_object_get_data (G_OBJECT (row), "ns-value");
  g_free (v->value);
  v->value = g_strdup (value);
  value_show (v);
}

/* ---- background actions: one spinner card ---- */

struct _NsBusy {
  GtkWidget *group, *row, *spinner, *cancel_row, *retry_row;
  guint      timer;
  gint64     t0;      /* monotonic µs: when the action began */
  gboolean   active;
  NsBusyText t;       /* the strings are the card's own copies */
};

static void
busy_text_clear (NsBusy *b)
{
  g_free ((char *) b->t.what);
  g_free ((char *) b->t.slow);
  g_free ((char *) b->t.cancel);
  g_free ((char *) b->t.retry);
  memset (&b->t, 0, sizeof b->t);
}

static void
busy_free (NsBusy *b)
{
  if (b->timer)
    g_source_remove (b->timer);
  busy_text_clear (b);
  g_free (b);
}

static void
busy_set_title (GtkWidget *row, const char *text)
{
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (row), "ns-title-label")), text ? text : "");
}

static void
busy_draw (NsBusy *b)
{
  guint secs = (guint) ((g_get_monotonic_time () - b->t0) / G_USEC_PER_SEC);
  gboolean slow = secs >= b->t.slow_s;
  g_autofree char *line = slow && b->t.slow ? g_strdup_printf (b->t.slow, secs) : NULL;
  ns_row_set_subtitle (b->row, line);
  gtk_widget_set_visible (b->cancel_row, slow && b->t.cancel && b->t.on_cancel);
  gtk_widget_set_visible (b->retry_row, slow && b->t.retry && b->t.on_retry);
}

static gboolean
busy_tick (gpointer data)
{
  busy_draw (data);
  return G_SOURCE_CONTINUE;
}

static void
busy_cancel_tapped (GtkWidget *row, gpointer data)
{
  NsBusy *b = data;
  if (b->t.on_cancel)
    b->t.on_cancel (b->t.data);
}

static void
busy_retry_tapped (GtkWidget *row, gpointer data)
{
  NsBusy *b = data;
  if (b->t.on_retry)
    b->t.on_retry (b->t.data);
}

NsBusy *
ns_busy_new (GtkWidget *content)
{
  NsBusy *b = g_new0 (NsBusy, 1);
  b->group = ns_group (content, NULL);
  b->row = ns_row (b->group, NULL, NULL, "", NULL);
  gtk_widget_set_focusable (b->row, FALSE);
  b->spinner = gtk_spinner_new ();
  ns_row_suffix (b->row, b->spinner);
  b->cancel_row = ns_row_button (b->group, "", FALSE, busy_cancel_tapped, b);
  b->retry_row = ns_row_button (b->group, "", FALSE, busy_retry_tapped, b);
  gtk_widget_set_visible (b->cancel_row, FALSE);
  gtk_widget_set_visible (b->retry_row, FALSE);
  ns_group_set_visible (b->group, FALSE);
  g_object_set_data_full (G_OBJECT (b->group), "ns-busy", b, (GDestroyNotify) busy_free);
  return b;
}

void
ns_busy_start (NsBusy *b, const NsBusyText *t)
{
  gboolean same = b->active && !g_strcmp0 (b->t.what, t->what);
  busy_text_clear (b);
  b->t = *t;
  b->t.what = g_strdup (t->what);
  b->t.slow = g_strdup (t->slow);
  b->t.cancel = g_strdup (t->cancel);
  b->t.retry = g_strdup (t->retry);
  busy_set_title (b->row, b->t.what);
  busy_set_title (b->cancel_row, b->t.cancel);
  busy_set_title (b->retry_row, b->t.retry);
  if (!same)
    b->t0 = g_get_monotonic_time ();
  b->active = TRUE;
  gtk_spinner_start (GTK_SPINNER (b->spinner));
  ns_group_set_visible (b->group, TRUE);
  if (!b->timer)
    b->timer = g_timeout_add_seconds (1, busy_tick, b);
  busy_draw (b);
}

void
ns_busy_restart (NsBusy *b)
{
  b->t0 = g_get_monotonic_time ();
  busy_draw (b);
}

void
ns_busy_stop (NsBusy *b)
{
  if (b->timer)
    g_source_remove (b->timer);
  b->timer = 0;
  b->active = FALSE;
  gtk_spinner_stop (GTK_SPINNER (b->spinner));
  ns_group_set_visible (b->group, FALSE);
}

gboolean
ns_busy_active (NsBusy *b)
{
  return b->active;
}

/* ---- notes and cards ---- */

GtkWidget *
ns_note (GtkWidget *content, const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "ns-note");
  gtk_box_append (GTK_BOX (content), l);
  return l;
}

void
ns_group_clear (GtkWidget *group)
{
  gtk_list_box_remove_all (GTK_LIST_BOX (group));
}

void
ns_group_set_visible (GtkWidget *group, gboolean visible)
{
  GtkWidget *t = g_object_get_data (G_OBJECT (group), "ns-group-title");
  gtk_widget_set_visible (group, visible);
  if (t)
    gtk_widget_set_visible (t, visible);
}

/* ---- lists that follow a service ---- */

typedef struct {
  guint  timer;
  NsFill fill;
} Later;

static gboolean
later_run (gpointer page)
{
  Later *l = g_object_get_data (page, "ns-later");
  l->timer = 0;
  l->fill (page);
  return G_SOURCE_REMOVE;
}

void
ns_later (AdwNavigationPage *page)
{
  Later *l = g_object_get_data (G_OBJECT (page), "ns-later");
  if (l->timer)
    g_source_remove (l->timer);
  l->timer = g_timeout_add (250, later_run, page);
}

static void
later_free (Later *l)
{
  if (l->timer)
    g_source_remove (l->timer);
  g_free (l);
}

void
ns_later_init (AdwNavigationPage *page, NsFill fill)
{
  Later *l = g_new0 (Later, 1);
  l->fill = fill;
  g_object_set_data_full (G_OBJECT (page), "ns-later", l, (GDestroyNotify) later_free);
}

/* ---- dialogs ---- */

typedef struct {
  NsEntered  done;
  NsAnswered answered;
  gpointer   data;
  GtkWidget *entry;
} Ask;

static void
ask_response (AdwAlertDialog *d, const char *response, Ask *a)
{
  gboolean ok = !g_strcmp0 (response, "ok");
  if (a->done)
    a->done (ok ? gtk_editable_get_text (GTK_EDITABLE (a->entry)) : NULL, a->data);
  else if (a->answered)
    a->answered (ok, a->data);
}

static AdwDialog *
alert (GtkWidget *parent, const char *heading, const char *body, const char *ok, gboolean destructive, Ask *a)
{
  AdwDialog *d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", ns_text (parent, NS_TEXT_CANCEL, "Cancel"), "ok", ok,
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "ok",
                                            destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect_data (d, "response", G_CALLBACK (ask_response), a, (GClosureNotify) (void (*) (void)) g_free, 0);
  return d;
}

void
ns_ask (GtkWidget *parent, const char *heading, const char *body, const char *initial, gboolean secret,
        const char *ok, NsEntered done, gpointer data)
{
  Ask *a = g_new0 (Ask, 1);
  a->done = done;
  a->data = data;
  a->entry = secret ? gtk_password_entry_new () : gtk_entry_new ();
  if (secret)
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (a->entry), TRUE);
  g_object_set (a->entry, "activates-default", TRUE, NULL);
  gtk_editable_set_text (GTK_EDITABLE (a->entry), initial ? initial : "");
  AdwDialog *d = alert (parent, heading, body, ok, FALSE, a);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), a->entry);
  adw_dialog_present (d, parent);
  gtk_widget_grab_focus (a->entry);
}

AdwDialog *
ns_confirm (GtkWidget *parent, const char *heading, const char *body, const char *yes, gboolean destructive,
            NsAnswered done, gpointer data)
{
  Ask *a = g_new0 (Ask, 1);
  a->answered = done;
  a->data = data;
  AdwDialog *d = alert (parent, heading, body, yes, destructive, a);
  adw_dialog_present (d, parent);
  return d;
}

/* ---- the rest ---- */

void
ns_spawn (const char *const *argv)
{
  g_autoptr (GError) err = NULL;
  if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err))
    g_warning ("%s: %s", argv[0], err->message);
}

/* ---- a slider that a scrolling finger cannot change ----
 * GtkRange jumps to a tap on its track and follows any drag that starts on it, so on a phone a finger scrolling the
 * page moves every slider it lands on. The guard takes the slider's pointer and touch events in the capture phase,
 * before the range's own gestures (the scrolled window above has seen them already, so it still scrolls): a drag
 * moves the value only once it went slider-slop-px sideways and more sideways than up or down, and then from where
 * the value was, by the finger's travel over the slider's width; a tap changes nothing. The keyboard is untouched. */

typedef struct {
  gboolean down, moving;
  double   x0, y0, value0;
  GdkEventSequence *seq;
} Guard;

static double
guard_slop (void)
{
  static double slop = -1;
  if (slop < 0) {
    g_autoptr (GSettings) s = ns_settings ("de.yesman.NeoSettings");
    g_autoptr (GSettingsSchema) sc = NULL;
    if (s)
      g_object_get (s, "settings-schema", &sc, NULL);
    slop = sc && g_settings_schema_has_key (sc, "slider-slop-px") ? g_settings_get_double (s, "slider-slop-px") : 0;
    if (!sc || !g_settings_schema_has_key (sc, "slider-slop-px"))   /* an older schema installed: no slop at all */
      g_warning ("de.yesman.NeoSettings has no slider-slop-px: sliders follow any sideways drag");
  }
  return slop;
}

static gboolean
guard_event (GtkEventControllerLegacy *c, GdkEvent *ev, Guard *g)
{
  GtkWidget *w = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (c));
  GdkEventType type = gdk_event_get_event_type (ev);
  GtkNative *native = gtk_widget_get_native (w);
  double sx, sy, nx, ny;
  graphene_point_t in, out;

  switch (type) {
  case GDK_BUTTON_PRESS: case GDK_TOUCH_BEGIN: case GDK_MOTION_NOTIFY: case GDK_TOUCH_UPDATE:
  case GDK_BUTTON_RELEASE: case GDK_TOUCH_END: case GDK_TOUCH_CANCEL:
    break;
  default:
    return FALSE;   /* scroll, keys, crossing: as GTK does them */
  }
  if (!gdk_event_get_position (ev, &sx, &sy))
    return TRUE;
  gtk_native_get_surface_transform (native, &nx, &ny);
  in = GRAPHENE_POINT_INIT (sx - nx, sy - ny);
  if (!gtk_widget_compute_point (GTK_WIDGET (native), w, &in, &out))
    return TRUE;
  if (type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN) {
    *g = (Guard) { .down = TRUE, .x0 = out.x, .y0 = out.y, .value0 = gtk_range_get_value (GTK_RANGE (w)),
                   .seq = gdk_event_get_event_sequence (ev) };
    return TRUE;
  }
  if (!g->down || gdk_event_get_event_sequence (ev) != g->seq)
    return TRUE;
  if (type == GDK_BUTTON_RELEASE || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL) {
    g->down = g->moving = FALSE;
    return TRUE;
  }
  double dx = out.x - g->x0, dy = out.y - g->y0;
  if (!g->moving && fabs (dx) >= guard_slop () && fabs (dx) > fabs (dy))
    g->moving = TRUE;
  if (g->moving && gtk_widget_get_width (w) > 0) {
    GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (w));
    double span = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_lower (adj);
    gtk_range_set_value (GTK_RANGE (w), g->value0 + dx / gtk_widget_get_width (w) * span);
  }
  return TRUE;
}

void
ns_scale_guard (GtkWidget *scale)
{
  GtkEventController *c = gtk_event_controller_legacy_new ();
  Guard *g = g_new0 (Guard, 1);

  gtk_event_controller_set_propagation_phase (c, GTK_PHASE_CAPTURE);
  g_signal_connect_data (c, "event", G_CALLBACK (guard_event), g, (GClosureNotify) g_free, 0);
  gtk_widget_add_controller (scale, c);
}

GSettings *
ns_settings (const char *schema)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  return sc ? g_settings_new_full (sc, NULL, NULL) : NULL;
}

gboolean
ns_settings_exists (const char *schema)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  return sc != NULL;
}

char *
ns_munge_app_id (const char *id)
{
  char *m = g_ascii_strdown (id, -1);
  if (g_str_has_suffix (m, ".desktop"))
    m[strlen (m) - strlen (".desktop")] = 0;
  return g_strcanon (m, "0123456789abcdefghijklmnopqrstuvwxyz-", '-');
}

GSettings *
ns_page_settings (AdwNavigationPage *page, const char *schema)
{
  GSettings *s = ns_settings (schema);
  if (s)
    g_object_set_data_full (G_OBJECT (page), schema, s, g_object_unref);
  return s;
}

/* The always-on display suits OLED panels: black costs them nothing, and gnome-phone's AOD (extension/aod.js) puts
 * them in their low power mode, where the panel itself keeps the picture at a few nits and ticks the clock. That
 * mode is the `lpm` attribute of the panel's DSI device, which only an OLED panel's driver has (the S10e's). An LCD
 * (the POCO's) lights its whole backlight for a black screen. */
gboolean
ns_panel_is_oled (void)
{
  g_autoptr (GDir) d = g_dir_open (NS_DSI_DEVICES, 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_autofree char *lpm = g_build_filename (NS_DSI_DEVICES, n, "lpm", NULL);
    if (g_file_test (lpm, G_FILE_TEST_EXISTS))
      return TRUE;
  }
  return FALSE;
}

gboolean
ns_has_key (GSettings *s, const char *key)
{
  g_autoptr (GSettingsSchema) sc = NULL;
  if (!s)
    return FALSE;
  g_object_get (s, "settings-schema", &sc, NULL);
  return g_settings_schema_has_key (sc, key);
}

GdkTexture *
ns_texture_for_pixbuf (GdkPixbuf *pb)
{
  g_autoptr (GBytes) b = gdk_pixbuf_read_pixel_bytes (pb);
  return gdk_memory_texture_new (gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb),
                                 gdk_pixbuf_get_has_alpha (pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, b,
                                 gdk_pixbuf_get_rowstride (pb));
}

char *
ns_run_line (const char *const *argv)
{
  g_autofree char *out = NULL;
  if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, NULL, NULL) || !out)
    return NULL;
  char *nl = strchr (out, '\n');
  if (nl)
    *nl = 0;
  g_strstrip (out);
  return *out ? g_strdup (out) : NULL;
}
