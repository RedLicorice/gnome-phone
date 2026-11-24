/* The rule that decides a conversation's group in the list: one provider's chat alone is that provider's; plain SMS,
 * groups of people and an agent among others are SMS. The provider ids and addresses are data, not the code's: the
 * file's, and the channel registry's (`minerva-config channels --json`) under it. */
#include "../src/sms-provider.h"

#include <glib.h>
#include <glib/gstdio.h>

static const char CONF[] =
  "[agent]\naddress = agent:\nlabel = Agent\n\n"
  "[echo]\naddress = echo:   ; a comment, as in the phone's file\nlabel = Echo\n\n"
  "[limits]\naddress = never:\n";

static char *
group (const char *first, const char *second)
{
  const char *destinations[] = { first, second, NULL };

  return sms_provider_group (destinations);
}

static void
test_group (void)
{
  g_autofree char *dir = g_dir_make_tmp ("test-sms-group-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "messaging.conf", NULL);
  g_autofree char *agent = NULL, *chat = NULL, *sms = NULL, *mixed = NULL, *none = NULL, *other = NULL;
  g_auto(GStrv) ids = NULL;

  g_assert_true (g_file_set_contents (path, CONF, -1, NULL));
  g_setenv ("MINERVA_MESSAGING_CONF", path, TRUE);
  g_setenv ("MINERVA_CONFIG_BIN", "/nonexistent/minerva-config", TRUE);
  sms_provider_reload ();

  agent = group ("agent:Minerva", NULL);
  chat = group ("echo:+393331234567", NULL);
  sms = group ("+393331234567", NULL);
  mixed = group ("echo:+393331234567", "+393339999999");
  none = group (NULL, NULL);
  other = group ("never:x", NULL);
  g_assert_cmpstr (agent, ==, "agent");
  g_assert_cmpstr (chat, ==, "echo");
  g_assert_null (sms);
  g_assert_null (mixed);
  g_assert_null (none);
  g_assert_null (other);        /* [limits] is not a provider, whatever it holds */
  ids = sms_provider_ids ();
  g_assert_cmpuint (g_strv_length (ids), ==, 2);

  g_unlink (path);
  g_rmdir (dir);
}

/* a channel's section is its manifest's "messaging" object, as minerva-config prints the registry (scalars as the INI
 * holds them); the file's keys go over it, and a channel with no key in the file is a provider all the same */
static const char REGISTRY[] =
  "#!/bin/sh\n"
  "[ \"$*\" = 'channels --json' ] || exit 2\n"
  "echo '{\"chan\": {\"label\": \"Chan\", \"messaging\": {\"address\": \"chan:\", \"label\": \"Chan\",'"
  "' \"show-label\": true, \"text-max\": 2000, \"groups\": [\"x\"]}},'"
  "' \"quiet\": {\"label\": \"Quiet\", \"messaging\": {\"address\": \"quiet:\"}}}'\n";

static void
test_registry (void)
{
  g_autofree char *dir = g_dir_make_tmp ("test-sms-registry-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "messaging.conf", NULL);
  g_autofree char *bin = g_build_filename (dir, "minerva-config", NULL);
  g_autofree char *chat = NULL, *label = NULL, *quiet = NULL, *max = NULL, *groups = NULL;
  g_auto(GStrv) ids = NULL;

  g_assert_true (g_file_set_contents (path, "[agent]\naddress = agent:\n[chan]\nlabel = Chan Net\n", -1, NULL));
  g_assert_true (g_file_set_contents (bin, REGISTRY, -1, NULL));
  g_assert_cmpint (g_chmod (bin, 0700), ==, 0);
  g_setenv ("MINERVA_MESSAGING_CONF", path, TRUE);
  g_setenv ("MINERVA_CONFIG_BIN", bin, TRUE);
  sms_provider_reload ();

  chat = group ("chan:42", NULL);
  quiet = group ("quiet:7", NULL);
  label = sms_provider_label ("chan");
  max = sms_provider_value ("chan", "text-max");
  groups = sms_provider_value ("chan", "groups");
  g_assert_cmpstr (chat, ==, "chan");
  g_assert_cmpstr (quiet, ==, "quiet");
  g_assert_cmpstr (label, ==, "Chan Net");               /* the file's key over the manifest's */
  g_assert_true (sms_provider_flag ("chan", "show-label"));
  g_assert_cmpstr (max, ==, "2000");
  g_assert_null (groups);                                /* a list is no INI value */
  ids = sms_provider_ids ();
  g_assert_cmpuint (g_strv_length (ids), ==, 3);

  g_unlink (bin);
  g_unlink (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/messaging/group", test_group);
  g_test_add_func ("/messaging/registry", test_registry);
  return g_test_run ();
}
