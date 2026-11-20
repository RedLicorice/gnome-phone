/* neo-voice: Neo Voice ("Rilevamenti"), the voice service's detection log: every wake detection, played, trimmed,
 * labelled and deleted by the owner (Minerva docs/detection-log.md). It reads and writes only through
 * org.minerva.Voice1's Detection* methods; every label and setting is data (neo-voice-ui.json).
 *
 * NEO_VOICE_SELECT=<id> selects a record at start (a test run, the screenshots). */
#include "nv-window.h"

#include <neo.h>

#define APP_ID "de.yesman.NeoVoice"

static NvStore   *store;
static GtkWidget *window;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/NeoVoice/neo-voice.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
  store = nv_store_new ();
}

static void
on_activate (GApplication *app, gpointer data)
{
  const char *select = g_getenv ("NEO_VOICE_SELECT");

  if (!window)
    window = nv_window_new (GTK_APPLICATION (app), store);
  gtk_window_present (GTK_WINDOW (window));
  if (select && *select)
    nv_window_select (window, select);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  if (store)
    nv_store_free (store);
  return status;
}
