/* Neo Settings' own pages; each pushes itself onto the navigation. */
#pragma once

#include <adwaita.h>
#include <json-glib/json-glib.h>

/* ns-pages.c: the hubs and the small pages */
void ns_page_connections (GtkWidget *row, gpointer data);
void ns_page_display (GtkWidget *row, gpointer data);
void ns_page_night_light (GtkWidget *row, gpointer data);
void ns_page_battery (GtkWidget *row, gpointer data);
void ns_page_battery_info (GtkWidget *row, gpointer data);
void ns_page_wallpaper (GtkWidget *row, gpointer data);
void ns_page_home (GtkWidget *row, gpointer data);
void ns_page_general (GtkWidget *row, gpointer data);
void ns_page_about (GtkWidget *row, gpointer data);

/* ns-net.c: NetworkManager */
void ns_page_wifi (GtkWidget *row, gpointer data);
void ns_page_mobile (GtkWidget *row, gpointer data);
void ns_page_hotspot (GtkWidget *row, gpointer data);
void ns_page_network (GtkWidget *row, gpointer data);
void ns_page_printers (GtkWidget *row, gpointer data);

/* ns-bt.c: BlueZ */
void ns_page_bluetooth (GtkWidget *row, gpointer data);
void ns_page_sharing (GtkWidget *row, gpointer data);

/* ns-sound.c: PulseAudio (PipeWire's), feedbackd, the Phone's and the Clock's sounds */
void ns_page_sound (GtkWidget *row, gpointer data);

/* ns-apps.c: apps, their notifications, default apps */
void ns_page_notifications (GtkWidget *row, gpointer data);
void ns_page_apps (GtkWidget *row, gpointer data);
void ns_page_default_apps (GtkWidget *row, gpointer data);
void ns_page_app (const char *id);

/* ns-shell.c: the lock screen (gnome-phone's keyguard), the on-screen keyboard (GNOME Shell's) */
void ns_page_lock_screen (GtkWidget *row, gpointer data);
void ns_page_keyboard (GtkWidget *row, gpointer data);
void ns_page_touch_keyboard (GtkWidget *row, gpointer data);

/* ns-fprint.c: fingerprints (fprintd); the Lock screen page's Biometrics card, shown once fprintd names a sensor */
void ns_page_fingerprint (GtkWidget *row, gpointer data);
void ns_fprint_lock_group (AdwNavigationPage *page, GtkWidget *content);

/* ns-privacy.c */
void ns_page_privacy (GtkWidget *row, gpointer data);
void ns_page_location (GtkWidget *row, gpointer data);
void ns_page_accounts (GtkWidget *row, gpointer data);
void ns_page_accessibility (GtkWidget *row, gpointer data);

/* ns-ssh.c: who may log in over SSH (authorized_keys), the user's own key pairs */
void ns_page_ssh (GtkWidget *row, gpointer data);
/* ssh-keygen's SSH_ASKPASS: this binary, run with NS_SSH_ASKPASS set, prints the passphrase it was given */
void ns_ssh_askpass (void);

/* ns-system.c */
void ns_page_language (GtkWidget *row, gpointer data);
void ns_page_datetime (GtkWidget *row, gpointer data);
void ns_page_users (GtkWidget *row, gpointer data);
/* Display's Screen group: dim and the power profile go in that group, automatic suspend after it (its own card) */
void ns_power_rows (GtkWidget *c, GtkWidget *g);
/* whether a daemon of the phone's own suspends it whenever the screen is off (then GNOME's automatic suspend has
 * nothing to do) */
gboolean ns_screen_off_sleeper (void);
void ns_page_mouse (GtkWidget *row, gpointer data);
void ns_page_wallpapers (GtkWidget *row, gpointer data);
void ns_wallpaper_ask (GtkWidget *w, const char *path, const char *dark);
void ns_page_software (GtkWidget *row, gpointer data);
/* the "Screen scale" row of Display: filled in once the compositor has answered */
void ns_page_scale_row (GtkWidget *group);

/* ns-minerva.c: Minerva's brain (models, fallbacks, voice, budgets), through minerva-config; shown only where it is
 * installed */
void     ns_page_minerva (GtkWidget *row, gpointer data);
/* the index's four pages: the model and its fallbacks, the voice, the quotas; their texts and order are
 * minerva_ui.json's (`minerva-config status --json` carries them) */
void     ns_page_minerva_brain (GtkWidget *row, gpointer data);
void     ns_page_minerva_voice (GtkWidget *row, gpointer data);
void     ns_page_minerva_budget (GtkWidget *row, gpointer data);
void     ns_page_api_keys (GtkWidget *row, gpointer data);
/* the agent's accounts (ns-account.c): a card for the Accounts page, filled from `minerva-config accounts list`
 * (hidden while it has none), and --page's way to the first account */
void     ns_accounts_minerva (GtkWidget *content);
void     ns_page_account (GtkWidget *row, gpointer data);
void     ns_page_prompt (GtkWidget *row, gpointer data);
/* ns-prompt.c: the system prompt and state template editors of Minerva > Prompt; `ui` is `prompt_ui` of the status */
void     ns_prompt_build (GtkWidget *content, GObject *owner, JsonObject *ui);
/* ns-people.c: Minerva > Persone (notebook, face samples, voiceprint, contact of each person); `--page people` is
 * the index, then Persone once `people ui` answers; the index's own row opens it directly */
void     ns_page_people_open (GtkWidget *row, gpointer data);
void     ns_page_people_index (GtkWidget *row, gpointer data);
/* a page of the models `minerva-config` offers, the current one marked; `picked` gets the model id (the source the
 * Minerva page's own pickers use) */
typedef void (*NsModelPicked) (const char *id, gpointer data);
void     ns_model_pick (GtkWidget *from, const char *title, const char *current, NsModelPicked picked,
                        gpointer data);
/* Battery page: the Max charge card (only where minerva-config is installed) */
void     ns_charge_limit_add (AdwNavigationPage *page, GtkWidget *content);

/* ns-perm.c: Security and privacy > Permissions (Minerva's agent, through minerva-perm); its row in the Security
 * page's first card, filled from `status --json` */
void ns_perm_add_row (GtkWidget *group);
void ns_page_permissions (GtkWidget *row, gpointer data);
