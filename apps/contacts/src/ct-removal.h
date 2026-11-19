/* Deleting contacts and merging two contacts, with what Minerva keeps about them.
 *
 * Deleting: the dialog says what goes (the contact, Minerva's notebook, voice and face, the channel links), a contact
 * that is the owner's takes a second, stronger confirmation (her name typed), then for each contact the owner's
 * approval of the memory service's own sentence (the platform's gate sheet, asked by minerva-config through the token
 * service: no sheet is asked here), the people archive following, the address book, and the memory service's commit
 * (`contact_deleted`, the whole cascade at once). Merging: the person picks the second contact, the dialog shows the
 * rule for what the merged card has and where the two differ, then the approval, the merged card written into the
 * contact that stays, the archive following, the other contact deleted, and the commit (`contact_merged`). The merged
 * card is written first and the other contact deleted last: whatever fails in between leaves the contact that goes in
 * the book, and doing it again is harmless; an intent never committed expires, and nothing was done in the memory. Slow steps say so (ct_window_toast_hold). Everything about Minerva goes through ct-mnv.h.
 *
 * Only where Minerva's texts are loaded (ct_removal_active): without Minerva the Contacts app deletes as it did. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

gboolean ct_removal_active (void);

/* @done: how many contacts were deleted (zero when the person said no or the first step failed) */
typedef void (*CtRemovalDone) (guint deleted, gpointer data);
/* @uids: strings, copied */
void ct_removal_delete (GtkWidget *window, GPtrArray *uids, CtRemovalDone done, gpointer data);

/* The contact @uid stays; the picker offers the others. @done: whether they were merged */
typedef void (*CtMergeDone) (gboolean merged, gpointer data);
void ct_removal_merge (GtkWidget *window, const char *uid, CtMergeDone done, gpointer data);

G_END_DECLS
