/* The logic of the Minerva parts of Contacts that needs neither GTK nor a running minerva-config: the texts of the
 * data with their {names} filled in, which entries of Minerva's archive are not contacts, whether a person's own
 * copy of the name must follow the contact's, and what a rule's text may be. Tested on its own (tests/test-ct-mnv.c). */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* @tpl with each "{key}" replaced by its value; @pairs are key, value, key, value … NULL. A key not in @pairs stays. */
char      *ct_mnv_format (const char *tpl, const char *const *pairs);

/* The entries of `people known` that are not contacts: no contact UID. They are the faces and voices the eyes found,
 * and the things taught by hand that no contact stands for yet. A new array of the answer's objects (borrowed),
 * in the answer's order. */
GPtrArray *ct_mnv_loose (JsonObject *known);

/* TRUE when the archive's copy of the name (@person_name) must be replaced by the contact's: the contact has a name
 * and it is not the same text, leading and trailing spaces ignored */
gboolean   ct_mnv_name_stale (const char *person_name, const char *contact_name);

/* A rule's value as typed (a comma or a dot for the decimals): TRUE and the text to hand to minerva-config in
 * @value when it is a number from @lo to @hi (a whole one when @integer); FALSE and @value NULL otherwise. */
gboolean   ct_mnv_rule_value (const char *text, double lo, double hi, gboolean integer, char **value);

/* A date of the answer's @member (Unix seconds or milliseconds, or ISO 8601 text) in the strftime @format; NULL when
 * the member has none (absent, 0, empty) */
char      *ct_mnv_date (JsonObject *o, const char *member, const char *format);

G_END_DECLS
