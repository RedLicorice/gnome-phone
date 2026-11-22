/* neo-remote: Neo Remote, the phone's IR LED as a remote for the TV and any other device.
 *
 * Everything IR (the remotes, macros, the code database, the scan by brand, sending) is minerva-ir's session service
 * org.minerva.Ir1; this program is its face. Its words, icons and settings are data (ui.json in the resources). */
#include "nr-ir.h"
#include "nr-window.h"

#include <neo.h>

#define APP_ID "de.yesman.NeoRemote"
#define RES    "/de/yesman/NeoRemote/"

static JsonParser *ui;
static GtkWidget  *window;

static void
on_startup (GApplication *app, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data (RES "ui.json", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  GtkCssProvider *provider = gtk_css_provider_new ();
  JsonObject *conf;

  neo_init ();
  gtk_css_provider_load_from_resource (provider, RES "neo-remote.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
  ui = json_parser_new ();
  if (!bytes || !json_parser_load_from_data (ui, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), &error))
    g_error ("ui.json: %s", error ? error->message : "missing");
  conf = json_object_get_object_member (json_node_get_object (json_parser_get_root (ui)), "conf");
  nr_ir_set_timeout ((int) json_object_get_int_member (conf, "dbus_timeout_ms"));
}

static void
on_activate (GApplication *app, gpointer data)
{
  if (!window)
    window = nr_window_new (GTK_APPLICATION (app), json_node_get_object (json_parser_get_root (ui)));
  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&ui);
  return status;
}
