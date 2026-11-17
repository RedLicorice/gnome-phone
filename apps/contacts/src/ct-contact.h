/* What the Contacts app reads from and writes to a vCard (spec/contacts.md §10): the names Android computes from a
 * contact (DISPLAY_NAME_PRIMARY / _ALTERNATIVE, the sort key, the list's section letter), favourites and labels
 * (both CATEGORIES), the letter tile's colour and letter (§6.2), each kind of data row with its type (§10.2-§10.4),
 * and search matching (§2.4).
 *
 * Nothing here touches GTK or evolution-data-server's address books: it works on an EContact (a vCard 3.0 object)
 * in memory, so the tests can run it without either.
 *
 * Round-trip safety (§10.8.1): a setter removes and rewrites only the vCard properties its kind owns, and keeps
 * every property and parameter it does not know (X-EVOLUTION-*, X-ABLabel, …). An unchanged row keeps its original
 * property, parameters and all. */
#pragma once

#include <libebook-contacts/libebook-contacts.h>

G_BEGIN_DECLS

/* --- names (§10.5, §10.6) */

/* "Name format" and "Sort by": the given name first (Android's PRIMARY) or the family name first (ALTERNATIVE) */
typedef enum {
  CT_ORDER_PRIMARY = 1,
  CT_ORDER_ALTERNATIVE = 2,
} CtNameOrder;

/* a StructuredName row plus the phonetic name (X-PHONETIC-*); empty strings, never NULL */
typedef struct {
  char *prefix, *given, *middle, *family, *suffix;
  char *phonetic_given, *phonetic_middle, *phonetic_family;
} CtName;

CtName *ct_name_new (void);
void    ct_name_free (CtName *name);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CtName, ct_name_free)
gboolean ct_name_is_empty (const CtName *name);
gboolean ct_name_equal (const CtName *a, const CtName *b);

CtName *ct_contact_get_name (EContact *contact);
/* writes N, FN (the primary display name), X-EVOLUTION-FILE-AS (the alternative one, for Evolution) and the
 * X-PHONETIC-* properties */
void    ct_contact_set_name (EContact *contact, const CtName *name);

/* NameSplitter.join: "Prefix Given Middle Family, Suffix" (PRIMARY) or "Family, Prefix Given Middle, Suffix"
 * (ALTERNATIVE); "" when the name is empty */
char *ct_name_join (const CtName *name, CtNameOrder order);

/* The name the list and the card show (DISPLAY_NAME_SOURCE's chain): the structured name, else the phonetic name,
 * the nickname, the company, the first phone number, the first email address; else FN as a last resort. "" when
 * the contact has none of them; the list then shows "(No name)". */
char *ct_contact_display_name (EContact *contact, CtNameOrder order);
/* family, middle and given phonetic names joined (Contacts.PHONETIC_NAME), or NULL */
char *ct_contact_phonetic_name (EContact *contact);
/* the given name for "About <name>", else the display name */
char *ct_contact_first_name (EContact *contact, CtNameOrder order);

/* --- sorting and sections (§1.3, §1.4) */

/* The list's sections (ContactLocaleUtils' buckets, §1.4): in list order, the underflow bucket "…" (a name that
 * starts with a symbol, punctuation or an emoji, or no name), "A" … "Z" (accents folded: "Ä" is "A"), the Greek and
 * Cyrillic letters, "#" (a name that starts with a digit: a number), and the overflow bucket "…" (other scripts).
 * The favourites come before all of them, in a section of their own (rank 0, no label). */
enum {
  CT_BUCKET_FAVOURITES = 0,
  CT_BUCKET_UNDERFLOW = 1,
  CT_BUCKET_LATIN = 100,                /* + 0 … 25 */
  CT_BUCKET_OTHER_ALPHABETS = 1000,     /* + the upper-case letter's code point */
  CT_BUCKET_NUMBER = 9000000,
  CT_BUCKET_OVERFLOW = 9000001,
};
/* the section's label, interned: "A", "Σ", "#", "…" */
const char *ct_section_of (const char *sort_name);
/* where the section goes in the list (two sections can both be "…": compare these, not the labels) */
int         ct_section_rank (const char *sort_name);
/* a key that orders contacts as the list does (the section, then the locale's collation of the name); free it */
char       *ct_sort_key (const char *sort_name);

/* --- favourites and labels (§10.1, §10.5) */

/* "Starred in Android" (Google sync's group, folks' is-favourite) or X-FOLKS-FAVOURITE: either one reads as a
 * favourite, both are written */
#define CT_CATEGORY_STARRED "Starred in Android"
gboolean   ct_contact_is_starred (EContact *contact);
void       ct_contact_set_starred (EContact *contact, gboolean starred);
/* the contact's labels: its CATEGORIES without the favourites one; a new array of strings */
GPtrArray *ct_contact_dup_labels (EContact *contact);
void       ct_contact_set_labels (EContact *contact, GPtrArray *labels);
gboolean   ct_contact_has_label (EContact *contact, const char *label);
/* TRUE when the contact changed */
gboolean   ct_contact_add_label (EContact *contact, const char *label);
gboolean   ct_contact_remove_label (EContact *contact, const char *label);
gboolean   ct_contact_rename_label (EContact *contact, const char *from, const char *to);

/* --- the letter tile (§6.2) */

/* Java's String.hashCode over the UTF-16 of @key (the vCard UID, Android's lookup key) */
gint32   ct_java_hash (const char *key);
/* which of the twelve tile colours: abs(hash) % 12, or −1 for the default grey (no key, or a hash of INT_MIN,
 * whose abs() stays negative in Java) */
int      ct_tile_color_index (const char *key);
/* the tile's letter: the name's first character upper-cased if it is a letter in the BMP, else 0 (the silhouette) */
gunichar ct_tile_letter (const char *display_name);

/* --- data rows (§10.2 - §10.4) */

typedef enum {
  CT_KIND_NICKNAME,
  CT_KIND_ORGANIZATION,
  CT_KIND_PHONE,
  CT_KIND_SIP,              /* read only (the editor's legacy kinds, §4.18) */
  CT_KIND_EMAIL,
  CT_KIND_ADDRESS,
  CT_KIND_IM,               /* read only */
  CT_KIND_WEBSITE,
  CT_KIND_EVENT,
  CT_KIND_RELATION,
  CT_KIND_NOTE,
  CT_N_KINDS
} CtKind;

/* CommonDataKinds' TYPE values, the ones the tables below use (Android's numbers, so the code reads like CP2) */
#define CT_TYPE_CUSTOM 0

typedef struct {
  int type;                 /* the Android TYPE (Phone.TYPE_MOBILE 2 …), 0 for a custom label */
  const char *label;        /* N_() */
  gboolean editor;          /* in the Device account's spinner (§4.10) */
} CtType;

/* the kind's types in the spinner's order, ending at a NULL label; NULL for a kind without types */
const CtType *ct_kind_types (CtKind kind);
/* the label of a type ("Mobile"); a custom type's is @custom_label */
const char   *ct_type_label (CtKind kind, int type, const char *custom_label);
/* the type a new row of @kind starts with (§4.9: Mobile, Home, Home, Birthday, Spouse) */
int           ct_kind_default_type (CtKind kind);

/* one data row */
typedef struct {
  CtKind kind;
  int type;
  char *label;              /* the custom label, when type is CT_TYPE_CUSTOM */
  char *value;              /* the number, address, URL, date (yyyy-MM-dd or --MM-dd), name … */
  char *value2;             /* ORGANIZATION: the title; IM: the protocol's name */
  char *value3;             /* ORGANIZATION: the department */
  gboolean primary;         /* PREF: the default number or address ("Set default") */
  EVCardAttribute *source;  /* the property it was read from, kept when the row is written back unchanged */
} CtItem;

CtItem    *ct_item_new (CtKind kind, int type, const char *value);
CtItem    *ct_item_copy (const CtItem *item);
void       ct_item_free (CtItem *item);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CtItem, ct_item_free)
gboolean   ct_item_equal (const CtItem *a, const CtItem *b);
gboolean   ct_item_is_empty (const CtItem *item);

/* the contact's rows of @kind, in vCard order (a new array that frees its items) */
GPtrArray *ct_contact_get_items (EContact *contact, CtKind kind);
/* replaces every row of @kind with @items (empty ones are dropped); not for the read-only kinds */
void       ct_contact_set_items (EContact *contact, CtKind kind, GPtrArray *items);

/* --- channel identities
 * What Minerva's messaging providers know a contact by (a WhatsApp number, a Telegram id) is an IM row of the contact
 * in the address book, written as Minerva's memory service reads it: IMPP:<scheme>:<id> ("IMPP:whatsapp:39333…").
 * An id an older Contacts wrote as @legacy_property (X-WHATSAPP, X-TELEGRAM) is read too, and moves to IMPP the
 * next time the channel is written. Everything else of the vCard is left as it is. */

/* the channel's first id, "" when the contact has none */
char *ct_contact_get_channel (EContact *contact, const char *scheme, const char *legacy_property);
/* sets it (the IMPP property keeps its parameters), or removes it when @value is empty; other ids of the same
 * channel stay, and so does every other IMPP property */
void  ct_contact_set_channel (EContact *contact, const char *scheme, const char *legacy_property, const char *value);

/* --- dates (§4.16, §10.4) */

/* yyyy-MM-dd, --MM-dd, yyyyMMdd, --MMdd, with an optional time part after a T; @year 0 when the date has none */
gboolean ct_date_parse (const char *text, int *year, int *month, int *day);
/* what the editor writes: yyyy-MM-dd, or --MM-dd without a year */
char    *ct_date_format_value (int year, int month, int day);
/* for the card and the editor's button: "March 15, 1990" / "March 15" (German: "15. März 1990" / "15. März") */
char    *ct_date_format_display (const char *value);

/* --- search (§2.3, §2.4) */

typedef struct {
  int name_start, name_length;        /* the highlighted word prefix in the display name (bytes), −1 for none */
  char *snippet;                      /* the data line that matched when no name word did, else NULL */
} CtMatch;

/* FormatUtils.indexOfWordPrefix: the byte offset of the first word in @text that starts with @prefix, ignoring
 * case (words are runs of letters and digits), or −1 */
int      ct_index_of_word_prefix (const char *text, const char *prefix);
/* whether the contact matches @query as CP2's filter does: a name word starting with it, a phone number containing
 * its digits, or a word of an email address, nickname, company or note starting with it. @match says what to show. */
gboolean ct_contact_match (EContact *contact, const char *display_name, const char *query, CtMatch *match);
void     ct_match_clear (CtMatch *match);

G_END_DECLS
