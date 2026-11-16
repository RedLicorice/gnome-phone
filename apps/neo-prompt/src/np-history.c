/* Neo Prompt's history: see np-history.h. */
#include "np-history.h"
#include "np-diff.h"
#include "np-ui.h"

#include <neo.h>
#include <string.h>

struct _NpHistory {
  NpStore   *store;
  GtkWindow *window;
  GtkWidget *snackbar;
  GtkWidget *root, *stack, *list, *diff_box, *diff_view, *diff_title, *restore_btn;
  NpDoc     *doc;              /* borrowed: the editor's */
  char      *shown;            /* the snapshot the diff shows */
  char *(*current_cb) (gpointer);
  gpointer  current_data;
  void    (*restore_cb) (gpointer, const char *text);
  gpointer  restore_data;
};

static void show_diff (NpHistory *self, const char *name);

gboolean
np_history_back (NpHistory *self)
{
  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (self->stack)), "diff") != 0)
    return FALSE;
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "list");
  g_clear_pointer (&self->shown, g_free);
  return TRUE;
}

static void
back_clicked (GtkWidget *button, gpointer data)
{
  np_history_back (data);
}

static void
restore_clicked (GtkWidget *button, gpointer data)
{
  NpHistory *self = data;
  g_autofree char *text = self->shown ? np_history_read (self->store, self->doc, self->shown) : NULL;

  if (!text || !self->restore_cb)
    return;
  self->restore_cb (self->restore_data, text);
  if (self->snackbar)
    neo_snackbar_show (NEO_SNACKBAR (self->snackbar), np_ui (self->store, "history.restored"), NULL, NULL, NULL,
                       NEO_SNACKBAR_SHORT);
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "list");
}

static void
row_clicked (GtkWidget *row, gpointer data)
{
  show_diff (data, g_object_get_data (G_OBJECT (row), "np-snapshot"));
}

static char *
stamp_of (const char *name)
{
  /* 20261116T103000-<hash>.md */
  g_autofree char *copy = g_strndup (name, 15);
  GDateTime *when = NULL;
  char *out = NULL;

  if (strlen (copy) < 15)
    return g_strdup (name);
  {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    if (sscanf (copy, "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
      return g_strdup (copy);
    when = g_date_time_new_local (y, mo, d, h, mi, s);
    out = when ? g_date_time_format (when, "%d/%m %H:%M") : g_strdup (copy);
    if (when)
      g_date_time_unref (when);
  }
  return out;
}

static void
show_diff (NpHistory *self, const char *name)
{
  g_autofree char *old = np_history_read (self->store, self->doc, name);
  g_autofree char *now = self->current_cb ? self->current_cb (self->current_data) : NULL;
  g_autoptr(GPtrArray) lines = old ? np_diff_lines (old, now ? now : "") : NULL;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->diff_view));
  GtkTextIter end;
  GdkRGBA add, del, same;

  g_free (self->shown);
  self->shown = g_strdup (name);
  gtk_label_set_text (GTK_LABEL (self->diff_title), stamp_of (name));
  gtk_text_buffer_set_text (buffer, "", 0);
  gtk_widget_set_sensitive (self->restore_btn, lines != NULL);
  if (!lines)
    {
      gtk_text_buffer_get_end_iter (buffer, &end);
      gtk_text_buffer_insert (buffer, &end, np_ui (self->store, "history.too_different"), -1);
      gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "diff");
      return;
    }
  np_lookup (self->diff_view, "neo_primary", &add);
  np_lookup (self->diff_view, "neo_error", &del);
  np_lookup (self->diff_view, "neo_on_surface_variant", &same);
  {
    GtkTextTag *t_add = gtk_text_buffer_create_tag (buffer, "np-diff-add", "foreground-rgba", &add, NULL);
    GtkTextTag *t_del = gtk_text_buffer_create_tag (buffer, "np-diff-del", "foreground-rgba", &del, NULL);
    GtkTextTag *t_same = gtk_text_buffer_create_tag (buffer, "np-diff-same", "foreground-rgba", &same, NULL);

    gtk_text_buffer_get_end_iter (buffer, &end);
    for (guint i = 0; i < lines->len; i++)
      {
        NpDiffLine *line = g_ptr_array_index (lines, i);
        const char *mark = line->kind == NP_DIFF_ADD ? "+ " : line->kind == NP_DIFF_DEL ? "- " : "  ";
        g_autofree char *text = g_strdup_printf ("%s%s\n", mark, line->text);
        int line0 = gtk_text_buffer_get_line_count (buffer) - 1;
        GtkTextIter a, b;

        gtk_text_buffer_get_end_iter (buffer, &a);
        gtk_text_buffer_insert (buffer, &a, text, -1);
        gtk_text_buffer_get_iter_at_line (buffer, &a, line0);
        b = a;
        gtk_text_iter_forward_to_line_end (&b);
        gtk_text_buffer_apply_tag (buffer, line->kind == NP_DIFF_ADD ? t_add
                                       : line->kind == NP_DIFF_DEL ? t_del : t_same, &a, &b);
      }
  }
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "diff");
}

void
np_history_refresh (NpHistory *self)
{
  g_autoptr(GPtrArray) names = NULL;
  GtkWidget *card = NULL;

  while (gtk_widget_get_first_child (self->list))
    gtk_box_remove (GTK_BOX (self->list), gtk_widget_get_first_child (self->list));
  if (!self->doc)
    return;
  names = np_history_list (self->store, self->doc);
  if (!names->len)
    {
      card = np_card ();
      gtk_box_append (GTK_BOX (card), np_row (np_ui (self->store, "history.empty"),
                                              np_ui (self->store, "history.empty_sub"), "np-block-flat"));
      gtk_box_append (GTK_BOX (self->list), card);
      return;
    }
  card = np_card ();
  for (guint i = 0; i < names->len; i++)
    {
      const char *name = g_ptr_array_index (names, i);
      g_autofree char *text = np_history_read (self->store, self->doc, name);
      g_autofree char *when = stamp_of (name);
      g_autofree char *sub = NULL;
      GtkWidget *row = NULL;

      if (text)
        {
          guint lines = 0;
          g_auto(GStrv) split = g_strsplit (text, "\n", -1);

          lines = g_strv_length (split);
          sub = g_strdup_printf ("%d righe · %d caratteri", lines, (int) g_utf8_strlen (text, -1));
        }
      else
        sub = g_strdup ("?");
      row = np_row (when, sub, "np-history-row");
      g_object_set_data_full (G_OBJECT (row), "np-snapshot", g_strdup (name), g_free);
      g_signal_connect (row, "clicked", G_CALLBACK (row_clicked), self);
      gtk_box_append (GTK_BOX (card), row);
    }
  gtk_box_append (GTK_BOX (self->list), card);
}

void
np_history_set_doc (NpHistory *self, NpDoc *doc)
{
  self->doc = doc;
  np_history_refresh (self);
}

NpHistory *
np_history_new (NpStore *store, GtkWindow *window, GtkWidget *snackbar)
{
  NpHistory *self = g_new0 (NpHistory, 1);
  GtkWidget *list_scroll = NULL, *diff_scroll = NULL, *bar = NULL, *back = NULL;

  self->store = store;
  self->window = window;
  self->snackbar = snackbar;
  self->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->stack = gtk_stack_new ();
  gtk_widget_add_css_class (self->stack, "np-history-stack");
  gtk_box_append (GTK_BOX (self->root), self->stack);

  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->list, 8);
  gtk_widget_set_margin_bottom (self->list, 24);
  list_scroll = np_scroller (self->list);
  gtk_stack_add_named (GTK_STACK (self->stack), list_scroll, "list");

  self->diff_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start (bar, 4);
  gtk_widget_set_margin_end (bar, 8);
  back = neo_icon_button_new (np_ui (store, "icons.back"));
  g_signal_connect (back, "clicked", G_CALLBACK (back_clicked), self);
  self->diff_title = np_label ("", "np-diff-title");
  gtk_widget_set_hexpand (self->diff_title, TRUE);
  self->restore_btn = neo_text_button_new (np_ui (store, "history.restore"));
  g_signal_connect (self->restore_btn, "clicked", G_CALLBACK (restore_clicked), self);
  gtk_box_append (GTK_BOX (bar), back);
  gtk_box_append (GTK_BOX (bar), self->diff_title);
  gtk_box_append (GTK_BOX (bar), self->restore_btn);
  gtk_box_append (GTK_BOX (self->diff_box), bar);
  self->diff_view = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->diff_view), TRUE);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->diff_view), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->diff_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->diff_view), 12);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->diff_view), 12);
  diff_scroll = np_scroller (self->diff_view);
  gtk_box_append (GTK_BOX (self->diff_box), diff_scroll);
  gtk_stack_add_named (GTK_STACK (self->stack), self->diff_box, "diff");
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "list");
  return self;
}

GtkWidget *
np_history_root (NpHistory *self)
{
  return self->root;
}

void
np_history_set_current_cb (NpHistory *self, char *(*cb) (gpointer), gpointer data)
{
  self->current_cb = cb;
  self->current_data = data;
}

void
np_history_set_restore_cb (NpHistory *self, void (*cb) (gpointer, const char *text), gpointer data)
{
  self->restore_cb = cb;
  self->restore_data = data;
}
