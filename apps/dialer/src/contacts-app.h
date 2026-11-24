/* What the Dialer and Messaging leave to the Contacts app (de.yesman.Contacts): a contact's card (QuickContact),
 * a new contact with a number in it (ACTION_INSERT, "Create new contact"), and a number added to a contact the user
 * picks there (ACTION_INSERT_OR_EDIT, "Add to a contact"); Settings, a contact made of a messaging chat. They are its
 * actions (apps/contacts, ct-main.c), activated
 * over D-Bus with org.freedesktop.Application.ActivateAction, so the app starts if it is not running.
 *
 * Without the Contacts app installed the rows that would open it are not shown (contacts_app_installed); a contact
 * tapped then opens GNOME Contacts at its list if that is there, else "No contacts app available", as the Dialer's
 * DialerUtils.startActivityWithErrorToast says it. */
#pragma once

#include <neo.h>

G_BEGIN_DECLS

/* de.yesman.Contacts is installed: "Create new contact" and "Add to a contact" can be offered */
gboolean contacts_app_installed (void);

/* the Contacts app at its list; @snackbar says so when there is none */
void contacts_app_open (GtkWidget *from, NeoSnackbar *snackbar);
/* the card of the contact with this uid (the vCard UID, as Contacts' uid has it) */
void contacts_app_view (GtkWidget *from, const char *uid, NeoSnackbar *snackbar);
/* the editor for a new contact, with @destination as its phone number, or its e-mail address if it has an @ in it
 * (NULL: an empty one) */
void contacts_app_insert (GtkWidget *from, const char *destination);
/* "Choose a contact" to add @destination to, "Create new contact" first in the list */
void contacts_app_insert_or_edit (GtkWidget *from, const char *destination);
/* the editor for a new contact filled in from a messaging provider's chat with one person (Contacts' import-chat:
 * its name, number, id there and picture, as Minerva's minerva-config gives them) */
void contacts_app_import_chat (GtkWidget *from, const char *provider, const char *chat);

G_END_DECLS
