/* msg_markdown_to_pango: what an agent's messages use comes out as Pango markup, and nothing typed can break it */

#include "msg-markdown.h"

static void
assert_markup (const char *markdown, const char *expected)
{
  static const GMarkupParser any = { 0 };
  g_autofree char *markup = msg_markdown_to_pango (markdown, "heavy");
  g_autofree char *document = g_strconcat ("<markup>", markup, "</markup>", NULL);
  g_autoptr(GMarkupParseContext) context = g_markup_parse_context_new (&any, 0, NULL, NULL);
  g_autoptr(GError) error = NULL;

  g_assert_cmpstr (markup, ==, expected);
  /* well formed, as GtkLabel reads it (it takes the links out, then hands the rest to Pango) */
  g_assert_true (g_markup_parse_context_parse (context, document, -1, &error));
  g_assert_true (g_markup_parse_context_end_parse (context, &error));
  g_assert_no_error (error);
}

static void
test_spans (void)
{
  assert_markup ("**bold** *it* `a<b` ~~no~~ [site](https://x.org/?a=1&b=2)",
                 "<span weight=\"heavy\">bold</span> <i>it</i> <tt>a&lt;b</tt> <s>no</s> <a href=\"https://x.org/?a=1&amp;b=2\">site</a>");
}

static void
test_blocks (void)
{
  assert_markup ("# Title\nline one\nline two\n\n- a\n- **b**\n  1. c\n\n```\nx & y\n```",
                 "<span weight=\"heavy\">Title</span>\n\nline one\nline two\n\n• a\n• <span weight=\"heavy\">b</span>\n    1. c\n\n<tt>x &amp; y</tt>");
}

/* raw HTML, entities, images and stray markers are text */
static void
test_rest_is_text (void)
{
  assert_markup ("<b>hi</b> &amp; ![cat](c.png) 2 * 3 <", "&lt;b&gt;hi&lt;/b&gt; &amp;amp; cat 2 * 3 &lt;");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/markdown/spans", test_spans);
  g_test_add_func ("/markdown/blocks", test_blocks);
  g_test_add_func ("/markdown/rest-is-text", test_rest_is_text);
  return g_test_run ();
}
