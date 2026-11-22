/* messaging.conf in its configuration layers (Minerva's docs/packaging.md, "Configuration"): the owner's file holds
 * only the keys he changed, over the package's defaults; a layer that changes is read again. */
#include <glib.h>
#include <glib/gstdio.h>

#include "../src/sms-provider.h"

static void
write (const char *dir, const char *name, const char *text)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);

  g_mkdir_with_parents (dir, 0700);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static void
test_layers (void)
{
  g_autofree char *root = g_dir_make_tmp ("sms-layers-XXXXXX", NULL);
  g_autofree char *shipped = g_build_filename (root, "shipped", NULL);
  g_autofree char *user = g_build_filename (root, "user", NULL);
  g_autofree char *layers = g_strdup_printf ("shipped_dir = %s\ndevice_dir = %s/device\nsystem_dir = %s/etc\n"
                                             "user_dir = %s\n", shipped, root, root, user);
  g_autofree char *text = NULL;

  write (root, "layers.conf", layers);
  g_autofree char *conf = g_build_filename (root, "layers.conf", NULL);
  g_setenv ("MINERVA_LAYERS_CONF", conf, TRUE);
  g_unsetenv ("MINERVA_MESSAGING_CONF");
  write (shipped, "messaging.conf", "[live]\ncancelled = Annullato  ; the shipped word\nunheard = Non ho capito\n");
  write (user, "messaging.conf", "[live]\ncancelled = Lasciato stare\n");

  text = sms_provider_text ("live", "cancelled");
  g_assert_cmpstr (text, ==, "Lasciato stare");   /* the owner's key */
  g_clear_pointer (&text, g_free);
  text = sms_provider_text ("live", "unheard");
  g_assert_cmpstr (text, ==, "Non ho capito");    /* the package's, which the owner did not change */
  g_clear_pointer (&text, g_free);

  write (user, "messaging.conf", "[live]\n");     /* he takes his change back: the package's word again */
  text = sms_provider_text ("live", "cancelled");
  g_assert_cmpstr (text, ==, "Annullato");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sms-provider/layers", test_layers);
  return g_test_run ();
}
