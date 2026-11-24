/* The screens the drawer's Minerva entries open (ct-people.c): the lists of what Minerva knows and the address book
 * has not (Sconosciuti, Animali, Oggetti, Luoghi), and "Minerva", the rules
 * of the archive (how long strangers are kept, how trust grows) and its counts. Everything goes through ct-mnv.h; the
 * texts are the data's (the "contacts" part of `people ui`). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* One of the data's lists ("lists" of `people ui`: Sconosciuti, Animali, Oggetti, Luoghi): the entries of the
 * archive of its kind (only those that are not contacts, for the strangers), a tap on one offers the list's actions
 * (rename, add to the contacts, assign an owner, forget). Minerva's owner can be none of them forgotten. */
GtkWidget *ct_mnv_list_new          (GtkWidget *window, const char *id);
GtkWidget *ct_minerva_settings_new  (GtkWidget *window);

/* A messaging provider's chats with one person (`messaging chats ID --kind user`), searchable: the one tapped goes to
 * @picked (the list closes first). A chat that already has a contact (the listing's "contact") is left out for a
 * new contact; with @linking (an existing contact's "Collega") it shows with that contact's name, and picking it
 * asks first, since the chat would then belong to two contacts. @destroy (or NULL) frees @data with the page. */
typedef void (*CtChatPicked) (const char *provider, const char *chat, gpointer data);
GtkWidget *ct_mnv_chats_new         (GtkWidget *window, const char *provider, gboolean linking, CtChatPicked picked,
                                     gpointer data, GDestroyNotify destroy);
/* "Importa da <provider>": that list, then the chat picked as a new contact (ct_mnv_import_chat) */
void       ct_mnv_import_pick       (GtkWidget *window, const char *provider);
/* the new-contact editor filled in with what the provider knows of @chat (`messaging contact`): its name, number,
 * id there and picture; the refusal's text as a toast when it cannot be (a group, an unknown chat) */
void       ct_mnv_import_chat       (GtkWidget *window, const char *provider, const char *chat);

G_END_DECLS
