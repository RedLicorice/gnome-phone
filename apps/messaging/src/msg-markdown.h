#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* @text read as Markdown (md4c, CommonMark with strikethrough, task lists and bare links), as Pango markup for a
   GtkLabel: emphasis, code, links, lists and headings; raw HTML and everything else shown as typed. Strong text and
   headings take Pango's @strong_weight (a weight name: "bold", "heavy"), so that they stand out of a body that
   is bold already. NULL if md4c refuses the text */
char *msg_markdown_to_pango (const char *text, const char *strong_weight);

G_END_DECLS
