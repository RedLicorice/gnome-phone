/* Neo Prompt's editor: see np-editor.h. */
#include "np-editor.h"
#include "np-ui.h"

#include <neo.h>
#include <string.h>

typedef struct {
  int line;
  int start;
} HitPos;

struct _NpEditor {
  NpStore      *store;
  GtkWindow    *window;
  GtkWidget    *snackbar;
  GtkWidget    *root, *view, *error, *searchbar, *entry, *hits;
  GtkTextBuffer *buffer;
  GtkWidget    *index_popover, *index_list;
  NpDoc        *doc;
  char         *mode;          /* "source" (the Markdown as written) or "rendered" (impaginated) */
  char         *loaded_hash;
  GArray       *hit_pos;
  guint         hit_index;
  guint         highlight_id, draft_id, token;
  int           cursor_line;   /* the line being edited: rendered mode leaves its markers visible */
  gboolean      loading, saving, overwrite;
  GtkTextTag   *t_heading, *t_jinja, *t_code, *t_strong, *t_em, *t_link, *t_comment, *t_quote, *t_hit, *t_mark;
  GtkTextTag   *t_hidden, *t_h[6], *t_chip, *t_list_indent, *t_quote_indent;
  void        (*saved_cb) (gpointer);
  gpointer      saved_data;
  void        (*changed_cb) (gpointer);
  gpointer      changed_data;
  void        (*save_done) (gboolean ok, gpointer data);
  gpointer      save_done_data;
};

static void paint (NpEditor *self);
static void save_finished (NpEditor *self, gboolean ok);
static void save_now (NpEditor *self);

static void
snack (NpEditor *self, const char *text)
{
  if (self->snackbar)
    neo_snackbar_show (NEO_SNACKBAR (self->snackbar), text, NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

/* ---- text ---- */

char *
np_editor_text (NpEditor *self)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  /* hidden characters are in the text: the rendered mode hides the Markdown markers with a tag, it never drops them */
  return gtk_text_buffer_get_text (self->buffer, &start, &end, TRUE);
}

static void
put_text (NpEditor *self, const char *text, gboolean modified)
{
  self->loading = TRUE;
  gtk_text_buffer_set_text (self->buffer, text ? text : "", -1);
  self->loading = FALSE;
  gtk_text_buffer_set_modified (self->buffer, modified);
}

void
np_editor_put_text (NpEditor *self, const char *text)
{
  put_text (self, text, TRUE);
  paint (self);
}

NpDoc *
np_editor_doc (NpEditor *self)
{
  return self->doc;
}

gboolean
np_editor_modified (NpEditor *self)
{
  return gtk_text_buffer_get_modified (self->buffer);
}

gboolean
np_editor_saving (NpEditor *self)
{
  return self->saving;
}

gboolean
np_editor_rendered (NpEditor *self)
{
  return g_strcmp0 (self->mode, "rendered") == 0;
}

static void
apply_mode (NpEditor *self)
{
  gboolean rendered = np_editor_rendered (self);

  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->view), !rendered);
  if (rendered)
    gtk_widget_add_css_class (self->view, "np-rendered");
  else
    gtk_widget_remove_css_class (self->view, "np-rendered");
  paint (self);
}

void
np_editor_set_rendered (NpEditor *self, gboolean rendered)
{
  const char *mode = rendered ? "rendered" : "source";

  if (g_strcmp0 (self->mode, mode) == 0)
    return;
  g_free (self->mode);
  self->mode = g_strdup (mode);
  if (self->doc)
    np_mode_set (self->store, np_doc_kind (self->doc), mode);
  apply_mode (self);
}

/* ---- highlighting: Markdown and Jinja2, line by line ---- */

static void
tag_set_colour (GtkTextTag *tag, const GdkRGBA *colour)
{
  if (colour)
    g_object_set (tag, "foreground-rgba", colour, NULL);
}

static void
tag_line (NpEditor *self, int line, int start, int end, GtkTextTag *tag)
{
  GtkTextIter a, b;

  if (end <= start || start < 0 || !tag)
    return;
  gtk_text_buffer_get_iter_at_line_index (self->buffer, &a, line, start);
  gtk_text_buffer_get_iter_at_line_index (self->buffer, &b, line, end);
  gtk_text_buffer_apply_tag (self->buffer, tag, &a, &b);
}

static void
tag_pattern (NpEditor *self, int line, const char *text, GRegex *regex, GtkTextTag *tag)
{
  GMatchInfo *info = NULL;
  gint pos = 0;

  if (!regex || !tag)
    return;
  while (g_regex_match_full (regex, text, -1, pos, 0, &info, NULL))
    {
      gint start = 0, end = 0;

      if (!g_match_info_fetch_pos (info, 0, &start, &end) || end <= start)
        break;
      tag_line (self, line, start, end, tag);
      pos = end;
      g_match_info_free (info);
      info = NULL;
    }
  g_clear_pointer (&info, g_match_info_free);
}

static void
tag_colours (NpEditor *self)
{
  GdkRGBA primary, variant, tertiary, container, secondary, on_surface;

  if (!np_lookup (self->view, "neo_on_surface", &on_surface))
    return;
  np_lookup (self->view, "neo_primary", &primary);
  np_lookup (self->view, "neo_on_surface_variant", &variant);
  np_lookup (self->view, "neo_tertiary", &tertiary);
  np_lookup (self->view, "neo_surface_container_high", &container);
  np_lookup (self->view, "neo_secondary", &secondary);

  tag_set_colour (self->t_heading, &primary);
  for (guint i = 0; i < G_N_ELEMENTS (self->t_h); i++)
    tag_set_colour (self->t_h[i], &primary);
  tag_set_colour (self->t_jinja, &tertiary);
  tag_set_colour (self->t_link, &primary);
  tag_set_colour (self->t_comment, &variant);
  tag_set_colour (self->t_quote, &variant);
  tag_set_colour (self->t_mark, &secondary);
  g_object_set (self->t_code, "background-rgba", &container, NULL);
  g_object_set (self->t_chip, "background-rgba", &container, "foreground-rgba", &tertiary, NULL);
  g_object_set (self->t_hit, "background-rgba", &container, "foreground-rgba", &on_surface, NULL);
}

static gboolean
highlight_idle (gpointer data)
{
  NpEditor *self = data;

  self->highlight_id = 0;
  paint (self);
  return G_SOURCE_REMOVE;
}

static void
schedule_highlight (NpEditor *self)
{
  if (self->highlight_id)
    g_source_remove (self->highlight_id);
  self->highlight_id = g_timeout_add (200, highlight_idle, self);
}

static void
highlight_source (NpEditor *self)
{
  g_autoptr(GRegex) re_jinja = NULL, re_heading = NULL, re_strong = NULL, re_em = NULL, re_code = NULL,
                    re_link = NULL, re_comment = NULL, re_mark = NULL;
  int lines = 0;
  gboolean fence = FALSE, front = FALSE;
  gboolean is_markdown = g_strcmp0 (np_doc_kind (self->doc), "markdown") == 0;
  gboolean is_jinja = g_strcmp0 (np_doc_kind (self->doc), "jinja") == 0;

  if (!is_markdown && !is_jinja)
    return;
  re_jinja = g_regex_new ("\\{\\{.*?\\}\\}|\\{%.*?%\\}|\\{#.*?#\\}", G_REGEX_OPTIMIZE, 0, NULL);
  re_heading = g_regex_new ("^#{1,6}[ \\t]+.*$", 0, 0, NULL);
  re_strong = g_regex_new ("\\*\\*[^*\\n]+\\*\\*|__[^_\\n]+__", 0, 0, NULL);
  re_em = g_regex_new ("(?<!\\*)\\*[^*\\n]+\\*(?!\\*)|(?<!_)_[^_\\n]+_(?!_)", 0, 0, NULL);
  re_code = g_regex_new ("`[^`\\n]+`", 0, 0, NULL);
  re_link = g_regex_new ("\\[[^\\]\\n]*\\]\\([^)\\n]*\\)", 0, 0, NULL);
  re_comment = g_regex_new ("<!--.*?-->", 0, 0, NULL);
  re_mark = g_regex_new ("^[ \\t]*([-*+]|[0-9]+\\.)[ \\t]", 0, 0, NULL);

  lines = gtk_text_buffer_get_line_count (self->buffer);
  for (int i = 0; i < lines; i++)
    {
      GtkTextIter ls, le;
      g_autofree char *text = NULL;
      gboolean line_fence = FALSE;

      gtk_text_buffer_get_iter_at_line (self->buffer, &ls, i);
      le = ls;
      if (!gtk_text_iter_ends_line (&le))
        gtk_text_iter_forward_to_line_end (&le);
      text = gtk_text_buffer_get_text (self->buffer, &ls, &le, TRUE);

      if (g_str_has_prefix (text, "```") || g_str_has_prefix (text, "~~~"))
        {
          line_fence = TRUE;
          fence = !fence;
        }
      if (fence || line_fence)
        {
          tag_line (self, i, 0, strlen (text), self->t_code);
          continue;
        }
      if (i == 0 && g_strcmp0 (text, "---") == 0)
        front = TRUE;
      if (front)
        {
          tag_line (self, i, 0, strlen (text), self->t_comment);
          if (i > 0 && g_strcmp0 (text, "---") == 0)
            front = FALSE;
          continue;
        }
      if (is_markdown)
        {
          if (g_str_has_prefix (text, "#"))
            {
              tag_pattern (self, i, text, re_heading, self->t_heading);
              tag_pattern (self, i, text, re_jinja, self->t_jinja);
              continue;
            }
          tag_pattern (self, i, text, re_mark, self->t_mark);
          tag_pattern (self, i, text, re_link, self->t_link);
          tag_pattern (self, i, text, re_code, self->t_code);
          tag_pattern (self, i, text, re_strong, self->t_strong);
          tag_pattern (self, i, text, re_em, self->t_em);
          tag_pattern (self, i, text, re_comment, self->t_comment);
          if (g_str_has_prefix (text, ">"))
            tag_line (self, i, 0, strlen (text), self->t_quote);
        }
      if (is_jinja)
        tag_pattern (self, i, text, re_jinja, self->t_jinja);
    }
}

/* ---- the rendered mode: the same buffer, drawn without the Markdown markers --------------------------
 *
 * The text is never rewritten: the markers stay in the buffer and only get an invisible tag, so a save writes
 * exactly what a save writes in source mode. The line the cursor is on keeps its markers visible (markers are
 * skipped by the cursor, and a Backspace would delete a hidden one), which is what makes it editable. */

static void
tag_span (NpEditor *self, int line_a, int start_a, int line_b, int start_b, GtkTextTag *tag)
{
  GtkTextIter a, b;

  if (!tag || line_b < line_a || (line_b == line_a && start_b <= start_a) || start_a < 0)
    return;
  gtk_text_buffer_get_iter_at_line_index (self->buffer, &a, line_a, start_a);
  gtk_text_buffer_get_iter_at_line_index (self->buffer, &b, line_b, start_b);
  gtk_text_buffer_apply_tag (self->buffer, tag, &a, &b);
}

/* a fence line with its newline: hidden, it takes no room, so a code block starts where the fence was */
static void
hide_line (NpEditor *self, int line, int lines, int length)
{
  if (line + 1 < lines)
    tag_span (self, line, 0, line + 1, 0, self->t_hidden);
  else
    tag_line (self, line, 0, length, self->t_hidden);
}

/* a run that wraps its text in markers (`**bold**`, `*italic*`, ``code``): the text is tagged, the markers hidden */
static void
wrap_matches (NpEditor *self, int line, const char *text, GRegex *regex, GtkTextTag *tag, gboolean hide,
              int prefix, int suffix)
{
  GMatchInfo *info = NULL;
  gint pos = 0;

  if (!regex || !tag)
    return;
  while (g_regex_match_full (regex, text, -1, pos, 0, &info, NULL))
    {
      gint start = 0, end = 0;

      if (!g_match_info_fetch_pos (info, 0, &start, &end) || end <= start)
        break;
      tag_line (self, line, start, end, tag);
      if (hide && end - start > prefix + suffix)
        {
          tag_line (self, line, start, start + prefix, self->t_hidden);
          tag_line (self, line, end - suffix, end, self->t_hidden);
        }
      pos = end;
      g_match_info_free (info);
      info = NULL;
    }
  g_clear_pointer (&info, g_match_info_free);
}

/* `[the text](the url)`: the text stays, underlined; the brackets and the url go */
static void
link_matches (NpEditor *self, int line, const char *text, GRegex *regex, gboolean hide)
{
  GMatchInfo *info = NULL;
  gint pos = 0;

  if (!regex)
    return;
  while (g_regex_match_full (regex, text, -1, pos, 0, &info, NULL))
    {
      gint start = 0, end = 0;
      const char *close = NULL;
      int suffix_at = 0;

      if (!g_match_info_fetch_pos (info, 0, &start, &end) || end <= start)
        break;
      close = strchr (text + start, ']');
      suffix_at = close && (int) (close - text) < end ? (int) (close - text) : start + 1;
      tag_line (self, line, start, suffix_at, self->t_link);
      if (hide)
        {
          tag_line (self, line, start, start + 1, self->t_hidden);
          tag_line (self, line, suffix_at, end, self->t_hidden);
        }
      pos = end;
      g_match_info_free (info);
      info = NULL;
    }
  g_clear_pointer (&info, g_match_info_free);
}

static void
render (NpEditor *self)
{
  g_autoptr(GRegex) re_jinja = NULL, re_heading = NULL, re_strong = NULL, re_em = NULL, re_code = NULL,
                    re_link = NULL, re_comment = NULL, re_quote = NULL, re_list = NULL;
  int lines = gtk_text_buffer_get_line_count (self->buffer);
  gboolean fence = FALSE, front = FALSE;

  re_jinja = g_regex_new ("\\{\\{.*?\\}\\}|\\{%.*?%\\}|\\{#.*?#\\}", G_REGEX_OPTIMIZE, 0, NULL);
  re_heading = g_regex_new ("^(#{1,6})[ \\t]+", 0, 0, NULL);
  re_strong = g_regex_new ("\\*\\*[^*\\n]+\\*\\*|__[^_\\n]+__", 0, 0, NULL);
  re_em = g_regex_new ("(?<!\\*)\\*[^*\\n]+\\*(?!\\*)|(?<!_)_[^_\\n]+_(?!_)", 0, 0, NULL);
  re_code = g_regex_new ("`[^`\\n]+`", 0, 0, NULL);
  re_link = g_regex_new ("\\[[^\\]\\n]*\\]\\([^)\\n]*\\)", 0, 0, NULL);
  re_comment = g_regex_new ("<!--.*?-->", 0, 0, NULL);
  re_quote = g_regex_new ("^>[ \\t]?", 0, 0, NULL);
  re_list = g_regex_new ("^[ \\t]*([-*+]|[0-9]+\\.)[ \\t]+", 0, 0, NULL);

  for (int i = 0; i < lines; i++)
    {
      GtkTextIter ls, le;
      g_autofree char *text = NULL;
      gboolean line_fence = FALSE;
      gboolean reveal = i == self->cursor_line;   /* the line being edited shows what it is made of */
      GMatchInfo *info = NULL;
      gint s = 0, e = 0;

      gtk_text_buffer_get_iter_at_line (self->buffer, &ls, i);
      le = ls;
      if (!gtk_text_iter_ends_line (&le))
        gtk_text_iter_forward_to_line_end (&le);
      text = gtk_text_buffer_get_text (self->buffer, &ls, &le, TRUE);

      if (g_str_has_prefix (text, "```") || g_str_has_prefix (text, "~~~"))
        {
          line_fence = TRUE;
          fence = !fence;
          if (!reveal)
            {
              hide_line (self, i, lines, strlen (text));
              continue;
            }
        }
      if (fence || line_fence)
        {
          tag_line (self, i, 0, strlen (text), self->t_code);
          continue;
        }
      if (i == 0 && g_strcmp0 (text, "---") == 0)
        front = TRUE;
      if (front)
        {
          tag_line (self, i, 0, strlen (text), self->t_comment);
          if (i > 0 && g_strcmp0 (text, "---") == 0)
            front = FALSE;
          continue;
        }
      if (g_regex_match (re_heading, text, 0, &info))
        {
          g_autofree char *hashes = g_match_info_fetch (info, 1);
          int level = CLAMP ((int) (hashes ? strlen (hashes) : 1), 1, 6);

          g_match_info_fetch_pos (info, 0, &s, &e);
          tag_line (self, i, e, strlen (text), self->t_h[level - 1]);
          if (!reveal)
            tag_line (self, i, s, e, self->t_hidden);
          g_match_info_free (info);
          info = NULL;
        }
      if (g_regex_match (re_quote, text, 0, &info))
        {
          g_match_info_fetch_pos (info, 0, &s, &e);
          tag_line (self, i, 0, strlen (text), self->t_quote);
          tag_line (self, i, 0, strlen (text), self->t_quote_indent);
          if (!reveal)
            tag_line (self, i, s, e, self->t_hidden);
          g_match_info_free (info);
          info = NULL;
        }
      if (g_regex_match (re_list, text, 0, &info))
        {
          g_match_info_fetch_pos (info, 0, &s, &e);
          tag_line (self, i, 0, strlen (text), self->t_list_indent);
          tag_line (self, i, s, e, self->t_mark);   /* the marker stays: it is the bullet */
          g_match_info_free (info);
          info = NULL;
        }
      link_matches (self, i, text, re_link, !reveal);
      wrap_matches (self, i, text, re_code, self->t_code, !reveal, 1, 1);
      wrap_matches (self, i, text, re_strong, self->t_strong, !reveal, 2, 2);
      wrap_matches (self, i, text, re_em, self->t_em, !reveal, 1, 1);
      tag_pattern (self, i, text, re_comment, self->t_comment);
      /* last, over whatever the line already is: a Jinja token is a chip, and it is never altered */
      wrap_matches (self, i, text, re_jinja, self->t_chip, FALSE, 0, 0);
    }
}

static void
paint (NpEditor *self)
{
  GtkTextIter start, end;

  if (!self->doc || !self->buffer)
    return;
  tag_colours (self);
  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  gtk_text_buffer_remove_all_tags (self->buffer, &start, &end);
  if (np_editor_rendered (self))
    render (self);
  else
    highlight_source (self);
}

/* ---- the index of the headings ---- */

typedef struct {
  NpEditor *self;
  int       line;
} IndexHit;

static void
index_picked (GtkWidget *row, gpointer data)
{
  IndexHit *hit = data;
  GtkTextIter it;

  gtk_text_buffer_get_iter_at_line (hit->self->buffer, &it, hit->line);
  gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (hit->self->view), &it, 0.2, TRUE, 0, 0);
  gtk_popover_popdown (GTK_POPOVER (hit->self->index_popover));
}

static void
index_fill (NpEditor *self)
{
  GtkWidget *list = gtk_list_box_new ();
  GtkTextIter it;
  int found = 0;

  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_list_box_set_show_separators (GTK_LIST_BOX (list), FALSE);
  gtk_widget_add_css_class (list, "np-index");

  gtk_text_buffer_get_start_iter (self->buffer, &it);
  while (!gtk_text_iter_is_end (&it))
    {
      int line = gtk_text_iter_get_line (&it);
      GtkTextIter le = it;
      g_autofree char *text = NULL;

      if (!gtk_text_iter_ends_line (&le))
        gtk_text_iter_forward_to_line_end (&le);
      text = gtk_text_buffer_get_text (self->buffer, &it, &le, TRUE);
      if (g_str_has_prefix (text, "#"))
        {
          int level = 0;
          const char *title = text;
          GtkWidget *row = NULL;

          while (*title == '#')
            {
              level++;
              title++;
            }
          while (*title == ' ')
            title++;
          row = np_row (*title ? title : "(senza titolo)", NULL, "np-index-row");
          gtk_widget_set_margin_start (row, 12 + (level - 1) * 16);
          IndexHit *hit = g_new0 (IndexHit, 1);

          hit->self = self;
          hit->line = line;
          g_object_set_data_full (G_OBJECT (row), "np-index-hit", hit, g_free);
          g_signal_connect (row, "clicked", G_CALLBACK (index_picked), hit);
          gtk_list_box_append (GTK_LIST_BOX (list), row);
          found++;
        }
      if (!gtk_text_iter_forward_line (&it))
        break;
    }
  if (!found)
    {
      GtkWidget *row = np_row (np_ui (self->store, "editor.no_headings"), NULL, "np-index-row");

      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }
  gtk_popover_set_child (GTK_POPOVER (self->index_popover), np_scroller (list));
}

void
np_editor_index (NpEditor *self, GtkWidget *anchor)
{
  if (!self->doc)
    return;
  index_fill (self);
  if (anchor && gtk_widget_get_parent (self->index_popover) != anchor)
    {
      if (gtk_widget_get_parent (self->index_popover))
        gtk_widget_unparent (self->index_popover);
      gtk_widget_set_parent (self->index_popover, anchor);
    }
  gtk_popover_popup (GTK_POPOVER (self->index_popover));
}

/* ---- the search ---- */

static void
scroll_to_hit (NpEditor *self, guint index)
{
  HitPos *hit = NULL;
  GtkTextIter it;

  if (!self->hit_pos || !self->hit_pos->len)
    return;
  self->hit_index = index % self->hit_pos->len;
  hit = &g_array_index (self->hit_pos, HitPos, self->hit_index);
  gtk_text_buffer_get_iter_at_line_index (self->buffer, &it, hit->line, hit->start);
  gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (self->view), &it, 0.15, TRUE, 0, 0);
  {
    g_autofree char *label = g_strdup_printf ("%u/%u", self->hit_index + 1, self->hit_pos->len);

    gtk_label_set_text (GTK_LABEL (self->hits), label);
  }
}

static void
hit_move (GtkWidget *button, gpointer data)
{
  NpEditor *self = data;
  int step = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "np-step"));

  if (!self->hit_pos || !self->hit_pos->len)
    return;
  scroll_to_hit (self, (self->hit_index + self->hit_pos->len + step) % self->hit_pos->len);
}

static void
search_hits (NpEditor *self)
{
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  g_autoptr(GRegex) regex = NULL;
  GtkTextIter start, end;
  int lines = 0, count = 0;
  g_autofree char *pattern = NULL;

  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  gtk_text_buffer_remove_tag_by_name (self->buffer, "np-hit", &start, &end);
  g_clear_pointer (&self->hit_pos, g_array_unref);
  self->hit_index = 0;
  if (!needle || !*needle)
    {
      gtk_label_set_text (GTK_LABEL (self->hits), "");
      return;
    }
  self->hit_pos = g_array_new (FALSE, FALSE, sizeof (HitPos));
  pattern = g_regex_escape_string (needle, -1);
  regex = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);
  lines = gtk_text_buffer_get_line_count (self->buffer);
  for (int i = 0; i < lines; i++)
    {
      GtkTextIter ls, le;
      g_autofree char *text = NULL;
      GMatchInfo *info = NULL;
      gint pos = 0;

      gtk_text_buffer_get_iter_at_line (self->buffer, &ls, i);
      le = ls;
      if (!gtk_text_iter_ends_line (&le))
        gtk_text_iter_forward_to_line_end (&le);
      text = gtk_text_buffer_get_text (self->buffer, &ls, &le, TRUE);
      while (g_regex_match_full (regex, text, -1, pos, 0, &info, NULL))
        {
          gint s = 0, e = 0;
          HitPos hit = { i, 0 };

          if (!g_match_info_fetch_pos (info, 0, &s, &e) || e <= s)
            break;
          tag_line (self, i, s, e, self->t_hit);
          hit.start = s;
          g_array_append_val (self->hit_pos, hit);
          count++;
          pos = e;
          g_match_info_free (info);
          info = NULL;
        }
      g_clear_pointer (&info, g_match_info_free);
    }
  {
    g_autofree char *label = g_strdup_printf ("%d", count);

    gtk_label_set_text (GTK_LABEL (self->hits), label);
  }
  if (count)
    scroll_to_hit (self, 0);
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer data)
{
  search_hits (data);
}

static void
on_search_stop (GtkSearchEntry *entry, gpointer data)
{
  NpEditor *self = data;

  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->searchbar), FALSE);
}

gboolean
np_editor_search_open (NpEditor *self)
{
  return gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (self->searchbar));
}

void
np_editor_search (NpEditor *self)
{
  gboolean on = !gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (self->searchbar));

  gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->searchbar), on);
  if (on)
    gtk_widget_grab_focus (self->entry);
  else
    {
      GtkTextIter start, end;

      gtk_text_buffer_get_bounds (self->buffer, &start, &end);
      gtk_text_buffer_remove_tag_by_name (self->buffer, "np-hit", &start, &end);
    }
}

/* ---- the draft ---- */

static gboolean
draft_write (gpointer data)
{
  NpEditor *self = data;
  g_autofree char *text = NULL;

  self->draft_id = 0;
  if (!self->doc || !gtk_text_buffer_get_modified (self->buffer))
    return G_SOURCE_REMOVE;
  text = np_editor_text (self);
  np_draft_write (self->store, self->doc, text);
  return G_SOURCE_REMOVE;
}

static void
schedule_draft (NpEditor *self)
{
  if (self->draft_id)
    g_source_remove (self->draft_id);
  self->draft_id = g_timeout_add_seconds (2, draft_write, self);
}

static void
on_modified_changed (GtkTextBuffer *buffer, gpointer data)
{
  NpEditor *self = data;

  if (self->loading)
    return;
  if (gtk_text_buffer_get_modified (buffer))
    schedule_draft (self);
  if (self->changed_cb)
    self->changed_cb (self->changed_data);
}

/* rendered mode leaves the markers of the line being edited visible: the cursor skips hidden text, so this is what
 * keeps a Backspace from eating a `**` it never showed */
static void
on_cursor_moved (GtkTextBuffer *buffer, GParamSpec *pspec, gpointer data)
{
  NpEditor *self = data;
  GtkTextIter it;
  int line = 0;

  if (!np_editor_rendered (self) || !self->doc)
    return;
  gtk_text_buffer_get_iter_at_mark (buffer, &it, gtk_text_buffer_get_insert (buffer));
  line = gtk_text_iter_get_line (&it);
  if (line == self->cursor_line)
    return;
  self->cursor_line = line;
  schedule_highlight (self);
}

static void
on_changed (GtkTextBuffer *buffer, gpointer data)
{
  NpEditor *self = data;

  if (!self->loading)
    schedule_highlight (self);
}

/* ---- the dialogs of a save ---- */

static void
conflict_answered (NeoDialog *dialog, int response, gpointer data)
{
  NpEditor *self = data;

  if (response == NEO_RESPONSE_OK)
    {
      self->overwrite = TRUE;
      self->saving = FALSE;
      np_editor_save_then (self, self->save_done, self->save_done_data);
    }
  else if (response == 2)
    {
      np_editor_reload (self);
      save_finished (self, FALSE);
    }
  else
    {
      self->saving = FALSE;
      save_finished (self, FALSE);
    }
}

static void
ask_conflict (NpEditor *self)
{
  NeoDialog *dialog = neo_dialog_new (np_ui (self->store, "save.conflict_title"));

  neo_dialog_set_message (dialog, np_ui (self->store, "save.conflict_body"));
  neo_dialog_add_button (dialog, np_ui (self->store, "save.cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, np_ui (self->store, "save.reload"), 2);
  neo_dialog_add_button (dialog, np_ui (self->store, "save.overwrite"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (conflict_answered), self);
  neo_dialog_present (dialog, GTK_WIDGET (self->window));
}

/* ---- the save ---- */

typedef struct {
  NpEditor *self;
  char     *text;
} Step;

/* the save is over, either way: whoever asked for it learns whether the text is safe (once) */
static void
save_finished (NpEditor *self, gboolean ok)
{
  void (*done) (gboolean, gpointer) = self->save_done;
  gpointer data = self->save_done_data;

  self->save_done = NULL;
  self->save_done_data = NULL;
  if (done)
    done (ok, data);
}

static void
finish_save (NpEditor *self, const char *text)
{
  self->saving = FALSE;
  self->overwrite = FALSE;
  g_free (self->loaded_hash);
  self->loaded_hash = np_checksum (text);
  if (self->doc)
    np_draft_drop (self->store, self->doc);
  gtk_text_buffer_set_modified (self->buffer, FALSE);
  np_msg_set (self->error, np_ui (self->store, "editor.saved"), FALSE);
  snack (self, np_ui (self->store, "editor.saved"));
  if (self->saved_cb)
    self->saved_cb (self->saved_data);
  save_finished (self, TRUE);
}

static void
write_answered (NpAns *ans, gpointer data)
{
  Step *step = data;

  if (!ans->ok)
    {
      step->self->saving = FALSE;
      np_msg_set (step->self->error, ans->error ? ans->error : "?", TRUE);
      save_finished (step->self, FALSE);
    }
  else
    finish_save (step->self, step->text);
  g_free (step->text);
  g_free (step);
}

static void
write_it (NpEditor *self, const char *text, const char *fresh)
{
  Step *step = g_new0 (Step, 1);

  step->self = self;
  step->text = g_strdup (text);
  if (self->doc->history && fresh)
    {
      g_autofree char *why = NULL;

      if (!np_history_push (self->store, self->doc, fresh, &why) && why)
        np_msg_set (self->error, why, TRUE);
    }
  if (!*text && self->doc->empty_argv)
    np_run (self->store, (const char *const *) self->doc->empty_argv, NULL, G_OBJECT (self->window), NULL,
            write_answered, step);
  else if (self->doc->save_argv)
    np_run (self->store, (const char *const *) self->doc->save_argv, text, G_OBJECT (self->window), NULL,
            write_answered, step);
  else if (self->doc->path)
    {
      g_autoptr(GError) error = NULL;
      g_autofree char *dir = g_path_get_dirname (self->doc->path);

      g_mkdir_with_parents (dir, 0700);
      if (!g_file_set_contents (self->doc->path, text, -1, &error))
        {
          self->saving = FALSE;
          np_msg_set (self->error, error->message, TRUE);
          save_finished (self, FALSE);
          g_free (step->text);
          g_free (step);
          return;
        }
      finish_save (self, text);
      g_free (step->text);
      g_free (step);
    }
  else
    {
      self->saving = FALSE;
      np_msg_set (self->error, "nessuna destinazione", TRUE);
      save_finished (self, FALSE);
      g_free (step->text);
      g_free (step);
    }
}

static void
fresh_answered (NpAns *ans, gpointer data)
{
  Step *step = data;
  NpEditor *self = step->self;
  g_autofree char *fresh_sum = NULL;
  gboolean changed = TRUE;

  if (!ans->ok)
    {
      /* nothing read (a file not made yet): no text to compare against, the save says why if it fails */
      write_it (self, step->text, "");
      g_free (step->text);
      g_free (step);
      return;
    }
  fresh_sum = np_checksum (ans->text ? ans->text : "");
  changed = g_strcmp0 (fresh_sum, self->loaded_hash) != 0;
  if (changed && !self->overwrite)
    {
      g_free (step->text);
      g_free (step);
      ask_conflict (self);
      return;
    }
  write_it (self, step->text, ans->text ? ans->text : "");
  g_free (step->text);
  g_free (step);
}

static void
validate_answered (NpAns *ans, gpointer data)
{
  Step *step = data;
  NpEditor *self = step->self;

  if (!ans->ok)
    {
      self->saving = FALSE;
      np_msg_set (self->error, ans->error ? ans->error : "?", TRUE);
      save_finished (self, FALSE);
      g_free (step->text);
      g_free (step);
      return;
    }
  np_doc_load (self->store, self->doc, G_OBJECT (self->window), NULL, fresh_answered, step);
}

void
np_editor_save_then (NpEditor *self, void (*done) (gboolean, gpointer), gpointer data)
{
  if (!self->doc || !self->doc->editable)
    {
      if (done)
        done (FALSE, data);
      return;
    }
  if (self->saving)
    return;
  self->save_done = done;
  self->save_done_data = data;
  save_now (self);
}

void
np_editor_save (NpEditor *self)
{
  np_editor_save_then (self, NULL, NULL);
}

static void
save_now (NpEditor *self)
{
  g_autofree char *text = NULL;
  Step *step = NULL;

  if (!gtk_text_buffer_get_modified (self->buffer))
    {
      snack (self, np_ui (self->store, "editor.unchanged"));
      save_finished (self, TRUE);
      return;
    }
  text = np_editor_text (self);
  if (g_strcmp0 (np_doc_kind (self->doc), "json") == 0)
    {
      g_autoptr(JsonParser) parser = json_parser_new ();
      g_autoptr(GError) error = NULL;

      if (!json_parser_load_from_data (parser, text, -1, &error))
        {
          np_msg_set (self->error, error->message, TRUE);
          save_finished (self, FALSE);
          return;
        }
    }
  self->saving = TRUE;
  np_msg_set (self->error, "", FALSE);
  step = g_new0 (Step, 1);
  step->self = self;
  step->text = g_strdup (text);
  if (self->doc->validate_argv)
    np_run (self->store, (const char *const *) self->doc->validate_argv, text, G_OBJECT (self->window), NULL,
            validate_answered, step);
  else
    np_doc_load (self->store, self->doc, G_OBJECT (self->window), NULL, fresh_answered, step);
}

/* ---- open ---- */

typedef struct {
  NpEditor *self;
  guint     token;
} Open;

static void
draft_answered (NeoDialog *dialog, int response, gpointer data)
{
  Step *step = data;

  if (response == NEO_RESPONSE_OK)
    {
      np_editor_put_text (step->self, step->text);
      np_msg_set (step->self->error, np_ui (step->self->store, "editor.draft_restored"), FALSE);
    }
  else
    np_draft_drop (step->self->store, step->self->doc);
  g_free (step->text);
  g_free (step);
}

static void
offer_draft (NpEditor *self, const char *loaded)
{
  g_autofree char *path = np_draft_path (self->store, self->doc);
  char *draft = NULL;

  if (!g_file_get_contents (path, &draft, NULL, NULL))
    return;
  if (g_strcmp0 (draft, loaded) == 0)
    {
      g_free (draft);
      return;
    }
  {
    NeoDialog *dialog = neo_dialog_new (np_ui (self->store, "editor.draft_title"));
    Step *step = g_new0 (Step, 1);

    step->self = self;
    step->text = draft;   /* the callback frees it */
    neo_dialog_set_message (dialog, np_ui (self->store, "editor.draft_body"));
    neo_dialog_add_button (dialog, np_ui (self->store, "editor.draft_discard"), NEO_RESPONSE_CANCEL);
    neo_dialog_add_button (dialog, np_ui (self->store, "editor.draft_restore"), NEO_RESPONSE_OK);
    g_signal_connect (dialog, "response", G_CALLBACK (draft_answered), step);
    neo_dialog_present (dialog, GTK_WIDGET (self->window));
  }
}

static void
open_answered (NpAns *ans, gpointer data)
{
  Open *open = data;
  NpEditor *self = open->self;

  if (open->token != self->token || !self->doc)
    {
      g_free (open);
      return;
    }
  if (!ans->ok)
    {
      put_text (self, "", FALSE);
      np_msg_set (self->error, ans->error ? ans->error : "?", TRUE);
    }
  else
    {
      put_text (self, ans->text, FALSE);
      g_free (self->loaded_hash);
      self->loaded_hash = np_checksum (ans->text ? ans->text : "");
      np_msg_set (self->error, "", FALSE);
      offer_draft (self, ans->text ? ans->text : "");
    }
  paint (self);
  {
    GtkTextIter start;

    gtk_text_buffer_get_start_iter (self->buffer, &start);
    gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (self->view), &start, 0, FALSE, 0, 0);
  }
  if (self->changed_cb)
    self->changed_cb (self->changed_data);
  g_free (open);
}

void
np_editor_open (NpEditor *self, NpDoc *doc)
{
  Open *open = NULL;

  if (self->draft_id)
    {
      g_source_remove (self->draft_id);
      self->draft_id = 0;
    }
  if (self->highlight_id)
    {
      g_source_remove (self->highlight_id);
      self->highlight_id = 0;
    }
  np_doc_free (self->doc);
  self->doc = doc;
  self->saving = FALSE;
  self->overwrite = FALSE;
  self->save_done = NULL;
  self->save_done_data = NULL;
  self->cursor_line = -1;
  g_free (self->mode);
  self->mode = np_mode_for (self->store, np_doc_kind (doc));
  apply_mode (self);
  gtk_text_buffer_set_modified (self->buffer, FALSE);
  self->token++;
  open = g_new0 (Open, 1);
  open->self = self;
  open->token = self->token;
  np_msg_set (self->error, "", FALSE);
  np_doc_load (self->store, doc, G_OBJECT (self->window), NULL, open_answered, open);
}

void
np_editor_reload (NpEditor *self)
{
  if (!self->doc)
    return;
  self->saving = FALSE;
  self->overwrite = FALSE;
  np_editor_open (self, np_doc_by_id (self->store, self->doc->id, self->doc->person));
}

void
np_editor_discard (NpEditor *self)
{
  if (!self->doc)
    return;
  np_draft_drop (self->store, self->doc);
  np_editor_reload (self);
}

static gboolean
on_map (GtkWidget *widget, gpointer data)
{
  paint (data);
  return FALSE;
}

/* a tag keeps the colour it was given: when the look changes, the colours are worked out again and the text redrawn */
static void
on_look_changed (gpointer data)
{
  paint (data);
}

/* ---- construction ---- */

NpEditor *
np_editor_new (NpStore *store, GtkWindow *window, GtkWidget *snackbar)
{
  NpEditor *self = g_new0 (NpEditor, 1);
  GtkWidget *box, *scroll, *search_box, *prev, *next;

  self->store = store;
  self->window = window;
  self->snackbar = snackbar;
  self->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->view = gtk_text_view_new ();
  self->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->view), 16);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->view), 16);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (self->view), 12);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->view), 32);
  gtk_widget_add_css_class (self->view, "np-editor");
  scroll = np_scroller (self->view);

  self->searchbar = gtk_search_bar_new ();
  self->entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->entry, TRUE);
  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->searchbar), GTK_EDITABLE (self->entry));
  gtk_search_bar_set_show_close_button (GTK_SEARCH_BAR (self->searchbar), TRUE);
  search_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  self->hits = np_label ("", "np-hits");
  prev = neo_icon_button_new (np_ui (store, "icons.hit_prev"));
  next = neo_icon_button_new (np_ui (store, "icons.hit_next"));
  g_object_set_data (G_OBJECT (prev), "np-step", GINT_TO_POINTER (-1));
  g_object_set_data (G_OBJECT (next), "np-step", GINT_TO_POINTER (1));
  g_signal_connect (prev, "clicked", G_CALLBACK (hit_move), self);
  g_signal_connect (next, "clicked", G_CALLBACK (hit_move), self);
  gtk_box_append (GTK_BOX (search_box), self->entry);
  gtk_box_append (GTK_BOX (search_box), self->hits);
  gtk_box_append (GTK_BOX (search_box), prev);
  gtk_box_append (GTK_BOX (search_box), next);
  gtk_search_bar_set_child (GTK_SEARCH_BAR (self->searchbar), search_box);
  g_signal_connect (self->entry, "search-changed", G_CALLBACK (on_search_changed), self);
  g_signal_connect (self->entry, "stop-search", G_CALLBACK (on_search_stop), self);

  self->error = np_msg ();
  gtk_widget_set_margin_start (self->error, 16);
  gtk_widget_set_margin_end (self->error, 16);
  gtk_widget_set_margin_top (self->error, 8);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (box), self->searchbar);
  gtk_box_append (GTK_BOX (box), self->error);
  gtk_box_append (GTK_BOX (box), scroll);
  gtk_box_append (GTK_BOX (self->root), box);

  self->t_heading = gtk_text_buffer_create_tag (self->buffer, "np-heading", "weight", PANGO_WEIGHT_BOLD, NULL);
  self->t_jinja = gtk_text_buffer_create_tag (self->buffer, "np-jinja", "weight", PANGO_WEIGHT_BOLD, NULL);
  self->t_code = gtk_text_buffer_create_tag (self->buffer, "np-code", "family", "monospace", NULL);
  self->t_strong = gtk_text_buffer_create_tag (self->buffer, "np-strong", "weight", PANGO_WEIGHT_BOLD, NULL);
  self->t_em = gtk_text_buffer_create_tag (self->buffer, "np-em", "style", PANGO_STYLE_ITALIC, NULL);
  self->t_link = gtk_text_buffer_create_tag (self->buffer, "np-link", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  self->t_comment = gtk_text_buffer_create_tag (self->buffer, "np-comment", "style", PANGO_STYLE_ITALIC, NULL);
  self->t_quote = gtk_text_buffer_create_tag (self->buffer, "np-quote", "style", PANGO_STYLE_ITALIC, NULL);
  self->t_mark = gtk_text_buffer_create_tag (self->buffer, "np-mark", "weight", PANGO_WEIGHT_BOLD, NULL);
  self->t_hit = gtk_text_buffer_create_tag (self->buffer, "np-hit", NULL);

  /* the rendered mode's tags: what it hides, and how it draws what it keeps */
  self->t_hidden = gtk_text_buffer_create_tag (self->buffer, "np-hidden", "invisible", TRUE, NULL);
  self->t_chip = gtk_text_buffer_create_tag (self->buffer, "np-chip", "family", "monospace",
                                             "weight", PANGO_WEIGHT_BOLD,
                                             "scale", self->store->conf.render.chip_scale, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (self->t_h); i++)
    {
      g_autofree char *name = g_strdup_printf ("np-h%u", i + 1);

      self->t_h[i] = gtk_text_buffer_create_tag (self->buffer, name,
                                                 "scale", self->store->conf.render.heading_scale[i],
                                                 "weight", PANGO_WEIGHT_BOLD,
                                                 "pixels-above-lines", self->store->conf.render.heading_above,
                                                 "pixels-below-lines", self->store->conf.render.heading_below,
                                                 NULL);
    }
  self->t_list_indent = gtk_text_buffer_create_tag (self->buffer, "np-list-indent",
                                                    "left-margin", self->store->conf.render.list_indent, NULL);
  self->t_quote_indent = gtk_text_buffer_create_tag (self->buffer, "np-quote-indent",
                                                     "left-margin", self->store->conf.render.quote_indent, NULL);

  self->mode = g_strdup ("source");
  self->cursor_line = -1;
  neo_on_look_changed (on_look_changed, self);

  self->index_popover = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (self->index_popover), FALSE);
  gtk_widget_add_css_class (self->index_popover, "np-index-popover");

  g_signal_connect (self->buffer, "changed", G_CALLBACK (on_changed), self);
  g_signal_connect (self->buffer, "modified-changed", G_CALLBACK (on_modified_changed), self);
  g_signal_connect (self->buffer, "notify::cursor-position", G_CALLBACK (on_cursor_moved), self);
  g_signal_connect (self->view, "map", G_CALLBACK (on_map), self);
  return self;
}

GtkWidget *
np_editor_root (NpEditor *self)
{
  return self->root;
}

void
np_editor_set_saved_cb (NpEditor *self, void (*cb) (gpointer), gpointer data)
{
  self->saved_cb = cb;
  self->saved_data = data;
}

void
np_editor_set_changed_cb (NpEditor *self, void (*cb) (gpointer), gpointer data)
{
  self->changed_cb = cb;
  self->changed_data = data;
}
