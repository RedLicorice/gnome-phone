/* Neo Settings: the phone's settings app, One UI style, in C and GTK4. The main menu groups everything the way a
 * phone does; each entry is one of our own pages (ns-pages.h), or an app (Software). The services the pages
 * follow (NetworkManager, BlueZ, the sound server) are connected at start, in the background. */
#include "msg-assistant.h"
#include "ns-bt.h"
#include "ns-look.h"
#include "ns-net.h"
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-sound.h"
#include "ns-ui.h"

typedef struct {
  const char *icon, *color, *title, *subtitle, *keywords;
  NsActivate  page;            /* our own page, or */
  const char *const *argv;     /* another app */
  const char *needs_schema;    /* shown only if this settings schema is installed */
  gboolean  (*available) (void); /* shown only if this says so */
  gboolean  search_only;       /* a sub-page's row: hidden in the menu, shown while a search matches it */
} Entry;

static const char *const software[] = { "gnome-software", "--mode=updates", NULL };

/* NULL title: the next card */
static const Entry entries[] = {
  { "network-wireless-symbolic", "blue", "Connections", "Wi-Fi • Bluetooth • Mobile network",
    "wlan wifi internet sim data vpn hotspot flight airplane", ns_page_connections },
  { "bluetooth-symbolic", "blue", "Connected devices", "Bluetooth • Pair headphones and more",
    "pair headphones speaker watch", ns_page_bluetooth },
  { NULL },
  { "audio-volume-high-symbolic", "violet", "Sounds and vibration", "Sound mode • Volume • Ringtone",
    "ringtone speaker microphone vibrate mute silent haptic", ns_page_sound },
  { "preferences-system-notifications-symbolic", "orange", "Notifications", "App notifications • Do not disturb",
    "dnd banner pop-up lock screen emergency alerts", ns_page_notifications },
  { NULL },
  { "display-brightness-symbolic", "lime", "Display", "Brightness • Font size • Screen timeout",
    "screen timeout dim suspend power saving rotate scale double tap wake adaptive brightness night light intensity "
    "warm amber aod always on always-on dock resta acceso carica",
    ns_page_display },
  { "battery-full-symbolic", "green", "Battery", "Charge • Battery health",
    "charge health temperature voltage capacity max charge limit",
    ns_page_battery },
  { NULL },
  { "preferences-desktop-wallpaper-symbolic", "pink", "Wallpaper and style",
    "Look • Dark mode • Fonts • Wallpapers",
    "dark theme look font fonts display body mono cyberpunk renaissance marble sea forest plum accent background",
    ns_page_wallpaper },
  { "view-app-grid-symbolic", "pink", "Home screen", "Grid • Icon size • Icon pack",
    "launcher neo grid columns rows layout icon size pack shape app names labels drawer search", ns_page_home, NULL,
    "de.yesman.neo" },
  { NULL },
  { "security-high-symbolic", "purple", "Security and privacy", "Lock screen • Location • Permissions • History",
    "lock screen lockscreen pattern pin swipe password unlock fingerprint finger biometrics gps geoclue location "
    "camera microphone history trash ssh keys permissions agent rules classifier allow deny",
    ns_page_privacy },
  { NULL },
  { "avatar-default-symbolic", "blue", "Accounts", "Online accounts • API keys",
    "google nextcloud email api key token provider endpoint llm secret", ns_page_accounts },
  { NULL },
  { "face-smile-symbolic", "purple", "Minerva", "Modello • Voce • Prompt • Budget",   /* the platform's page */
    "ai agent brain cervello llm opencode piper voice voce wakeword prompt budget cost", ns_page_minerva, NULL,
    NULL, ns_minerva_available },
  { NULL },
  /* the Minerva index's pages: one row each, hidden in the menu and shown only while a search matches it, so
   * "wakeword" or "budget" lands on its own page. The titles are the index's (minerva_ui.json). */
  { "applications-science-symbolic", "blue", "Cervello", "Modello • Catena di riserva • Ruoli",
    "model modello brain cervello fallback catena riserva ruoli roles", ns_page_minerva_brain, NULL, NULL,
    ns_minerva_available, TRUE },
  { "audio-input-microphone-symbolic", "violet", "Voce", "Parola di attivazione • Risposta • Riconoscimento",
    "voice voce wake wakeword parola attivazione risposta reply stt riconoscimento voiceprint impronta vocale engine "
    "motore", ns_page_minerva_voice, NULL, NULL, ns_minerva_available, TRUE },
  { "format-justify-left-symbolic", "violet", "Prompt", "System prompt • Template di stato",
    "prompt system stato state template", ns_page_prompt, NULL, NULL, ns_minerva_available, TRUE },
  { "preferences-system-notifications-symbolic", "orange", "Eventi", "App • Canali • Livelli • Sveglie",
    "eventi events app canali canale livelli livello notifiche sveglie sveglia wake", ns_page_events, NULL, NULL,
    ns_minerva_available, TRUE },
  { "chat-message-new-symbolic", "green", "Messaggi", "WhatsApp • Telegram",
    "messaggi messaging whatsapp telegram chat provider lettura scrittura whitelist token", ns_page_messaging, NULL, NULL,
    ns_minerva_available, TRUE },
  { "x-office-spreadsheet-symbolic", "orange", "Budget", "Tetti di spesa • Eventi • Avvisi",
    "budget budgets spesa costo costi quota tetti limiti eventi events bypass", ns_page_minerva_budget, NULL, NULL,
    ns_minerva_available, TRUE },
  { "network-wireless-symbolic", "green", "Rete intelligente", "Soglie del Wi-Fi • Controllo di internet • Ripristino",
    "rete intelligente smart network wifi dati mobili sim internet offline controllo ripristino soglia segnale debole "
    "connectivity", ns_page_minerva_smart_network, NULL, NULL, ns_minerva_available, TRUE },
  { "document-open-recent-symbolic", "purple", "Memoria", "Impara • Ricordi • Quaderno • Fiducia",
    "memoria memory impara learning ricordi recall quaderno notebook fiducia trust sconosciuti strangers",
    ns_page_minerva_memory, NULL, NULL, ns_minerva_available, TRUE },
  { "face-smile-big-symbolic", "pink", "Avatar", "Dimensioni • Animazione • Aspetto • Occhi • Gesti",
    "avatar viso faccia face dimensione grandezza size animazione aspetto colore occhi gesti tocco pulsante flottante fab",
    ns_page_minerva_avatar, NULL, NULL, ns_minerva_available, TRUE },
  { NULL },
  { "view-grid-symbolic", "blue", "Apps", "Default apps • App settings",
    "permissions storage", ns_page_apps },
  { "preferences-system-symbolic", "violet", "General management", "Language and keyboard • Date and time",
    "region locale clock users mouse touchpad", ns_page_general },
  { "preferences-desktop-accessibility-symbolic", "green", "Accessibility", "Vision • Interaction",
    "zoom contrast screen reader animations", ns_page_accessibility },
  { NULL },
  { "software-update-available-symbolic", "blue", "Software update", "Download and install",
    "upgrade packages", NULL, software },
  { "help-about-symbolic", "grey", "About phone", "Status • Legal information • Phone name",
    "model kernel version memory storage", ns_page_about },
};

static GPtrArray *groups;        /* the cards, to hide the ones a search empties */
static GPtrArray *menu_rows;     /* the menu's rows, each with its Entry ("ns-entry") */
static GtkWidget *search_entry, *title_box;

/* Recent Android's two-pane Settings: wide enough (the split-condition setting), the menu on the left and the page
 * on the right (AdwNavigationSplitView, expanded by an AdwBreakpoint); otherwise collapsed, the menu opens the page
 * over itself, as before. The page pane is a navigation of its own (nav_view), so a page's sub-pages open inside
 * it with a back arrow while the menu stays; its first page has no back arrow side by side. */
static GtkWidget *split, *nav_view;
static GSettings *app_settings;  /* de.yesman.NeoSettings; NULL where it is not installed: never side by side */
static GtkWidget *current_row;   /* the menu row whose page the pane shows, marked side by side */

static const char *entry_id (const Entry *e);

static GtkWidget *
menu_row_for (NsActivate page)
{
  for (guint i = 0; menu_rows && i < menu_rows->len; i++) {
    const Entry *e = g_object_get_data (menu_rows->pdata[i], "ns-entry");
    if (e->page == page)
      return menu_rows->pdata[i];
  }
  return NULL;
}

/* the menu row of the pane's first page is the current one, and is remembered (a page not in the menu: none) */
static void
mark_current (NsActivate page)
{
  if (current_row)
    gtk_widget_remove_css_class (current_row, "ns-current");
  current_row = menu_row_for (page);
  if (!current_row)
    return;
  gtk_widget_add_css_class (current_row, "ns-current");
  const char *id = entry_id (g_object_get_data (G_OBJECT (current_row), "ns-entry"));
  if (app_settings && id)
    g_settings_set_string (app_settings, "last-page", id);
}

/* `page` becomes the pane's first page; `show`: and the pane is shown (collapsed, over the menu) */
static gboolean
open_root (NsActivate page, GtkWidget *row, gpointer data, gboolean show)
{
  if (!ns_open_root (page, row, data))
    return FALSE;
  mark_current (page);
  if (show)
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (split), TRUE);
  return TRUE;
}

static void
activate_entry (GtkWidget *row, gpointer data)
{
  const Entry *e = data;
  if (e->page)
    open_root (e->page, row, NULL, TRUE);
  else if (e->argv)
    ns_spawn (e->argv);
}

/* side by side the pane is never empty: the page opened last, else the menu's first */
static void
fill_pane (void)
{
  if (adw_navigation_view_get_visible_page (ADW_NAVIGATION_VIEW (nav_view)))
    return;
  g_autofree char *last = app_settings ? g_settings_get_string (app_settings, "last-page") : NULL;
  GtkWidget *row = NULL;
  for (guint i = 0; !row && menu_rows && i < menu_rows->len; i++) {
    const Entry *e = g_object_get_data (menu_rows->pdata[i], "ns-entry");
    const char *id = entry_id (e);
    if (last && id && g_str_equal (last, id))
      row = menu_rows->pdata[i];
  }
  for (guint i = 0; !row && menu_rows && i < menu_rows->len; i++) {
    const Entry *e = g_object_get_data (menu_rows->pdata[i], "ns-entry");
    if (e->page && !e->search_only)
      row = menu_rows->pdata[i];
  }
  if (row) {
    const Entry *e = g_object_get_data (G_OBJECT (row), "ns-entry");
    /* the pane fills beside the menu: turned back upright, the menu is what shows, as it did */
    open_root (e->page, row, NULL, FALSE);
  }
}

static gboolean
installed (const Entry *e)
{
  if (e->available && !e->available ())
    return FALSE;
  if (e->needs_schema) {
    if (!ns_settings_exists (e->needs_schema))
      return FALSE;
  }
  if (e->argv) {
    g_autofree char *path = g_find_program_in_path (e->argv[0]);
    return path != NULL;
  }
  return TRUE;
}

/* ---- search: every word somewhere in the title, subtitle or keywords ---- */

static void
search_changed (GtkSearchEntry *entry, gpointer data)
{
  g_autofree char *q = g_utf8_strdown (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  g_auto (GStrv) words = g_str_tokenize_and_fold (q, NULL, NULL);
  for (guint i = 0; i < groups->len; i++) {
    GtkWidget *box = groups->pdata[i];
    gboolean any = FALSE;
    for (GtkWidget *r = gtk_widget_get_first_child (box); r; r = gtk_widget_get_next_sibling (r)) {
      const char *text = g_object_get_data (G_OBJECT (r), "ns-search");
      if (!text)
        continue;
      gboolean all = TRUE;
      for (guint w = 0; words && words[w]; w++)
        all = all && strstr (text, words[w]);
      /* a sub-page's row shows only while a search names it: the menu keeps one Minerva row */
      if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (r), "ns-search-only")))
        all = all && words && words[0] != NULL;
      gtk_widget_set_visible (r, all);
      any = any || all;
    }
    gtk_widget_set_visible (box, any);
    gtk_list_box_invalidate_headers (GTK_LIST_BOX (box));
  }
}

static void
search_toggled (GtkToggleButton *b, gpointer data)
{
  gboolean on = gtk_toggle_button_get_active (b);
  gtk_widget_set_visible (search_entry, on);
  gtk_widget_set_visible (title_box, !on);
  if (on)
    gtk_widget_grab_focus (search_entry);
  else
    gtk_editable_set_text (GTK_EDITABLE (search_entry), "");
}

static AdwNavigationPage *
main_page (void)
{
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* One UI's head: "Settings" large on the left, search on the right */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (head, "ns-main-head");
  title_box = gtk_label_new ("Settings");
  gtk_label_set_xalign (GTK_LABEL (title_box), 0);
  gtk_widget_set_hexpand (title_box, TRUE);
  gtk_widget_add_css_class (title_box, "ns-big");
  search_entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_entry), "Search settings");
  gtk_widget_set_hexpand (search_entry, TRUE);
  gtk_widget_set_visible (search_entry, FALSE);
  g_signal_connect (search_entry, "search-changed", G_CALLBACK (search_changed), NULL);
  GtkWidget *find = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (find), "system-search-symbolic");
  gtk_widget_add_css_class (find, "flat");
  gtk_widget_add_css_class (find, "circular");
  gtk_widget_set_valign (find, GTK_ALIGN_CENTER);
  g_signal_connect (find, "toggled", G_CALLBACK (search_toggled), NULL);
  gtk_box_append (GTK_BOX (head), title_box);
  gtk_box_append (GTK_BOX (head), search_entry);
  gtk_box_append (GTK_BOX (head), find);
  gtk_box_append (GTK_BOX (outer), head);

  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (col, "ns-column");
  GtkWidget *sc = ns_scroller_new (col);
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_box_append (GTK_BOX (outer), sc);

  groups = g_ptr_array_new ();
  menu_rows = g_ptr_array_new ();
  GtkWidget *g = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (entries); i++) {
    const Entry *e = &entries[i];
    if (!e->title) {
      g = NULL;
      continue;
    }
    if (!installed (e))
      continue;
    if (!g) {
      g = ns_group (col, NULL);
      g_ptr_array_add (groups, g);
    }
    g_autofree char *title = msg_assistant_fill (e->title);   /* {assistant} in a title: the node's assistant */
    GtkWidget *r = ns_row (g, e->icon, e->color, title, e->subtitle);
    gtk_widget_set_visible (r, !e->search_only);
    g_object_set_data (G_OBJECT (r), "ns-search-only", GINT_TO_POINTER (e->search_only));
    g_autofree char *text = g_strjoin (" ", title, e->subtitle, e->keywords, NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-search", g_utf8_strdown (text, -1), g_free);
    ns_row_on_activate (r, activate_entry, (gpointer) e);
    g_object_set_data (G_OBJECT (r), "ns-entry", (gpointer) e);
    g_ptr_array_add (menu_rows, r);
  }

  AdwNavigationPage *page = adw_navigation_page_new (outer, "Settings");
  adw_navigation_page_set_tag (page, "main");
  gtk_widget_add_css_class (GTK_WIDGET (page), "ns-root");
  return page;
}

/* ---- the app ---- */

/* back (Alt+Left from Neo's edge swipe) and Escape: the page before in the pane; collapsed, from its first page the
 * menu (focus on the row it came from); from the menu, or side by side from the pane's first page, out of Settings */
static gboolean
go_back (GtkWidget *win, GVariant *args, gpointer data)
{
  AdwNavigationSplitView *sv = ADW_NAVIGATION_SPLIT_VIEW (split);
  gboolean over_menu = adw_navigation_split_view_get_collapsed (sv) && adw_navigation_split_view_get_show_content (sv);
  if (over_menu || !adw_navigation_split_view_get_collapsed (sv))
    if (adw_navigation_view_pop (ADW_NAVIGATION_VIEW (nav_view)))
      return TRUE;
  if (over_menu) {
    adw_navigation_split_view_set_show_content (sv, FALSE);
    if (current_row)
      gtk_widget_grab_focus (current_row);
    return TRUE;
  }
  gtk_window_close (GTK_WINDOW (win));
  return TRUE;
}

static void
add_back (GtkWidget *win)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      gtk_callback_action_new (go_back, NULL, NULL)));
  gtk_widget_add_controller (win, keys);
}

/* ---- side by side ---- */

static void
split_applied (AdwBreakpoint *bp, GtkWidget *win)
{
  gtk_widget_add_css_class (win, "ns-split");
  fill_pane ();
}

static void
split_unapplied (AdwBreakpoint *bp, GtkWidget *win)
{
  gtk_widget_remove_css_class (win, "ns-split");
}

/* the condition follows the setting live; an empty or unreadable one never holds */
static void
split_condition_changed (GSettings *s, const char *key, AdwBreakpoint *bp)
{
  g_autofree char *text = g_settings_get_string (s, key);
  AdwBreakpointCondition *c = *g_strstrip (text) ? adw_breakpoint_condition_parse (text) : NULL;
  adw_breakpoint_set_condition (bp, c);
  if (c)
    adw_breakpoint_condition_free (c);
}

/* the pane's page carries the title of the page it shows (what the window and the screen reader name) */
static void
pane_page_changed (AdwNavigationView *nav, GParamSpec *ps, AdwNavigationPage *pane)
{
  AdwNavigationPage *p = adw_navigation_view_get_visible_page (nav);
  adw_navigation_page_set_title (pane, p ? adw_navigation_page_get_title (p) : "Settings");
}

/* side by side, a page opened inside the pane is what the user is at: turned upright, it shows, not the menu */
static void
pane_pushed (AdwNavigationView *nav, AdwNavigationSplitView *sv)
{
  if (!adw_navigation_split_view_get_collapsed (sv))
    adw_navigation_split_view_set_show_content (sv, TRUE);
}

static GtkWidget *
split_new (GtkWidget *win)
{
  split = adw_navigation_split_view_new ();
  /* collapsed unless the breakpoint holds: what the app was before, also where the schema is missing */
  adw_navigation_split_view_set_collapsed (ADW_NAVIGATION_SPLIT_VIEW (split), TRUE);
  nav_view = adw_navigation_view_new ();
  ns_ui_init (ADW_NAVIGATION_VIEW (nav_view));
  AdwNavigationPage *pane = adw_navigation_page_new (nav_view, "Settings");
  g_signal_connect_object (nav_view, "notify::visible-page", G_CALLBACK (pane_page_changed), pane, 0);
  g_signal_connect_object (nav_view, "pushed", G_CALLBACK (pane_pushed), split, 0);
  adw_navigation_split_view_set_sidebar (ADW_NAVIGATION_SPLIT_VIEW (split), main_page ());
  adw_navigation_split_view_set_content (ADW_NAVIGATION_SPLIT_VIEW (split), pane);

  app_settings = ns_settings ("de.yesman.NeoSettings");
  if (!app_settings) {
    g_message ("de.yesman.NeoSettings is not installed: the menu and the page never side by side");
    return split;
  }
  g_settings_bind (app_settings, "sidebar-width-fraction", split, "sidebar-width-fraction", G_SETTINGS_BIND_GET);
  g_settings_bind (app_settings, "sidebar-min-width", split, "min-sidebar-width", G_SETTINGS_BIND_GET);
  g_settings_bind (app_settings, "sidebar-max-width", split, "max-sidebar-width", G_SETTINGS_BIND_GET);
  g_settings_bind (app_settings, "window-min-width", win, "width-request", G_SETTINGS_BIND_GET);
  g_settings_bind (app_settings, "window-min-height", win, "height-request", G_SETTINGS_BIND_GET);
  /* no condition yet (adw_breakpoint_new wants one): the setting gives it below */
  AdwBreakpoint *bp = g_object_new (ADW_TYPE_BREAKPOINT, NULL);
  g_auto (GValue) expanded = G_VALUE_INIT;
  g_value_init (&expanded, G_TYPE_BOOLEAN);
  g_value_set_boolean (&expanded, FALSE);
  adw_breakpoint_add_setter (bp, G_OBJECT (split), "collapsed", &expanded);
  g_signal_connect_object (bp, "apply", G_CALLBACK (split_applied), win, 0);
  g_signal_connect_object (bp, "unapply", G_CALLBACK (split_unapplied), win, 0);
  g_signal_connect_object (app_settings, "changed::split-condition", G_CALLBACK (split_condition_changed), bp, 0);
  split_condition_changed (app_settings, "split-condition", bp);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (win), bp);
  return split;
}

static void
dark_changed (AdwStyleManager *sm, GParamSpec *ps, GtkWidget *win)
{
  if (adw_style_manager_get_dark (sm))
    gtk_widget_add_css_class (win, "ns-dark");
  else
    gtk_widget_remove_css_class (win, "ns-dark");
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "Settings");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  gtk_widget_add_css_class (win, "ns-window");
  AdwStyleManager *sm = adw_style_manager_get_default ();
  g_signal_connect_object (sm, "notify::dark", G_CALLBACK (dark_changed), win, 0);
  dark_changed (sm, NULL, win);
  /* style.css with the tokens of the Renaissance look in force, and the look's classes on the window, live with the
   * settings (the theme's wallpaper is swapped in by the shell extension, theme.js) */
  ns_look_init (win);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), split_new (win));
  add_back (win);
  gtk_window_present (GTK_WINDOW (win));
}

/* the app list, read once in the background so that pages listing apps find it read */
static void
apps_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  g_list_free_full (g_app_info_get_all (), g_object_unref);
  g_task_return_boolean (task, TRUE);
}

static void
startup (GApplication *app, gpointer data)
{
  /* the buses once, here: the pages then get them without waiting */
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, NULL, NULL);
  ns_net_init ();
  ns_bt_init ();
  ns_sound_init ();
  g_autoptr (GTask) t = g_task_new (NULL, NULL, NULL, NULL);
  g_task_run_in_thread (t, apps_thread);
}

/* a page by name: `--page home` (the launcher's "Settings" opens the home screen's), the panel names gnome-phone asks
 * for (a quick settings tile's long press, the volume panel's and the notification shade's settings buttons) and
 * GNOME Settings' names, which other apps ask for. "network" is the flight mode tile's: Connections has the switch.
 * The first name of a page is its primary one: --list-pages (below) prints it as the page's id. */
static const struct {
  const char *name;
  NsActivate  page;
} direct[] = {
  { "home", ns_page_home }, { "multitasking", ns_page_home },
  { "wallpaper", ns_page_wallpaper }, { "background", ns_page_wallpaper }, { "appearance", ns_page_wallpaper },
  { "theme", ns_page_wallpaper }, { "fonts", ns_page_wallpaper },
  { "wifi", ns_page_wifi }, { "wlan", ns_page_wifi },
  { "hotspot", ns_page_hotspot }, { "wifi-hotspot", ns_page_hotspot },
  { "wwan", ns_page_mobile }, { "mobile-broadband", ns_page_mobile }, { "mobile", ns_page_mobile },
  { "connections", ns_page_connections }, { "network", ns_page_connections },
  { "vpn", ns_page_network }, { "wired", ns_page_network },
  { "bluetooth", ns_page_bluetooth },
  { "sharing", ns_page_sharing }, { "printers", ns_page_printers },
  { "sound", ns_page_sound }, { "feedback", ns_page_sound },
  { "notifications", ns_page_notifications },
  { "power", ns_page_battery }, { "battery", ns_page_battery },
  { "battery-info", ns_page_battery_info }, { "battery-information", ns_page_battery_info },
  { "aod", ns_page_display }, { "always-on", ns_page_display }, { "display", ns_page_display }, { "displays", ns_page_display }, { "compositor", ns_page_display },
  { "night-light", ns_page_night_light }, { "nightlight", ns_page_night_light },
  { "fingerprint", ns_page_fingerprint }, { "fingerprints", ns_page_fingerprint },
  { "lockscreen", ns_page_lock_screen }, { "lock-screen", ns_page_lock_screen }, { "screen-lock", ns_page_lock_screen },
  { "keyboard", ns_page_keyboard }, { "osk", ns_page_keyboard }, { "touch-keyboard", ns_page_touch_keyboard },
  { "mouse", ns_page_mouse },
  { "security", ns_page_privacy }, { "privacy", ns_page_privacy },
  { "location", ns_page_location },
  { "accounts", ns_page_accounts }, { "online-accounts", ns_page_accounts },
  { "account", ns_page_account },
  { "applications", ns_page_apps }, { "apps", ns_page_apps }, { "overview", ns_page_apps },
  { "default-apps", ns_page_default_apps },
  { "region", ns_page_language }, { "language", ns_page_language },
  { "datetime", ns_page_datetime }, { "date-time", ns_page_datetime },
  { "users", ns_page_users }, { "user-accounts", ns_page_users },
  { "ssh", ns_page_ssh },
  { "system", ns_page_general },
  { "accessibility", ns_page_accessibility }, { "universal-access", ns_page_accessibility },
  { "about", ns_page_about }, { "info-overview", ns_page_about },
  { "minerva", ns_page_minerva },
  { "brain", ns_page_minerva_brain }, { "cervello", ns_page_minerva_brain }, { "modello", ns_page_minerva_brain },
  { "voice", ns_page_minerva_voice }, { "voce", ns_page_minerva_voice },
  { "wakeword", ns_page_minerva_voice }, { "wake-word", ns_page_minerva_voice }, { "wake", ns_page_minerva_voice },
  { "stt", ns_page_minerva_voice }, { "voiceprint", ns_page_minerva_voice }, { "impronta-vocale", ns_page_minerva_voice },
  { "minerva-prompt", ns_page_prompt }, { "prompt", ns_page_prompt },
  { "budget", ns_page_minerva_budget }, { "budgets", ns_page_minerva_budget }, { "spesa", ns_page_minerva_budget },
  { "smart-network", ns_page_minerva_smart_network }, { "minerva-network", ns_page_minerva_smart_network },
  { "memory", ns_page_minerva_memory }, { "memoria", ns_page_minerva_memory },
  { "dock", ns_page_display },
  { "minerva-avatar", ns_page_minerva_avatar }, { "avatar", ns_page_minerva_avatar },
  { "events", ns_page_events }, { "eventi", ns_page_events },
  { "messaging", ns_page_messaging }, { "messaggi", ns_page_messaging },
  { "federation", ns_page_federation }, { "federazione", ns_page_federation },
  { "chat", ns_page_messaging }, { "chats", ns_page_messaging },
  { "permissions", ns_page_permissions }, { "agent-permissions", ns_page_permissions },
};

/* an entry's id: the primary --page name of its page, NULL when it has none */
static const char *
entry_id (const Entry *e)
{
  for (guint d = 0; e->page && d < G_N_ELEMENTS (direct); d++)
    if (direct[d].page == e->page)
      return direct[d].name;
  return NULL;
}

/* `--list-pages`: the pages the menu shows, as one JSON array, for the launcher's search provider: id (the primary
 * --page name from direct[]), title, subtitle, keywords, icon. Derived from entries[] and direct[], no table of
 * its own; what the menu hides (schema not installed, not available) and the entries that open another app are
 * left out. Sub-pages (wifi, hotspot, ...) have no title or keywords of their own, so they are not listed, though
 * --page opens them. Needs no display and no running app. */
static int
list_pages (void)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (guint i = 0; i < G_N_ELEMENTS (entries); i++) {
    const Entry *e = &entries[i];
    if (!e->title || !e->page || !installed (e))
      continue;
    const char *id = entry_id (e);
    if (!id)
      continue;
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "id");
    json_builder_add_string_value (b, id);
    g_autofree char *title = msg_assistant_fill (e->title);
    json_builder_set_member_name (b, "title");
    json_builder_add_string_value (b, title);
    json_builder_set_member_name (b, "subtitle");
    json_builder_add_string_value (b, e->subtitle ? e->subtitle : "");
    json_builder_set_member_name (b, "keywords");
    json_builder_add_string_value (b, e->keywords ? e->keywords : "");
    json_builder_set_member_name (b, "icon");
    json_builder_add_string_value (b, e->icon ? e->icon : "");
    json_builder_end_object (b);
  }
  json_builder_end_array (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  g_autofree char *out = json_generator_to_data (gen, NULL);
  g_print ("%s\n", out);
  return 0;
}

static void
app_page (GtkWidget *row, gpointer id)
{
  ns_page_app (id);
}

/* the page asked for becomes the pane's first, the pages before gone (the menu's row marked, if it is one) */
static void
open_page (GApplication *app, const char *name, const char *arg)
{
  g_application_activate (app);
  if (arg && *arg && (!g_strcmp0 (name, "applications") || !g_strcmp0 (name, "apps"))) {
    open_root (app_page, NULL, (gpointer) arg, TRUE);
    return;
  }
  for (guint i = 0; name && *name && i < G_N_ELEMENTS (direct); i++)
    if (g_str_equal (name, direct[i].name)) {
      open_root (direct[i].page, NULL, NULL, TRUE);
      return;
    }
  if (name && *name)
    g_message ("no page for the panel '%s': the main menu", name);
  adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (split), FALSE);
}

/* org.gnome.Settings' "launch-panel" (and Mobile Settings' "set-panel"): (panel, [parameters]) */
static void
launch_panel (GSimpleAction *action, GVariant *param, gpointer app)
{
  const char *name = NULL;
  g_autoptr (GVariant) args = NULL;
  g_variant_get (param, "(&s@av)", &name, &args);
  /* the first parameter, a string: GNOME Settings' "applications" takes the app's id there */
  g_autoptr (GVariant) first = g_variant_n_children (args) ? g_variant_get_child_value (args, 0) : NULL;
  g_autoptr (GVariant) inner = first ? g_variant_get_variant (first) : NULL;
  const char *arg = inner && g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING) ? g_variant_get_string (inner, NULL)
                                                                                   : NULL;
  open_page (app, name, arg);
}

static int
command_line (GApplication *app, GApplicationCommandLine *cl, gpointer data)
{
  const char *name = NULL, *id = NULL;
  GVariantDict *opts = g_application_command_line_get_options_dict (cl);
  g_variant_dict_lookup (opts, "page", "&s", &name);
  g_variant_dict_lookup (opts, "app", "&s", &id);
  if (id && !name)
    name = "applications";
  if (name)
    open_page (app, name, id);
  else
    g_application_activate (app);
  return 0;
}

int
main (int argc, char **argv)
{
  /* run by ssh-keygen as its SSH_ASKPASS (ns-ssh.c): print the passphrase, no app */
  if (g_getenv ("NS_SSH_ASKPASS")) {
    ns_ssh_askpass ();
    return 0;
  }
  /* before the app: no display, no instance */
  for (int i = 1; i < argc; i++)
    if (g_str_equal (argv[i], "--list-pages"))
      return list_pages ();
  g_autoptr (AdwApplication) app = adw_application_new ("de.yesman.NeoSettings", G_APPLICATION_HANDLES_COMMAND_LINE);
  const GOptionEntry options[] = {
    { "page", 0, 0, G_OPTION_ARG_STRING, NULL, "Open a page: home, wallpaper, wifi, hotspot, bluetooth, power…", "PAGE" },
    { "app", 0, 0, G_OPTION_ARG_STRING, NULL, "Open an app's page (its desktop file id)", "ID" },
    { "list-pages", 0, 0, G_OPTION_ARG_NONE, NULL, "Print the pages as JSON and exit (handled before the app)", NULL },
    { NULL }
  };
  g_application_add_main_option_entries (G_APPLICATION (app), options);
  static const GActionEntry actions[] = {
    { .name = "launch-panel", .activate = launch_panel, .parameter_type = "(sav)" },
    { .name = "set-panel", .activate = launch_panel, .parameter_type = "(sav)" },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (command_line), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
