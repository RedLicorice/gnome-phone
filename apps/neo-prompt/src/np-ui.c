/* Neo Prompt's small building blocks: see np-ui.h. */
#include "np-ui.h"

#include <neo.h>

GtkWidget *
np_label (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text ? text : "");

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  if (css)
    gtk_widget_add_css_class (label, css);
  return label;
}

GtkWidget *
np_sub (const char *text)
{
  return np_label (text, "np-sub");
}

GtkWidget *
np_card (void)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (card, "neo-card");
  gtk_widget_add_css_class (card, "np-card");
  return card;
}

GtkWidget *
np_row (const char *title, const char *subtitle, const char *css)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *t = np_label (title, "np-row-title");
  GtkWidget *s = np_sub (subtitle ? subtitle : "");
  GtkWidget *pressable = neo_pressable_new (box);

  gtk_widget_set_visible (s, subtitle && *subtitle);
  gtk_box_append (GTK_BOX (box), t);
  gtk_box_append (GTK_BOX (box), s);
  g_object_set_data (G_OBJECT (pressable), "np-sub", s);
  gtk_widget_add_css_class (pressable, "np-row");
  if (css)
    gtk_widget_add_css_class (pressable, css);
  neo_pressable_set_ripple (NEO_PRESSABLE (pressable), NEO_RIPPLE_BOUNDED, 0);
  return pressable;
}

void
np_row_set_sub (GtkWidget *row, const char *subtitle)
{
  GtkWidget *sub = g_object_get_data (G_OBJECT (row), "np-sub");

  if (!sub)
    return;
  gtk_label_set_text (GTK_LABEL (sub), subtitle ? subtitle : "");
  gtk_widget_set_visible (sub, subtitle && *subtitle);
}

GtkWidget *
np_msg (void)
{
  GtkWidget *label = np_label ("", "np-msg");

  gtk_widget_set_visible (label, FALSE);
  return label;
}

void
np_msg_set (GtkWidget *label, const char *text, gboolean error)
{
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  gtk_widget_set_visible (label, text && *text);
  if (error)
    gtk_widget_add_css_class (label, "np-error");
  else
    gtk_widget_remove_css_class (label, "np-error");
}

GtkWidget *
np_badge (const char *text, const char *tone)
{
  GtkWidget *badge = np_label (text, "np-badge");

  gtk_label_set_wrap (GTK_LABEL (badge), FALSE);
  gtk_widget_set_halign (badge, GTK_ALIGN_START);
  if (tone)
    gtk_widget_add_css_class (badge, tone);
  return badge;
}

GtkWidget *
np_scroller (GtkWidget *child)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), child);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

const char *
np_short_path (const char *path)
{
  const char *home = g_get_home_dir ();
  static char buffer[512];

  if (!path)
    return "";
  if (home && g_str_has_prefix (path, home))
    {
      g_snprintf (buffer, sizeof buffer, "~%s", path + strlen (home));
      return buffer;
    }
  return path;
}

gboolean
np_lookup (GtkWidget *widget, const char *name, GdkRGBA *out)
{
  GtkStyleContext *context = gtk_widget_get_style_context (widget);

  return gtk_style_context_lookup_color (context, name, out);
}
