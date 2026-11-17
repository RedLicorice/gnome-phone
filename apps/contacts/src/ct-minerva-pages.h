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

G_END_DECLS
