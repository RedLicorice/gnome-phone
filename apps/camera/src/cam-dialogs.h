/* cam-dialogs: the two dialogs of the Camera.
 *
 *   cam_list_dialog       Settings' list preference (an AlertDialog's single-choice list and Cancel; §20.1): the
 *                         choice answers at once, as ListPreferenceDialogFragmentCompat does
 *   cam_location_dialog   "Add photo and video locations" (§19.2, L/location_permissions_dialog.xml): asked once,
 *                         when "Save location data" was never set and the phone can locate at all */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @chosen: the index picked, or -1 for Cancel (or Back, or a tap outside) */
typedef void (*CamChoiceDone) (int chosen, gpointer data);
void cam_list_dialog     (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                          CamChoiceDone done, gpointer data);

/* @turn_on: "Turn on"; FALSE for "Later", Back or a tap outside */
typedef void (*CamLocationDone) (gboolean turn_on, gpointer data);
void cam_location_dialog (GtkWidget *parent, CamLocationDone done, gpointer data);

/* A passphrase asked for (an API key code that is encrypted): `message` under the title, a hidden field, Cancel and
 * Import. @passphrase is handed over (wipe it with cam_scrub), or NULL for Cancel, Back or a tap outside. */
typedef void (*CamPassphraseDone) (char *passphrase, gpointer data);
void cam_passphrase_dialog (GtkWidget *parent, const char *message, CamPassphraseDone done, gpointer data);

G_END_DECLS
