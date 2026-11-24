#include "../src/im-form.h"

/* Phone and Messages find a chat's contact on UMI too: a handle is read from X-<CHANNEL>:<id> (EDS 3.62 drops IMPP
 * from vCard 3.0 cards) and from IMPP:<channel>:<id>, each once; an X- field of no channel is no handle */
static void
test_handles (void)
{
  /* as Minerva's memory service reads them ([eds] im_read_fields, [contacts] channels) */
  const char *const reads[] = { "IMPP", "X-", NULL };
  const char *const channels[] = { "whatsapp", "telegram", NULL };
  g_autoptr(EVCard) card = e_vcard_new_from_string (
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u1\r\nFN:Andrea\r\nX-TELEGRAM:8675309\r\nIMPP:whatsapp:+39333\r\n"
    "X-WHATSAPP:+39333\r\nX-EVOLUTION-FILE-AS:Andrea\r\nEND:VCARD\r\n");
  g_auto(GStrv) handles = im_form_handles (card, reads, channels);
  g_auto(GStrv) fields = im_form_fields (reads, channels);
  const char *const want[] = { "whatsapp:+39333", "telegram:8675309", NULL };
  const char *const want_fields[] = { "IMPP", "X-WHATSAPP", "X-TELEGRAM", NULL };

  g_assert_true (g_strv_equal ((const char *const *) handles, want));
  g_assert_true (g_strv_equal ((const char *const *) fields, want_fields));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/im-form/handles", test_handles);
  return g_test_run ();
}
