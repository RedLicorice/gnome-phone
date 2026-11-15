/* Neo Prompt's line diff: see np-diff.h. */
#include "np-diff.h"

#define NP_DIFF_MAX_CELLS 4000000.0

static void
line_free (gpointer item)
{
  NpDiffLine *line = item;

  g_free (line->text);
  g_free (line);
}

void
np_diff_free (GPtrArray *lines)
{
  if (lines)
    g_ptr_array_unref (lines);
}

static void
add (GPtrArray *out, NpDiffKind kind, const char *text)
{
  NpDiffLine *line = g_new0 (NpDiffLine, 1);

  line->kind = kind;
  line->text = g_strdup (text);
  g_ptr_array_add (out, line);
}

GPtrArray *
np_diff_lines (const char *old_text, const char *new_text)
{
  g_auto(GStrv) old_lines = g_strsplit (old_text ? old_text : "", "\n", -1);
  g_auto(GStrv) new_lines = g_strsplit (new_text ? new_text : "", "\n", -1);
  guint n = g_strv_length (old_lines);
  guint m = g_strv_length (new_lines);
  guint head = 0, tail = 0;
  GPtrArray *out = g_ptr_array_new_with_free_func (line_free);

  while (head < n && head < m && g_strcmp0 (old_lines[head], new_lines[head]) == 0)
    head++;
  while (tail < n - head && tail < m - head
         && g_strcmp0 (old_lines[n - 1 - tail], new_lines[m - 1 - tail]) == 0)
    tail++;

  for (guint i = 0; i < head; i++)
    add (out, NP_DIFF_SAME, old_lines[i]);

  guint a = n - head - tail;    /* the changed middle of the old text */
  guint b = m - head - tail;    /* and of the new one */
  if ((double) (a + 1) * (double) (b + 1) > NP_DIFF_MAX_CELLS)
    {
      np_diff_free (out);
      return NULL;
    }
  if (a > 0 && b > 0)
    {
      /* lcs[i][j]: the common lines of old[head+i..] and new[head+j..] */
      guint *lcs = g_new0 (guint, (size_t) (a + 1) * (b + 1));
      #define CELL(i, j) lcs[(size_t) (i) * (b + 1) + (j)]

      for (guint i = a; i-- > 0; )
        for (guint j = b; j-- > 0; )
          CELL (i, j) = g_strcmp0 (old_lines[head + i], new_lines[head + j]) == 0
                        ? CELL (i + 1, j + 1) + 1
                        : MAX (CELL (i + 1, j), CELL (i, j + 1));
      guint i = 0, j = 0;

      while (i < a && j < b)
        {
          if (g_strcmp0 (old_lines[head + i], new_lines[head + j]) == 0)
            {
              add (out, NP_DIFF_SAME, old_lines[head + i]);
              i++;
              j++;
            }
          else if (CELL (i + 1, j) >= CELL (i, j + 1))
            add (out, NP_DIFF_DEL, old_lines[head + i++]);
          else
            add (out, NP_DIFF_ADD, new_lines[head + j++]);
        }
      while (i < a)
        add (out, NP_DIFF_DEL, old_lines[head + i++]);
      while (j < b)
        add (out, NP_DIFF_ADD, new_lines[head + j++]);
      g_free (lcs);
      #undef CELL
    }
  else
    {
      for (guint i = 0; i < a; i++)
        add (out, NP_DIFF_DEL, old_lines[head + i]);
      for (guint j = 0; j < b; j++)
        add (out, NP_DIFF_ADD, new_lines[head + j]);
    }
  for (guint i = n - tail; i < n; i++)
    add (out, NP_DIFF_SAME, old_lines[i]);
  return out;
}
