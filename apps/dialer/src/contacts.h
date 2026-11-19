/* The contacts with a phone number, from Evolution Data Server's address books (spec §14.2), which stand in for
 * Android's contacts provider: who a number belongs to, and the list the Contacts tab and the search show.
 *
 * It reads every enabled address book through a live view, so a contact added or changed elsewhere (GNOME Contacts,
 * a synced account) shows here as it changes. */
#pragma once

#include <gio/gio.h>

typedef struct {
  char *number;                 /* as stored ("+49 30 2357 5124") */
  char *label;                  /* its kind, as the Dialer words it: "Mobile", "Home", "Work", or a custom label */
} ContactNumber;

typedef struct {
  char *uid;                    /* the contact's lookup key: its letter tile's colour comes from it */
  /* the name the way Settings' Display options say (spec §9.3): one of the two below, not owned */
  char *name;                   /* Name format: what every screen shows */
  char *sort_name;              /* Sort by: what the list is ordered by and its letters come from */
  char *sort_key;               /* sort_name as the locale sorts it */
  char *name_primary;           /* first name first (DISPLAY_NAME): FN, else the name's parts, "Anna Schmidt" */
  char *name_alternative;       /* last name first (DISPLAY_NAME_ALTERNATIVE): "Schmidt, Anna"; with no family
                                 * name the same as the other */
  char *sort_key_primary, *sort_key_alternative;
  GPtrArray *numbers;           /* ContactNumber */
  GStrv handles;                /* the IMPP values, a chat's address: "whatsapp:+39...", "telegram:8675309" */
  GBytes *photo;                /* the contact's picture as image data, or NULL */
} Contact;

#define CONTACTS_TYPE (contacts_get_type ())
G_DECLARE_FINAL_TYPE (Contacts, contacts, NEO_DIALER, CONTACTS, GObject)

/* the contacts, the address books read in the background; "changed" is emitted as they come and change, and when
 * the Phone's Display options do (de.yesman.Dialer's sort-order and display-order) */
Contacts      *contacts_new (void);

/* every contact with a number (one known only by a chat handle is not), sorted by sort_name: Contact, owned by the model (valid until the next "changed") */
GPtrArray     *contacts_get_all (Contacts *self);
/* the contact with that uid, or NULL (a favourite whose contact was deleted) */
const Contact *contacts_get (Contacts *self, const char *uid);
/* the contact a number belongs to, or NULL. @country: where the phone is, for numbers stored without a country
 * code */
const Contact *contacts_lookup (Contacts *self, const char *number, const char *country);
/* how many contacts a chat is: those whose IMPP is @handle, or that list @number (either may be NULL); @contact, when
 * not NULL, gets the one contact when there is exactly one. Answers are cached until the next "changed" */
guint          contacts_match (Contacts *self, const char *handle, const char *number, const char *country,
                               const Contact **contact);
/* the label of the contact's number that matches @number ("Mobile"), or NULL */
const char    *contact_number_label (const Contact *contact, const char *number, const char *country);
/* the model has read every address book once: a lookup that finds nothing now means "not a contact" */
gboolean       contacts_is_loaded (Contacts *self);
