/* The Contacts window: Android's task of activities as a stack of pages (ct-page.h), the contact list at the
 * bottom. A page pushed on top slides in as activity_open_enter does and the one under it slides away
 * (activity_open_exit); Back slides them the other way (activity_close_*), spec §7.5. QuickContact is translucent:
 * it covers the page under it with its own scrim and animates itself.
 *
 * The window also has what outlives a page: the toast (drawn by SystemUI on Android) and the snackbar. */
#pragma once

#include "ct-store.h"

#include <gtk/gtk.h>
#include <neo.h>

G_BEGIN_DECLS

GtkWidget *ct_window_new           (GtkApplication *app, CtStore *store, GSettings *settings);

CtStore   *ct_window_get_store     (GtkWidget *window);
GSettings *ct_window_get_settings  (GtkWidget *window);

/* a page on top, and a page away (with whatever is above it) */
void       ct_window_push          (GtkWidget *window, GtkWidget *page);
void       ct_window_pop           (GtkWidget *window, GtkWidget *page);
/* the page on top */
GtkWidget *ct_window_top           (GtkWidget *window);

/* "Text copied", "Contact saved" (Toast.LENGTH_SHORT) */
void       ct_window_toast         (GtkWidget *window, const char *text);
/* the same toast, kept on screen until the next ct_window_toast: what a slow action is doing, where the busy line of
 * a long page is out of sight (ct-minerva.c) */
void       ct_window_toast_hold    (GtkWidget *window, const char *text);
/* the full-width snackbar ("Label deleted" UNDO), Snackbar.LENGTH_LONG */
void       ct_window_snackbar      (GtkWidget *window, const char *text, const char *action,
                                    NeoSnackbarAction callback, gpointer data);
NeoSnackbar *ct_window_get_snackbar (GtkWidget *window);

/* the screens, opened from anywhere (the list, another app's request) */
void       ct_window_show_contact  (GtkWidget *window, const char *uid);          /* QuickContact */
void       ct_window_edit_contact  (GtkWidget *window, const char *uid);          /* the editor */
/* the editor for a new contact, with @prefill's fields (a number from the Dialer) when not NULL */
void       ct_window_new_contact   (GtkWidget *window, EContact *prefill);
/* the people list: back to the top screen (a request from another app lands there) */
GtkWidget *ct_window_get_people    (GtkWidget *window);

G_END_DECLS
