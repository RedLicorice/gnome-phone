/* How a chat channel id (what Minerva's messaging providers know a contact by: a WhatsApp number, a Telegram id) is
 * written and read on a vCard, as Minerva's memory service does ([eds] im_write_field, im_read_fields, [contacts]
 * channels; `minerva-contacts signing` reports them): a field ending in '-' is the prefix of X-<CHANNEL>:<id>
 * ("X-TELEGRAM:8675309"), any other holds <channel>:<id> itself ("IMPP:telegram:8675309"). One reader for Contacts,
 * Phone and Messages. */
#pragma once

#include <libebook-contacts/libebook-contacts.h>

G_BEGIN_DECLS

typedef struct {
  const char *write_field;          /* "X-" or "IMPP" */
  const char *const *read_fields;   /* NULL-terminated, in the order they are tried */
} ImForm;

/* there is a form to write a channel id in and one to read it */
gboolean im_form_usable   (const ImForm *form);
/* the vCard property @channel's id is held in when read or written in @field: X-TELEGRAM, IMPP */
char    *im_form_property (const char *field, const char *channel);
/* @attr holds @channel's id in @field; the id (trimmed) in @id, when not NULL */
gboolean im_form_holds    (EVCardAttribute *attr, const char *field, const char *channel, char **id);
/* every chat handle of @vcard, "<channel>:<id>": a field holding <channel>:<id> gives each of its values, a prefix
 * gives the X-<CHANNEL> of each of @channels; in @read_fields' order, each handle once */
GStrv    im_form_handles  (EVCard *vcard, const char *const *read_fields, const char *const *channels);
/* the properties that may hold a handle (an address book query): each field holding <channel>:<id>, and each
 * X-<CHANNEL> of @channels for a prefix */
GStrv    im_form_fields   (const char *const *read_fields, const char *const *channels);

G_END_DECLS
