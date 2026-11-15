/* QuickContact's Minerva card: the person of Minerva's people (Minerva's repository, people/) linked to the contact.
 *
 * Linked: her notebook ("Quaderno") in a box at most max_height_dp tall, Edit (a page with the whole text) and "Apri
 * in Persone" (Neo Settings' Persone page, through its launch-panel action). Not linked: "Collega a una persona",
 * a list of the people and "Nuova persona" (made with the contact's name), then linked.
 *
 * Everything goes through `minerva-config people … --json`, run by Neo Settings' own ns-mc.c, the same door
 * Settings > Minerva > Persone uses: the texts, sizes, poll interval and Settings' page are the "contacts" part of
 * `people ui` (config/minerva_config/people_ui.json); the person comes from `people by-contact`, the notebook is
 * read and written only by `people notebook`, the link made by `people create` and `people link`. While the card
 * shows, it asks again every poll_s seconds, so a notebook changed elsewhere shows here. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* NULL when minerva-config is not installed; the card stays hidden until `people ui` answers */
GtkWidget *ct_minerva_card_new    (GtkWidget *window, const char *uid);
/* the contact's name now (a new person's name, the editor's title) and the card title's theme colour (ARGB) */
void       ct_minerva_card_update (GtkWidget *card, const char *name, guint32 title_argb);

G_END_DECLS
