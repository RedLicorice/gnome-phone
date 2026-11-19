#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* @text read as Markdown (md4c, CommonMark with strikethrough, task lists and bare links), as Pango markup for a
   GtkLabel: emphasis, code, links, lists and headings; raw HTML and everything else shown as typed. Strong text and
   headings take Pango's @strong_weight (a weight name: "bold", "heavy"), so that they stand out of a body that
   is bold already. NULL if md4c refuses the text */
char *msg_markdown_to_pango (const char *text, const char *strong_weight);

/* @text's preview, for a notification: its Markdown as plain text on one line (every run of white space one space),
   at most @chars characters (0: no limit) of whole words, an ellipsis where it is cut. NULL @text is empty; never NULL */
char *msg_markdown_preview (const char *text, guint chars);

G_END_DECLS
