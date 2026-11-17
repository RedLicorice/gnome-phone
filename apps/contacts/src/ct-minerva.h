/* QuickContact's Minerva section: everything Minerva keeps about the contact, on the contact's own page.
 *
 * Her notebook (view, and Modifica in Neo Prompt); the channels, WhatsApp and Telegram, as IM rows of the contact
 * (add, change, remove) and the Telegram invitation, not built yet, as a row that says so; whether her face and voice are known,
 * recording and forgetting the voice, learning it with the PIN; the kind, the numbers (samples, sightings,
 * confidence), merging a stranger's record into her, detaching and forgetting. A contact with no notebook is offered
 * "Crea il notebook", or the link to a record Minerva already has (a stranger's).
 *
 * The person is the one linked to the contact's UID (`people by-contact`), and her name is the contact's. Everything
 * goes through ct-mnv.h; the texts, sizes and poll interval are the data's. While the section shows it asks again
 * every poll_s seconds, so a notebook changed elsewhere, or a voice being recorded, shows here. Slow actions show
 * ct_busy's line. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* NULL when minerva-config is not installed; the card stays hidden until `people ui` answers */
GtkWidget *ct_minerva_card_new    (GtkWidget *window, const char *uid);
/* the contact's name now (a new person's name, the editor's title) and the card title's theme colour (ARGB) */
void       ct_minerva_card_update (GtkWidget *card, const char *name, guint32 title_argb);

G_END_DECLS
