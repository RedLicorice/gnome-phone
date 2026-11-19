/* A message's Markdown as Pango markup. md4c parses; this only maps its blocks and spans to the few tags a GtkLabel
   draws. A chat bubble is not a page: a paragraph break is a blank line, a list item a bullet line, a heading strong
   text, a code block monospace; images show their alt text and raw HTML shows as typed (MD_FLAG_NOHTML). Every
   piece of text is escaped, so the result is always valid markup. */

#include "msg-markdown.h"

#include <md4c.h>

/* the dialect: CommonMark, plus what chat writers use (~~struck~~, - [x] tasks, bare www. and https:// links); a
   single line break stays a line break, as the writer typed it */
#define DIALECT (MD_FLAG_NOHTML | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS | \
                 MD_FLAG_HARD_SOFT_BREAKS)

typedef struct {
  GString *out;
  const char *strong; /* the opening tag of strong text */
  guint breaks;     /* the line breaks owed before the next block */
  GArray *numbers;  /* per open list: the next item's number, 0 for a bullet list */
  gboolean bullet;  /* an item's bullet was just written: its first paragraph goes on the same line */
} Render;

static void
owe (Render *render, guint breaks)
{
  render->breaks = MAX (render->breaks, breaks);
}

/* before a block's first text: the breaks owed, never at the start */
static void
pay (Render *render)
{
  if (render->out->len)
    for (guint i = 0; i < render->breaks; i++)
      g_string_append_c (render->out, '\n');
  render->breaks = 0;
  render->bullet = FALSE;
}

static void
escaped (Render *render, const char *text, gsize size)
{
  g_autofree char *safe = g_markup_escape_text (text, size);

  g_string_append (render->out, safe);
}

static int
enter_block (MD_BLOCKTYPE type, void *detail, void *data)
{
  Render *render = data;
  guint number;

  switch (type)
    {
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      number = type == MD_BLOCK_OL ? ((MD_BLOCK_OL_DETAIL *) detail)->start : 0;
      g_array_append_val (render->numbers, number);
      owe (render, render->numbers->len > 1 ? 1 : 2);
      break;
    case MD_BLOCK_LI:
      {
        MD_BLOCK_LI_DETAIL *item = detail;
        guint *next = &g_array_index (render->numbers, guint, render->numbers->len - 1);

        owe (render, 1);
        pay (render);
        for (guint depth = 1; depth < render->numbers->len; depth++)
          g_string_append (render->out, "    ");
        if (item->is_task)
          g_string_append (render->out, item->task_mark == ' ' ? "☐ " : "☑ ");
        else if (*next)
          g_string_append_printf (render->out, "%u. ", (*next)++);
        else
          g_string_append (render->out, "• ");
        render->bullet = TRUE;
        break;
      }
    case MD_BLOCK_HR:
      pay (render);
      g_string_append (render->out, "──────────");
      owe (render, 2);
      break;
    case MD_BLOCK_H:
      pay (render);
      g_string_append (render->out, render->strong);
      break;
    case MD_BLOCK_CODE:
      pay (render);
      g_string_append (render->out, "<tt>");
      break;
    case MD_BLOCK_P:
      /* an item's first paragraph follows its bullet */
      if (!render->bullet)
        pay (render);
      render->bullet = FALSE;
      break;
    default:
      break;
    }
  return 0;
}

static int
leave_block (MD_BLOCKTYPE type, void *detail, void *data)
{
  Render *render = data;

  switch (type)
    {
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      g_array_set_size (render->numbers, render->numbers->len - 1);
      owe (render, render->numbers->len ? 1 : 2);
      break;
    case MD_BLOCK_H:
      g_string_append (render->out, "</span>");
      owe (render, 2);
      break;
    case MD_BLOCK_CODE:
      /* md4c ends each of its lines with \n: the last one is the block's end */
      if (render->out->len && render->out->str[render->out->len - 1] == '\n')
        g_string_truncate (render->out, render->out->len - 1);
      g_string_append (render->out, "</tt>");
      owe (render, 2);
      break;
    case MD_BLOCK_P:
      owe (render, render->numbers->len ? 1 : 2);
      break;
    default:
      break;
    }
  return 0;
}

static int
enter_span (MD_SPANTYPE type, void *detail, void *data)
{
  Render *render = data;

  switch (type)
    {
    case MD_SPAN_EM:
      g_string_append (render->out, "<i>");
      break;
    case MD_SPAN_STRONG:
      g_string_append (render->out, render->strong);
      break;
    case MD_SPAN_CODE:
      g_string_append (render->out, "<tt>");
      break;
    case MD_SPAN_DEL:
      g_string_append (render->out, "<s>");
      break;
    case MD_SPAN_A:
      {
        MD_SPAN_A_DETAIL *link = detail;

        g_string_append (render->out, "<a href=\"");
        escaped (render, link->href.text, link->href.size);
        g_string_append (render->out, "\">");
        break;
      }
    default:
      break;
    }
  return 0;
}

static int
leave_span (MD_SPANTYPE type, void *detail, void *data)
{
  static const char *const closing[] = {
    [MD_SPAN_EM] = "</i>", [MD_SPAN_STRONG] = "</span>", [MD_SPAN_CODE] = "</tt>", [MD_SPAN_DEL] = "</s>",
    [MD_SPAN_A] = "</a>",
  };
  Render *render = data;

  if ((gsize) type < G_N_ELEMENTS (closing) && closing[type])
    g_string_append (render->out, closing[type]);
  return 0;
}

static int
text (MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data)
{
  Render *render = data;

  render->bullet = FALSE;
  switch (type)
    {
    case MD_TEXT_NULLCHAR:
      g_string_append (render->out, "\xEF\xBF\xBD");
      break;
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
      g_string_append_c (render->out, '\n');
      break;
    default:
      /* entities too: shown as typed, never decoded into markup */
      escaped (render, text, size);
      break;
    }
  return 0;
}

char *
msg_markdown_to_pango (const char *markdown, const char *strong_weight)
{
  g_autofree char *weight = g_markup_escape_text (strong_weight, -1);
  g_autofree char *strong = g_strdup_printf ("<span weight=\"%s\">", weight);
  const MD_PARSER parser = {
    .flags = DIALECT, .enter_block = enter_block, .leave_block = leave_block, .enter_span = enter_span,
    .leave_span = leave_span, .text = text,
  };
  Render render = { g_string_new (NULL), strong, 0, g_array_new (FALSE, FALSE, sizeof (guint)), FALSE };
  int failed = md_parse (markdown, strlen (markdown), &parser, &render);

  g_array_unref (render.numbers);
  if (failed)
    {
      g_string_free (render.out, TRUE);
      return NULL;
    }
  return g_string_free (render.out, FALSE);
}
