/* Settings (ContactsPreferenceActivity, spec §5.2), with its sub-screens: About Contacts, "Contacts to display"
 * (§5.3), and the import and export dialogs (§5.4). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_settings_new (GtkWidget *window);

/* a framework preference row (preference_material.xml): the title over an optional summary; @summary_label gets the
 * summary's label for ct_pref_set_summary; a tap runs @clicked (NeoPressable "clicked"), a row with none is not
 * tappable. The pages of the Minerva parts (ct-minerva-pages.c) are made of these too. */
GtkWidget *ct_pref_row         (const char *title, const char *summary, GtkWidget **summary_label, GCallback clicked,
                                gpointer data);
void       ct_pref_set_summary (GtkWidget *label, const char *text);

/* the .vcf import, as Settings' Import and a .vcf file opened with the app start it: the cards of @file into the
 * default address book, a toast when done */
void       ct_import_file  (GtkWidget *window, GFile *file);
/* the contacts @uids (strings) as one .vcf file, wherever the person saves it: Settings' "Export", and Share */
void       ct_export_contacts (GtkWidget *window, GPtrArray *uids, const char *title);

G_END_DECLS
