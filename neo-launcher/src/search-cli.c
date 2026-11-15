/* neo-search-cli [--debounce] TEXT...: ask the GNOME search providers on the session bus (search.c) and print what they
 * answer; a test for the launcher's search client. */
#include "search.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  GMainLoop *loop;
  gint64     start;
} Cli;

static void
on_group (Search *self, guint generation, const SearchGroup *group, gboolean last, gpointer data)
{
  Cli *cli = data;

  if (group) {
    g_print ("[%.0f ms] %s (%s) total=%u%s\n", group->ms, group->id, group->title, group->n_total,
             group->timed_out ? " [TIMEOUT]" : "");
    for (guint i = 0; i < group->hits->len; i++) {
      const SearchHit *h = g_ptr_array_index (group->hits, i);
      if (h->description)
        g_print ("    %s — %s [id=%s]\n", h->name, h->description, h->id);
      else
        g_print ("    %s [id=%s]\n", h->name, h->id);
    }
  } else
    g_print ("(no query: the text is too short or there is no provider)\n");
  if (last) {
    g_print ("done in %.0f ms\n", (g_get_monotonic_time () - cli->start) / 1000.0);
    g_main_loop_quit (cli->loop);
  }
}

static gboolean
on_safety_timeout (gpointer data)
{
  Cli *cli = data;

  g_printerr ("neo-search-cli: no end after 5 s\n");
  g_main_loop_quit (cli->loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  gboolean debounce = FALSE;
  int first = 1;

  if (argc > 1 && !strcmp (argv[1], "--debounce")) {
    debounce = TRUE;
    first = 2;
  }
  if (first >= argc) {
    g_printerr ("usage: neo-search-cli [--debounce] TEXT\n");
    return 2;
  }
  g_autofree char *text = g_strjoinv (" ", argv + first);
  Cli cli = { .loop = g_main_loop_new (NULL, FALSE), .start = g_get_monotonic_time () };
  Search *search = search_new (on_group, &cli);

  g_printerr ("%u providers\n", search_n_providers (search));
  cli.start = g_get_monotonic_time ();
  if (debounce)
    search_query (search, text);
  else
    search_query_now (search, text);
  g_timeout_add (5000, on_safety_timeout, &cli);
  g_main_loop_run (cli.loop);
  search_free (search);
  g_main_loop_unref (cli.loop);
  return 0;
}
