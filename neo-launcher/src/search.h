/* The search client: GNOME's search providers (org.gnome.Shell.SearchProvider2) as the launcher's backend.
 *
 * Providers are found from the *.search-provider.ini files (keys DesktopId, BusName, ObjectPath, Version=2) in
 * <data dir>/gnome-shell/search-providers of every XDG data dir, the user's included (~/.local/share first, so a
 * user's file of the same name wins). org.gnome.desktop.search-providers is honoured: `disabled` (desktop ids that
 * never get a query) and `sort-order` (those desktop ids first, in that order; the rest by desktop id). The settings
 * are read again when they change.
 *
 * Loop: the launcher runs a GLib main loop (main.c: g_main_loop_run on the default context), so every call here is
 * asynchronous GDBus and its results come back in the default GMainContext; a program without a loop of its own (the
 * test CLI) runs one on the default context. Nothing here blocks. Call from the thread that runs that context.
 *
 * One query ("generation"): search_query() with the text; the client waits 300 ms of quiet (each new text restarts
 * the wait), then asks every provider in parallel: GetInitialResultSet(terms), or GetSubsearchResultSet(previous
 * ids, terms) when the text extends the one of the last generation (its terms, the first n the same, the last
 * longer, and that provider has answered), then GetResultMetas for the first SEARCH_METAS_PER_PROVIDER ids. A
 * provider that has not answered both in SEARCH_PROVIDER_TIMEOUT_MS is given up on (its callback says timed out).
 * Text shorter than SEARCH_MIN_CHARS (characters) is no query: the running generation is cancelled and the
 * callback gets one call with provider NULL and last TRUE (so the view can clear itself). */
#pragma once

#include <glib.h>

#define SEARCH_MIN_CHARS            2
#define SEARCH_DEBOUNCE_MS          300
#define SEARCH_PROVIDER_TIMEOUT_MS  1500
#define SEARCH_METAS_PER_PROVIDER   3

typedef struct _Search Search;

typedef struct {
  char *id;            /* the provider's result id, what ActivateResult takes */
  char *name;          /* GetResultMetas "name" */
  char *description;   /* "description", or NULL */
  char *icon;          /* a GIcon as a string (g_icon_to_string: a themed name or a file path), or NULL */
} SearchHit;

typedef struct {
  char       *id;      /* the .ini's file name without the suffix: unique, what activate and launch take */
  char       *desktop_id;   /* DesktopId */
  char       *title;   /* the desktop file's name (the group's heading), the id when it has none */
  char       *icon;    /* the desktop file's icon as a string, or NULL */
  gboolean    timed_out;    /* it did not answer in time, or failed: no hits */
  GPtrArray  *hits;    /* SearchHit*: the first SEARCH_METAS_PER_PROVIDER with metas; the others only an id */
  guint       n_total; /* how many results the provider has (hits->len is at most SEARCH_METAS_PER_PROVIDER) */
  double      ms;      /* from the query going out to this answer */
  guint       order;   /* the provider's place in the sort order (0 first): the view keeps its groups in it */
} SearchGroup;

/* A provider answered (or timed out): its group, and whether it is the last of this generation (every provider has
 * answered or timed out). The group is only valid in the call. A group with no hits is passed too (the view drops a
 * provider that had results for the text before). With the text under SEARCH_MIN_CHARS: group NULL, last TRUE. */
typedef void (*SearchGroupFunc) (Search *self, guint generation, const SearchGroup *group, gboolean last,
                                 gpointer data);

Search *search_new      (SearchGroupFunc func, gpointer data);
void    search_free     (Search *self);
/* the number of providers that will be asked (after `disabled`); for a status line */
guint   search_n_providers (Search *self);
/* A new text (the whole text of the field): debounce, then query. Returns the generation it will have; an older one
 * running is cancelled (its callbacks stop at once). */
guint   search_query    (Search *self, const char *text);
/* the same without the debounce wait (Enter, the CLI) */
guint   search_query_now (Search *self, const char *text);
/* the generation of the latest query */
guint   search_generation (Search *self);
/* ActivateResult(id, terms, timestamp) on that provider; the terms are those of the text last queried */
void    search_activate (Search *self, const char *provider, const char *result_id);
/* LaunchSearch(terms, timestamp): the provider's own app opens on its search (the group's "more") */
void    search_launch_search (Search *self, const char *provider);
