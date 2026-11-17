/* Deleting contacts and merging two contacts, with what Minerva keeps about them.
 *
 * Deleting: the dialog says what goes (the contact, Minerva's notebook, voice and face, the channel links), a contact
 * that is the owner's takes a second, stronger confirmation (her name typed, her PIN), then for each contact a human
 * proof, the memory service's `contact_deleted` (the whole cascade at once), the people archive following, and the
 * address book. Merging: the person picks the second contact, the dialog shows the rule for what the merged card has
 * and where the two differ, then the proof, the merged card written into the contact that stays, the memory service's
 * `contact_merged`, the archive following, and the other contact deleted. The merged card is written first and the
 * other contact deleted last: whatever fails in between leaves the contact that goes in the book, and doing it again
 * is harmless. Slow steps say so (ct_window_toast_hold). Everything about Minerva goes through ct-mnv.h and the proof
 * through ct-proof.h.
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
