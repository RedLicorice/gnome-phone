/* The rule that decides a conversation's group in the list: one provider's chat alone is that provider's; plain SMS,
 * groups of people and an agent among others are SMS. The provider ids and addresses are the file's, not the code's. */
#include "../src/sms-provider.h"

#include <glib.h>
#include <glib/gstdio.h>

static const char CONF[] =
  "[agent]\naddress = agent:\nlabel = Agent\n\n"
  "[whatsapp]\naddress = whatsapp:   ; a comment, as in the phone's file\nlabel = WhatsApp\n\n"
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
  sms_provider_reload ();

  agent = group ("agent:Minerva", NULL);
  chat = group ("whatsapp:+393331234567", NULL);
  sms = group ("+393331234567", NULL);
  mixed = group ("whatsapp:+393331234567", "+393339999999");
  none = group (NULL, NULL);
  other = group ("never:x", NULL);
  g_assert_cmpstr (agent, ==, "agent");
  g_assert_cmpstr (chat, ==, "whatsapp");
  g_assert_null (sms);
  g_assert_null (mixed);
  g_assert_null (none);
  g_assert_null (other);        /* [limits] is not a provider, whatever it holds */
  ids = sms_provider_ids ();
  g_assert_cmpuint (g_strv_length (ids), ==, 2);

  g_unlink (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/messaging/group", test_group);
  return g_test_run ();
}
