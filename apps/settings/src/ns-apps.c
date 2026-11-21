/* Notifications, Apps and Default apps. An app's notifications are GNOME's per-app settings
 * (org.gnome.desktop.notifications.application at .../application/<munged id>/, listed in application-children).
 * GNOME Shell follows them (messageTray.js NotificationApplicationPolicy): "enable" keeps the app's notifications
 * or drops them, "show-banners" (with the global one, Do not disturb) lets them pop up (gnome-phone's headsup.js
 * asks the policy), "show-in-lock-screen" (with the global one) puts the app's icon on the lock screen
 * (keyguard.js and aod.js ask the policy). Emergency alerts are cellbroadcastd's (org.freedesktop.cbd). */
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>

#define NOTIFY_APP "org.gnome.desktop.notifications.application"

static gint
by_name (gconstpointer a, gconstpointer b, gpointer data)
{
  g_autofree char *x = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) a), -1);
  g_autofree char *y = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) b), -1);
  return g_utf8_collate (x, y);
}

/* a relocatable schema at <prefix><munged id>/, or NULL when it is not installed */
static GSettings *
app_settings (const char *schema, const char *prefix, GAppInfo *app)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  if (!sc)
    return NULL;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_autofree char *path = g_strconcat (prefix, id, "/", NULL);
  return g_settings_new_full (sc, NULL, path);
}

/* GNOME Settings' way: an app's notification settings count once its id is in application-children */
static GSettings *
notify_settings (GAppInfo *app)
{
  GSettings *s = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (!s)
    return NULL;
  g_autoptr (GSettings) top = ns_settings ("org.gnome.desktop.notifications");
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_auto (GStrv) kids = top ? g_settings_get_strv (top, "application-children") : NULL;
  if (top && !g_strv_contains ((const char *const *) kids, id)) {
    GStrvBuilder *b = g_strv_builder_new ();
    g_strv_builder_addv (b, (const char **) kids);
    g_strv_builder_add (b, id);
    g_auto (GStrv) nk = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    g_settings_set_strv (top, "application-children", (const char *const *) nk);
    if (ns_has_key (s, "application-id"))
      g_settings_set_string (s, "application-id", g_app_info_get_id (app));
  }
  return s;
}

static gboolean
uses_notifications (GAppInfo *app, const char *const *kids)
{
  if (!G_IS_DESKTOP_APP_INFO (app) || !g_app_info_should_show (app))
    return FALSE;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  return g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (app), "X-GNOME-UsesNotifications")
         || g_strv_contains (kids, id);
}

/* ---- an app's notifications ---- */

static void
app_notify_page (GtkWidget *row, gpointer app)
{
  GSettings *s = notify_settings (app);
  if (!s)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-notify", s, g_object_unref);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "Allow notifications", NULL, s, "enable");
  g = ns_group (c, NULL);
  GtkWidget *rows[4] = { NULL };
  guint n = 0;
  if (ns_has_key (s, "show-banners"))
    rows[n++] = ns_row_switch (g, "Pop-up", "Show a banner at the top of the screen", s, "show-banners");
  if (ns_has_key (s, "show-in-lock-screen"))
    rows[n++] = ns_row_switch (g, "Show on the lock screen", "The app's icon below the clock", s,
                               "show-in-lock-screen");
  /* No "Sound" switch (enable-sound-alerts): GNOME Shell honours it, but only for a notification that names a
   * sound, and no app here sends one (Sounds and vibration says more). */
  for (guint i = 0; i < n; i++)
    g_settings_bind (s, "enable", rows[i], "sensitive", G_SETTINGS_BIND_GET);
  ns_push (page);
}

static void
allowed_changed (GSettings *s, const char *key, GtkWidget *row)
{
  ns_row_set_subtitle (row, g_settings_get_boolean (s, "enable") ? "Allowed" : "Off");
}

/* ---- Emergency alerts: cellbroadcastd notifies of the alert levels its "levels" flags name ---- */

/* the alerts come only from a modem with cell broadcast: the row shows once ModemManager says one has it */
static void
cbc_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  gboolean any = FALSE;
  if (v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    GVariantIter it;
    GVariant *ifaces;
    g_variant_iter_init (&it, objs);
    while (!any && g_variant_iter_next (&it, "{&o@a{sa{sv}}}", NULL, &ifaces)) {
      g_autoptr (GVariant) cb = g_variant_lookup_value (ifaces, "org.freedesktop.ModemManager1.Modem.CellBroadcast",
                                                        NULL);
      any = cb != NULL;
      g_variant_unref (ifaces);
    }
  }
  gtk_widget_set_visible (row, any);
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (row)));
  g_object_unref (row);
}

static void
alerts_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Emergency alerts", &c);
  GSettings *cbd = ns_page_settings (page, "org.freedesktop.cbd");
  GtkWidget *g = ns_group (c, "Alerts from the mobile network");
  ns_row_flag (g, "Extreme threats", "To life and safety", cbd, "levels", "extreme");
  ns_row_flag (g, "Severe threats", "To life and safety", cbd, "levels", "severe");
  ns_row_flag (g, "Public safety", "What to do in a risky situation", cbd, "levels", "public-safety");
  ns_row_flag (g, "Amber alerts", "Child abduction emergencies", cbd, "levels", "amber");
  ns_row_flag (g, "Test alerts", "Periodic tests of the alert system", cbd, "levels", "test");
  ns_note (c, "Presidential alerts are always shown. Alerts need a SIM and a modem that receives them.");
  ns_push (page);
}

/* a hidden row for the alerts, shown if the modem can receive them */
static void
alerts_row (GtkWidget *g)
{
  if (!ns_settings_exists ("org.freedesktop.cbd"))
    return;
  GtkWidget *r = ns_row (g, NULL, NULL, "Emergency alerts", "Cell broadcast warnings");
  ns_row_on_activate (r, alerts_page, NULL);
  gtk_widget_set_visible (r, FALSE);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.ModemManager1", "/org/freedesktop/ModemManager1",
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, cbc_got,
                            g_object_ref (r));
}

/* ---- Notifications ---- */

static void
app_switched (GtkSwitch *sw, GParamSpec *ps, GAppInfo *app)
{
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  g_autoptr (GSettings) s = notify_settings (app);
  if (s)
    g_settings_set_boolean (s, "enable", gtk_switch_get_active (sw));
}

void
ns_page_notifications (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Notifications", &c);
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *r = ns_row_switch (g, "Do not disturb", "No pop-ups: notifications wait in the list", NULL, NULL);
  g_settings_bind (n, "show-banners", ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  ns_row_switch (g, "Show on the lock screen", "Their apps' icons below the clock", n, "show-in-lock-screen");
  /* No "Wake the screen" (Phosh's wakeup-screen-* keys): gnome-phone leaves the screen off for a notification.
   * Besides the power key and a double tap, only a full-screen app (an incoming call, a ringing alarm: service.js
   * LaunchFullScreen) turns it on. */
  alerts_row (g);
  ns_minerva_embed (page, c, "notifications");   /* the events that reach the assistant: Minerva > Eventi */

  g = ns_group (c, "App notifications");
  g_auto (GStrv) kids = g_settings_get_strv (n, "application-children");
  GList *all = g_app_info_get_all ();
  all = g_list_sort_with_data (all, by_name, NULL);
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = g_settings_schema_source_lookup (src, NOTIFY_APP, TRUE);
  for (GList *l = all; sc && l; l = l->next) {
    GAppInfo *app = l->data;
    if (!uses_notifications (app, (const char *const *) kids))
      continue;
    g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
    g_autofree char *path = g_strconcat ("/org/gnome/desktop/notifications/application/", id, "/", NULL);
    g_autoptr (GSettings) s = g_settings_new_full (sc, NULL, path);
    GtkWidget *ar = ns_row_app (g, app, NULL);
    GtkWidget *sw = gtk_switch_new ();
    ns_switch_quiet (sw, g_settings_get_boolean (s, "enable"));
    g_object_set_data_full (G_OBJECT (ar), "ns-app", g_object_ref (app), g_object_unref);
    g_signal_connect (sw, "notify::active", G_CALLBACK (app_switched), app);
    ns_row_suffix (ar, sw);
    ns_row_on_activate (ar, app_notify_page, app);
  }
  g_list_free_full (all, g_object_unref);
  ns_push (page);
}

/* ---- an app ---- */

static void
open_app (GtkWidget *row, gpointer app)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GAppLaunchContext) ctx = G_APP_LAUNCH_CONTEXT (
    gdk_display_get_app_launch_context (gtk_widget_get_display (row)));
  if (!g_app_info_launch (app, NULL, ctx, &err))
    ns_row_set_subtitle (row, err->message);
}

/* its package and version: dpkg-query, in the background */
static void
version_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out) {
    /* "pkg: /usr/share/applications/x.desktop" then the version, both from one shell */
    g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", 2);
    if (l[0] && l[1])
      ns_row_set_subtitle (row, g_strstrip (l[1]));
  }
  g_object_unref (row);
}

static void
version_row (GtkWidget *g, GAppInfo *app)
{
  const char *file = G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app)) : NULL;
  if (!file || !g_str_has_prefix (file, "/usr/"))
    return;
  GtkWidget *r = ns_row_info (g, "Version", NULL);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "echo \"$p\" && dpkg-query -W -f='${Version}' \"$p\"",
                                                "sh", file, NULL);
  if (p)
    g_subprocess_communicate_utf8_async (p, NULL, NULL, version_got, g_object_ref (r));
}

/* Uninstall: the package that owns the app's desktop file, removed with apt behind pkexec (GNOME Shell's polkit prompt asks
 * for the password). apt-get -s first, as the user: the button shows only when apt would remove something, and the
 * question names every package that goes (a game can take a meta package with it). Essential packages make apt
 * refuse the simulation, so they get no button. Apps not from a package (/usr/local, Flatpak) get none either. */
typedef struct {
  GtkWidget *button;
  char      *name, *pkg;
  char     **removes;
} Uninstall;

static void
uninstall_free (Uninstall *u)
{
  g_object_unref (u->button);
  g_free (u->name);
  g_free (u->pkg);
  g_strfreev (u->removes);
  g_free (u);
}

static void
uninstall_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autoptr (GError) err = NULL;
  if (g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err)) {
    GtkWidget *nav = gtk_widget_get_ancestor (u->button, ADW_TYPE_NAVIGATION_VIEW);
    if (nav)
      adw_navigation_view_pop (ADW_NAVIGATION_VIEW (nav));
  } else {
    int st = g_subprocess_get_if_exited (G_SUBPROCESS (src)) ? g_subprocess_get_exit_status (G_SUBPROCESS (src)) : -1;
    /* 126: the password was not given (pkexec's "not authorized"), nothing to report */
    gtk_button_set_label (GTK_BUTTON (u->button), st == 126 ? "Uninstall" : "Could not uninstall, try again");
    gtk_widget_set_sensitive (u->button, TRUE);
  }
  uninstall_free (u);
}

static void
uninstall_answered (gboolean yes, gpointer data)
{
  Uninstall *u = data;                   /* the question's own copy: button and package */
  if (!yes) {
    uninstall_free (u);
    return;
  }
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                &err, "pkexec", "env", "DEBIAN_FRONTEND=noninteractive",
                                                "apt-get", "-y", "remove", u->pkg, NULL);
  if (!p) {
    gtk_button_set_label (GTK_BUTTON (u->button), err->message);
    uninstall_free (u);
    return;
  }
  gtk_button_set_label (GTK_BUTTON (u->button), "Uninstalling…");
  gtk_widget_set_sensitive (u->button, FALSE);
  g_subprocess_wait_check_async (p, NULL, uninstall_done, u);
}

static void
uninstall_tapped (GtkButton *b, Uninstall *u)
{
  g_autofree char *list = g_strjoinv (", ", u->removes);
  g_autofree char *heading = g_strdup_printf ("Uninstall %s?", u->name);
  g_autofree char *body = g_strv_length (u->removes) > 1
    ? g_strdup_printf ("These packages will be removed: %s.", list)
    : g_strdup_printf ("The package %s will be removed.", list);
  Uninstall *q = g_new0 (Uninstall, 1);
  q->button = g_object_ref (u->button);
  q->pkg = g_strdup (u->pkg);
  ns_confirm (GTK_WIDGET (b), heading, body, "Uninstall", TRUE, uninstall_answered, q);
}

static void
uninstall_got (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autofree char *out = NULL;
  /* the package on the first line, then what apt would remove, one per line */
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out) {
    g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", -1);
    if (l[0] && *l[0] && l[1] && *l[1]) {
      u->pkg = g_strdup (l[0]);
      u->removes = g_strdupv (l + 1);
      g_signal_connect_data (u->button, "clicked", G_CALLBACK (uninstall_tapped), u,
                             (GClosureNotify) (void (*) (void)) uninstall_free, 0);
      gtk_widget_set_visible (u->button, TRUE);
      return;
    }
  }
  uninstall_free (u);
}

static void
uninstall_button (GtkWidget *c, GAppInfo *app)
{
  const char *file = G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app)) : NULL;
  if (!file || !g_str_has_prefix (file, "/usr/share/"))
    return;
  GtkWidget *b = gtk_button_new_with_label ("Uninstall");
  gtk_widget_add_css_class (b, "destructive-action");
  gtk_widget_add_css_class (b, "ns-uninstall");
  gtk_widget_set_visible (b, FALSE);
  gtk_box_append (GTK_BOX (c), b);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "[ -n \"$p\" ] && echo \"$p\" && "
                                                "LC_ALL=C apt-get -s remove \"$p\" 2>/dev/null | sed -n 's/^Remv \\([^ ]*\\).*/\\1/p'",
                                                "sh", file, NULL);
  if (!p)
    return;
  Uninstall *u = g_new0 (Uninstall, 1);
  u->button = g_object_ref (b);
  u->name = g_strdup (g_app_info_get_display_name (app));
  g_subprocess_communicate_utf8_async (p, NULL, NULL, uninstall_got, u);
}

static void
app_page (GAppInfo *app)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-app", g_object_ref (app), g_object_unref);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (head, "ns-page-head");
  GIcon *gi = g_app_info_get_icon (app);
  GtkWidget *img = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 72);
  GtkWidget *name = gtk_label_new (g_app_info_get_display_name (app));
  gtk_widget_add_css_class (name, "ns-big");
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_box_append (GTK_BOX (head), img);
  gtk_box_append (GTK_BOX (head), name);
  const char *desc = g_app_info_get_description (app);
  if (desc) {
    GtkWidget *d = gtk_label_new (desc);
    gtk_label_set_wrap (GTK_LABEL (d), TRUE);
    gtk_label_set_justify (GTK_LABEL (d), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (d, "ns-sub");
    gtk_box_append (GTK_BOX (head), d);
  }
  gtk_box_append (GTK_BOX (c), head);

  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Open", FALSE, open_app, app);

  g = ns_group (c, NULL);
  GSettings *ns = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (ns) {
    g_object_set_data_full (G_OBJECT (page), "ns-notify", ns, g_object_unref);
    GtkWidget *r = ns_row (g, "preferences-system-notifications-symbolic", "orange", "Notifications", NULL);
    allowed_changed (ns, NULL, r);
    g_signal_connect_object (ns, "changed::enable", G_CALLBACK (allowed_changed), r, 0);
    ns_row_on_activate (r, app_notify_page, app);
  }
  /* No per-app "Sounds and vibration" (feedbackd's per-app profile): feedbackd applies it to the events an app
   * sends it, and apps here send none (the Phone and the Clock use its Haptic interface, which the profile does
   * not touch). No "Scale to fit" either: that was phoc's; mutter has no such thing. */

  g = ns_group (c, NULL);
  version_row (g, app);
  ns_row_info (g, "App ID", g_app_info_get_id (app));
  uninstall_button (c, app);
  ns_push (page);
}

/* ---- Apps: one list, built whole: a list view binds its recycled rows again as they scroll in (labels laid out,
 * icons looked up), which a fling cannot keep up with on the phone; the icons still load only once shown ---- */

/* one app's page straight away (the launcher's "App info", GNOME Settings' "applications" panel with an app id) */
void
ns_page_app (const char *id)
{
  g_autofree char *file = g_str_has_suffix (id, ".desktop") ? g_strdup (id) : g_strconcat (id, ".desktop", NULL);
  g_autoptr (GDesktopAppInfo) app = g_desktop_app_info_new (file);
  if (app)
    app_page (G_APP_INFO (app));
  else
    ns_page_apps (NULL, NULL);
}

/* the list's first row is "Default apps" (o NULL) on a card of its own, then one card of all apps */
static GtkWidget *
app_item_new (GAppInfo *app, gboolean first, gboolean last)
{
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (v, "ns-li");
  if (first)
    gtk_widget_add_css_class (v, "ns-li-first");
  if (last)
    gtk_widget_add_css_class (v, "ns-li-last");
  if (!first)
    gtk_box_append (GTK_BOX (v), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_add_css_class (h, "ns-li-row");
  GtkWidget *icon;
  const char *title, *sub;
  if (app) {
    GIcon *gi = g_app_info_get_icon (app);
    icon = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 40);
    title = g_app_info_get_display_name (app);
    sub = g_app_info_get_description (app);
  } else {
    icon = gtk_image_new_from_icon_name ("view-grid-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 20);
    gtk_widget_add_css_class (icon, "ns-icon");
    gtk_widget_add_css_class (icon, "ns-blue");
    title = "Default apps";
    sub = "Browser • Email • Music • Photos";
  }
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (h), icon);
  g_object_set_data (G_OBJECT (v), "ns-icon", icon);
  GtkWidget *t = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign (t, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (t, TRUE);
  GtkWidget *l = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (l, "ns-title");
  gtk_box_append (GTK_BOX (t), l);
  if (sub && *sub) {
    l = gtk_label_new (sub);
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (l, "ns-sub");
    gtk_box_append (GTK_BOX (t), l);
  }
  gtk_box_append (GTK_BOX (h), t);
  gtk_box_append (GTK_BOX (v), h);
  return v;
}

/* App icons as textures, each drawn once at the screen's size: GTK draws a scalable (SVG) icon from its paths on
 * every frame it shows, rasterised and uploaded again, which a fling past a screen full of them cannot afford. In
 * idle time after the page is up, a few at a time. */
typedef struct {
  GPtrArray   *images;      /* GtkImage, a weak ref each */
  GskRenderer *renderer;
  guint        next;
} IconBake;

static void
icon_bake_free (IconBake *b)
{
  g_ptr_array_unref (b->images);
  if (b->renderer) {
    gsk_renderer_unrealize (b->renderer);
    g_object_unref (b->renderer);
  }
  g_free (b);
}

static gboolean
icon_bake_step (gpointer data)
{
  IconBake *b = data;
  for (int n = 0; n < 4 && b->next < b->images->len; b->next++) {
    GWeakRef *ref = b->images->pdata[b->next];
    g_autoptr (GtkWidget) image = g_weak_ref_get (ref);
    if (!image || gtk_image_get_storage_type (GTK_IMAGE (image)) != GTK_IMAGE_GICON)
      continue;
    GdkDisplay *display = gtk_widget_get_display (image);
    if (!b->renderer) {
      b->renderer = gsk_cairo_renderer_new ();
      gsk_renderer_realize_for_display (b->renderer, display, NULL);
    }
    GListModel *monitors = gdk_display_get_monitors (display);
    g_autoptr (GdkMonitor) monitor = g_list_model_get_n_items (monitors) ? g_list_model_get_item (monitors, 0) : NULL;
    double scale = monitor ? gdk_monitor_get_scale (monitor) : 1;
    int size = gtk_image_get_pixel_size (GTK_IMAGE (image));
    float px = ceil (size * scale);
    g_autoptr (GtkIconPaintable) icon = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_for_display (display),
                                                                       gtk_image_get_gicon (GTK_IMAGE (image)), size,
                                                                       (int) ceil (scale), GTK_TEXT_DIR_NONE, 0);
    GtkSnapshot *s = gtk_snapshot_new ();
    gdk_paintable_snapshot (GDK_PAINTABLE (icon), s, px, px);
    g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (s);
    if (!node)
      continue;
    g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (b->renderer, node, &GRAPHENE_RECT_INIT (0, 0, px, px));
    gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
    gtk_image_set_pixel_size (GTK_IMAGE (image), size);
    n++;
  }
  return b->next < b->images->len ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
app_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  GAppInfo *app = g_object_get_data (G_OBJECT (row), "ns-app");
  if (app)
    app_page (app);
  else
    ns_page_default_apps (NULL, NULL);
}

void
ns_page_apps (GtkWidget *row, gpointer data)
{
  GList *all = g_app_info_get_all ();
  all = g_list_sort_with_data (all, by_name, NULL);
  GPtrArray *shown = g_ptr_array_new ();
  for (GList *l = all; l; l = l->next)
    if (g_app_info_should_show (l->data))
      g_ptr_array_add (shown, l->data);

  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_list_box_append (GTK_LIST_BOX (list), app_item_new (NULL, TRUE, TRUE));
  IconBake *bake = g_new0 (IconBake, 1);
  bake->images = g_ptr_array_new_with_free_func ((GDestroyNotify) weak_ref_free);
  for (guint i = 0; i < shown->len; i++) {
    GtkWidget *item = app_item_new (shown->pdata[i], i == 0, i == shown->len - 1);
    gtk_list_box_append (GTK_LIST_BOX (list), item);
    g_object_set_data_full (G_OBJECT (gtk_widget_get_parent (item)), "ns-app", g_object_ref (shown->pdata[i]),
                            g_object_unref);
    GWeakRef *ref = g_new0 (GWeakRef, 1);
    g_weak_ref_init (ref, g_object_get_data (G_OBJECT (item), "ns-icon"));
    g_ptr_array_add (bake->images, ref);
  }
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, icon_bake_step, bake, (GDestroyNotify) icon_bake_free);
  g_ptr_array_free (shown, TRUE);
  g_list_free_full (all, g_object_unref);
  g_signal_connect (list, "row-activated", G_CALLBACK (app_activated), NULL);
  ns_push (ns_page_new_list ("Apps", list));
}

/* ---- Default apps ---- */

typedef struct {
  const char *title, *const *types;
} Kind;

static const char *const web[] = { "x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                                   "x-scheme-handler/about", "x-scheme-handler/unknown", NULL };
static const char *const mail[] = { "x-scheme-handler/mailto", NULL };
static const char *const cal[] = { "text/calendar", NULL };
static const char *const music[] = { "audio/x-vorbis+ogg", "audio/mpeg", "audio/x-wav", "audio/x-flac", "audio/ogg",
                                     "audio/mp4", "audio/x-opus+ogg", NULL };
static const char *const video[] = { "video/x-ogm+ogg", "video/mp4", "video/webm", "video/x-matroska",
                                     "video/quicktime", "video/mpeg", NULL };
static const char *const photos[] = { "image/jpeg", "image/png", "image/gif", "image/webp", "image/heic",
                                      "image/tiff", NULL };

static const Kind kinds[] = {
  { "Browser", web }, { "Email", mail }, { "Calendar", cal }, { "Music", music }, { "Video", video },
  { "Photos", photos },
};

static void
default_picked (GtkWidget *row, guint i, gpointer kind)
{
  const Kind *k = kind;
  GPtrArray *apps = g_object_get_data (G_OBJECT (row), "ns-apps");
  GAppInfo *app = apps->pdata[i];
  for (guint t = 0; k->types[t]; t++)
    g_app_info_set_as_default_for_type (app, k->types[t], NULL);
}

void
ns_page_default_apps (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Default apps", &c);
  GtkWidget *g = ns_group (c, NULL);
  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++) {
    GList *all = g_app_info_get_all_for_type (kinds[k].types[0]);
    g_autoptr (GAppInfo) def = g_app_info_get_default_for_type (kinds[k].types[0], FALSE);
    GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GPtrArray) labels = g_ptr_array_new ();
    guint sel = G_MAXUINT;
    for (GList *l = all; l; l = l->next) {
      if (!g_app_info_should_show (l->data))
        continue;
      if (def && g_app_info_equal (def, l->data))
        sel = apps->len;
      g_ptr_array_add (labels, (gpointer) g_app_info_get_display_name (l->data));
      g_ptr_array_add (apps, g_object_ref (l->data));
    }
    g_ptr_array_add (labels, NULL);
    g_list_free_full (all, g_object_unref);
    GtkWidget *r;
    if (apps->len) {
      r = ns_row_pick (g, kinds[k].title, (const char *const *) labels->pdata, sel, default_picked,
                       (gpointer) &kinds[k]);
      if (sel == G_MAXUINT)
        ns_row_set_subtitle (r, "None chosen");
    } else {
      r = ns_row_info (g, kinds[k].title, "No app installed");
    }
    g_object_set_data_full (G_OBJECT (r), "ns-apps", apps, (GDestroyNotify) g_ptr_array_unref);
  }
  ns_push (page);
}
