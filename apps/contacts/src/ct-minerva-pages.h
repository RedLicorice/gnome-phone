/* The two screens the drawer's Minerva entries open (ct-people.c): "Sconosciuti", the faces and voices Minerva's eyes
 * found that are not in the address book (each can be added to the contacts or forgotten), and "Minerva", the rules
 * of the archive (how long strangers are kept, how trust grows) and its counts. Everything goes through ct-mnv.h; the
 * texts are the data's (the "contacts" part of `people ui`). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_strangers_new         (GtkWidget *window);
GtkWidget *ct_minerva_settings_new  (GtkWidget *window);

G_END_DECLS
