/* neo-prompt: Neo Prompt, the long texts of Minerva and the prompt the agent is given.
 *
 * The files it opens, how each is read and saved, and every label are data (config/minerva-config's
 * neo-prompt.json, neo-prompt-files.json, neo-prompt-ui.json); this program is the editor and the context view.
 *
 * Another app opens a document with neo-prompt://open/<id>[?person=<id>] through GApplication's Open: Settings'
 * editor rows and the Contacts card do. NEO_PROMPT_OPEN="<id>[:<person>]" does the same for a test run. */
#include "np-window.h"

#include <neo.h>
#include <string.h>

#define APP_ID "de.yesman.NeoPrompt"

static NpStore  *store;
static GtkWidget *window;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/NeoPrompt/neo-prompt.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
open_uri (GtkApplication *app, const char *uri)
{
  g_autoptr(GUri) parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
  const char *path = parsed ? g_uri_get_path (parsed) : NULL;
  const char *query = parsed ? g_uri_get_query (parsed) : NULL;
  g_autofree char *id = path && *path == '/' ? g_strdup (path + 1) : NULL;
  g_autofree char *person = NULL;

  if (!id || !*id)
    return;
  if (query && g_str_has_prefix (query, "person="))
    person = g_strdup (query + strlen ("person="));
  if (!window)
    window = np_window_new (app, store);
  np_window_open_doc (window, id, person);
}

/* the text sizes are data (editor.read.font_px, editor.font_px): a stylesheet made from them, over the app's */
static void
load_sizes (void)
{
  g_autoptr (GString) css = g_string_new ("");
  GtkCssProvider *provider = NULL;

  if (store->conf.read_font_px > 0)
    g_string_append_printf (css, ".np-editor.np-reading { font-size: %dpx; }\n", store->conf.read_font_px);
  if (store->conf.edit_font_px > 0)
    g_string_append_printf (css, ".np-editor.np-editing { font-size: %dpx; }\n", store->conf.edit_font_px);
  if (!css->len)
    return;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css->str);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
  store = np_store_new (NULL);
  load_sizes ();
}

static void
on_activate (GApplication *app, gpointer data)
{
  const char *open = g_getenv ("NEO_PROMPT_OPEN");

  if (!window)
    window = np_window_new (GTK_APPLICATION (app), store);
  gtk_window_present (GTK_WINDOW (window));
  if (open && *open)
    {
      g_auto(GStrv) parts = g_strsplit (open, ":", 2);

      np_window_open_doc (window, parts[0], parts[1]);
    }
  {
    const char *tab = g_getenv ("NEO_PROMPT_TAB");   /* a test run: the tab to show */

    if (tab && *tab)
      np_window_present_tab (window, atoi (tab));
  }
}

static void
on_open (GApplication *app, GFile **files, gint n_files, const char *hint, gpointer data)
{
  if (!window)
    window = np_window_new (GTK_APPLICATION (app), store);
  gtk_window_present (GTK_WINDOW (window));
  for (gint i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);

      if (uri)
        open_uri (GTK_APPLICATION (app), uri);
    }
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  int status;

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  if (store)
    np_store_free (store);
  return status;
}
