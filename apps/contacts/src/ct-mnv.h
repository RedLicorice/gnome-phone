/* Contacts' one door to Minerva's people archive. Every call the Contacts app makes about a person's notebook, voice,
 * face samples, kind, channel links, strangers or rules goes through this file and nothing else: today they run
 * `minerva-config people … --json` (people library, eyes' people_link), through Neo Settings' ns-mc.c; when the
 * archive moves to Minerva's memory service, only this file changes.
 *
 * The key of a person is the contact's UID in the address book: ct_mnv_by_contact finds the person linked to a
 * contact, and a contact with none is offered ct_mnv_create then ct_mnv_link. The name is the contact's: the
 * archive's own copy follows it (ct_mnv_sync_name).
 *
 * Every call is asynchronous and answers (@root, @error): @error is NULL when it was done, else the message to show
 * (the program failed, or minerva-config refused: {"ok": false, "error": …}); @root is valid during the callback.
 * The texts, sizes and intervals are data: the "contacts" part of `people ui` (config/minerva_config/people_ui.json),
 * its maps "kinds", "kind_one"; ct_mnv_text and ct_mnv_int read them once ct_mnv_ui_load has answered. */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "ct-contact.h"

G_BEGIN_DECLS

typedef void (*CtMnvDone) (JsonObject *root, const char *error, gpointer data);

/* whether `minerva-config` is installed: without it, none of the Minerva parts of Contacts shows */
gboolean    ct_mnv_available (void);

/* `people ui`, once per process; @done (or NULL) runs when it is here (at once when it already is). FALSE texts
 * until then. */
void        ct_mnv_ui_load  (GObject *owner, CtMnvDone done, gpointer data);
gboolean    ct_mnv_ui_ready (void);
/* a text of the data's "contacts" part, "" when it has none; the same with {name}-style fields filled in by
 * @pairs (key, value … NULL) */
const char *ct_mnv_text     (const char *key);
char       *ct_mnv_textf    (const char *key, const char *const *pairs);
int         ct_mnv_int      (const char *key);
/* how a chat channel id is written and read on a card, as Minerva's memory service does (the data's
 * im_write_field, im_read_fields); not usable (im_form_usable) until the data is here or when it has none */
const ImForm *ct_mnv_im_form (void);
double      ct_mnv_double   (const char *key);
/* a member of one of the data's top-level maps ("kinds", "kind_one"), "" when absent */
const char *ct_mnv_map      (const char *map, const char *key);
/* the data's list under "contacts" (the channels, the rules), borrowed, NULL when absent */
JsonArray  *ct_mnv_list     (const char *key);

/* the person linked to the contact @uid: {"person": {…} or null} */
void ct_mnv_by_contact  (GObject *owner, const char *uid, CtMnvDone done, gpointer data);
/* the notebook's text: {"text"} */
void ct_mnv_notebook    (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
/* empties the notebook: {"text": ""} */
void ct_mnv_notebook_clear (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
/* the voice service: {running, enrolling, person, got, need, voices, learn{learned, needed}} */
void ct_mnv_voice_status (GObject *owner, CtMnvDone done, gpointer data);
void ct_mnv_voice_enroll (GObject *owner, const char *pid, const char *consent, CtMnvDone done, gpointer data);
void ct_mnv_voice_forget (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
/* the opt-in to learn the voice with the PIN, and the learned clips */
void ct_mnv_voice_learn  (GObject *owner, const char *pid, gboolean on, const char *consent, CtMnvDone done,
                          gpointer data);
void ct_mnv_clips_forget (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
void ct_mnv_set_kind    (GObject *owner, const char *pid, const char *kind, CtMnvDone done, gpointer data);
/* a new person (a notebook for a contact that has none): {id, name, …} */
void ct_mnv_create      (GObject *owner, const char *name, CtMnvDone done, gpointer data);
void ct_mnv_link        (GObject *owner, const char *pid, const char *contact_uid, CtMnvDone done, gpointer data);
void ct_mnv_unlink      (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
/* the archive's own copy of the name; done at once (NULL error, no root) when it already is @name */
void ct_mnv_sync_name   (GObject *owner, const char *pid, const char *person_name, const char *name, CtMnvDone done,
                         gpointer data);
/* @pid's samples, sightings and notebook go to @into, and @pid is gone */
void ct_mnv_merge       (GObject *owner, const char *pid, const char *into, CtMnvDone done, gpointer data);
/* the person with her notebook, voiceprint, consent and face samples */
void ct_mnv_forget      (GObject *owner, const char *pid, CtMnvDone done, gpointer data);
/* the whole archive: {people: [{id, name, kind, contact_uid, samples, sightings, …}], kinds, rules} */
void ct_mnv_known       (GObject *owner, CtMnvDone done, gpointer data);
/* one rule of `known` (people.conf): {rules: {…}} */
void ct_mnv_rule_set    (GObject *owner, const char *key, const char *value, CtMnvDone done, gpointer data);

/* The memory service's side of deleting or merging a whole contact. No app asks the shell's sheets: minerva-config
 * asks the memory service for the intent and the owner's approval of its sentence through the token service (the
 * platform's gate sheet, finger then PIN, whose method, timing and texts are the token service's data), keeps the
 * approval, and answers {"intent": id} once the owner said yes (a no: the refusal's text). After the address book
 * did its part, the commit presents it. ct_mnv_memory_configured: whether this phone has the Contacts app's identity
 * (`people ui` contacts.memory_ops); without it there is no memory service to tell. */
gboolean ct_mnv_memory_configured (void);
void ct_mnv_contact_approve_delete (GObject *owner, const char *uid, CtMnvDone done, gpointer data);
void ct_mnv_contact_approve_merge  (GObject *owner, const char *from, const char *to, CtMnvDone done, gpointer data);
void ct_mnv_contact_commit         (GObject *owner, const char *intent, CtMnvDone done, gpointer data);

/* What the people archive (this file's other calls) does when the contact goes: @uid's person is forgotten and every
 * animal and object that was hers has no owner; or, for a merge, @from's person joins @to's (or becomes it when
 * @to has none) and what was @from's now belongs to @to. @known is a `people known` answer, ref'd until done. */
void ct_mnv_archive_contact_deleted (GObject *owner, const char *uid, JsonObject *known, CtMnvDone done,
                                     gpointer data);
void ct_mnv_archive_contact_merged  (GObject *owner, const char *from, const char *to, JsonObject *known,
                                     CtMnvDone done, gpointer data);

/* an animal's or object's owner: a contact's UID, "" for nobody */
void ct_mnv_set_owner   (GObject *owner, const char *pid, const char *contact_uid, CtMnvDone done, gpointer data);
void ct_mnv_rename      (GObject *owner, const char *pid, const char *name, CtMnvDone done, gpointer data);

G_END_DECLS
