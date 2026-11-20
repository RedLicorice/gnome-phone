/* The vCard side of a contact (ct-contact.h). */
#include "ct-contact.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>

/* --- small string helpers */

static char *
dup_or_empty (const char *s)
{
  return g_strdup (s ? s : "");
}

static gboolean
is_blank (const char *s)
{
  if (!s)
    return TRUE;
  for (; *s; s = g_utf8_next_char (s))
    if (!g_unichar_isspace (g_utf8_get_char (s)))
      return FALSE;
  return TRUE;
}

static char *
trimmed (const char *s)
{
  return g_strstrip (g_strdup (s ? s : ""));
}

/* the X-<name> property's value, NULL when absent or empty */
static char *
x_value (EContact *contact, const char *name)
{
  char *value = e_vcard_util_dup_x_attribute (E_VCARD (contact), name);

  if (is_blank (value))
    g_clear_pointer (&value, g_free);
  return value;
}

/* a property's TYPE parameter as a set of tokens, as written (a custom label keeps its case); TYPE=HOME,VOICE and a 2.1 card's bare
 * ;HOME;VOICE both come here (EVCard reads bare parameters as TYPE values) */
static GPtrArray *
type_tokens (EVCardAttribute *attr)
{
  GPtrArray *tokens = g_ptr_array_new_with_free_func (g_free);
  GList *values = e_vcard_attribute_get_param (attr, EVC_TYPE);

  for (GList *l = values; l; l = l->next)
    {
      /* "TYPE=HOME,VOICE" may come as one value */
      g_auto(GStrv) parts = g_strsplit (l->data, ",", -1);

      for (int i = 0; parts[i]; i++)
        if (*g_strstrip (parts[i]))
          g_ptr_array_add (tokens, g_strdup (parts[i]));
    }
  return tokens;
}

/* vCard parameter values are case-insensitive */
static gboolean
has_token (GPtrArray *tokens, const char *token)
{
  for (guint i = 0; i < tokens->len; i++)
    if (g_ascii_strcasecmp (tokens->pdata[i], token) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
in_list (const char *const *list, const char *token)
{
  for (int i = 0; list[i]; i++)
    if (g_ascii_strcasecmp (list[i], token) == 0)
      return TRUE;
  return FALSE;
}

/* --- names */

CtName *
ct_name_new (void)
{
  CtName *name = g_new0 (CtName, 1);

  name->prefix = g_strdup ("");
  name->given = g_strdup ("");
  name->middle = g_strdup ("");
  name->family = g_strdup ("");
  name->suffix = g_strdup ("");
  name->phonetic_given = g_strdup ("");
  name->phonetic_middle = g_strdup ("");
  name->phonetic_family = g_strdup ("");
  return name;
}

void
ct_name_free (CtName *name)
{
  if (!name)
    return;
  g_free (name->prefix);
  g_free (name->given);
  g_free (name->middle);
  g_free (name->family);
  g_free (name->suffix);
  g_free (name->phonetic_given);
  g_free (name->phonetic_middle);
  g_free (name->phonetic_family);
  g_free (name);
}

gboolean
ct_name_is_empty (const CtName *name)
{
  return is_blank (name->prefix) && is_blank (name->given) && is_blank (name->middle) && is_blank (name->family)
         && is_blank (name->suffix) && is_blank (name->phonetic_given) && is_blank (name->phonetic_middle)
         && is_blank (name->phonetic_family);
}

gboolean
ct_name_equal (const CtName *a, const CtName *b)
{
  return g_str_equal (a->prefix, b->prefix) && g_str_equal (a->given, b->given) && g_str_equal (a->middle, b->middle)
         && g_str_equal (a->family, b->family) && g_str_equal (a->suffix, b->suffix)
         && g_str_equal (a->phonetic_given, b->phonetic_given)
         && g_str_equal (a->phonetic_middle, b->phonetic_middle)
         && g_str_equal (a->phonetic_family, b->phonetic_family);
}

static void
replace (char **field, char *value)
{
  g_free (*field);
  *field = value ? value : g_strdup ("");
}

CtName *
ct_contact_get_name (EContact *contact)
{
  CtName *name = ct_name_new ();
  EContactName *n = e_contact_get (contact, E_CONTACT_NAME);

  if (n)
    {
      replace (&name->prefix, trimmed (n->prefixes));
      replace (&name->given, trimmed (n->given));
      replace (&name->middle, trimmed (n->additional));
      replace (&name->family, trimmed (n->family));
      replace (&name->suffix, trimmed (n->suffixes));
      e_contact_name_free (n);
    }
  replace (&name->phonetic_given, x_value (contact, "X-PHONETIC-FIRST-NAME"));
  replace (&name->phonetic_middle, x_value (contact, "X-PHONETIC-MIDDLE-NAME"));
  replace (&name->phonetic_family, x_value (contact, "X-PHONETIC-LAST-NAME"));
  return name;
}

static void
set_x (EContact *contact, const char *x_name, const char *value)
{
  g_autofree char *clean = trimmed (value);

  /* an empty value removes the property */
  e_vcard_util_set_x_attribute (E_VCARD (contact), x_name, *clean ? clean : NULL);
}

void
ct_contact_set_name (EContact *contact, const CtName *name)
{
  g_autofree char *primary = ct_name_join (name, CT_ORDER_PRIMARY);
  g_autofree char *alternative = ct_name_join (name, CT_ORDER_ALTERNATIVE);

  if (is_blank (name->prefix) && is_blank (name->given) && is_blank (name->middle) && is_blank (name->family)
      && is_blank (name->suffix))
    e_vcard_remove_attributes (E_VCARD (contact), NULL, EVC_N);
  else
    {
      EContactName n = { 0 };
      g_autofree char *family = trimmed (name->family), *given = trimmed (name->given);
      g_autofree char *middle = trimmed (name->middle), *prefix = trimmed (name->prefix);
      g_autofree char *suffix = trimmed (name->suffix);

      n.family = family;
      n.given = given;
      n.additional = middle;
      n.prefixes = prefix;
      n.suffixes = suffix;
      e_contact_set (contact, E_CONTACT_NAME, &n);
    }
  /* FN is required by vCard 3.0; the store fills it from the display name's other sources when there is no name */
  e_contact_set (contact, E_CONTACT_FULL_NAME, primary);
  e_contact_set (contact, E_CONTACT_FILE_AS, *alternative ? alternative : NULL);
  set_x (contact, "X-PHONETIC-FIRST-NAME", name->phonetic_given);
  set_x (contact, "X-PHONETIC-MIDDLE-NAME", name->phonetic_middle);
  set_x (contact, "X-PHONETIC-LAST-NAME", name->phonetic_family);
}

/* NameSplitter.join, Western style (spec §10.5): "Prefix Given Middle Family, Suffix" given-first (PRIMARY), and
 * "Family, Prefix Given Middle, Suffix" family-first (ALTERNATIVE). Empty parts and their separators are left out. */
static void
append_words (GString *out, const char *const *parts, int n)
{
  for (int i = 0; i < n; i++)
    {
      g_autofree char *part = trimmed (parts[i]);

      if (*part)
        g_string_append_printf (out, "%s%s", out->len ? " " : "", part);
    }
}

char *
ct_name_join (const CtName *name, CtNameOrder order)
{
  GString *out = g_string_new (NULL);
  g_autofree char *suffix = trimmed (name->suffix);

  if (order == CT_ORDER_PRIMARY)
    append_words (out, (const char *const[]) { name->prefix, name->given, name->middle, name->family }, 4);
  else
    {
      g_autofree char *family = trimmed (name->family);
      g_autoptr(GString) rest = g_string_new (NULL);

      append_words (rest, (const char *const[]) { name->prefix, name->given, name->middle }, 3);
      g_string_append (out, family);
      if (rest->len)
        g_string_append_printf (out, "%s%s", out->len ? ", " : "", rest->str);
    }
  if (*suffix)
    g_string_append_printf (out, "%s%s", out->len ? ", " : "", suffix);
  return g_string_free (out, FALSE);
}

static char *
first_value (EContact *contact, const char *attr_name)
{
  EVCardAttribute *attr = e_vcard_get_attribute (E_VCARD (contact), attr_name);
  char *value;

  if (!attr)
    return NULL;
  value = e_vcard_attribute_get_value (attr);
  if (is_blank (value))
    g_clear_pointer (&value, g_free);
  return value ? g_strstrip (value) : NULL;
}

/* A card that has only FN (made by some other program): Android would split it into a structured name
 * (NameSplitter.split); this splits it the simple Western way, the last word being the family name, so that the
 * family-first form and the sort by last name still work. */
static CtName *
name_from_full_name (const char *full)
{
  CtName *name = ct_name_new ();
  g_auto(GStrv) words = g_strsplit_set (full, " \t", -1);
  GPtrArray *kept = g_ptr_array_new ();
  g_autoptr(GString) middle = g_string_new (NULL);

  for (int i = 0; words[i]; i++)
    if (*words[i])
      g_ptr_array_add (kept, words[i]);
  if (kept->len == 1)
    replace (&name->given, g_strdup (kept->pdata[0]));
  else if (kept->len > 1)
    {
      replace (&name->given, g_strdup (kept->pdata[0]));
      replace (&name->family, g_strdup (kept->pdata[kept->len - 1]));
      for (guint i = 1; i + 1 < kept->len; i++)
        g_string_append_printf (middle, "%s%s", middle->len ? " " : "", (char *) kept->pdata[i]);
      replace (&name->middle, g_strdup (middle->str));
    }
  g_ptr_array_unref (kept);
  return name;
}

char *
ct_contact_phonetic_name (EContact *contact)
{
  g_autoptr(CtName) name = ct_contact_get_name (contact);
  GString *out = g_string_new (NULL);
  const char *parts[] = { name->phonetic_family, name->phonetic_middle, name->phonetic_given };

  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    if (*parts[i])
      g_string_append_printf (out, "%s%s", out->len ? " " : "", parts[i]);
  if (!out->len)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}

char *
ct_contact_display_name (EContact *contact, CtNameOrder order)
{
  g_autoptr(CtName) name = ct_contact_get_name (contact);
  char *value = ct_name_join (name, order);

  if (*value)
    return value;
  g_free (value);
  /* the next source in DISPLAY_NAME_SOURCE's order: FN of a card without N (a name all the same, so before the
   * phonetic name), then phonetic name, nickname, organization, phone, email */
  if ((value = first_value (contact, EVC_FN)))
    {
      g_autoptr(CtName) split = name_from_full_name (value);

      if (order == CT_ORDER_PRIMARY)
        return value;
      g_free (value);
      return ct_name_join (split, order);
    }
  if ((value = ct_contact_phonetic_name (contact)))
    return value;
  if ((value = first_value (contact, EVC_NICKNAME)))
    return value;
  {
    EVCardAttribute *org = e_vcard_get_attribute (E_VCARD (contact), EVC_ORG);

    if (org)
      {
        GList *values = e_vcard_attribute_get_values (org);

        if (values && !is_blank (values->data))
          return trimmed (values->data);
      }
  }
  if ((value = first_value (contact, EVC_TEL)))
    return value;
  if ((value = first_value (contact, EVC_EMAIL)))
    return value;
  return g_strdup ("");
}

char *
ct_contact_first_name (EContact *contact, CtNameOrder order)
{
  g_autoptr(CtName) name = ct_contact_get_name (contact);

  if (*name->given)
    return g_strdup (name->given);
  return ct_contact_display_name (contact, order);
}

/* --- sorting and sections */

/* the base letter of a character: NFD, then the first code point ("Ä" → "A", "ç" → "c"); the letters that do not
 * decompose but that the collation treats as a variant of a Latin letter, as ICU's root collation does */
static gunichar
base_char (gunichar c)
{
  static const struct { gunichar from, to; } variants[] = {
    { 0x00D8, 'O' }, { 0x00F8, 'o' },   /* Ø */
    { 0x0110, 'D' }, { 0x0111, 'd' },   /* Đ */
    { 0x0141, 'L' }, { 0x0142, 'l' },   /* Ł */
    { 0x00C6, 'A' }, { 0x00E6, 'a' },   /* Æ */
    { 0x0152, 'O' }, { 0x0153, 'o' },   /* Œ */
    { 0x00DF, 's' },                    /* ß */
    { 0x0126, 'H' }, { 0x0127, 'h' },   /* Ħ */
    { 0x0131, 'i' },                    /* dotless ı */
  };
  gunichar decomposed[G_UNICHAR_MAX_DECOMPOSITION_LENGTH];
  gsize n;

  for (guint i = 0; i < G_N_ELEMENTS (variants); i++)
    if (c == variants[i].from)
      return variants[i].to;
  n = g_unichar_fully_decompose (c, FALSE, decomposed, G_N_ELEMENTS (decomposed));
  return n > 0 ? decomposed[0] : c;
}

/* Greek and Cyrillic have buckets of their own: ContactLocaleUtils adds the Greek, Ukrainian and Serbian alphabets
 * to every index */
static gboolean
has_own_buckets (gunichar c)
{
  GUnicodeScript script = g_unichar_get_script (c);

  return script == G_UNICODE_SCRIPT_GREEK || script == G_UNICODE_SCRIPT_CYRILLIC;
}

/* ContactLocaleUtils.getBucketIndex: a name whose first character after spaces and + ( ) . - # is a digit goes to
 * the number bucket "#"; otherwise ICU's AlphabeticIndex decides: a Latin letter is its letter's bucket, a symbol,
 * an emoji or punctuation sorts before every letter and so lands in the underflow bucket "…", a letter of a script
 * the index has no labels for in the overflow bucket "…" at the very end. The number bucket goes just before the
 * overflow one (mNumberBucketIndex). */
static int
bucket_of (const char *sort_name, gunichar *letter)
{
  *letter = 0;
  if (!sort_name)
    return CT_BUCKET_UNDERFLOW;
  for (const char *p = sort_name; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isdigit (c))
        return CT_BUCKET_NUMBER;
      if (!g_unichar_isspace (c) && !strchr ("+().-#", (int) c))
        break;
    }
  while (*sort_name && g_unichar_isspace (g_utf8_get_char (sort_name)))
    sort_name = g_utf8_next_char (sort_name);
  if (!*sort_name)
    return CT_BUCKET_UNDERFLOW;            /* no name: BLANK_HEADER_STRING, which is "…" too */
  {
    gunichar c = g_utf8_get_char (sort_name);
    gunichar base = g_unichar_toupper (base_char (c));

    if (base >= 'A' && base <= 'Z')
      {
        *letter = base;
        return CT_BUCKET_LATIN + (int) (base - 'A');
      }
    if (g_unichar_isalpha (c) && has_own_buckets (c))
      {
        *letter = base;
        return CT_BUCKET_OTHER_ALPHABETS + (int) base;
      }
    return g_unichar_isalpha (c) ? CT_BUCKET_OVERFLOW : CT_BUCKET_UNDERFLOW;
  }
}

int
ct_section_rank (const char *sort_name)
{
  gunichar letter;

  return bucket_of (sort_name, &letter);
}

const char *
ct_section_of (const char *sort_name)
{
  gunichar letter;
  int bucket = bucket_of (sort_name, &letter);
  char text[8] = { 0 };

  if (bucket == CT_BUCKET_NUMBER)
    return g_intern_static_string ("#");
  if (!letter)
    return g_intern_static_string ("…");
  g_unichar_to_utf8 (letter, text);
  return g_intern_string (text);
}

char *
ct_sort_key (const char *sort_name)
{
  g_autofree char *folded = g_utf8_casefold (sort_name ? sort_name : "", -1);
  g_autofree char *collated = g_utf8_collate_key (folded, -1);

  /* the bucket first, as CP2 orders by phonebook_bucket before the sort key: "…" symbols before A, "#" after the
   * alphabets, whatever the collation says about them */
  return g_strdup_printf ("%07d%s", ct_section_rank (sort_name), collated);
}

/* --- favourites and labels */

static GList *
categories (EContact *contact)
{
  return e_contact_get (contact, E_CONTACT_CATEGORY_LIST);
}

static gboolean
is_favourite_category (const char *category)
{
  return g_str_equal (category, CT_CATEGORY_STARRED);
}

static void
set_categories (EContact *contact, GList *list)
{
  if (list)
    e_contact_set (contact, E_CONTACT_CATEGORY_LIST, list);
  else
    e_vcard_remove_attributes (E_VCARD (contact), NULL, EVC_CATEGORIES);
}

gboolean
ct_contact_is_starred (EContact *contact)
{
  GList *list = categories (contact);
  g_autofree char *folks = x_value (contact, "X-FOLKS-FAVOURITE");
  gboolean starred = folks && g_ascii_strcasecmp (folks, "true") == 0;

  for (GList *l = list; l && !starred; l = l->next)
    starred = is_favourite_category (l->data);
  g_list_free_full (list, g_free);
  return starred;
}

void
ct_contact_set_starred (EContact *contact, gboolean starred)
{
  GList *list = categories (contact), *kept = NULL;

  for (GList *l = list; l; l = l->next)
    if (!is_favourite_category (l->data))
      kept = g_list_append (kept, l->data);
  if (starred)
    kept = g_list_append (kept, (gpointer) CT_CATEGORY_STARRED);
  set_categories (contact, kept);
  g_list_free (kept);
  g_list_free_full (list, g_free);
  /* folks' own flag, so that GNOME Contacts and anything else on folks agree */
  e_vcard_util_set_x_attribute (E_VCARD (contact), "X-FOLKS-FAVOURITE", starred ? "true" : NULL);
}

GPtrArray *
ct_contact_dup_labels (EContact *contact)
{
  GPtrArray *labels = g_ptr_array_new_with_free_func (g_free);
  GList *list = categories (contact);

  for (GList *l = list; l; l = l->next)
    if (!is_favourite_category (l->data) && !is_blank (l->data))
      g_ptr_array_add (labels, g_strdup (l->data));
  g_list_free_full (list, g_free);
  return labels;
}

void
ct_contact_set_labels (EContact *contact, GPtrArray *labels)
{
  gboolean starred = FALSE;
  GList *list = categories (contact), *out = NULL;

  for (GList *l = list; l; l = l->next)
    starred |= is_favourite_category (l->data);
  for (guint i = 0; labels && i < labels->len; i++)
    out = g_list_append (out, labels->pdata[i]);
  if (starred)
    out = g_list_append (out, (gpointer) CT_CATEGORY_STARRED);
  set_categories (contact, out);
  g_list_free (out);
  g_list_free_full (list, g_free);
}

static gboolean
find_label (GPtrArray *labels, const char *label, guint *index)
{
  for (guint i = 0; i < labels->len; i++)
    if (g_str_equal (labels->pdata[i], label))
      {
        if (index)
          *index = i;
        return TRUE;
      }
  return FALSE;
}

gboolean
ct_contact_has_label (EContact *contact, const char *label)
{
  g_autoptr(GPtrArray) labels = ct_contact_dup_labels (contact);

  return find_label (labels, label, NULL);
}

gboolean
ct_contact_add_label (EContact *contact, const char *label)
{
  g_autoptr(GPtrArray) labels = ct_contact_dup_labels (contact);

  if (find_label (labels, label, NULL))
    return FALSE;
  g_ptr_array_add (labels, g_strdup (label));
  ct_contact_set_labels (contact, labels);
  return TRUE;
}

gboolean
ct_contact_remove_label (EContact *contact, const char *label)
{
  g_autoptr(GPtrArray) labels = ct_contact_dup_labels (contact);
  guint index;

  if (!find_label (labels, label, &index))
    return FALSE;
  g_ptr_array_remove_index (labels, index);
  ct_contact_set_labels (contact, labels);
  return TRUE;
}

gboolean
ct_contact_rename_label (EContact *contact, const char *from, const char *to)
{
  g_autoptr(GPtrArray) labels = ct_contact_dup_labels (contact);
  guint index;

  if (!find_label (labels, from, &index))
    return FALSE;
  if (find_label (labels, to, NULL))
    g_ptr_array_remove_index (labels, index);
  else
    {
      g_free (labels->pdata[index]);
      labels->pdata[index] = g_strdup (to);
    }
  ct_contact_set_labels (contact, labels);
  return TRUE;
}

/* --- the letter tile */

gint32
ct_java_hash (const char *key)
{
  glong n = 0;
  g_autofree gunichar2 *utf16 = key ? g_utf8_to_utf16 (key, -1, NULL, &n, NULL) : NULL;
  guint32 hash = 0;

  /* h = 31 * h + c over the UTF-16 code units, wrapping as Java's int does */
  for (glong i = 0; utf16 && i < n; i++)
    hash = 31 * hash + utf16[i];
  return (gint32) hash;
}

int
ct_tile_color_index (const char *key)
{
  gint32 hash;

  if (!key || !*key)
    return -1;
  hash = ct_java_hash (key);
  if (hash == G_MININT32)
    return -1;
  return ABS (hash) % 12;
}

gunichar
ct_tile_letter (const char *display_name)
{
  gunichar c;

  if (!display_name || !*display_name)
    return 0;
  c = g_utf8_get_char (display_name);
  /* LetterTileDrawable: a char (UTF-16 unit) that Character.isLetter; one outside the BMP is a surrogate there */
  if (c > 0xffff || !g_unichar_isalpha (c))
    return 0;
  return g_unichar_toupper (c);
}

/* --- types */

static const CtType phone_types[] = {
  /* the Device account's spinner (FallbackAccountType.addDataKindPhone, §4.10.1) */
  { 2, N_("Mobile"), TRUE },
  { 3, N_("Work"), TRUE },
  { 1, N_("Home"), TRUE },
  { 12, N_("Main"), TRUE },
  { 4, N_("Work Fax"), TRUE },
  { 5, N_("Home Fax"), TRUE },
  { 6, N_("Pager"), TRUE },
  { 7, N_("Other"), TRUE },
  { CT_TYPE_CUSTOM, N_("Custom"), TRUE },
  /* the rest of BaseAccountType's list: shown on the card, kept when the row is not edited */
  { 8, N_("Callback"), FALSE },
  { 9, N_("Car"), FALSE },
  { 10, N_("Company Main"), FALSE },
  { 11, N_("ISDN"), FALSE },
  { 13, N_("Other Fax"), FALSE },
  { 14, N_("Radio"), FALSE },
  { 15, N_("Telex"), FALSE },
  { 16, N_("TTY TDD"), FALSE },
  { 17, N_("Work Mobile"), FALSE },
  { 18, N_("Work Pager"), FALSE },
  { 19, N_("Assistant"), FALSE },
  { 20, N_("MMS"), FALSE },
  { 0, NULL, FALSE },
};

static const CtType email_types[] = {
  { 1, N_("Home"), TRUE },
  { 2, N_("Work"), TRUE },
  { 3, N_("Other"), TRUE },
  { CT_TYPE_CUSTOM, N_("Custom"), TRUE },
  { 4, N_("Mobile"), FALSE },
  { 0, NULL, FALSE },
};

static const CtType address_types[] = {
  { 1, N_("Home"), TRUE },
  { 2, N_("Work"), TRUE },
  { 3, N_("Other"), TRUE },
  { CT_TYPE_CUSTOM, N_("Custom"), TRUE },
  { 0, NULL, FALSE },
};

/* IM's TYPE (the legacy section shows the protocol, not this) */
static const CtType im_types[] = {
  { 1, N_("Home"), FALSE },
  { 2, N_("Work"), FALSE },
  { 3, N_("Other"), FALSE },
  { CT_TYPE_CUSTOM, N_("Custom"), FALSE },
  { 0, NULL, FALSE },
};

static const CtType event_types[] = {
  { 3, N_("Birthday"), TRUE },
  { 1, N_("Anniversary"), TRUE },
  { 2, N_("Other"), TRUE },
  { CT_TYPE_CUSTOM, N_("Custom"), TRUE },
  { 0, NULL, FALSE },
};

static const CtType relation_types[] = {
  { 1, N_("Assistant"), TRUE },
  { 2, N_("Brother"), TRUE },
  { 3, N_("Child"), TRUE },
  { 4, N_("Domestic Partner"), TRUE },
  { 5, N_("Father"), TRUE },
  { 6, N_("Friend"), TRUE },
  { 7, N_("Manager"), TRUE },
  { 8, N_("Mother"), TRUE },
  { 9, N_("Parent"), TRUE },
  { 10, N_("Partner"), TRUE },
  { 11, N_("Referred by"), TRUE },
  { 12, N_("Relative"), TRUE },
  { 13, N_("Sister"), TRUE },
  { 14, N_("Spouse"), TRUE },
  { CT_TYPE_CUSTOM, N_("Custom"), TRUE },
  { 0, NULL, FALSE },
};

const CtType *
ct_kind_types (CtKind kind)
{
  switch (kind)
    {
    case CT_KIND_PHONE: return phone_types;
    case CT_KIND_EMAIL: return email_types;
    case CT_KIND_ADDRESS: return address_types;
    case CT_KIND_IM: return im_types;
    case CT_KIND_EVENT: return event_types;
    case CT_KIND_RELATION: return relation_types;
    default: return NULL;
    }
}

const char *
ct_type_label (CtKind kind, int type, const char *custom_label)
{
  const CtType *types = ct_kind_types (kind);

  if (type == CT_TYPE_CUSTOM && !is_blank (custom_label))
    return custom_label;
  for (int i = 0; types && types[i].label; i++)
    if (types[i].type == type)
      return _(types[i].label);
  return "";
}

int
ct_kind_default_type (CtKind kind)
{
  switch (kind)
    {
    case CT_KIND_PHONE: return 2;           /* Mobile: the first unused primary type, §4.13 */
    case CT_KIND_EMAIL: return 1;           /* Home */
    case CT_KIND_ADDRESS: return 1;
    case CT_KIND_EVENT: return 3;           /* Birthday */
    case CT_KIND_RELATION: return 14;       /* Spouse */
    case CT_KIND_WEBSITE: return 7;         /* Website.TYPE_OTHER */
    default: return 0;
    }
}

/* --- items */

CtItem *
ct_item_new (CtKind kind, int type, const char *value)
{
  CtItem *item = g_new0 (CtItem, 1);

  item->kind = kind;
  item->type = type;
  item->label = g_strdup ("");
  item->value = dup_or_empty (value);
  item->value2 = g_strdup ("");
  item->value3 = g_strdup ("");
  return item;
}

CtItem *
ct_item_copy (const CtItem *item)
{
  CtItem *copy = g_new0 (CtItem, 1);

  *copy = *item;
  copy->label = g_strdup (item->label);
  copy->value = g_strdup (item->value);
  copy->value2 = g_strdup (item->value2);
  copy->value3 = g_strdup (item->value3);
  copy->source = item->source ? e_vcard_attribute_copy (item->source) : NULL;
  return copy;
}

void
ct_item_free (CtItem *item)
{
  if (!item)
    return;
  g_free (item->label);
  g_free (item->value);
  g_free (item->value2);
  g_free (item->value3);
  g_clear_pointer (&item->source, e_vcard_attribute_free);
  g_free (item);
}

gboolean
ct_item_equal (const CtItem *a, const CtItem *b)
{
  return a->kind == b->kind && a->type == b->type && !a->primary == !b->primary
         && g_str_equal (a->type == CT_TYPE_CUSTOM ? a->label : "", b->type == CT_TYPE_CUSTOM ? b->label : "")
         && g_str_equal (a->value, b->value) && g_str_equal (a->value2, b->value2)
         && g_str_equal (a->value3, b->value3);
}

gboolean
ct_item_is_empty (const CtItem *item)
{
  return is_blank (item->value) && is_blank (item->value2) && is_blank (item->value3);
}

/* the Android TYPE of a TEL's TYPE tokens (§10.3), and whether PREF made it the default number */
static int
phone_type_of (GPtrArray *tokens, char **custom, gboolean *primary)
{
  static const char *const known[] = {
    "HOME", "WORK", "CELL", "FAX", "PAGER", "VOICE", "CAR", "ISDN", "MSG", "PREF", "OTHER", "X-MAIN", "MAIN",
    "X-EVOLUTION-CALLBACK", "CALLBACK", "X-EVOLUTION-COMPANY", "COMPANY-MAIN", "X-EVOLUTION-RADIO", "RADIO",
    "X-EVOLUTION-TELEX", "TLX", "X-EVOLUTION-TTYTDD", "TTY-TDD", "X-EVOLUTION-ASSISTANT", "ASSISTANT",
    "INTERNET", "X-EVOLUTION-UI-SLOT", NULL,
  };
  gboolean pref = has_token (tokens, "PREF");
  guint others = 0;

  *primary = FALSE;
  for (guint i = 0; i < tokens->len; i++)
    {
      if (!in_list (known, tokens->pdata[i]))
        {
          /* an unknown token is a custom label (AOSP's import: minus a leading X-) */
          const char *label = tokens->pdata[i];

          *custom = g_strdup (g_ascii_strncasecmp (label, "X-", 2) == 0 ? label + 2 : label);
          *primary = pref;
          return CT_TYPE_CUSTOM;
        }
      if (g_ascii_strcasecmp (tokens->pdata[i], "PREF") != 0 && g_ascii_strcasecmp (tokens->pdata[i], "VOICE") != 0)
        others++;
    }
  *primary = pref && others > 0;
#define HAS(t) has_token (tokens, t)
  if (HAS ("FAX"))
    return HAS ("WORK") ? 4 : HAS ("HOME") ? 5 : 13;
  if (HAS ("PAGER"))
    return HAS ("WORK") ? 18 : 6;
  if (HAS ("CELL"))
    return HAS ("WORK") ? 17 : 2;
  if (HAS ("X-EVOLUTION-CALLBACK") || HAS ("CALLBACK"))
    return 8;
  if (HAS ("CAR"))
    return 9;
  if (HAS ("X-EVOLUTION-COMPANY") || HAS ("COMPANY-MAIN"))
    return 10;
  if (HAS ("ISDN"))
    return 11;
  if (HAS ("X-EVOLUTION-RADIO") || HAS ("RADIO"))
    return 14;
  if (HAS ("X-EVOLUTION-TELEX") || HAS ("TLX"))
    return 15;
  if (HAS ("X-EVOLUTION-TTYTDD") || HAS ("TTY-TDD"))
    return 16;
  if (HAS ("X-EVOLUTION-ASSISTANT") || HAS ("ASSISTANT"))
    return 19;
  if (HAS ("MSG"))
    return 20;
  if (HAS ("WORK"))
    return 3;
  if (HAS ("HOME"))
    return 1;
  if (HAS ("X-MAIN") || HAS ("MAIN"))
    return 12;
  /* a bare PREF is Main (EDS's E_CONTACT_PHONE_PRIMARY slot, AOSP's TYPE_MAIN) */
  if (pref)
    return 12;
#undef HAS
  return 7;
}

/* the TYPE tokens a phone type is written with (§10.3, EDS's slots where it has one) */
static const char *const *
phone_type_tokens (int type)
{
  static const char *const home[] = { "HOME", "VOICE", NULL }, *const cell[] = { "CELL", NULL };
  static const char *const work[] = { "WORK", "VOICE", NULL }, *const work_fax[] = { "WORK", "FAX", NULL };
  static const char *const home_fax[] = { "HOME", "FAX", NULL }, *const pager[] = { "PAGER", NULL };
  static const char *const other[] = { "VOICE", NULL }, *const callback[] = { "X-EVOLUTION-CALLBACK", NULL };
  static const char *const car[] = { "CAR", NULL }, *const company[] = { "X-EVOLUTION-COMPANY", NULL };
  static const char *const isdn[] = { "ISDN", NULL }, *const main_[] = { "PREF", NULL };
  static const char *const other_fax[] = { "FAX", NULL }, *const radio[] = { "X-EVOLUTION-RADIO", NULL };
  static const char *const telex[] = { "X-EVOLUTION-TELEX", NULL }, *const tty[] = { "X-EVOLUTION-TTYTDD", NULL };
  static const char *const work_cell[] = { "WORK", "CELL", NULL }, *const work_pager[] = { "WORK", "PAGER", NULL };
  static const char *const assistant[] = { "X-EVOLUTION-ASSISTANT", NULL }, *const mms[] = { "MSG", NULL };

  switch (type)
    {
    case 1: return home;
    case 2: return cell;
    case 3: return work;
    case 4: return work_fax;
    case 5: return home_fax;
    case 6: return pager;
    case 8: return callback;
    case 9: return car;
    case 10: return company;
    case 11: return isdn;
    case 12: return main_;
    case 13: return other_fax;
    case 14: return radio;
    case 15: return telex;
    case 16: return tty;
    case 17: return work_cell;
    case 18: return work_pager;
    case 19: return assistant;
    case 20: return mms;
    default: return other;
    }
}

/* Email and postal types (§10.4): HOME 1, WORK 2, OTHER 3 (also no TYPE at all), CELL 4 for email */
static int
simple_type_of (GPtrArray *tokens, gboolean email, char **custom, gboolean *primary)
{
  static const char *const ignored[] = { "INTERNET", "PREF", "X-EVOLUTION-UI-SLOT", "POSTAL", "PARCEL", "DOM",
                                         "INTL", "VOICE", NULL };

  *primary = has_token (tokens, "PREF");
  if (has_token (tokens, "HOME"))
    return 1;
  if (has_token (tokens, "WORK"))
    return 2;
  if (email && has_token (tokens, "CELL"))
    return 4;
  if (has_token (tokens, "OTHER"))
    return 3;
  for (guint i = 0; i < tokens->len; i++)
    if (!in_list (ignored, tokens->pdata[i]))
      {
        const char *label = tokens->pdata[i];

        *custom = g_strdup (g_ascii_strncasecmp (label, "X-", 2) == 0 ? label + 2 : label);
        return CT_TYPE_CUSTOM;
      }
  return 3;
}

static const char *
simple_type_token (int type, gboolean email)
{
  switch (type)
    {
    case 1: return "HOME";
    case 2: return "WORK";
    case 4: return email ? "CELL" : "OTHER";
    default: return "OTHER";
    }
}

/* the IM properties: Android's, EDS's and the few EDS adds (§10.2 Im) */
static const struct { const char *property; const char *protocol; } im_properties[] = {
  { "X-AIM", N_("AIM") },
  { "X-MSN", N_("Windows Live") },
  { "X-YAHOO", N_("Yahoo") },
  { "X-SKYPE", N_("Skype") },
  { "X-SKYPE-USERNAME", N_("Skype") },
  { "X-QQ", N_("QQ") },
  { "X-GOOGLE-TALK", N_("Hangouts") },
  { "X-ICQ", N_("ICQ") },
  { "X-JABBER", N_("Jabber") },
  { "X-NETMEETING", N_("NetMeeting") },
  { "X-GADUGADU", N_("Gadu-Gadu") },
  { "X-GROUPWISE", N_("GroupWise") },
  { "X-MATRIX", N_("Matrix") },
  { "X-WHATSAPP", N_("WhatsApp") },
  { "X-TELEGRAM", N_("Telegram") },
};

/* the protocol's name for an IMPP scheme: the known ones by name, the others as the scheme with a capital */
static const char *
impp_protocol (const char *scheme)
{
  static const struct { const char *scheme; const char *protocol; } known[] = {
    { "whatsapp", N_("WhatsApp") }, { "telegram", N_("Telegram") }, { "xmpp", N_("Jabber") },
    { "matrix", N_("Matrix") }, { "skype", N_("Skype") },
  };
  static char other[64];

  for (guint i = 0; i < G_N_ELEMENTS (known); i++)
    if (g_ascii_strcasecmp (scheme, known[i].scheme) == 0)
      return known[i].protocol;
  g_snprintf (other, sizeof other, "%s", scheme);
  if (other[0])
    other[0] = g_ascii_toupper (other[0]);
  return other;
}

#define ANDROID_CUSTOM "X-ANDROID-CUSTOM"
#define MIME_EVENT "vnd.android.cursor.item/contact_event"
#define MIME_RELATION "vnd.android.cursor.item/relation"

static char *
nth_value (EVCardAttribute *attr, guint n)
{
  GList *l = g_list_nth (e_vcard_attribute_get_values (attr), n);

  return trimmed (l ? l->data : NULL);
}

/* the formatted address of an ADR (CP2 formats one it did not write: street, PO box and neighbourhood on their
 * lines, then "City Region Postcode", then the country); the editor's own rows have all of it in the street */
static char *
format_address (EVCardAttribute *attr)
{
  g_autofree char *po = nth_value (attr, 0), *ext = nth_value (attr, 1), *street = nth_value (attr, 2);
  g_autofree char *city = nth_value (attr, 3), *region = nth_value (attr, 4), *code = nth_value (attr, 5);
  g_autofree char *country = nth_value (attr, 6);
  GString *out = g_string_new (NULL);
  GString *line = g_string_new (NULL);
  const char *lines[] = { street, ext, po };

  for (guint i = 0; i < G_N_ELEMENTS (lines); i++)
    if (*lines[i])
      g_string_append_printf (out, "%s%s", out->len ? "\n" : "", lines[i]);
  for (const char *const *p = (const char *const[]) { city, region, code, NULL }; *p; p++)
    if (**p)
      g_string_append_printf (line, "%s%s", line->len ? " " : "", *p);
  if (line->len)
    g_string_append_printf (out, "%s%s", out->len ? "\n" : "", line->str);
  if (*country)
    g_string_append_printf (out, "%s%s", out->len ? "\n" : "", country);
  g_string_free (line, TRUE);
  return g_string_free (out, FALSE);
}

/* the row a property is, for @kind, or NULL when the property is not one of @kind's */
static CtItem *
item_from_attribute (CtKind kind, EVCardAttribute *attr)
{
  const char *name = e_vcard_attribute_get_name (attr);
  g_autoptr(CtItem) item = NULL;
  g_autoptr(GPtrArray) tokens = NULL;
  char *custom = NULL;

  switch (kind)
    {
    case CT_KIND_PHONE:
      if (g_ascii_strcasecmp (name, EVC_TEL) != 0)
        return NULL;
      item = ct_item_new (kind, 0, NULL);
      replace (&item->value, nth_value (attr, 0));
      tokens = type_tokens (attr);
      item->type = phone_type_of (tokens, &custom, &item->primary);
      break;

    case CT_KIND_EMAIL:
      if (g_ascii_strcasecmp (name, EVC_EMAIL) != 0)
        return NULL;
      item = ct_item_new (kind, 0, NULL);
      replace (&item->value, nth_value (attr, 0));
      tokens = type_tokens (attr);
      item->type = simple_type_of (tokens, TRUE, &custom, &item->primary);
      break;

    case CT_KIND_ADDRESS:
      if (g_ascii_strcasecmp (name, EVC_ADR) != 0)
        return NULL;
      item = ct_item_new (kind, 0, NULL);
      replace (&item->value, format_address (attr));
      tokens = type_tokens (attr);
      item->type = simple_type_of (tokens, FALSE, &custom, &item->primary);
      break;

    case CT_KIND_IM:
      if (g_ascii_strcasecmp (name, "IMPP") == 0)
        {
          g_autofree char *whole = nth_value (attr, 0);
          const char *colon = strchr (whole, ':');

          /* IMPP:<scheme>:<id>, as Minerva's memory service writes it; the scheme names the protocol */
          item = ct_item_new (kind, 0, NULL);
          replace (&item->value, trimmed (colon ? colon + 1 : whole));
          if (colon && colon > whole)
            {
              g_autofree char *scheme = g_strndup (whole, colon - whole);

              replace (&item->value2, g_strdup (_(impp_protocol (scheme))));
            }
          tokens = type_tokens (attr);
          item->type = simple_type_of (tokens, FALSE, &custom, &item->primary);
          break;
        }
      for (guint i = 0; i < G_N_ELEMENTS (im_properties); i++)
        if (g_ascii_strcasecmp (name, im_properties[i].property) == 0)
          {
            item = ct_item_new (kind, 0, NULL);
            replace (&item->value, nth_value (attr, 0));
            replace (&item->value2, g_strdup (_(im_properties[i].protocol)));
            tokens = type_tokens (attr);
            item->type = simple_type_of (tokens, FALSE, &custom, &item->primary);
            break;
          }
      if (!item)
        return NULL;
      break;

    case CT_KIND_SIP:
      if (g_ascii_strcasecmp (name, EVC_X_SIP) != 0)
        return NULL;
      item = ct_item_new (kind, 0, NULL);
      replace (&item->value, nth_value (attr, 0));
      /* the sip: prefix is stripped (VCardBuilder.appendSipAddresses) */
      if (g_ascii_strncasecmp (item->value, "sip:", 4) == 0)
        replace (&item->value, g_strdup (item->value + 4));
      break;

    case CT_KIND_WEBSITE:
      if (g_ascii_strcasecmp (name, EVC_URL) != 0)
        return NULL;
      item = ct_item_new (kind, 7, NULL);
      replace (&item->value, nth_value (attr, 0));
      break;

    case CT_KIND_NICKNAME:
      if (g_ascii_strcasecmp (name, EVC_NICKNAME) != 0)
        return NULL;
      item = ct_item_new (kind, 1, NULL);
      replace (&item->value, nth_value (attr, 0));
      break;

    case CT_KIND_NOTE:
      if (g_ascii_strcasecmp (name, EVC_NOTE) != 0)
        return NULL;
      item = ct_item_new (kind, 0, NULL);
      replace (&item->value, nth_value (attr, 0));
      break;

    case CT_KIND_EVENT:
      if (g_ascii_strcasecmp (name, EVC_BDAY) == 0)
        item = ct_item_new (kind, 3, NULL);
      else if (g_ascii_strcasecmp (name, EVC_X_ANNIVERSARY) == 0 || g_ascii_strcasecmp (name, "ANNIVERSARY") == 0)
        item = ct_item_new (kind, 1, NULL);
      if (item)
        {
          replace (&item->value, nth_value (attr, 0));
          break;
        }
      G_GNUC_FALLTHROUGH;
    case CT_KIND_RELATION:
      {
        /* X-ANDROID-CUSTOM:<mime type>;data1;data2 (TYPE);data3 (LABEL);… (VCardBuilder.appendAndroidSpecificProperty) */
        g_autofree char *mime = NULL, *type = NULL;

        if (g_ascii_strcasecmp (name, ANDROID_CUSTOM) != 0)
          return NULL;
        mime = nth_value (attr, 0);
        if (!g_str_equal (mime, kind == CT_KIND_EVENT ? MIME_EVENT : MIME_RELATION))
          return NULL;
        item = ct_item_new (kind, 0, NULL);
        replace (&item->value, nth_value (attr, 1));
        type = nth_value (attr, 2);
        item->type = *type ? atoi (type) : ct_kind_default_type (kind);
        if (item->type == CT_TYPE_CUSTOM)
          custom = nth_value (attr, 3);
      }
      break;

    case CT_KIND_ORGANIZATION:
    default:
      return NULL;
    }
  if (custom)
    replace (&item->label, custom);
  item->source = e_vcard_attribute_copy (attr);
  return g_steal_pointer (&item);
}

/* Skype is X-SKYPE to EDS and X-SKYPE-USERNAME to Android; a card written for both has it twice */
static gboolean
has_item (GPtrArray *items, const CtItem *item)
{
  for (guint i = 0; i < items->len; i++)
    if (ct_item_equal (items->pdata[i], item))
      return TRUE;
  return FALSE;
}

GPtrArray *
ct_contact_get_items (EContact *contact, CtKind kind)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);

  if (kind == CT_KIND_ORGANIZATION)
    {
      /* one row: ORG's company and department, and TITLE */
      EVCardAttribute *org = e_vcard_get_attribute (E_VCARD (contact), EVC_ORG);
      g_autofree char *title = first_value (contact, EVC_TITLE);
      CtItem *item = ct_item_new (kind, 0, NULL);

      if (org)
        {
          replace (&item->value, nth_value (org, 0));
          replace (&item->value3, nth_value (org, 1));
        }
      replace (&item->value2, g_strdup (title));
      if (ct_item_is_empty (item))
        ct_item_free (item);
      else
        g_ptr_array_add (items, item);
      return items;
    }
  for (GList *l = e_vcard_get_attributes (E_VCARD (contact)); l; l = l->next)
    {
      CtItem *item = item_from_attribute (kind, l->data);

      if (item && (ct_item_is_empty (item) || (kind == CT_KIND_IM && has_item (items, item))))
        ct_item_free (item);
      else if (item)
        g_ptr_array_add (items, item);
    }
  return items;
}

static void
add_type_param (EVCardAttribute *attr, const char *const *tokens)
{
  EVCardAttributeParam *param = e_vcard_attribute_param_new (EVC_TYPE);

  for (int i = 0; tokens[i]; i++)
    e_vcard_attribute_param_add_value (param, tokens[i]);
  e_vcard_attribute_add_param (attr, param);
}

/* a custom label as a TYPE token (AOSP's 3.0 export writes the label itself; a 2.1 one X-<label>) */
static void
add_custom_param (EVCardAttribute *attr, const char *label, gboolean primary)
{
  g_autofree char *clean = trimmed (label);
  const char *tokens[3] = { clean, primary ? "PREF" : NULL, NULL };

  if (*clean)
    add_type_param (attr, tokens);
  else if (primary)
    add_type_param (attr, (const char *const[]) { "PREF", NULL });
}

static EVCardAttribute *
android_custom (const char *mime, const CtItem *item)
{
  EVCardAttribute *attr = e_vcard_attribute_new (NULL, ANDROID_CUSTOM);
  g_autofree char *type = g_strdup_printf ("%d", item->type);

  /* the MIME type and data1 … data15, as Android writes them */
  e_vcard_attribute_add_value (attr, mime);
  e_vcard_attribute_add_value (attr, item->value);
  e_vcard_attribute_add_value (attr, type);
  e_vcard_attribute_add_value (attr, item->type == CT_TYPE_CUSTOM ? item->label : "");
  for (int i = 4; i <= 15; i++)
    e_vcard_attribute_add_value (attr, "");
  return attr;
}

static EVCardAttribute *
attribute_from_item (const CtItem *item, gboolean first_of_type)
{
  EVCardAttribute *attr = NULL;
  g_autofree char *value = trimmed (item->value);

  switch (item->kind)
    {
    case CT_KIND_PHONE:
      attr = e_vcard_attribute_new (NULL, EVC_TEL);
      if (item->type == CT_TYPE_CUSTOM)
        add_custom_param (attr, item->label, item->primary);
      else
        {
          const char *const *tokens = phone_type_tokens (item->type);
          g_autoptr(GPtrArray) all = g_ptr_array_new ();

          for (int i = 0; tokens[i]; i++)
            g_ptr_array_add (all, (gpointer) tokens[i]);
          /* the default number gets PREF; Main already is PREF and cannot say both */
          if (item->primary && item->type != 12)
            g_ptr_array_add (all, (gpointer) "PREF");
          g_ptr_array_add (all, NULL);
          add_type_param (attr, (const char *const *) all->pdata);
        }
      e_vcard_attribute_add_value (attr, value);
      break;

    case CT_KIND_EMAIL:
    case CT_KIND_ADDRESS:
      attr = e_vcard_attribute_new (NULL, item->kind == CT_KIND_EMAIL ? EVC_EMAIL : EVC_ADR);
      if (item->type == CT_TYPE_CUSTOM)
        add_custom_param (attr, item->label, item->primary);
      else
        add_type_param (attr, (const char *const[]) { simple_type_token (item->type, item->kind == CT_KIND_EMAIL),
                                                      item->primary ? "PREF" : NULL, NULL });
      if (item->kind == CT_KIND_EMAIL)
        e_vcard_attribute_add_value (attr, value);
      else
        {
          /* the editor's one formatted field goes into the street, as CP2's PostalSplitter does for a
           * non-CJK locale: ";;<text>;;;;" */
          e_vcard_attribute_add_values (attr, "", "", value, "", "", "", "", NULL);
        }
      break;

    case CT_KIND_WEBSITE:
      attr = e_vcard_attribute_new (NULL, EVC_URL);
      e_vcard_attribute_add_value (attr, value);
      break;

    case CT_KIND_NICKNAME:
      attr = e_vcard_attribute_new (NULL, EVC_NICKNAME);
      e_vcard_attribute_add_value (attr, value);
      break;

    case CT_KIND_NOTE:
      attr = e_vcard_attribute_new (NULL, EVC_NOTE);
      e_vcard_attribute_add_value (attr, item->value);   /* a note keeps its spaces and line breaks */
      break;

    case CT_KIND_EVENT:
      /* the birthday is BDAY (a year-less --MM-DD kept as the raw string, §10.8.2), one anniversary EDS's own
       * property, every other date Android's X-ANDROID-CUSTOM line */
      if (item->type == 3 && first_of_type)
        {
          attr = e_vcard_attribute_new (NULL, EVC_BDAY);
          e_vcard_attribute_add_value (attr, value);
        }
      else if (item->type == 1 && first_of_type)
        {
          attr = e_vcard_attribute_new (NULL, EVC_X_ANNIVERSARY);
          e_vcard_attribute_add_value (attr, value);
        }
      else
        attr = android_custom (MIME_EVENT, item);
      break;

    case CT_KIND_RELATION:
      attr = android_custom (MIME_RELATION, item);
      break;

    default:
      break;
    }
  return attr;
}

void
ct_contact_set_items (EContact *contact, CtKind kind, GPtrArray *items)
{
  EVCard *vcard = E_VCARD (contact);
  g_autoptr(GPtrArray) old = g_ptr_array_new ();
  gboolean seen_birthday = FALSE, seen_anniversary = FALSE;

  g_return_if_fail (kind != CT_KIND_IM && kind != CT_KIND_SIP);

  if (kind == CT_KIND_ORGANIZATION)
    {
      CtItem *item = items && items->len ? items->pdata[0] : NULL;
      g_autofree char *company = trimmed (item ? item->value : NULL);
      g_autofree char *title = trimmed (item ? item->value2 : NULL);
      g_autofree char *department = trimmed (item ? item->value3 : NULL);

      e_vcard_remove_attributes (vcard, NULL, EVC_ORG);
      e_vcard_remove_attributes (vcard, NULL, EVC_TITLE);
      if (*company || *department)
        {
          EVCardAttribute *org = e_vcard_attribute_new (NULL, EVC_ORG);

          e_vcard_attribute_add_value (org, company);
          if (*department)
            e_vcard_attribute_add_value (org, department);
          e_vcard_append_attribute (vcard, org);
        }
      if (*title)
        e_vcard_append_attribute_with_value (vcard, e_vcard_attribute_new (NULL, EVC_TITLE), title);
      return;
    }

  /* the kind's old properties go, and nothing else */
  for (GList *l = e_vcard_get_attributes (vcard); l; l = l->next)
    {
      CtItem *probe = item_from_attribute (kind, l->data);

      if (probe)
        g_ptr_array_add (old, l->data);
      ct_item_free (probe);
    }
  for (guint i = 0; i < old->len; i++)
    e_vcard_remove_attribute (vcard, old->pdata[i]);
  if (kind == CT_KIND_ADDRESS)
    /* the formatted labels Evolution writes beside ADR would go stale; the ADR itself carries the text now */
    e_vcard_remove_attributes (vcard, NULL, EVC_LABEL);

  for (guint i = 0; items && i < items->len; i++)
    {
      const CtItem *item = items->pdata[i];
      gboolean first = TRUE;
      EVCardAttribute *attr = NULL;

      if (ct_item_is_empty (item))
        continue;
      if (kind == CT_KIND_EVENT && item->type == 3)
        {
          first = !seen_birthday;
          seen_birthday = TRUE;
        }
      else if (kind == CT_KIND_EVENT && item->type == 1)
        {
          first = !seen_anniversary;
          seen_anniversary = TRUE;
        }
      /* a row the user did not change keeps its property as it was, unknown parameters and all */
      if (item->source)
        {
          g_autoptr(CtItem) was = item_from_attribute (kind, item->source);
          const char *source_name = e_vcard_attribute_get_name (item->source);
          gboolean same_property = kind != CT_KIND_EVENT
                                   || (first == (g_ascii_strcasecmp (source_name, ANDROID_CUSTOM) != 0));

          if (was && ct_item_equal (was, item) && same_property)
            attr = e_vcard_attribute_copy (item->source);
        }
      if (!attr)
        attr = attribute_from_item (item, first);
      if (attr)
        e_vcard_append_attribute (vcard, attr);
    }
}

/* --- channel identities */

static char *
ct_contact_get_channel_legacy (EContact *contact, const char *property)
{
  EVCardAttribute *attr = e_vcard_get_attribute (E_VCARD (contact), property);

  return attr ? nth_value (attr, 0) : g_strdup ("");
}

/* the IMPP property of the contact that holds @scheme's id ("IMPP:whatsapp:39333…", with or without an
 * X-SERVICE-TYPE parameter), the first one; the id (after the scheme's colon) in @id */
static EVCardAttribute *
find_impp (EContact *contact, const char *scheme, char **id)
{
  for (GList *l = e_vcard_get_attributes (E_VCARD (contact)); l; l = l->next)
    {
      EVCardAttribute *attr = l->data;
      g_autofree char *value = NULL;
      const char *colon;

      if (g_ascii_strcasecmp (e_vcard_attribute_get_name (attr), "IMPP") != 0)
        continue;
      value = nth_value (attr, 0);
      colon = strchr (value, ':');
      if (colon && g_ascii_strncasecmp (value, scheme, colon - value) == 0 && !scheme[colon - value])
        {
          if (id)
            *id = trimmed (colon + 1);
          return attr;
        }
    }
  return NULL;
}

char *
ct_contact_get_channel (EContact *contact, const char *scheme, const char *legacy_property)
{
  char *id = NULL;

  if (find_impp (contact, scheme, &id))
    return id;
  return legacy_property && *legacy_property ? ct_contact_get_channel_legacy (contact, legacy_property)
                                             : g_strdup ("");
}

void
ct_contact_set_channel (EContact *contact, const char *scheme, const char *legacy_property, const char *value)
{
  EVCard *vcard = E_VCARD (contact);
  g_autofree char *text = trimmed (value);
  g_autofree char *line = g_strconcat (scheme, ":", text, NULL);
  EVCardAttribute *impp = find_impp (contact, scheme, NULL);

  /* an id an older Contacts wrote as X-WHATSAPP / X-TELEGRAM moves to IMPP with this write */
  if (legacy_property && *legacy_property)
    e_vcard_remove_attributes (vcard, NULL, legacy_property);
  if (!*text)
    {
      if (impp)
        e_vcard_remove_attribute (vcard, impp);
      return;
    }
  if (impp)
    {
      /* the property stays as it is, its parameters too; another id of the same channel is left alone */
      e_vcard_attribute_remove_values (impp);
      e_vcard_attribute_add_value (impp, line);
      return;
    }
  e_vcard_append_attribute_with_value (vcard, e_vcard_attribute_new (NULL, "IMPP"), line);
}

/* --- merging two contacts */

/* two rows say the same: the same text without case or spaces, the same digits for a number */
static gboolean
same_row (const CtItem *a, const CtItem *b)
{
  g_autofree char *x = trimmed (a->value), *y = trimmed (b->value);

  if (a->kind == CT_KIND_PHONE)
    {
      g_autoptr(GString) dx = g_string_new (NULL), dy = g_string_new (NULL);

      for (const char *p = x; *p; p++)
        if (g_ascii_isdigit (*p))
          g_string_append_c (dx, *p);
      for (const char *p = y; *p; p++)
        if (g_ascii_isdigit (*p))
          g_string_append_c (dy, *p);
      /* as the people library and the memory service compare numbers: by their last digits */
      if (dx->len >= CT_MERGE_MIN_MATCH_DIGITS && dy->len >= CT_MERGE_MIN_MATCH_DIGITS)
        return g_str_equal (dx->str + dx->len - CT_MERGE_MIN_MATCH_DIGITS, dy->str + dy->len - CT_MERGE_MIN_MATCH_DIGITS);
      return dx->len && g_str_equal (dx->str, dy->str);
    }
  return g_ascii_strcasecmp (x, y) == 0;
}

static gboolean
has_same_row (GPtrArray *items, const CtItem *item)
{
  for (guint i = 0; i < items->len; i++)
    if (same_row (items->pdata[i], item))
      return TRUE;
  return FALSE;
}

/* the first row of @items of @type (an event's), or NULL */
static const CtItem *
row_of_type (GPtrArray *items, int type)
{
  for (guint i = 0; i < items->len; i++)
    if (((CtItem *) items->pdata[i])->type == type)
      return items->pdata[i];
  return NULL;
}

static gboolean
has_attribute_like (EVCard *vcard, EVCardAttribute *attr)
{
  g_autofree char *wanted = nth_value (attr, 0);

  for (GList *l = e_vcard_get_attributes (vcard); l; l = l->next)
    if (g_ascii_strcasecmp (e_vcard_attribute_get_name (l->data), e_vcard_attribute_get_name (attr)) == 0)
      {
        g_autofree char *value = nth_value (l->data, 0);

        if (g_ascii_strcasecmp (value, wanted) == 0)
          return TRUE;
      }
  return FALSE;
}

guint
ct_contact_merge (EContact *into, EContact *from)
{
  static const CtKind kinds[] = { CT_KIND_NICKNAME, CT_KIND_PHONE, CT_KIND_EMAIL, CT_KIND_ADDRESS, CT_KIND_WEBSITE,
                                  CT_KIND_EVENT, CT_KIND_RELATION, CT_KIND_NOTE };
  EVCard *a = E_VCARD (into), *b = E_VCARD (from);
  guint conflicts = 0;
  g_autoptr(CtName) name_a = ct_contact_get_name (into);
  g_autoptr(CtName) name_b = ct_contact_get_name (from);
  EVCardAttribute *photo_a = e_vcard_get_attribute (a, EVC_PHOTO), *photo_b = e_vcard_get_attribute (b, EVC_PHOTO);
  g_autoptr(GPtrArray) org_a = ct_contact_get_items (into, CT_KIND_ORGANIZATION);
  g_autoptr(GPtrArray) org_b = ct_contact_get_items (from, CT_KIND_ORGANIZATION);
  g_autoptr(GPtrArray) labels = ct_contact_dup_labels (from);

  /* the names, the photo, the company: one each, the surviving contact's; the other's only when it has none */
  if (ct_name_is_empty (name_a))
    {
      if (!ct_name_is_empty (name_b))
        ct_contact_set_name (into, name_b);
    }
  else if (!ct_name_is_empty (name_b) && !ct_name_equal (name_a, name_b))
    conflicts |= CT_MERGE_NAME;
  if (photo_b)
    {
      g_autofree char *x = nth_value (photo_b, 0);
      g_autofree char *y = photo_a ? nth_value (photo_a, 0) : NULL;

      if (!photo_a)
        e_vcard_append_attribute (a, e_vcard_attribute_copy (photo_b));
      else if (g_strcmp0 (x, y))
        conflicts |= CT_MERGE_PHOTO;
    }
  if (org_b->len && !ct_item_is_empty (org_b->pdata[0]))
    {
      if (!org_a->len || ct_item_is_empty (org_a->pdata[0]))
        ct_contact_set_items (into, CT_KIND_ORGANIZATION, org_b);
      else if (!ct_item_equal (org_a->pdata[0], org_b->pdata[0]))
        conflicts |= CT_MERGE_ORGANIZATION;
    }

  /* everything with many rows adds up, without a row that is there already; one birthday and one anniversary */
  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++)
    {
      g_autoptr(GPtrArray) mine = ct_contact_get_items (into, kinds[k]);
      g_autoptr(GPtrArray) theirs = ct_contact_get_items (from, kinds[k]);
      gboolean added = FALSE;

      for (guint i = 0; i < theirs->len; i++)
        {
          CtItem *item = theirs->pdata[i];

          if (kinds[k] == CT_KIND_EVENT && (item->type == 3 || item->type == 1))
            {
              const CtItem *have = row_of_type (mine, item->type);

              if (have)
                {
                  if (item->type == 3 && !same_row (have, item))
                    conflicts |= CT_MERGE_BIRTHDAY;
                  continue;
                }
            }
          if (ct_item_is_empty (item) || has_same_row (mine, item))
            continue;
          g_ptr_array_add (mine, ct_item_copy (item));
          added = TRUE;
        }
      if (added)
        ct_contact_set_items (into, kinds[k], mine);
    }

  /* the instant-messaging ids (IMPP, X-JABBER …) one by one: they carry the channels of Minerva */
  for (GList *l = e_vcard_get_attributes (b); l; l = l->next)
    {
      g_autoptr(CtItem) im = item_from_attribute (CT_KIND_IM, l->data);

      if (im && !has_attribute_like (a, l->data))
        e_vcard_append_attribute (a, e_vcard_attribute_copy (l->data));
    }

  for (guint i = 0; i < labels->len; i++)
    ct_contact_add_label (into, labels->pdata[i]);
  if (ct_contact_is_starred (from))
    ct_contact_set_starred (into, TRUE);
  return conflicts;
}

/* --- dates */

static gboolean
parse_digits (const char *s, int n, int *out)
{
  int value = 0;

  for (int i = 0; i < n; i++)
    {
      if (!g_ascii_isdigit (s[i]))
        return FALSE;
      value = value * 10 + (s[i] - '0');
    }
  *out = value;
  return TRUE;
}

gboolean
ct_date_parse (const char *text, int *year, int *month, int *day)
{
  g_autofree char *s = trimmed (text);
  char *t = strchr (s, 'T');
  size_t len;
  int y = 0, m = 0, d = 0;
  gboolean ok = FALSE;

  if (t)
    *t = '\0';
  len = strlen (s);
  if (len == 10 && s[4] == '-' && s[7] == '-')                      /* yyyy-MM-dd */
    ok = parse_digits (s, 4, &y) && parse_digits (s + 5, 2, &m) && parse_digits (s + 8, 2, &d);
  else if (len == 8 && g_str_has_prefix (s, "--"))                   /* --MMdd */
    ok = parse_digits (s + 2, 2, &m) && parse_digits (s + 4, 2, &d) && s[4] != '-';
  else if (len == 7 && g_str_has_prefix (s, "--") && s[4] == '-')    /* --MM-dd */
    ok = parse_digits (s + 2, 2, &m) && parse_digits (s + 5, 2, &d);
  else if (len == 6 && g_str_has_prefix (s, "--"))                   /* --MMdd, the vCard 4.0 form */
    ok = parse_digits (s + 2, 2, &m) && parse_digits (s + 4, 2, &d);
  else if (len == 8)                                                 /* yyyyMMdd */
    ok = parse_digits (s, 4, &y) && parse_digits (s + 4, 2, &m) && parse_digits (s + 6, 2, &d);
  if (!ok || m < 1 || m > 12 || d < 1 || d > 31)
    return FALSE;
  /* a year-less date (Feb 29 too) is checked against a leap year */
  if (d > g_date_get_days_in_month (m, y > 0 ? y : 2000))
    return FALSE;
  *year = y;
  *month = m;
  *day = d;
  return TRUE;
}

char *
ct_date_format_value (int year, int month, int day)
{
  if (year > 0)
    return g_strdup_printf ("%04d-%02d-%02d", year, month, day);
  return g_strdup_printf ("--%02d-%02d", month, day);
}

char *
ct_date_format_display (const char *value)
{
  int year, month, day;
  g_autoptr(GDateTime) date = NULL;
  const char *locale = setlocale (LC_TIME, NULL);
  gboolean german = locale && g_str_has_prefix (locale, "de");

  if (!ct_date_parse (value, &year, &month, &day))
    return g_strdup (value ? value : "");
  date = g_date_time_new_utc (year > 0 ? year : 2000, month, day, 0, 0, 0);
  /* DateUtils.formatDateTime(FORMAT_SHOW_DATE, with FORMAT_SHOW_YEAR only when there is one) */
  if (german)
    return g_date_time_format (date, year > 0 ? "%-d. %B %Y" : "%-d. %B");
  return g_date_time_format (date, year > 0 ? "%B %-d, %Y" : "%B %-d");
}

/* --- search */

/* Character.isLetterOrDigit */
static gboolean
is_word_char (gunichar c)
{
  return g_unichar_isalpha (c) || g_unichar_isdigit (c);
}

int
ct_index_of_word_prefix (const char *text, const char *prefix)
{
  glong n_text = 0, n_prefix = 0;
  g_autofree gunichar *t = NULL, *p = NULL;
  glong i = 0;

  if (!text || !prefix)
    return -1;
  t = g_utf8_to_ucs4_fast (text, -1, &n_text);
  p = g_utf8_to_ucs4_fast (prefix, -1, &n_prefix);
  if (n_prefix == 0 || n_text < n_prefix)
    return -1;
  for (glong k = 0; k < n_prefix; k++)
    p[k] = g_unichar_toupper (p[k]);
  while (i < n_text)
    {
      glong j;

      /* skip the characters between words */
      while (i < n_text && !is_word_char (t[i]))
        i++;
      if (i + n_prefix > n_text)
        return -1;
      for (j = 0; j < n_prefix; j++)
        if (g_unichar_toupper (t[i + j]) != p[j])
          break;
      if (j == n_prefix)
        return (int) (g_utf8_offset_to_pointer (text, i) - text);
      /* skip the rest of this word */
      while (i < n_text && is_word_char (t[i]))
        i++;
    }
  return -1;
}

/* text folded for matching: case and accents gone, as the provider's search index normalizes names */
static char *
fold (const char *text)
{
  g_autofree char *decomposed = g_utf8_normalize (text ? text : "", -1, G_NORMALIZE_NFD);
  GString *out = g_string_new (NULL);

  for (const char *s = decomposed; s && *s; s = g_utf8_next_char (s))
    {
      gunichar c = g_utf8_get_char (s);

      if (g_unichar_ismark (c))
        continue;
      g_string_append_unichar (out, g_unichar_tolower (c));
    }
  return g_string_free (out, FALSE);
}

/* whether a word of @text (folded) starts with @word (folded) */
static gboolean
has_word_prefix (const char *text, const char *word)
{
  g_autofree char *folded = fold (text);
  size_t n = strlen (word);
  const char *s = folded;

  while (*s)
    {
      while (*s && !is_word_char (g_utf8_get_char (s)))
        s = g_utf8_next_char (s);
      if (!*s)
        break;
      if (strncmp (s, word, n) == 0)
        return TRUE;
      while (*s && is_word_char (g_utf8_get_char (s)))
        s = g_utf8_next_char (s);
    }
  return FALSE;
}

static char *
digits_of (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *s = text; s && *s; s++)
    if (g_ascii_isdigit (*s))
      g_string_append_c (out, *s);
  return g_string_free (out, FALSE);
}

/* a query that is a phone number being typed: digits and the dialable punctuation */
static gboolean
is_number_query (const char *query)
{
  gboolean digit = FALSE;

  for (const char *s = query; *s; s++)
    {
      if (g_ascii_isdigit (*s))
        digit = TRUE;
      else if (!strchr ("+-() ./", *s))
        return FALSE;
    }
  return digit;
}

/* the number's digits as written and in its national form ("+49 170 …" is also "0170 …"), so that a query typed
 * either way finds it, as the provider's search index has both */
static gboolean
number_matches (const char *number, const char *query_digits)
{
  g_autofree char *digits = digits_of (number);
  g_autoptr(EPhoneNumber) parsed = NULL;

  if (g_str_has_prefix (digits, query_digits))
    return TRUE;
  if (e_phone_number_is_supported ())
    parsed = e_phone_number_from_string (number, NULL, NULL);
  if (parsed)
    {
      g_autofree char *national = e_phone_number_to_string (parsed, E_PHONE_NUMBER_FORMAT_NATIONAL);
      g_autofree char *national_digits = digits_of (national);

      if (g_str_has_prefix (national_digits, query_digits))
        return TRUE;
    }
  return FALSE;
}

/* SearchUtil's snippetize: a line longer than 30 characters is cut to whole words around the match, with "…" where
 * text was left out */
static char *
snippetize (const char *line, const char *word)
{
  g_autofree char *flat = g_strdup (line);
  g_auto(GStrv) words = NULL;
  int hit = -1, from, to;
  GString *out;
  guint n;

  g_strdelimit (flat, "\r\n\t", ' ');
  if (g_utf8_strlen (flat, -1) <= 30)
    return g_strdup (flat);
  words = g_strsplit (flat, " ", -1);
  n = g_strv_length (words);
  for (guint i = 0; i < n && hit < 0; i++)
    if (has_word_prefix (words[i], word))
      hit = (int) i;
  if (hit < 0)
    hit = 0;
  /* the matching word with neighbours on both sides while the snippet stays within 30 characters */
  from = to = hit;
  for (gboolean grew = TRUE; grew;)
    {
      glong length = 0;

      grew = FALSE;
      for (int i = from; i <= to; i++)
        length += g_utf8_strlen (words[i], -1) + 1;
      if (to + 1 < (int) n && length + g_utf8_strlen (words[to + 1], -1) + 1 <= 30)
        {
          to++;
          grew = TRUE;
        }
      else if (from > 0 && length + g_utf8_strlen (words[from - 1], -1) + 1 <= 30)
        {
          from--;
          grew = TRUE;
        }
    }
  out = g_string_new (from > 0 ? "…" : "");
  for (int i = from; i <= to; i++)
    g_string_append_printf (out, "%s%s", i > from ? " " : "", words[i]);
  if (to + 1 < (int) n)
    g_string_append (out, "…");
  return g_string_free (out, FALSE);
}

void
ct_match_clear (CtMatch *match)
{
  g_clear_pointer (&match->snippet, g_free);
  match->name_start = -1;
  match->name_length = 0;
}

/* the first line of @text with a word starting with @word */
static char *
matching_line (const char *text, const char *word)
{
  g_auto(GStrv) lines = g_strsplit (text ? text : "", "\n", -1);

  for (int i = 0; lines[i]; i++)
    if (has_word_prefix (lines[i], word))
      return snippetize (lines[i], word);
  return NULL;
}

gboolean
ct_contact_match (EContact *contact, const char *display_name, const char *query, CtMatch *match)
{
  g_autofree char *clean = trimmed (query);
  g_autofree char *folded = fold (clean);
  g_auto(GStrv) words = g_strsplit_set (folded, " \t", -1);
  g_autofree char *phonetic = ct_contact_phonetic_name (contact);
  g_autofree char *alternative = ct_contact_display_name (contact, CT_ORDER_ALTERNATIVE);
  g_autofree char *nickname = first_value (contact, EVC_NICKNAME);
  const char *highlight = clean;
  gboolean name_match = TRUE;
  int words_seen = 0;

  match->name_start = -1;
  match->name_length = 0;
  match->snippet = NULL;
  if (!*clean)
    return TRUE;

  /* TextHighlighter.applyPrefixHighlight drops what is not a letter or digit from the front of the prefix */
  while (*highlight && !is_word_char (g_utf8_get_char (highlight)))
    highlight = g_utf8_next_char (highlight);

  /* every word of the query must start a word of one of the name's forms */
  for (int i = 0; words[i]; i++)
    {
      if (!*words[i])
        continue;
      words_seen++;
      if (!has_word_prefix (display_name, words[i]) && !has_word_prefix (alternative, words[i])
          && !(phonetic && has_word_prefix (phonetic, words[i])) && !(nickname && has_word_prefix (nickname, words[i])))
        name_match = FALSE;
    }
  if (words_seen && name_match)
    {
      int index = ct_index_of_word_prefix (display_name, highlight);

      if (index >= 0)
        {
          match->name_start = index;
          match->name_length = (int) strlen (highlight);
        }
      return TRUE;
    }

  /* then the data, the matching line becoming the snippet under the name */
  if (is_number_query (clean))
    {
      g_autofree char *query_digits = digits_of (clean);
      g_autoptr(GPtrArray) phones = ct_contact_get_items (contact, CT_KIND_PHONE);

      for (guint i = 0; i < phones->len; i++)
        if (number_matches (((CtItem *) phones->pdata[i])->value, query_digits))
          {
            match->snippet = g_strdup (((CtItem *) phones->pdata[i])->value);
            return TRUE;
          }
    }
  {
    g_autoptr(GPtrArray) emails = ct_contact_get_items (contact, CT_KIND_EMAIL);

    for (guint i = 0; i < emails->len; i++)
      {
        const char *address = ((CtItem *) emails->pdata[i])->value;
        g_autofree char *folded_address = fold (address);

        if (g_str_has_prefix (folded_address, folded) || has_word_prefix (address, folded))
          {
            match->snippet = g_strdup (address);
            return TRUE;
          }
      }
  }
  {
    g_autoptr(GPtrArray) orgs = ct_contact_get_items (contact, CT_KIND_ORGANIZATION);
    g_autoptr(GPtrArray) notes = ct_contact_get_items (contact, CT_KIND_NOTE);
    g_autoptr(GPtrArray) addresses = ct_contact_get_items (contact, CT_KIND_ADDRESS);
    GPtrArray *sources[] = { orgs, notes, addresses };

    for (guint s = 0; s < G_N_ELEMENTS (sources); s++)
      for (guint i = 0; i < sources[s]->len; i++)
        {
          CtItem *item = sources[s]->pdata[i];
          const char *fields[] = { item->value, item->value2, item->value3 };

          for (guint f = 0; f < G_N_ELEMENTS (fields); f++)
            if ((match->snippet = matching_line (fields[f], folded)))
              return TRUE;
        }
  }
  return FALSE;
}

/* --- what a signature covers */

static gboolean
unsigned_field (const char *name, const char *const *unsigned_fields)
{
  for (guint i = 0; unsigned_fields && unsigned_fields[i]; i++)
    {
      g_autofree char *u = g_ascii_strup (g_strstrip (g_strdup (unsigned_fields[i])), -1);
      gsize len = strlen (u);
      gboolean prefix = len && u[len - 1] == '*';

      if (prefix)
        u[len - 1] = '\0';
      if (prefix ? g_str_has_prefix (name, u) : g_str_equal (name, u))
        return TRUE;
    }
  return FALSE;
}

char *
ct_contact_signed_text (EContact *contact, const char *const *unsigned_fields)
{
  g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  GString *out = g_string_new (NULL);

  for (GList *l = e_vcard_get_attributes (E_VCARD (contact)); l; l = l->next)
    {
      g_autofree char *name = g_ascii_strup (e_vcard_attribute_get_name (l->data), -1);
      g_autoptr(GString) value = g_string_new (NULL);

      if (unsigned_field (name, unsigned_fields))
        continue;
      for (GList *v = e_vcard_attribute_get_values (l->data); v; v = v->next)
        {
          g_auto(GStrv) words = g_strsplit_set (v->data ? v->data : "", " \t\r\n", -1);
          g_autofree char *joined = NULL;
          g_autoptr(GPtrArray) kept = g_ptr_array_new ();

          for (guint w = 0; words[w]; w++)
            if (*words[w])
              g_ptr_array_add (kept, words[w]);
          g_ptr_array_add (kept, NULL);
          joined = g_strjoinv (" ", (char **) kept->pdata);
          if (value->len)
            g_string_append_c (value, ';');
          g_string_append (value, joined);
        }
      {
        g_autofree char *lower = g_utf8_strdown (value->str, -1);

        g_ptr_array_add (lines, g_strdup_printf ("%s:%s", name, lower));
      }
    }
  g_ptr_array_sort_values (lines, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < lines->len; i++)
    g_string_append_printf (out, "%s\n", (char *) lines->pdata[i]);
  return g_string_free (out, FALSE);
}
