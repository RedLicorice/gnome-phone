#include "ct-mnv-core.h"
#include "ct-proof.h"

#include <stdlib.h>

static void
test_format (void)
{
  g_autofree char *a = ct_mnv_format ("Persona: {name}, {n} volti, {x}", (const char *const[]) { "name", "Andrea", "n", "3", NULL });
  g_autofree char *b = ct_mnv_format (NULL, NULL);

  g_assert_cmpstr (a, ==, "Persona: Andrea, 3 volti, {x}");
  g_assert_cmpstr (b, ==, "");
}

static void
test_loose (void)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GPtrArray) loose = NULL;
  JsonObject *root;

  g_assert_true (json_parser_load_from_data (parser,
    "{\"people\":[{\"id\":\"a\",\"contact_uid\":\"u1\"},{\"id\":\"b\",\"contact_uid\":\"\"},"
    "{\"id\":\"c\"},{\"contact_uid\":\"\"}]}", -1, NULL));
  root = json_node_get_object (json_parser_get_root (parser));
  loose = ct_mnv_loose (root);
  g_assert_cmpuint (loose->len, ==, 2);
  g_assert_cmpstr (json_object_get_string_member (loose->pdata[0], "id"), ==, "b");
  g_assert_cmpstr (json_object_get_string_member (loose->pdata[1], "id"), ==, "c");
  g_clear_pointer (&loose, g_ptr_array_unref);
  loose = ct_mnv_loose (NULL);
  g_assert_cmpuint (loose->len, ==, 0);
}

static void
test_name_stale (void)
{
  g_assert_false (ct_mnv_name_stale ("Andrea", "Andrea"));
  g_assert_false (ct_mnv_name_stale (" Andrea ", "Andrea  "));
  g_assert_true (ct_mnv_name_stale ("Andrea", "Andrea Rossi"));
  g_assert_false (ct_mnv_name_stale ("Andrea", ""));     /* a contact with no name never wipes the archive's */
  g_assert_false (ct_mnv_name_stale ("Andrea", NULL));
  g_assert_true (ct_mnv_name_stale (NULL, "Andrea"));
}

static void
test_rule_value (void)
{
  char *value;

  g_assert_true (ct_mnv_rule_value ("30", 1, 365, TRUE, &value));
  g_assert_cmpstr (value, ==, "30");
  g_free (value);
  g_assert_false (ct_mnv_rule_value ("30.5", 1, 365, TRUE, &value));
  g_assert_null (value);
  g_assert_false (ct_mnv_rule_value ("0", 1, 365, TRUE, &value));
  g_assert_false (ct_mnv_rule_value ("366", 1, 365, TRUE, &value));
  g_assert_true (ct_mnv_rule_value (" 0,05 ", 0, 1, FALSE, &value));
  g_assert_cmpstr (value, ==, "0.05");
  g_free (value);
  g_assert_true (ct_mnv_rule_value ("1", 0, 1, FALSE, &value));
  g_assert_cmpstr (value, ==, "1.00");
  g_free (value);
  g_assert_false (ct_mnv_rule_value ("1.01", 0, 1, FALSE, &value));
  g_assert_false (ct_mnv_rule_value ("abc", 0, 1, FALSE, &value));
  g_assert_false (ct_mnv_rule_value ("", 0, 1, FALSE, &value));
  g_assert_false (ct_mnv_rule_value ("nan", 0, 1, FALSE, &value));
  g_assert_false (ct_mnv_rule_value ("0.5x", 0, 1, FALSE, &value));
}

static void
test_date (void)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonObject *o;
  g_autofree char *a = NULL, *b = NULL, *c = NULL, *d = NULL;

  g_assert_true (json_parser_load_from_data (parser,
    "{\"unix\":1700000000,\"ms\":1700000000000,\"iso\":\"2023-11-14\",\"zero\":0,\"empty\":\"\"}", -1, NULL));
  o = json_node_get_object (json_parser_get_root (parser));
  a = ct_mnv_date (o, "unix", "%Y");
  b = ct_mnv_date (o, "ms", "%Y");
  c = ct_mnv_date (o, "iso", "%d/%m/%Y");
  d = ct_mnv_date (o, "zero", "%Y");
  g_assert_cmpstr (a, ==, "2023");
  g_assert_cmpstr (b, ==, "2023");
  g_assert_cmpstr (c, ==, "14/11/2023");
  g_assert_null (d);
  g_assert_null (ct_mnv_date (o, "empty", "%Y"));
  g_assert_null (ct_mnv_date (o, "missing", "%Y"));
}

static JsonObject *
parse (JsonParser *parser, const char *text)
{
  g_assert_true (json_parser_load_from_data (parser, text, -1, NULL));
  return json_node_get_object (json_parser_get_root (parser));
}

static void
test_owner_and_kinds (void)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonObject *known = parse (parser,
    "{\"owner\":\"giuseppe\",\"people\":["
    "{\"id\":\"giuseppe\",\"kind\":\"person\",\"contact_uid\":\"u-g\"},"
    "{\"id\":\"anna\",\"kind\":\"person\",\"contact_uid\":\"u-a\"},"
    "{\"id\":\"sconosciuto-1\",\"kind\":\"person\",\"contact_uid\":\"\"},"
    "{\"id\":\"rex\",\"kind\":\"pet\",\"owner_contact\":\"u-g\"},"
    "{\"id\":\"tazza\",\"kind\":\"object\",\"owner_contact\":\"u-g\"},"
    "{\"id\":\"fido\",\"kind\":\"pet\",\"owner_contact\":\"\"}]}");
  g_autoptr(GPtrArray) owned = ct_mnv_owned_by (known, "u-g");
  g_autoptr(GPtrArray) pets = ct_mnv_of_kind (known, "pet", FALSE);
  g_autoptr(GPtrArray) loose = ct_mnv_of_kind (known, "person", TRUE);
  g_autoptr(GPtrArray) none = ct_mnv_owned_by (known, "");
  JsonArray *people = json_object_get_array_member (known, "people");

  g_assert_cmpstr (ct_mnv_owner_contact (known), ==, "u-g");
  g_assert_cmpstr (ct_mnv_owner_contact (NULL), ==, "");
  g_assert_true (ct_mnv_is_owner (known, json_array_get_object_element (people, 0)));
  g_assert_false (ct_mnv_is_owner (known, json_array_get_object_element (people, 1)));
  g_assert_cmpuint (owned->len, ==, 2);
  g_assert_cmpuint (none->len, ==, 0);
  g_assert_cmpuint (pets->len, ==, 2);
  g_assert_cmpuint (loose->len, ==, 1);
  g_assert_cmpstr (json_object_get_string_member (loose->pdata[0], "id"), ==, "sconosciuto-1");
}

static void
proof_done (gboolean ok, const char *why, gpointer data)
{
  *(int *) data = ok ? 1 : 2;
}

static void
test_proof (void)
{
  g_autofree char *del = ct_proof_action (CT_PROOF_DELETE, "pas-id-1", NULL);
  g_autofree char *merge = ct_proof_action (CT_PROOF_MERGE, "a", "b");
  g_autofree char *json = ct_proof_json ("giuseppe", merge);
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonObject *o = parse (parser, json);
  CtProofRequest request = { CT_PROOF_FINGER, "Confermi?", 30, 0, NULL };
  int answer = 0;

  /* the sentences the memory service compares the proof with */
  g_assert_cmpstr (del, ==, "delete contact pas-id-1");
  g_assert_cmpstr (merge, ==, "merge contact a into b");
  g_assert_cmpstr (json_object_get_string_member (o, "approver"), ==, "giuseppe");
  g_assert_cmpstr (json_object_get_string_member (o, "action"), ==, "merge contact a into b");
  g_assert_true (g_regex_match_simple ("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ$",
                                       json_object_get_string_member (o, "at"), 0, 0));
  /* the development provider answers at once, and remembers what the sheet would have said */
  ct_proof_set_provider (ct_proof_dev_provider ());
  ct_proof_ask (NULL, &request, proof_done, &answer);
  g_assert_cmpint (answer, ==, 1);
  g_assert_cmpstr (ct_proof_dev_last ()->reason, ==, "Confermi?");
  ct_proof_dev_refuse (TRUE);
  ct_proof_ask (NULL, &request, proof_done, &answer);
  g_assert_cmpint (answer, ==, 2);
  ct_proof_dev_refuse (FALSE);
  ct_proof_set_provider (NULL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mnv/format", test_format);
  g_test_add_func ("/mnv/loose", test_loose);
  g_test_add_func ("/mnv/name-stale", test_name_stale);
  g_test_add_func ("/mnv/date", test_date);
  g_test_add_func ("/mnv/owner-and-kinds", test_owner_and_kinds);
  g_test_add_func ("/mnv/proof", test_proof);
  g_test_add_func ("/mnv/rule-value", test_rule_value);
  return g_test_run ();
}
