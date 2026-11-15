/* A line-by-line difference between the text in the editor and a snapshot: the common head and tail are skipped,
 * the middle is a longest-common-subsequence table (bounded: past the cap the page says the two are too different
 * rather than freeze the phone). Removed lines come before added ones, as the eye reads them. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NP_DIFF_SAME,
  NP_DIFF_ADD,
  NP_DIFF_DEL,
} NpDiffKind;

typedef struct {
  NpDiffKind kind;
  char      *text;
} NpDiffLine;

/* newest first? no: in reading order; NULL when the two are too different to compare */
GPtrArray *np_diff_lines (const char *old_text, const char *new_text);
void       np_diff_free  (GPtrArray *lines);

G_END_DECLS
