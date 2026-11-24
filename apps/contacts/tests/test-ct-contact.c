/* ct-contact: names, sections and sorting, favourites and labels, the tile, data rows and their types, dates,
 * and search matching (spec §1.3-§1.4, §2.4, §6.2, §10). */
#include "ct-contact.h"

#include <locale.h>
#include <string.h>

static EContact *
card (const char *body)
{
  g_autofree char *text = g_strdup_printf ("BEGIN:VCARD\r\nVERSION:3.0\r\n%sEND:VCARD\r\n", body);

  return e_contact_new_from_vcard (text);
}

static void
test_display_names (void)
{
  g_autoptr(EContact) full = card ("N:Public;John;Quincy;Dr.;Jr.\r\nFN:x\r\n");
  g_autoptr(EContact) plain = card ("N:Smith;Anna;;;\r\n");
  g_autoptr(EContact) fn_only = card ("FN:Maria van Dijk\r\n");
  g_autoptr(EContact) phone_only = card ("TEL;TYPE=CELL:+49 170 1234567\r\n");
  g_autoptr(EContact) org_only = card ("ORG:Acme GmbH;Sales\r\n");
  g_autoptr(EContact) nothing = card ("NOTE:hello\r\n");
  g_autofree char *a = ct_contact_display_name (full, CT_ORDER_PRIMARY);
  g_autofree char *b = ct_contact_display_name (full, CT_ORDER_ALTERNATIVE);
  g_autofree char *c = ct_contact_display_name (plain, CT_ORDER_PRIMARY);
  g_autofree char *d = ct_contact_display_name (plain, CT_ORDER_ALTERNATIVE);
  g_autofree char *e = ct_contact_display_name (fn_only, CT_ORDER_PRIMARY);
  g_autofree char *f = ct_contact_display_name (fn_only, CT_ORDER_ALTERNATIVE);
  g_autofree char *g = ct_contact_display_name (phone_only, CT_ORDER_PRIMARY);
  g_autofree char *h = ct_contact_display_name (org_only, CT_ORDER_PRIMARY);
  g_autofree char *i = ct_contact_display_name (nothing, CT_ORDER_PRIMARY);

  g_assert_cmpstr (a, ==, "Dr. John Quincy Public, Jr.");
  g_assert_cmpstr (b, ==, "Public, Dr. John Quincy, Jr.");
  g_assert_cmpstr (c, ==, "Anna Smith");
  g_assert_cmpstr (d, ==, "Smith, Anna");
  g_assert_cmpstr (e, ==, "Maria van Dijk");
  g_assert_cmpstr (f, ==, "Dijk, Maria van");
  g_assert_cmpstr (g, ==, "+49 170 1234567");
  g_assert_cmpstr (h, ==, "Acme GmbH");
  g_assert_cmpstr (i, ==, "");
}

static void
test_name_round_trip (void)
{
  g_autoptr(EContact) contact = card ("X-KEEP-ME:yes\r\n");
  g_autoptr(CtName) name = ct_name_new ();
  g_autoptr(CtName) back = NULL;
  g_autofree char *fn = NULL, *file_as = NULL, *keep = NULL;

  g_free (name->given);
  name->given = g_strdup ("Ünal");
  g_free (name->family);
  name->family = g_strdup ("Öztürk");
  g_free (name->phonetic_given);
  name->phonetic_given = g_strdup ("Uenal");
  ct_contact_set_name (contact, name);
  back = ct_contact_get_name (contact);
  g_assert_true (ct_name_equal (name, back));
  fn = e_contact_get (contact, E_CONTACT_FULL_NAME);
  file_as = e_contact_get (contact, E_CONTACT_FILE_AS);
  g_assert_cmpstr (fn, ==, "Ünal Öztürk");
  g_assert_cmpstr (file_as, ==, "Öztürk, Ünal");
  keep = e_vcard_util_dup_x_attribute (E_VCARD (contact), "X-KEEP-ME");
  g_assert_cmpstr (keep, ==, "yes");
}

static void
test_sections (void)
{
  g_assert_cmpstr (ct_section_of ("Anna"), ==, "A");
  g_assert_cmpstr (ct_section_of ("ängel"), ==, "A");
  g_assert_cmpstr (ct_section_of ("Øystein"), ==, "O");      /* no decomposition, but a variant of O to ICU */
  g_assert_cmpstr (ct_section_of ("Çelik"), ==, "C");
  g_assert_cmpstr (ct_section_of ("+49 170"), ==, "#");
  g_assert_cmpstr (ct_section_of ("42"), ==, "#");
  g_assert_cmpstr (ct_section_of ("Σοφία"), ==, "Σ");
  g_assert_cmpstr (ct_section_of ("Ольга"), ==, "О");
  g_assert_cmpstr (ct_section_of ("שרה"), ==, "…");
  g_assert_cmpstr (ct_section_of (""), ==, "…");
  g_assert_cmpstr (ct_section_of ("$#"), ==, "…");
  g_assert_cmpstr (ct_section_of ("\xe2\x9d\xa4Lea"), ==, "…");   /* a heart first */
  g_assert_cmpstr (ct_section_of ("  zed"), ==, "Z");
  g_assert_true (ct_section_of ("Bob") == ct_section_of ("bert"));   /* interned */
  /* the order the still shows: symbols' "…" before A, then the letters, the other alphabets, "#", then the
   * other scripts' "…" */
  g_assert_cmpint (ct_section_rank ("?"), <, ct_section_rank ("Anna"));
  g_assert_cmpint (ct_section_rank ("Zed"), <, ct_section_rank ("Σοφία"));
  g_assert_cmpint (ct_section_rank ("Σοφία"), <, ct_section_rank ("Ольга"));
  g_assert_cmpint (ct_section_rank ("Ольга"), <, ct_section_rank ("42"));
  g_assert_cmpint (ct_section_rank ("42"), <, ct_section_rank ("שרה"));
  g_assert_cmpint (ct_section_rank ("Anna"), ==, ct_section_rank ("ärger"));
}

static int
by_key (gconstpointer a, gconstpointer b)
{
  g_autofree char *ka = ct_sort_key (*(const char **) a), *kb = ct_sort_key (*(const char **) b);

  return strcmp (ka, kb);
}

static void
test_sort_order (void)
{
  const char *names[] = { "+49 170 111", "zoë", "Émile", "Σοφία", "anna", "Bob", "Ärger", "Adam", "?", "Ольга" };
  const char *expected[] = { "?", "Adam", "anna", "Ärger", "Bob", "Émile", "zoë", "Σοφία", "Ольга", "+49 170 111" };

  qsort (names, G_N_ELEMENTS (names), sizeof names[0], by_key);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    g_assert_cmpstr (names[i], ==, expected[i]);
}

static void
test_starred_and_labels (void)
{
  g_autoptr(EContact) contact = card ("CATEGORIES:Family,Work\r\n");
  g_autoptr(EContact) folks = card ("X-FOLKS-FAVOURITE:true\r\n");
  g_autoptr(GPtrArray) labels = NULL;
  g_autofree char *x = NULL;

  g_assert_false (ct_contact_is_starred (contact));
  g_assert_true (ct_contact_is_starred (folks));
  ct_contact_set_starred (contact, TRUE);
  g_assert_true (ct_contact_is_starred (contact));
  x = e_vcard_util_dup_x_attribute (E_VCARD (contact), "X-FOLKS-FAVOURITE");
  g_assert_cmpstr (x, ==, "true");
  labels = ct_contact_dup_labels (contact);
  g_assert_cmpuint (labels->len, ==, 2);           /* the favourites category is not a label */
  g_assert_true (ct_contact_has_label (contact, "Work"));
  g_assert_true (ct_contact_rename_label (contact, "Work", "Office"));
  g_assert_false (ct_contact_has_label (contact, "Work"));
  g_assert_true (ct_contact_has_label (contact, "Office"));
  g_assert_true (ct_contact_remove_label (contact, "Family"));
  g_assert_false (ct_contact_remove_label (contact, "Family"));
  g_assert_true (ct_contact_add_label (contact, "Friends"));
  g_assert_false (ct_contact_add_label (contact, "Friends"));
  g_assert_true (ct_contact_is_starred (contact));   /* labels leave the star alone */
  ct_contact_set_starred (contact, FALSE);
  g_assert_false (ct_contact_is_starred (contact));
  g_assert_true (ct_contact_has_label (contact, "Friends"));
}

static void
test_tile (void)
{
  /* Java's "hello".hashCode() and friends, computed with Java's rules */
  g_assert_cmpint (ct_java_hash ("hello"), ==, 99162322);
  g_assert_cmpint (ct_java_hash ("pas-id-1234"), ==, -254220407);
  g_assert_cmpint (ct_java_hash ("ä€😀"), ==, 16603051);       /* a surrogate pair counts as two units */
  g_assert_cmpint (ct_tile_color_index ("hello"), ==, 10);
  g_assert_cmpint (ct_tile_color_index ("pas-id-1234"), ==, 11);
  g_assert_cmpint (ct_tile_color_index (""), ==, -1);
  g_assert_cmpint (ct_tile_letter ("anna"), ==, 'A');
  g_assert_cmpint (ct_tile_letter ("ölaf"), ==, 0x00d6);
  g_assert_cmpint (ct_tile_letter ("+49"), ==, 0);
  g_assert_cmpint (ct_tile_letter ("😀"), ==, 0);
  g_assert_cmpint (ct_tile_letter (""), ==, 0);
}

static int
phone_type (const char *line, gboolean *primary)
{
  g_autoptr(EContact) contact = card (line);
  g_autoptr(GPtrArray) items = ct_contact_get_items (contact, CT_KIND_PHONE);
  CtItem *item;

  g_assert_cmpuint (items->len, ==, 1);
  item = items->pdata[0];
  if (primary)
    *primary = item->primary;
  return item->type;
}

static void
test_phone_types (void)
{
  gboolean primary;

  g_assert_cmpint (phone_type ("TEL;TYPE=CELL:1\r\n", NULL), ==, 2);
  g_assert_cmpint (phone_type ("TEL;TYPE=HOME,VOICE:1\r\n", NULL), ==, 1);
  g_assert_cmpint (phone_type ("TEL;TYPE=WORK;TYPE=VOICE:1\r\n", NULL), ==, 3);
  g_assert_cmpint (phone_type ("TEL;TYPE=WORK,FAX:1\r\n", NULL), ==, 4);
  g_assert_cmpint (phone_type ("TEL;TYPE=FAX:1\r\n", NULL), ==, 13);
  g_assert_cmpint (phone_type ("TEL;TYPE=VOICE:1\r\n", NULL), ==, 7);
  g_assert_cmpint (phone_type ("TEL:1\r\n", NULL), ==, 7);
  g_assert_cmpint (phone_type ("TEL;TYPE=X-EVOLUTION-CALLBACK:1\r\n", NULL), ==, 8);
  g_assert_cmpint (phone_type ("TEL;TYPE=WORK,CELL:1\r\n", NULL), ==, 17);
  g_assert_cmpint (phone_type ("TEL;TYPE=PREF:1\r\n", &primary), ==, 12);
  g_assert_false (primary);
  g_assert_cmpint (phone_type ("TEL;TYPE=CELL,PREF:1\r\n", &primary), ==, 2);
  g_assert_true (primary);
  /* Android 2.1's bare parameters */
  {
    g_autoptr(EContact) contact = e_contact_new_from_vcard ("BEGIN:VCARD\r\nVERSION:2.1\r\nTEL;HOME:1\r\nEND:VCARD\r\n");
    g_autoptr(GPtrArray) items = ct_contact_get_items (contact, CT_KIND_PHONE);

    g_assert_cmpint (((CtItem *) items->pdata[0])->type, ==, 1);
  }
  /* a custom label */
  {
    g_autoptr(EContact) contact = card ("TEL;TYPE=X-Boat:1\r\n");
    g_autoptr(GPtrArray) items = ct_contact_get_items (contact, CT_KIND_PHONE);
    CtItem *item = items->pdata[0];

    g_assert_cmpint (item->type, ==, CT_TYPE_CUSTOM);
    g_assert_cmpstr (item->label, ==, "Boat");
  }
}

/* every editable kind written and read back gives the same rows, and leaves unknown properties alone */
static void
test_items_round_trip (void)
{
  g_autoptr(EContact) contact = card ("X-ABLabel:keep\r\nTEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=1:+49 170 1\r\n");
  struct { CtKind kind; int type; const char *label, *value; gboolean primary; } rows[] = {
    { CT_KIND_PHONE, 2, NULL, "+49 170 1", FALSE },
    { CT_KIND_PHONE, 3, NULL, "030 1234", TRUE },
    { CT_KIND_PHONE, 12, NULL, "0800 1", FALSE },
    { CT_KIND_PHONE, CT_TYPE_CUSTOM, "Boat", "0171 2", FALSE },
    { CT_KIND_EMAIL, 1, NULL, "anna@example.org", TRUE },
    { CT_KIND_EMAIL, 3, NULL, "a@example.com", FALSE },
    { CT_KIND_ADDRESS, 2, NULL, "Hauptstr. 1\n10115 Berlin", FALSE },
    { CT_KIND_WEBSITE, 7, NULL, "https://example.org", FALSE },
    { CT_KIND_EVENT, 3, NULL, "--03-15", FALSE },
    { CT_KIND_EVENT, 1, NULL, "2010-06-01", FALSE },
    { CT_KIND_EVENT, CT_TYPE_CUSTOM, "Name day", "2020-01-02", FALSE },
    { CT_KIND_RELATION, 14, NULL, "Bob", FALSE },
    { CT_KIND_RELATION, CT_TYPE_CUSTOM, "Coach", "Carla", FALSE },
    { CT_KIND_NICKNAME, 1, NULL, "Annie", FALSE },
    { CT_KIND_NOTE, 0, NULL, "two\nlines", FALSE },
  };
  g_autofree char *keep = NULL;

  for (CtKind kind = 0; kind < CT_N_KINDS; kind++)
    {
      g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);
      g_autoptr(GPtrArray) back = NULL;

      if (kind == CT_KIND_IM || kind == CT_KIND_SIP || kind == CT_KIND_ORGANIZATION)
        continue;
      for (guint i = 0; i < G_N_ELEMENTS (rows); i++)
        if (rows[i].kind == kind)
          {
            CtItem *item = ct_item_new (kind, rows[i].type, rows[i].value);

            g_free (item->label);
            item->label = g_strdup (rows[i].label ? rows[i].label : "");
            item->primary = rows[i].primary;
            g_ptr_array_add (items, item);
          }
      ct_contact_set_items (contact, kind, items);
      /* through the vCard text and back, as the address book stores it */
      {
        g_autofree char *text = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
        g_autoptr(EContact) reread = e_contact_new_from_vcard (text);

        back = ct_contact_get_items (reread, kind);
      }
      g_assert_cmpuint (back->len, ==, items->len);
      for (guint i = 0; i < items->len; i++)
        if (!ct_item_equal (items->pdata[i], back->pdata[i]))
          g_error ("kind %d row %u: %s/%d/%s came back as %s/%d/%s", kind, i, ((CtItem *) items->pdata[i])->value,
                   ((CtItem *) items->pdata[i])->type, ((CtItem *) items->pdata[i])->label,
                   ((CtItem *) back->pdata[i])->value, ((CtItem *) back->pdata[i])->type,
                   ((CtItem *) back->pdata[i])->label);
    }
  keep = e_vcard_util_dup_x_attribute (E_VCARD (contact), "X-ABLABEL");
  g_assert_cmpstr (keep, ==, "keep");

  /* an unchanged row keeps its own parameters */
  {
    g_autoptr(EContact) slot = card ("TEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=1:+49 170 1\r\n");
    g_autoptr(GPtrArray) items = ct_contact_get_items (slot, CT_KIND_PHONE);
    EVCardAttribute *tel;

    ct_contact_set_items (slot, CT_KIND_PHONE, items);
    tel = e_vcard_get_attribute (E_VCARD (slot), EVC_TEL);
    g_assert_nonnull (e_vcard_attribute_get_param (tel, "X-EVOLUTION-UI-SLOT"));
  }
}

static void
test_organization (void)
{
  g_autoptr(EContact) contact = card ("ORG:Acme;Research\r\nTITLE:Boss\r\n");
  g_autoptr(GPtrArray) items = ct_contact_get_items (contact, CT_KIND_ORGANIZATION);
  CtItem *org;

  g_assert_cmpuint (items->len, ==, 1);
  org = items->pdata[0];
  g_assert_cmpstr (org->value, ==, "Acme");
  g_assert_cmpstr (org->value2, ==, "Boss");
  g_assert_cmpstr (org->value3, ==, "Research");
  g_free (org->value2);
  org->value2 = g_strdup ("");
  ct_contact_set_items (contact, CT_KIND_ORGANIZATION, items);
  g_assert_null (e_vcard_get_attribute (E_VCARD (contact), EVC_TITLE));
  g_assert_nonnull (e_vcard_get_attribute (E_VCARD (contact), EVC_ORG));
}

static void
test_dates (void)
{
  int y, m, d;
  g_autofree char *with_year = NULL, *without = NULL;

  g_assert_true (ct_date_parse ("1990-03-15", &y, &m, &d));
  g_assert_cmpint (y, ==, 1990);
  g_assert_true (ct_date_parse ("--03-15", &y, &m, &d));
  g_assert_cmpint (y, ==, 0);
  g_assert_cmpint (m, ==, 3);
  g_assert_cmpint (d, ==, 15);
  g_assert_true (ct_date_parse ("19900315", &y, &m, &d));
  g_assert_true (ct_date_parse ("--0315", &y, &m, &d));
  g_assert_true (ct_date_parse ("1990-03-15T00:00:00Z", &y, &m, &d));
  g_assert_true (ct_date_parse ("--02-29", &y, &m, &d));
  g_assert_false (ct_date_parse ("1990-02-29", &y, &m, &d));
  g_assert_false (ct_date_parse ("tomorrow", &y, &m, &d));
  with_year = ct_date_format_value (1990, 3, 15);
  without = ct_date_format_value (0, 3, 15);
  g_assert_cmpstr (with_year, ==, "1990-03-15");
  g_assert_cmpstr (without, ==, "--03-15");
}

/* how the memory service writes and reads a channel id (Minerva's memory [eds] im_write_field, im_read_fields) */
static const char *const im_reads[] = { "IMPP", "X-", NULL };
static const ImForm im_form = { "X-", im_reads };

/* a channel id saved from the editor is written as X-CHAN:<id> (EDS 3.62 drops IMPP from vCard 3.0 cards); both
 * forms are read, the old id goes in either form, other channels' rows stay */
static void
test_channels (void)
{
  g_autoptr(EContact) contact = e_contact_new_from_vcard (
    "BEGIN:VCARD\nVERSION:3.0\nUID:u1\nFN:Andrea\nIMPP;X-SERVICE-TYPE=Chan:chan:777\nX-CHAN:777\n"
    "IMPP:xmpp:andrea@example.net\nX-ECHO:+39333\nEND:VCARD\n");
  g_autofree char *echo_id = ct_contact_get_channel (contact, "echo", "X-ECHO", &im_form);
  g_autofree char *chan_id = ct_contact_get_channel (contact, "chan", "X-CHAN", &im_form);
  g_autofree char *text = NULL;

  g_assert_cmpstr (echo_id, ==, "+39333");
  g_assert_cmpstr (chan_id, ==, "777");
  ct_contact_set_channel (contact, "chan", "X-CHAN", &im_form, " 888 ");
  text = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  g_assert_nonnull (strstr (text, "X-CHAN:888\r\n"));
  g_assert_null (strstr (text, "chan:"));
  g_assert_null (strstr (text, "777"));
  g_assert_nonnull (strstr (text, "IMPP:xmpp:andrea@example.net"));
  g_assert_nonnull (strstr (text, "X-ECHO:+39333"));
  /* a channel the registry describes names its IM rows, in both forms; another scheme shows as written */
  ct_contact_register_channel ("chan", "X-CHAN", "Chan Net");
  {
    g_autoptr(EContact) rows = card ("IMPP:chan:1\r\nX-CHAN:2\r\nIMPP:other:3\r\nX-UNKNOWN:4\r\n");
    g_autoptr(GPtrArray) items = ct_contact_get_items (rows, CT_KIND_IM);

    g_assert_cmpuint (items->len, ==, 3);
    g_assert_cmpstr (((CtItem *) items->pdata[0])->value2, ==, "Chan Net");
    g_assert_cmpstr (((CtItem *) items->pdata[1])->value2, ==, "Chan Net");
    g_assert_cmpstr (((CtItem *) items->pdata[2])->value2, ==, "Other");
  }
}

static void
test_merge (void)
{
  g_autoptr(EContact) into = e_contact_new_from_vcard (
    "BEGIN:VCARD\nVERSION:3.0\nUID:to\nN:Rossi;Andrea;;;\nFN:Andrea Rossi\nTEL;TYPE=CELL:+39 333 111 2222\n"
    "EMAIL;TYPE=HOME:andrea@example.org\nBDAY:1990-03-15\nORG:Acme;\nIMPP:echo:393331112222\n"
    "X-EVOLUTION-FILE-AS:Rossi\\, Andrea\nEND:VCARD\n");
  g_autoptr(EContact) from = e_contact_new_from_vcard (
    "BEGIN:VCARD\nVERSION:3.0\nUID:from\nN:Rossi;Andy;;;\nFN:Andy Rossi\nTEL;TYPE=WORK:0039 333 111 2222\n"
    "TEL;TYPE=HOME:+39 06 555 0100\nEMAIL;TYPE=WORK:ANDREA@example.org\nEMAIL;TYPE=WORK:work@example.org\n"
    "BDAY:1991-01-01\nORG:Other;\nPHOTO;ENCODING=b;TYPE=PNG:aGVsbG8=\nIMPP:echo:393331112222\n"
    "IMPP;X-SERVICE-TYPE=Chan:chan:777\nNOTE:from note\nCATEGORIES:Friends\nEND:VCARD\n");
  g_autoptr(GPtrArray) phones = NULL, emails = NULL;
  g_autofree char *text = NULL;
  guint conflicts = ct_contact_merge (into, from);

  /* where both have one, the surviving contact's stays, and the conflict is said */
  g_assert_cmpuint (conflicts, ==, CT_MERGE_NAME | CT_MERGE_ORGANIZATION | CT_MERGE_BIRTHDAY);
  text = e_vcard_to_string (E_VCARD (into), EVC_FORMAT_VCARD_30);
  g_assert_nonnull (strstr (text, "FN:Andrea Rossi"));
  g_assert_nonnull (strstr (text, "1990-03-15"));
  g_assert_nonnull (strstr (text, "ORG:Acme"));
  g_assert_null (strstr (text, "1991-01-01"));
  g_assert_nonnull (strstr (text, "PHOTO"));                       /* it had none: the other's */
  g_assert_nonnull (strstr (text, "X-EVOLUTION-FILE-AS:Rossi"));   /* what its vCard has besides stays */
  /* rows add up without doubles: the same number by its digits is one, an email is the same without case */
  phones = ct_contact_get_items (into, CT_KIND_PHONE);
  g_assert_cmpuint (phones->len, ==, 2);
  emails = ct_contact_get_items (into, CT_KIND_EMAIL);
  g_assert_cmpuint (emails->len, ==, 2);
  g_assert_nonnull (strstr (text, "from note"));
  g_assert_nonnull (strstr (text, "Friends"));
  /* the channels: the same id once, the other one added with its parameters */
  {
    g_autofree char *echo_id = ct_contact_get_channel (into, "echo", NULL, &im_form);
    g_autofree char *chan_id = ct_contact_get_channel (into, "chan", NULL, &im_form);

    g_assert_cmpstr (echo_id, ==, "393331112222");
    g_assert_cmpstr (chan_id, ==, "777");
    g_assert_nonnull (strstr (text, "X-SERVICE-TYPE=Chan"));
  }
  /* once more changes nothing */
  g_assert_cmpuint (ct_contact_merge (into, from), ==, CT_MERGE_NAME | CT_MERGE_ORGANIZATION | CT_MERGE_BIRTHDAY);
  {
    g_autofree char *again = e_vcard_to_string (E_VCARD (into), EVC_FORMAT_VCARD_30);

    g_assert_cmpstr (again, ==, text);
  }
}

static void
test_merge_fills_what_is_missing (void)
{
  g_autoptr(EContact) into = e_contact_new_from_vcard ("BEGIN:VCARD\nVERSION:3.0\nUID:to\nTEL:+39 333 1\nEND:VCARD\n");
  g_autoptr(EContact) from = e_contact_new_from_vcard (
    "BEGIN:VCARD\nVERSION:3.0\nUID:from\nN:Bianchi;Marco;;;\nFN:Marco Bianchi\nORG:Acme;\nBDAY:1990-03-15\nEND:VCARD\n");
  g_autofree char *name = NULL;

  g_assert_cmpuint (ct_contact_merge (into, from), ==, 0);
  name = ct_contact_display_name (into, CT_ORDER_PRIMARY);
  g_assert_cmpstr (name, ==, "Marco Bianchi");
}

static void
test_word_prefix (void)
{
  g_assert_cmpint (ct_index_of_word_prefix ("John Smith", "SM"), ==, 5);
  g_assert_cmpint (ct_index_of_word_prefix ("John Smith", "jo"), ==, 0);
  g_assert_cmpint (ct_index_of_word_prefix ("John Smith", "OHN"), ==, -1);
  g_assert_cmpint (ct_index_of_word_prefix ("Jean-Luc Picard", "LUC"), ==, 5);
  g_assert_cmpint (ct_index_of_word_prefix ("Ölaf Ärger", "ÄR"), ==, 6);   /* a byte offset */
  g_assert_cmpint (ct_index_of_word_prefix ("John Smith", "JO SM"), ==, -1);
}

static void
test_search (void)
{
  g_autoptr(EContact) contact = card ("N:Smith;John;;;\r\nTEL;TYPE=CELL:+49 170 1234567\r\n"
                                      "EMAIL;TYPE=WORK:jsmith@acme.example\r\nORG:Acme Corporation\r\n"
                                      "NOTE:Met him at the very long conference about databases in Berlin\r\n");
  const char *name = "John Smith";
  CtMatch match;

  g_assert_true (ct_contact_match (contact, name, "smi", &match));
  g_assert_cmpint (match.name_start, ==, 5);
  g_assert_cmpint (match.name_length, ==, 3);
  g_assert_null (match.snippet);                     /* a name word matched: no snippet (deferred snippeting) */
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "jo sm", &match));
  g_assert_cmpint (match.name_start, ==, -1);        /* indexOfWordPrefix finds no single word for it */
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "0170", &match));   /* the national form */
  g_assert_cmpstr (match.snippet, ==, "+49 170 1234567");
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "49170", &match));
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "acme", &match));   /* a word of the address */
  g_assert_cmpstr (match.snippet, ==, "jsmith@acme.example");
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "jsmith", &match));
  g_assert_cmpstr (match.snippet, ==, "jsmith@acme.example");
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "corp", &match));
  g_assert_cmpstr (match.snippet, ==, "Acme Corporation");
  ct_match_clear (&match);

  g_assert_true (ct_contact_match (contact, name, "databases", &match));
  g_assert_nonnull (match.snippet);
  g_assert_true (g_utf8_strlen (match.snippet, -1) <= 32);
  g_assert_nonnull (strstr (match.snippet, "databases"));
  g_assert_true (g_str_has_prefix (match.snippet, "…"));
  ct_match_clear (&match);

  g_assert_false (ct_contact_match (contact, name, "xyz", &match));
  g_assert_false (ct_contact_match (contact, name, "999", &match));
  g_assert_true (ct_contact_match (contact, name, "", &match));
}

/* what a signature covers: an added channel id changes it, a photo or a re-spaced, re-cased value does not */
static void
test_signed_text (void)
{
  const char *const unsigned_fields[] = { "VERSION", "REV", "PHOTO", "NOTE", "X-EVOLUTION-*", NULL };
  g_autoptr(EContact) a = e_contact_new_from_vcard ("BEGIN:VCARD\r\nVERSION:3.0\r\nUID:pas-1\r\nFN:Giuseppe\r\n"
                                                     "TEL:+39 345 1\r\nX-EVOLUTION-FILE-AS:G\r\nEND:VCARD\r\n");
  g_autoptr(EContact) b = e_contact_duplicate (a);
  g_autofree char *before = ct_contact_signed_text (a, unsigned_fields), *same = NULL, *changed = NULL;

  e_vcard_append_attribute_with_value (E_VCARD (b), e_vcard_attribute_new (NULL, "NOTE"), "una nota");
  e_vcard_remove_attributes (E_VCARD (b), NULL, "X-EVOLUTION-FILE-AS");
  same = ct_contact_signed_text (b, unsigned_fields);
  g_assert_cmpstr (before, ==, same);
  ct_contact_set_channel (b, "chan", "X-CHAN", &im_form, "25730854");
  changed = ct_contact_signed_text (b, unsigned_fields);
  g_assert_cmpstr (before, !=, changed);
  g_assert_nonnull (strstr (changed, "X-CHAN:25730854\n"));
}

/* "Collega <channel>": a chat picked for an existing contact goes through the editor's channel write. It adds the id
 * in the written form and leaves every other line as it was, the signed ones (X-MINERVA-OWNER-SIG, X-MINERVA-ROLE-*)
 * included; the same id once more writes nothing. */
static void
test_link_channel (void)
{
  g_autoptr(EContact) contact = e_contact_new_from_vcard (
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:pas-7\r\nFN:Maria Rossi\r\nN:Rossi;Maria;;;\r\nTEL;TYPE=CELL:+39 333 1234567\r\n"
    "X-CHAN:25730854\r\nX-MINERVA-ROLE-FAMILY:2026-11-01\r\nX-MINERVA-OWNER-SIG:MEUCIQDx7Yz\r\n"
    "NOTE:una nota\r\nEND:VCARD\r\n");
  g_autofree char *original = ct_contact_get_channel (contact, "echo", "X-ECHO", &im_form);
  g_autofree char *before = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  g_autofree char *after = NULL, *linked = NULL, *again = NULL;
  g_autoptr(GString) without = NULL;

  g_assert_cmpstr (original, ==, "");
  g_assert_true (ct_contact_apply_channel (contact, "echo", "X-ECHO", &im_form, original, " +393331234567 "));
  linked = ct_contact_get_channel (contact, "echo", "X-ECHO", &im_form);
  g_assert_cmpstr (linked, ==, "+393331234567");
  after = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  /* the card is the one it was, with the id's line and nothing else */
  without = g_string_new (after);
  g_assert_cmpuint (g_string_replace (without, "X-ECHO:+393331234567\r\n", "", 0), ==, 1);
  g_assert_cmpstr (without->str, ==, before);
  g_assert_false (ct_contact_apply_channel (contact, "echo", "X-ECHO", &im_form, linked, "+393331234567"));
  again = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  g_assert_cmpstr (again, ==, after);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/contact/display-names", test_display_names);
  g_test_add_func ("/contact/name-round-trip", test_name_round_trip);
  g_test_add_func ("/contact/sections", test_sections);
  g_test_add_func ("/contact/sort-order", test_sort_order);
  g_test_add_func ("/contact/starred-and-labels", test_starred_and_labels);
  g_test_add_func ("/contact/tile", test_tile);
  g_test_add_func ("/contact/phone-types", test_phone_types);
  g_test_add_func ("/contact/items-round-trip", test_items_round_trip);
  g_test_add_func ("/contact/organization", test_organization);
  g_test_add_func ("/contact/dates", test_dates);
  g_test_add_func ("/contact/channels", test_channels);
  g_test_add_func ("/contact/signed-text", test_signed_text);
  g_test_add_func ("/contact/link-channel", test_link_channel);
  g_test_add_func ("/contact/merge", test_merge);
  g_test_add_func ("/contact/merge-fills", test_merge_fills_what_is_missing);
  g_test_add_func ("/contact/word-prefix", test_word_prefix);
  g_test_add_func ("/contact/search", test_search);
  return g_test_run ();
}
