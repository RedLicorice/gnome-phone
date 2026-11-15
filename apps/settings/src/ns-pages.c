/* Neo Settings' own pages (see ns-pages.h): the hubs and the small pages. The bigger ones have files of their own
 * (ns-net.c, ns-bt.c, ns-sound.c, ns-apps.c, ns-shell.c, ns-privacy.c, ns-system.c). Nothing here blocks: D-Bus
 * answers fill the page when they come. */
#include "ns-pages.h"
#include "ns-parallax.h"
#include "ns-ui.h"

#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static GtkWidget *
page_row (GtkWidget *g, const char *icon, const char *color, const char *title, const char *sub, NsActivate page)
{
  GtkWidget *r = ns_row (g, icon, color, title, sub);
  ns_row_on_activate (r, page, NULL);
  return r;
}

static void
toggle (GtkWidget *row, gpointer sw)
{
  gtk_switch_set_active (sw, !gtk_switch_get_active (sw));
}

/* ---- Connections ---- */

/* Flight mode is gnome-settings-daemon's rfkill switch */
static void
airplane_set (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  if (g_object_get_data (G_OBJECT (sw), "ns-syncing"))
    return;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.SettingsDaemon.Rfkill", "/org/gnome/SettingsDaemon/Rfkill",
                            "org.freedesktop.DBus.Properties", "Set",
                            g_variant_new ("(ssv)", "org.gnome.SettingsDaemon.Rfkill", "AirplaneMode",
                                           g_variant_new_boolean (gtk_switch_get_active (sw))),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
airplane_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) on = NULL;
  if (v) {
    g_variant_get (v, "(v)", &on);
    GtkWidget *sw = ns_row_get_switch (row);
    g_object_set_data (G_OBJECT (sw), "ns-syncing", GINT_TO_POINTER (1));
    gtk_switch_set_active (GTK_SWITCH (sw), g_variant_get_boolean (on));
    g_object_set_data (G_OBJECT (sw), "ns-syncing", NULL);
    gtk_widget_set_visible (row, TRUE);
  }
  g_object_unref (row);
}

void
ns_page_connections (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Connections", &c);
  GtkWidget *g = ns_group (c, NULL);
  page_row (g, "network-wireless-symbolic", "blue", "Wi-Fi", "Networks • Saved networks", ns_page_wifi);
  page_row (g, "bluetooth-symbolic", "blue", "Bluetooth", "Pair • Connected devices", ns_page_bluetooth);
  GtkWidget *air = ns_row (g, "airplane-mode-symbolic", "orange", "Flight mode",
                           "Turn off calls, messages and mobile data");
  GtkWidget *sw = gtk_switch_new ();
  ns_row_suffix (air, sw);
  ns_row_on_activate (air, toggle, sw);
  g_signal_connect (sw, "notify::active", G_CALLBACK (airplane_set), NULL);
  gtk_widget_set_visible (air, FALSE);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.SettingsDaemon.Rfkill", "/org/gnome/SettingsDaemon/Rfkill",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.gnome.SettingsDaemon.Rfkill", "AirplaneMode"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, airplane_got,
                            g_object_ref (air));
  g = ns_group (c, NULL);
  page_row (g, "network-cellular-symbolic", "green", "Mobile network", "Mobile data • Access points • SIM",
            ns_page_mobile);
  page_row (g, "network-wireless-hotspot-symbolic", "green", "Mobile hotspot", "Share the mobile data",
            ns_page_hotspot);
  page_row (g, "network-wired-symbolic", "violet", "Network and VPN", "VPN • USB • Proxy", ns_page_network);
  g = ns_group (c, NULL);
  page_row (g, "emblem-shared-symbolic", "orange", "Sharing", "Phone name • Visibility", ns_page_sharing);
  /* printing needs CUPS, which a phone rarely has: GNOME Settings' page, only when it is there */
  g_autofree char *cups = g_find_program_in_path ("lpstat");
  if (cups)
    page_row (g, "printer-symbolic", "grey", "Printers", NULL, ns_page_printers);
  ns_push (page);
}

/* ---- Display ---- */

/* Night light is a gamma table the compositor gives the display: a CRTC with a GAMMA_LUT (or a legacy gamma ramp)
 * can take one. None that can (the S10e's DECON): no row. No card to ask: assume it can. */
static gboolean
display_has_gamma (void)
{
  static int known = -1;
  if (known >= 0)
    return known;
  gboolean asked = FALSE, gamma = FALSE;
  for (int i = 0; i < 8 && !gamma; i++) {
    g_autofree char *path = g_strdup_printf ("/dev/dri/card%d", i);
    int fd = open (path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    drmModeRes *res = drmModeGetResources (fd);
    for (int c = 0; res && c < res->count_crtcs && !gamma; c++) {
      asked = TRUE;
      drmModeCrtc *crtc = drmModeGetCrtc (fd, res->crtcs[c]);
      gamma = crtc && crtc->gamma_size > 0;
      drmModeFreeCrtc (crtc);
      drmModeObjectProperties *props = drmModeObjectGetProperties (fd, res->crtcs[c], DRM_MODE_OBJECT_CRTC);
      for (guint p = 0; props && p < props->count_props && !gamma; p++) {
        drmModePropertyRes *pr = drmModeGetProperty (fd, props->props[p]);
        gamma = pr && !g_strcmp0 (pr->name, "GAMMA_LUT");
        drmModeFreeProperty (pr);
      }
      drmModeFreeObjectProperties (props);
    }
    drmModeFreeResources (res);
    close (fd);
  }
  known = gamma || !asked;
  return known;
}

/* ---- Night light: gnome-settings-daemon's, on a page of its own ----
 *
 * gsd-color owns night light, through its keys (org.gnome.settings-daemon.plugins.color):
 *   night-light-enabled        on or off: "Use night light"
 *   night-light-temperature    how warm, in kelvin (lower is warmer): "Intensity"
 *   night-light-schedule-*     when it shows: from sunset to sunrise (-automatic, the times from where the phone
 *                              is), or from one hour to another (-from, -to: hours, 20.5 is 20:30)
 * It turns the temperature into a gamma table and mutter gives that to the display (the CRTC's GAMMA_LUT, see
 * display_has_gamma): a change shows within a second, faded in. Nothing here talks to the display.
 *
 * gsd has no "on until I turn it off": whenever it is on it follows the schedule. But it counts a schedule whose
 * start is its end as the whole day, and that is what "None" is here: on whenever Use night light is.
 *
 * gsd-color's own D-Bus object (org.gnome.SettingsDaemon.Color) says whether night light shows right now
 * (NightLightActive) and when the sun sets and rises where the phone is (Sunset, Sunrise: hours, 0 while it does
 * not know), and can show night light for a few seconds while the schedule says no (NightLightPreview): the
 * Intensity slider asks for that, so that moving it at noon shows what it does, as GNOME Settings' slider does. */

#define NIGHT_WARMEST 1700      /* the slider's ends, in kelvin: GNOME Settings' range */
#define NIGHT_COOLEST 4700

enum { NIGHT_NONE, NIGHT_CUSTOM, NIGHT_SUN };
static const char *const night_schedules[] = { "None", "Turns on at custom time", "Turns on from sunset to sunrise",
                                               NULL };

typedef struct {
  GSettings  *color, *iface;
  GDBusProxy *gsd;              /* gsd-color's object, or NULL while there is none */
  GtkWidget  *use, *schedule, *start, *end, *intensity, *scale;
  gboolean    syncing;          /* the slider is being moved to the key, not by a finger */
  gint64      previewed;        /* when a preview was last asked for */
} Night;

static void
night_free (Night *n)
{
  g_signal_handlers_disconnect_by_data (n->color, n);
  g_object_unref (n->color);
  g_clear_object (&n->iface);
  if (n->gsd)
    g_signal_handlers_disconnect_by_data (n->gsd, n);
  g_clear_object (&n->gsd);
  g_free (n);
}

static guint
night_mode (GSettings *color)
{
  if (g_settings_get_boolean (color, "night-light-schedule-automatic"))
    return NIGHT_SUN;
  return g_settings_get_double (color, "night-light-schedule-from")
         == g_settings_get_double (color, "night-light-schedule-to") ? NIGHT_NONE : NIGHT_CUSTOM;
}

/* an hour of the day as gsd keeps it (20.5), the way the clock shows times: "20:30" or "8:30 PM" */
static char *
night_time (GSettings *iface, double hours)
{
  int minutes = ((int) lround (hours * 60) % (24 * 60) + 24 * 60) % (24 * 60);
  g_autoptr (GDateTime) t = g_date_time_new_utc (2000, 1, 1, minutes / 60, minutes % 60, 0);
  g_autofree char *format = iface ? g_settings_get_string (iface, "clock-format") : NULL;
  return g_date_time_format (t, !g_strcmp0 (format, "12h") ? "%-I:%M %p" : "%H:%M");
}

/* gsd's sunset or sunrise, or 0 when it does not know (no object, no location yet) */
static double
night_sun (Night *n, const char *which)
{
  g_autoptr (GVariant) v = n && n->gsd ? g_dbus_proxy_get_cached_property (n->gsd, which) : NULL;
  return v && g_variant_is_of_type (v, G_VARIANT_TYPE_DOUBLE) ? g_variant_get_double (v) : 0;
}

/* what the switch does, in a line, for the Display row and the page's switch */
static char *
night_summary (GSettings *color, GSettings *iface)
{
  if (!g_settings_get_boolean (color, "night-light-enabled"))
    return g_strdup ("Off");
  switch (night_mode (color)) {
  case NIGHT_SUN:
    return g_strdup ("On from sunset to sunrise");
  case NIGHT_CUSTOM: {
    g_autofree char *from = night_time (iface, g_settings_get_double (color, "night-light-schedule-from"));
    g_autofree char *to = night_time (iface, g_settings_get_double (color, "night-light-schedule-to"));
    return g_strdup_printf ("On from %s to %s", from, to);
  }
  default:
    return g_strdup ("On");
  }
}

/* a time row of the custom schedule: its picker's choice (on the half hour), or the time itself when gsd's is
 * between two (set elsewhere) */
static void
night_time_show (Night *n, GtkWidget *row, const char *key)
{
  double h = g_settings_get_double (n->color, key);
  gboolean half = fabs (h * 2 - round (h * 2)) < 1e-6 && h >= 0 && h < 24;
  ns_row_pick_select (row, half ? (guint) lround (h * 2) : G_MAXUINT);
  if (!half) {
    g_autofree char *t = night_time (n->iface, h);
    ns_row_set_subtitle (row, t);
  }
}

/* the page from the keys and from gsd: called whenever either changes */
static void
night_show (Night *n)
{
  g_autofree char *summary = night_summary (n->color, n->iface);
  ns_row_set_subtitle (n->use, summary);

  guint mode = night_mode (n->color);
  ns_row_pick_select (n->schedule, mode);
  double sunset = night_sun (n, "Sunset"), sunrise = night_sun (n, "Sunrise");
  if (mode == NIGHT_SUN && sunset > 0 && sunrise > 0) {
    g_autofree char *set = night_time (n->iface, sunset), *rise = night_time (n->iface, sunrise);
    g_autofree char *line = g_strdup_printf ("From sunset (%s) to sunrise (%s)", set, rise);
    ns_row_set_subtitle (n->schedule, line);
  } else if (mode == NIGHT_SUN && n->gsd) {
    /* gsd falls back to the custom times while it does not know where the phone is */
    g_autofree char *from = night_time (n->iface, g_settings_get_double (n->color, "night-light-schedule-from"));
    g_autofree char *to = night_time (n->iface, g_settings_get_double (n->color, "night-light-schedule-to"));
    g_autofree char *line = g_strdup_printf ("The phone's location is not known yet: %s to %s until it is", from, to);
    ns_row_set_subtitle (n->schedule, line);
  }

  gtk_widget_set_visible (n->start, mode == NIGHT_CUSTOM);
  gtk_widget_set_visible (n->end, mode == NIGHT_CUSTOM);
  night_time_show (n, n->start, "night-light-schedule-from");
  night_time_show (n, n->end, "night-light-schedule-to");
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (n->start)));

  /* the slider: right is warmer, as Android's Intensity; greyed out while night light is off, as Android's */
  double k = g_settings_get_uint (n->color, "night-light-temperature");
  n->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (n->scale),
                       CLAMP (100.0 * (NIGHT_COOLEST - k) / (NIGHT_COOLEST - NIGHT_WARMEST), 0, 100));
  n->syncing = FALSE;
  gtk_widget_set_sensitive (n->intensity, g_settings_get_boolean (n->color, "night-light-enabled"));
}

static void
night_changed (GSettings *s, const char *key, Night *n)
{
  night_show (n);
}

static void
night_gsd_changed (GDBusProxy *gsd, GVariant *changed, GStrv invalidated, Night *n)
{
  night_show (n);
}

static void
night_gsd_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Night *n = g_object_get_data (page, "ns-night");
  GDBusProxy *gsd = g_dbus_proxy_new_for_bus_finish (res, NULL);
  /* a proxy is made for a name nobody has: only one with an owner is gsd */
  g_autofree char *owner = gsd ? g_dbus_proxy_get_name_owner (gsd) : NULL;
  if (owner) {
    n->gsd = g_steal_pointer (&gsd);
    g_signal_connect (n->gsd, "g-properties-changed", G_CALLBACK (night_gsd_changed), n);
    night_show (n);
  }
  g_clear_object (&gsd);
  g_object_unref (page);
}

static void
night_schedule_picked (GtkWidget *row, guint i, gpointer data)
{
  /* The keys change together, so that gsd sees one change and not a schedule on the way that would turn the tint
   * off and on again. A settings object of its own in "delay" mode holds them until apply: the page's own stays
   * immediate, as the slider needs it. */
  g_autoptr (GSettings) s = ns_settings ("org.gnome.settings-daemon.plugins.color");
  if (!s)
    return;
  g_settings_delay (s);
  /* gsd falls back to the custom times when it cannot tell sunset (no location): those are 20:00 to 06:00 again
   * rather than None's whole day, as they are for Custom */
  if (i != NIGHT_NONE && night_mode (s) == NIGHT_NONE) {
    g_settings_reset (s, "night-light-schedule-from");
    g_settings_reset (s, "night-light-schedule-to");
  }
  if (i == NIGHT_NONE)
    g_settings_set_double (s, "night-light-schedule-to", g_settings_get_double (s, "night-light-schedule-from"));
  g_settings_set_boolean (s, "night-light-schedule-automatic", i == NIGHT_SUN);
  g_settings_apply (s);
}

/* a start equal to the end is the whole day: None, which the Schedule row then says */
static void
night_start_picked (GtkWidget *row, guint i, gpointer n)
{
  g_settings_set_double (((Night *) n)->color, "night-light-schedule-from", i / 2.0);
}

static void
night_end_picked (GtkWidget *row, guint i, gpointer n)
{
  g_settings_set_double (((Night *) n)->color, "night-light-schedule-to", i / 2.0);
}

/* The key is written as the slider moves, and gsd fades the screen to it. While the schedule keeps night light
 * off, gsd shows it for five seconds from each move instead, asked at most every two. */
static void
night_moved (GtkRange *r, Night *n)
{
  if (n->syncing)
    return;
  double k = NIGHT_COOLEST - gtk_range_get_value (r) / 100.0 * (NIGHT_COOLEST - NIGHT_WARMEST);
  g_settings_set_uint (n->color, "night-light-temperature", (guint32) lround (k));
  if (!n->gsd || !g_settings_get_boolean (n->color, "night-light-enabled"))
    return;
  g_autoptr (GVariant) active = g_dbus_proxy_get_cached_property (n->gsd, "NightLightActive");
  gint64 now = g_get_monotonic_time ();
  if ((active && g_variant_get_boolean (active)) || now - n->previewed < 2 * G_USEC_PER_SEC)
    return;
  n->previewed = now;
  g_dbus_proxy_call (n->gsd, "NightLightPreview", g_variant_new ("(u)", 5), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                     NULL);
}

void
ns_page_night_light (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Night light", &c);
  GSettings *color = ns_page_settings (page, "org.gnome.settings-daemon.plugins.color");
  GtkWidget *intro = ns_note (c, "Night light tints your screen amber. This makes it easier to look at your screen "
                                 "or read in dim light, and may help you fall asleep more easily.");
  gtk_widget_remove_css_class (intro, "ns-note");
  gtk_widget_add_css_class (intro, "ns-intro");
  if (!color || !display_has_gamma ()) {
    ns_note (c, color ? "This screen cannot change its colours." : "Night light is GNOME's settings daemon's, "
                                                                   "which is not installed.");
    ns_push (page);
    return;
  }
  Night *n = g_new0 (Night, 1);
  n->color = g_object_ref (color);
  n->iface = ns_settings ("org.gnome.desktop.interface");
  g_object_set_data_full (G_OBJECT (page), "ns-night", n, (GDestroyNotify) night_free);

  GtkWidget *g = ns_group (c, NULL);
  n->use = ns_row_switch (g, "Use night light", NULL, color, "night-light-enabled");

  g = ns_group (c, NULL);
  n->schedule = ns_row_pick (g, "Schedule", night_schedules, night_mode (color), night_schedule_picked, NULL);
  g_autoptr (GPtrArray) times = g_ptr_array_new_with_free_func (g_free);
  for (int i = 0; i < 48; i++)
    g_ptr_array_add (times, night_time (n->iface, i / 2.0));
  g_ptr_array_add (times, NULL);
  n->start = ns_row_pick (g, "Start time", (const char *const *) times->pdata, 0, night_start_picked, n);
  n->end = ns_row_pick (g, "End time", (const char *const *) times->pdata, 0, night_end_picked, n);

  g = ns_group (c, NULL);
  n->intensity = ns_row_scale (g, "Intensity", NULL, 0, 100, 1, &n->scale);
  g_signal_connect (n->scale, "value-changed", G_CALLBACK (night_moved), n);

  /* the page follows the keys, whoever changes them (the shell's quick setting, GNOME Settings) */
  g_signal_connect (color, "changed", G_CALLBACK (night_changed), n);
  night_show (n);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.gnome.SettingsDaemon.Color",
                            "/org/gnome/SettingsDaemon/Color", "org.gnome.SettingsDaemon.Color", NULL,
                            night_gsd_got, g_object_ref (page));
  ns_push (page);
}

/* Display's row: the switch, and a tap opens the page; the line under it says what the switch does */
static void
night_row_changed (GSettings *color, const char *key, GtkWidget *row)
{
  g_autofree char *summary = night_summary (color, g_object_get_data (G_OBJECT (row), "ns-iface"));
  ns_row_set_subtitle (row, summary);
}

static void
night_row (GtkWidget *g, GSettings *color, GSettings *iface)
{
  GtkWidget *r = ns_row (g, NULL, NULL, "Night light", NULL);
  GtkWidget *sw = gtk_switch_new ();
  g_settings_bind (color, "night-light-enabled", sw, "active", G_SETTINGS_BIND_DEFAULT);
  ns_row_suffix (r, sw);
  ns_row_on_activate (r, ns_page_night_light, NULL);
  g_object_set_data (G_OBJECT (r), "ns-iface", iface);
  g_signal_connect_object (color, "changed", G_CALLBACK (night_row_changed), r, 0);
  night_row_changed (color, NULL, r);
}

/* ---- Brightness: the backlight as the compositor has it ----
 *
 * Mutter owns the backlight: its DisplayConfig lists it (the Backlight property: a serial, then each panel's
 * connector, min, max and value) and sets it (SetBacklight), and the shell's brightness slider goes through the
 * same object. Writing /sys behind mutter's back would leave the shell's slider and its saved level behind.
 *
 * The slider stands where the shell's does: gnome-phone's brightness.js maps its slider to the backlight on the
 * power GAMMA (1.9), so that the levels comfortable indoors are not all in the first fifth. The two must agree, or
 * one slider would jump when the other moves. Adaptive brightness (brightness.js too) keeps a level set here as
 * its offset from the light's level, as Android does. */

#define BRIGHTNESS_GAMMA 1.9

typedef struct {
  GDBusProxy *config;           /* org.gnome.Mutter.DisplayConfig */
  GtkWidget  *row, *scale;
  guint       serial;           /* the Backlight property's, which SetBacklight must name */
  char       *connector;
  int         min, max;
  gboolean    syncing;          /* the slider is being moved to the backlight, not by a finger */
} Brightness;

static void
brightness_free (Brightness *b)
{
  if (b->config)
    g_signal_handlers_disconnect_by_data (b->config, b);
  g_clear_object (&b->config);
  g_free (b->connector);
  g_free (b);
}

/* the first panel mutter lists that is on: its connector, range and level, the slider moved to it */
static void
brightness_read (Brightness *b)
{
  /* the page went while mutter was being asked: nothing to show it in */
  if (!gtk_widget_get_parent (b->row))
    return;
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (b->config, "Backlight");
  gboolean found = FALSE;
  int value = 0;
  if (v && g_variant_is_of_type (v, G_VARIANT_TYPE ("(uaa{sv})"))) {
    g_autoptr (GVariant) monitors = NULL;
    g_variant_get (v, "(u@aa{sv})", &b->serial, &monitors);
    for (gsize i = 0; i < g_variant_n_children (monitors) && !found; i++) {
      g_autoptr (GVariant) m = g_variant_get_child_value (monitors, i);
      const char *connector;
      gboolean active = TRUE;
      g_variant_lookup (m, "active", "b", &active);
      if (!active || !g_variant_lookup (m, "connector", "&s", &connector) || !g_variant_lookup (m, "min", "i", &b->min)
          || !g_variant_lookup (m, "max", "i", &b->max) || !g_variant_lookup (m, "value", "i", &value)
          || b->max <= b->min)
        continue;
      g_free (b->connector);
      b->connector = g_strdup (connector);
      found = TRUE;
    }
  }
  gtk_widget_set_visible (b->row, found);
  if (found)
    ns_group_set_visible (gtk_widget_get_parent (b->row), TRUE);
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (b->row)));
  /* not while a finger is on it */
  if (!found || gtk_widget_get_state_flags (b->scale) & GTK_STATE_FLAG_ACTIVE)
    return;
  double level = CLAMP ((double) (value - b->min) / (b->max - b->min), 0, 1);
  b->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (b->scale), 100 * pow (level, 1 / BRIGHTNESS_GAMMA));
  b->syncing = FALSE;
}

static void
brightness_moved (GtkRange *r, Brightness *b)
{
  if (b->syncing || !b->connector)
    return;
  double level = pow (gtk_range_get_value (r) / 100, BRIGHTNESS_GAMMA);
  int value = (int) lround (b->min + (b->max - b->min) * level);
  /* a backlight whose range starts at 0 would go dark at the slider's start: the darkest step is 1 */
  value = MAX (value, MAX (b->min, 1));
  g_dbus_proxy_call (b->config, "SetBacklight", g_variant_new ("(usi)", b->serial, b->connector, value),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
brightness_changed (GDBusProxy *config, GVariant *changed, GStrv invalidated, Brightness *b)
{
  if (g_variant_lookup_value (changed, "Backlight", NULL))
    brightness_read (b);
}

static void
brightness_proxy (GObject *src, GAsyncResult *res, gpointer row)
{
  Brightness *b = g_object_get_data (row, "ns-brightness");
  b->config = g_dbus_proxy_new_for_bus_finish (res, NULL);
  if (b->config) {
    g_signal_connect (b->config, "g-properties-changed", G_CALLBACK (brightness_changed), b);
    brightness_read (b);
  }
  g_object_unref (row);
}

/* the slider's row, hidden until mutter lists a backlight */
static void
brightness_row (GtkWidget *g)
{
  Brightness *b = g_new0 (Brightness, 1);
  b->row = ns_row_scale (g, "Brightness", "display-brightness-symbolic", 0, 100, 1, &b->scale);
  gtk_widget_set_visible (b->row, FALSE);
  g_object_set_data_full (G_OBJECT (b->row), "ns-brightness", b, (GDestroyNotify) brightness_free);
  g_signal_connect (b->scale, "value-changed", G_CALLBACK (brightness_moved), b);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                            "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                            "org.gnome.Mutter.DisplayConfig", NULL, brightness_proxy, g_object_ref (b->row));
}

/* Adaptive brightness is gnome-settings-daemon's ambient-enabled key, which brightness.js reads; it needs a light
 * sensor, which iio-sensor-proxy has or not (HasAmbientLight): the row shows only with one */
static void
light_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) has = NULL;
  if (v)
    g_variant_get (v, "(v)", &has);
  if (has && g_variant_is_of_type (has, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean (has)
      && gtk_widget_get_parent (row)) {
    gtk_widget_set_visible (row, TRUE);
    ns_group_set_visible (gtk_widget_get_parent (row), TRUE);
    gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (row)));
  }
  g_object_unref (row);
}

static void
adaptive_row (GtkWidget *g, GSettings *power)
{
  if (!ns_has_key (power, "ambient-enabled"))
    return;
  GtkWidget *r = ns_row_switch (g, "Adaptive brightness", "Follows the light around you", power, "ambient-enabled");
  gtk_widget_set_visible (r, FALSE);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "net.hadess.SensorProxy", "/net/hadess/SensorProxy",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "net.hadess.SensorProxy", "HasAmbientLight"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, light_got, g_object_ref (r));
}

static gboolean
dark_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, !g_strcmp0 (g_variant_get_string (v, NULL), "prefer-dark"));
  return TRUE;
}

static GVariant *
dark_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? "prefer-dark" : "default");
}

static GtkWidget *
dark_row (GtkWidget *g, GSettings *iface)
{
  GtkWidget *r = ns_row_switch (g, "Dark mode", "Dark backgrounds for apps and the system", NULL, NULL);
  g_settings_bind_with_mapping (iface, "color-scheme", ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT,
                                dark_get, dark_set, NULL, NULL);
  return r;
}

static const double text_scales[] = { 0.9, 1.0, 1.15, 1.3 };
static const char *const text_labels[] = { "Small", "Default", "Large", "Larger", NULL };
static const guint timeouts[] = { 30, 60, 120, 300, 600, 0 };
static const char *const timeout_labels[] = { "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes",
                                              "Never", NULL };

static void
text_picked (GtkWidget *row, guint i, gpointer iface)
{
  g_settings_set_double (iface, "text-scaling-factor", text_scales[i]);
}

static void
timeout_picked (GtkWidget *row, guint i, gpointer session)
{
  g_settings_set_uint (session, "idle-delay", timeouts[i]);
}

/* The same picker on Display and on Wallpaper and style: one setting, two doors. */
static void
font_size_row (GtkWidget *g, GSettings *iface)
{
  double f = g_settings_get_double (iface, "text-scaling-factor");
  guint ti = 1;
  for (guint i = 0; i < G_N_ELEMENTS (text_scales); i++)
    if (ABS (text_scales[i] - f) < 0.01)
      ti = i;
  ns_row_pick (g, "Font size", text_labels, ti, text_picked, iface);
}

static int
name_cmp (gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp (a, b);
}

/* A font role of the theme: "Theme default" (the schema's chain), every installed family, and the current chain when it
 * is a custom one set by hand. The picked family is written alone; libneo and the shell append the stock fallback. The
 * lists belong to the row (the choice keeps pointers), so they are leaked with the page: a few kB. */
static void
font_row (GtkWidget *g, GSettings *look, const char *title, const char *key, gboolean can_default)
{
  GSettingsSchema *schema = NULL;
  g_object_get (look, "settings-schema", &schema, NULL);
  GSettingsSchemaKey *k = g_settings_schema_get_key (schema, key);
  GVariant *def = g_settings_schema_key_get_default_value (k);
  g_autofree char *current = g_settings_get_string (look, key);

  PangoFontMap *map = pango_cairo_font_map_get_default ();
  PangoFontFamily **fams;
  int n;
  pango_font_map_list_families (map, &fams, &n);
  GPtrArray *names = g_ptr_array_new ();
  for (int i = 0; i < n; i++)
    g_ptr_array_add (names, (gpointer) pango_font_family_get_name (fams[i]));
  g_ptr_array_sort_values (names, name_cmp);

  GPtrArray *values = g_ptr_array_new (), *labels = g_ptr_array_new ();
  g_ptr_array_add (values, g_variant_dup_string (def, NULL));
  g_ptr_array_add (labels, g_strdup (can_default ? "Theme default" : "Stock face"));
  gboolean found = !g_strcmp0 (current, values->pdata[0]);
  for (guint i = 0; i < names->len; i++) {
    if (!*(char *) names->pdata[i] || !g_strcmp0 (names->pdata[i], "Sans") || !g_strcmp0 (names->pdata[i], "Serif"))
      continue;
    g_ptr_array_add (values, g_strdup (names->pdata[i]));
    g_ptr_array_add (labels, g_strdup (names->pdata[i]));
    if (!g_strcmp0 (current, names->pdata[i]))
      found = TRUE;
  }
  if (!found) {
    g_ptr_array_add (values, g_strdup (current));
    g_ptr_array_add (labels, g_strdup (*current ? current : "Stock face"));
  }
  g_ptr_array_add (values, NULL);
  g_ptr_array_add (labels, NULL);
  ns_row_choice_font (g, title, look, key, (const char *const *) values->pdata, (const char *const *) labels->pdata);

  g_free (fams);
  g_ptr_array_free (names, TRUE);
  g_variant_unref (def);
  g_settings_schema_key_unref (k);
  g_settings_schema_unref (schema);
}

static gboolean
renaissance_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, !g_strcmp0 (g_variant_get_string (v, NULL), "renaissance"));
  return TRUE;
}

void
ns_page_display (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Display", &c);
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");

  /* the look, the fonts and Dark mode live on Wallpaper and style (ns_page_wallpaper); Display keeps the screen */
  GtkWidget *g;
  GSettings *color = ns_page_settings (page, "org.gnome.settings-daemon.plugins.color");
  if (color && display_has_gamma ()) {
    g = ns_group (c, NULL);
    night_row (g, color, iface);
  }

  /* both rows wait for an answer (mutter's, the sensor's): the card shows with the first that has something */
  g = ns_group (c, NULL);
  ns_group_set_visible (g, FALSE);
  brightness_row (g);
  adaptive_row (g, ns_page_settings (page, "org.gnome.settings-daemon.plugins.power"));

  g = ns_group (c, NULL);
  font_size_row (g, iface);
  ns_page_scale_row (g);

  /* the screen's sleep behaviour, one group: when it goes off, how it turns, and the power rows Battery used to
   * carry (ns_power_rows, ns-system.c) */
  GSettings *session = ns_page_settings (page, "org.gnome.desktop.session");
  guint idle = g_settings_get_uint (session, "idle-delay"), sel = 1;
  for (guint i = 0; i < G_N_ELEMENTS (timeouts); i++)
    if (timeouts[i] == idle)
      sel = i;
  g = ns_group (c, "Screen");
  ns_row_pick (g, "Screen timeout", timeout_labels, sel, timeout_picked, session);
  GSettings *touch = ns_page_settings (page, "org.gnome.settings-daemon.peripherals.touchscreen");
  if (touch) {
    GtkWidget *r = ns_row_switch (g, "Auto rotate", "Turn the screen with the phone", NULL, NULL);
    g_settings_bind (touch, "orientation-lock", ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  }
  GSettings *neo = ns_page_settings (page, "de.yesman.neo");
  if (neo && ns_has_key (neo, "aod"))
    ns_row_switch (g, "Always On Display",
                   ns_panel_is_oled () ? "The time, the date and notifications while the screen is off"
                                       : "Keeps the whole backlight on while the screen is off: it costs battery",
                   neo, "aod");
  ns_power_rows (c, g);
  if (neo) {
    g = ns_group (c, NULL);
    ns_row_switch (g, "Double tap to wake", "Wake the screen with a double tap", neo, "double-tap-wake");
  }
  ns_push (page);
}

/* ---- Battery ---- */

static char *
duration (gint64 s)
{
  if (s <= 0)
    return NULL;
  return s >= 3600 ? g_strdup_printf ("%d h %d min", (int) (s / 3600), (int) (s % 3600 / 60))
                   : g_strdup_printf ("%d min", (int) (s / 60));
}

typedef struct {
  GtkWidget *big, *bar, *state;
} Charge;

static void
charge_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Charge *ch = g_object_get_data (page, "charge");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v)
    g_variant_get (v, "(@a{sv})", &props);
  double p = 0;
  guint32 st = 0;
  gint64 empty = 0, full = 0;
  if (props) {
    g_variant_lookup (props, "Percentage", "d", &p);
    g_variant_lookup (props, "State", "u", &st);
    g_variant_lookup (props, "TimeToEmpty", "x", &empty);
    g_variant_lookup (props, "TimeToFull", "x", &full);
  }
  g_autofree char *big = g_strdup_printf ("%.0f%%", p);
  gtk_label_set_text (GTK_LABEL (ch->big), big);
  gtk_level_bar_set_value (GTK_LEVEL_BAR (ch->bar), p);
  g_autofree char *left = duration (empty);
  g_autofree char *tofull = duration (full);
  g_autofree char *say = st == 1 ? (tofull ? g_strdup_printf ("Charging • full in %s", tofull) : g_strdup ("Charging"))
                       : st == 4 ? g_strdup ("Fully charged")
                       : left ? g_strdup_printf ("About %s left", left) : g_strdup ("On battery");
  gtk_label_set_text (GTK_LABEL (ch->state), props ? say : "No battery information");
  g_object_unref (page);
}

/* ---- Battery information: the battery's own figures, as the kernel has them ----
 *
 * The kernel's power supply class lists every supply in /sys/class/power_supply, each with a "type" file: the
 * phone's battery is the one of type Battery that is not a device's own (a mouse's, a headset's: "scope" Device).
 * Its files hold plain numbers in fixed units: temp in tenths of a °C, voltage_now in µV, charge_full_design and
 * charge_full in µAh (or energy_full_design and energy_full, in µWh, for a gauge that counts energy). A row whose
 * file the battery does not have is left out: drivers differ in what they report.
 *
 * Health is the battery's own when it has the file. Some fuel gauges do not; then the charger may know: Qualcomm's
 * SMB charger (qcom-smbx-charger, the Poco's) reports the battery's over-voltage and temperature zones as its
 * health, so its Good/Cool/Overheat is the battery's. Another charger's health may be its own input's, so only a
 * driver known to report the battery's is asked.
 *
 * The page reads them again every two seconds while it is open (the temperature and the voltage move), in a
 * thread: a fuel gauge answers over a bus (SPMI, I2C), which can take a while. UPower has some of these, not the
 * design capacity in mAh or the health. */

#define POWER_SUPPLY "/sys/class/power_supply"

/* chargers whose "health" is the battery's (see above) */
static const char *const battery_health_chargers[] = { "qcom-smbx-charger", NULL };

enum { BAT_TECHNOLOGY, BAT_HEALTH, BAT_TEMPERATURE, BAT_VOLTAGE, BAT_DESIGN, BAT_MAXIMUM, BAT_ROWS };
static const char *const battery_titles[BAT_ROWS] = { "Technology", "Health", "Temperature", "Voltage",
                                                      "Design capacity", "Maximum capacity" };

static char *
supply_read (const char *dir, const char *file)
{
  g_autofree char *path = g_build_filename (dir, file, NULL);
  char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return NULL;
  g_strstrip (text);
  if (!*text || !g_strcmp0 (text, "Unknown")) {
    g_free (text);
    return NULL;
  }
  return text;
}

/* a number file, or FALSE without one */
static gboolean
supply_number (const char *dir, const char *file, gint64 *out)
{
  g_autofree char *text = supply_read (dir, file);
  char *end;
  if (!text)
    return FALSE;
  *out = g_ascii_strtoll (text, &end, 10);
  return end != text;
}

/* the health of a charger whose driver reports the battery's */
static char *
charger_health (void)
{
  g_autoptr (GDir) d = g_dir_open (POWER_SUPPLY, 0, NULL);
  for (const char *name; d && (name = g_dir_read_name (d));) {
    g_autofree char *dir = g_build_filename (POWER_SUPPLY, name, NULL);
    g_autofree char *link = g_build_filename (dir, "device", "driver", NULL);
    g_autofree char *target = g_file_read_link (link, NULL);
    g_autofree char *driver = target ? g_path_get_basename (target) : NULL;
    if (driver && g_strv_contains (battery_health_chargers, driver))
      return supply_read (dir, "health");
  }
  return NULL;
}

/* the six values, NULL where the battery has none; a thread's work */
static void
battery_read (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  char **v = g_new0 (char *, BAT_ROWS + 1);
  g_autofree char *dir = NULL;
  g_autoptr (GDir) d = g_dir_open (POWER_SUPPLY, 0, NULL);
  for (const char *name; d && !dir && (name = g_dir_read_name (d));) {
    g_autofree char *path = g_build_filename (POWER_SUPPLY, name, NULL);
    g_autofree char *type = supply_read (path, "type");
    g_autofree char *scope = supply_read (path, "scope");
    if (!g_strcmp0 (type, "Battery") && g_strcmp0 (scope, "Device"))
      dir = g_steal_pointer (&path);
  }
  if (!dir) {
    g_task_return_pointer (task, v, (GDestroyNotify) g_strfreev);
    return;
  }

  v[BAT_TECHNOLOGY] = supply_read (dir, "technology");
  v[BAT_HEALTH] = supply_read (dir, "health");
  if (!v[BAT_HEALTH])
    v[BAT_HEALTH] = charger_health ();
  gint64 n, design, full;
  if (supply_number (dir, "temp", &n))
    v[BAT_TEMPERATURE] = g_strdup_printf ("%.1f°C", n / 10.0);
  if (supply_number (dir, "voltage_now", &n))
    v[BAT_VOLTAGE] = g_strdup_printf ("%.3f V", n / 1e6);
  /* in charge (µAh, shown as mAh, as the box says) or, for a gauge that counts energy, in µWh */
  const char *unit = "mAh";
  gboolean got_design = supply_number (dir, "charge_full_design", &design);
  gboolean got_full = supply_number (dir, "charge_full", &full);
  if (!got_design && !got_full) {
    unit = "Wh";
    got_design = supply_number (dir, "energy_full_design", &design);
    got_full = supply_number (dir, "energy_full", &full);
  }
  gboolean mah = !g_strcmp0 (unit, "mAh");
  if (got_design && design > 0)
    v[BAT_DESIGN] = mah ? g_strdup_printf ("%" G_GINT64_FORMAT " mAh", design / 1000)
                        : g_strdup_printf ("%.1f Wh", design / 1e6);
  if (got_full && full > 0) {
    g_autofree char *amount = mah ? g_strdup_printf ("%" G_GINT64_FORMAT " mAh", full / 1000)
                                  : g_strdup_printf ("%.1f Wh", full / 1e6);
    /* what it holds now, of what it held new */
    v[BAT_MAXIMUM] = got_design && design > 0
                     ? g_strdup_printf ("%s (%d%%)", amount, (int) lround (100.0 * full / design))
                     : g_steal_pointer (&amount);
  }
  g_task_return_pointer (task, v, (GDestroyNotify) g_strfreev);
}

typedef struct {
  GtkWidget *none, *rows[BAT_ROWS];
  guint      timer;
  gboolean   reading;
} BatInfo;

static void
bat_info_free (BatInfo *b)
{
  g_clear_handle_id (&b->timer, g_source_remove);
  g_free (b);
}

/* The task and the timer hold the card (the list), on which the state is: the page may go while the battery is
 * being read, and then the card has no parent and nothing is shown */
static void
bat_info_got (GObject *src, GAsyncResult *res, gpointer group)
{
  BatInfo *b = g_object_get_data (group, "ns-batinfo");
  g_auto (GStrv) v = g_task_propagate_pointer (G_TASK (res), NULL);
  b->reading = FALSE;
  if (v && gtk_widget_get_parent (group)) {
    gboolean any = FALSE;
    for (int i = 0; i < BAT_ROWS; i++) {
      gtk_widget_set_visible (b->rows[i], v[i] != NULL);
      if (v[i])
        ns_row_set_subtitle (b->rows[i], v[i]);
      any |= v[i] != NULL;
    }
    ns_group_set_visible (group, any);
    gtk_list_box_invalidate_headers (GTK_LIST_BOX (group));
    gtk_widget_set_visible (b->none, !any);
  }
  g_object_unref (group);
}

static gboolean
bat_info_tick (gpointer group)
{
  BatInfo *b = g_object_get_data (group, "ns-batinfo");
  if (!gtk_widget_get_parent (group)) {
    b->timer = 0;
    return G_SOURCE_REMOVE;
  }
  if (!b->reading) {
    b->reading = TRUE;
    g_autoptr (GTask) t = g_task_new (NULL, NULL, bat_info_got, g_object_ref (group));
    g_task_run_in_thread (t, battery_read);
  }
  return G_SOURCE_CONTINUE;
}

void
ns_page_battery_info (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Battery information", &c);
  BatInfo *b = g_new0 (BatInfo, 1);
  GtkWidget *g = ns_group (c, NULL);
  g_object_set_data_full (G_OBJECT (g), "ns-batinfo", b, (GDestroyNotify) bat_info_free);
  for (int i = 0; i < BAT_ROWS; i++) {
    b->rows[i] = ns_row_info (g, battery_titles[i], NULL);
    gtk_widget_set_visible (b->rows[i], FALSE);
  }
  ns_group_set_visible (g, FALSE);
  b->none = ns_note (c, "This phone's battery reports none of these.");
  gtk_widget_set_visible (b->none, FALSE);
  /* shown once read: the first reading comes before the page has slid in */
  ns_push (page);
  bat_info_tick (g);
  /* the timer holds no reference: the card's going removes it (bat_info_free) */
  b->timer = g_timeout_add_seconds (2, bat_info_tick, g);
}

void
ns_page_battery (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Battery", &c);
  Charge *ch = g_new0 (Charge, 1);
  g_object_set_data_full (G_OBJECT (page), "charge", ch, g_free);

  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  ch->big = gtk_label_new ("–");
  gtk_label_set_xalign (GTK_LABEL (ch->big), 0);
  gtk_widget_add_css_class (ch->big, "ns-huge");
  gtk_box_append (GTK_BOX (v), ch->big);
  ch->bar = gtk_level_bar_new_for_interval (0, 100);
  gtk_box_append (GTK_BOX (v), ch->bar);
  ch->state = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ch->state), 0);
  gtk_widget_add_css_class (ch->state, "ns-sub");
  gtk_box_append (GTK_BOX (v), ch->state);
  ns_row_widget (g, NULL, v);
  ns_charge_limit_add (page, c);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice",
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.UPower.Device"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, charge_got, g_object_ref (page));

  /* No "Show battery percentage" (org.gnome.desktop.interface show-battery-percentage): the status bar draws
   * Android 16's battery (statusbar.js paintBattery), which has no number in it yet. It needs the level drawn into
   * the battery's body, as Android's BatteryWithPercent does, when the key is on. */
  g = ns_group (c, NULL);
  page_row (g, NULL, NULL, "Battery information", "Health • Temperature • Capacity", ns_page_battery_info);
  ns_push (page);
}

/* ---- Wallpaper and style ---- */

static const char *const accents[][2] = {
  { "blue", "#3584e4" }, { "teal", "#2190a4" }, { "green", "#3a944a" }, { "yellow", "#c88800" },
  { "orange", "#ed5b00" }, { "red", "#e62d42" }, { "pink", "#d56199" }, { "purple", "#9141ac" },
  { "slate", "#6f8396" },
};

static void
accent_toggled (GtkToggleButton *b, GSettings *iface)
{
  if (gtk_toggle_button_get_active (b))
    g_settings_set_enum (iface, "accent-color", GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "ns-accent")));
}

static GtkWidget *
accent_picker (GSettings *iface)
{
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 5);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 9);
  int cur = g_settings_get_enum (iface, "accent-color");
  GtkWidget *first = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (accents); i++) {
    GtkWidget *b = gtk_toggle_button_new ();
    gtk_widget_set_tooltip_text (b, accents[i][0]);
    g_autofree char *cls = g_strconcat ("ns-accent-", accents[i][0], NULL);
    gtk_widget_add_css_class (b, "ns-accent-dot");
    gtk_widget_add_css_class (b, cls);
    gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
    if (first)
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), GTK_TOGGLE_BUTTON (first));
    else
      first = b;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), (int) i == cur);
    g_object_set_data (G_OBJECT (b), "ns-accent", GINT_TO_POINTER (i));
    g_signal_connect (b, "toggled", G_CALLBACK (accent_toggled), iface);
    gtk_flow_box_append (GTK_FLOW_BOX (flow), b);
  }
  return flow;
}

static void
picture_changed (GSettings *bg, const char *key, GtkWidget *row)
{
  g_autofree char *uri = g_settings_get_string (bg, "picture-uri");
  g_autoptr (GFile) f = g_file_new_for_uri (uri);
  g_autofree char *base = g_file_get_basename (f);
  /* a parallax set's picture is named for its set, as its card is (every set's is wallpaper.jpg) */
  g_autoptr (GFile) dir = g_file_get_parent (f);
  g_autoptr (GFile) json = dir ? g_file_get_child (dir, "parallax.json") : NULL;
  if (json && g_file_query_exists (json, NULL)) {
    g_free (base);
    base = g_file_get_basename (dir);
    base[0] = g_ascii_toupper (base[0]);
  }
  ns_row_set_subtitle (row, base);
}

void
ns_page_wallpaper (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Wallpaper and style", &c);
  GSettings *bg = ns_page_settings (page, "org.gnome.desktop.background");
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");

  GSettings *neo = ns_page_settings (page, "de.yesman.neo");
  if (neo) {
    GtkWidget *g = ns_group (c, "Parallax wallpaper");
    GtkWidget *hint = gtk_label_new ("Layers that move as you tilt the phone. Off picks a still picture instead.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 0);
    gtk_widget_add_css_class (hint, "ns-sub");
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_box_append (GTK_BOX (v), hint);
    gtk_box_append (GTK_BOX (v), ns_parallax_grid (bg, iface));
    ns_row_widget (g, NULL, v);
  }

  GtkWidget *g = ns_group (c, "Picture");
  GtkWidget *r = page_row (g, "preferences-desktop-wallpaper-symbolic", "pink", "Wallpapers", NULL,
                           ns_page_wallpapers);
  picture_changed (bg, NULL, r);
  g_signal_connect_object (bg, "changed::picture-uri", G_CALLBACK (picture_changed), r, 0);

  g = ns_group (c, "Style");
  GSettings *look = ns_page_settings (page, "de.yesman.GnomePhone.Theme");
  if (look) {
    static const char *const looks[] = { "default", "renaissance", NULL };
    static const char *const look_labels[] = { "Stock", "Cyberpunk Renaissance", NULL };
    ns_row_choice (g, "Look", look, "name", looks, look_labels);
    /* the two variants: the day one is used while Dark mode (below) is off, the night one while it is on. Picking
     * applies at once (the choice writes the setting, libneo, the shell and this window follow it live). Older schemas
     * have no such keys. */
    if (ns_has_key (look, "day-look") && ns_has_key (look, "night-look")) {
      static const char *const days[] = { "marble", "sea", NULL };
      static const char *const day_labels[] = { "Marmo", "Marino", NULL };
      static const char *const nights[] = { "forest", "plum", NULL };
      static const char *const night_labels[] = { "Bosco", "Prugna", NULL };
      GtkWidget *day = ns_row_choice (g, "Day look", look, "day-look", days, day_labels);
      GtkWidget *night = ns_row_choice (g, "Night look", look, "night-look", nights, night_labels);
      /* they only mean something while the Renaissance look is on */
      g_settings_bind_with_mapping (look, "name", day, "visible", G_SETTINGS_BIND_GET, renaissance_get, NULL, NULL, NULL);
      g_settings_bind_with_mapping (look, "name", night, "visible", G_SETTINGS_BIND_GET, renaissance_get, NULL, NULL, NULL);
    }
  }
  dark_row (g, iface);
  ns_row_widget (g, "Accent colour", accent_picker (iface));

  /* the font families and the size are one decision: they sit together here, and Font size keeps its Display row too */
  g = ns_group (c, "Fonts");
  if (look) {
    GSettingsSchema *schema = NULL;
    g_object_get (look, "settings-schema", &schema, NULL);
    if (g_settings_schema_has_key (schema, "font-display")) {
      font_row (g, look, "Display font", "font-display", TRUE);
      font_row (g, look, "Body font", "font-body", FALSE);
      font_row (g, look, "Mono font", "font-mono", TRUE);
    }
    g_settings_schema_unref (schema);
  }
  font_size_row (g, iface);
  ns_push (page);
}

/* ---- Home screen (Neo) ---- */

static const char *const shape_values[] = { "system", "circle", "squircle", "rounded", "square", "teardrop",
                                            "cupertino", "cylinder", "egg", "octagon", "hexagon", "diamond", NULL };
static const char *const shape_labels[] = { "Pack / system", "Circle", "Squircle", "Rounded square", "Square",
                                            "Teardrop", "Cupertino", "Cylinder", "Egg", "Octagon", "Hexagon",
                                            "Diamond", NULL };

typedef struct {
  char *id, *title;
} PackName;

static void
pack_name_free (PackName *p)
{
  g_free (p->id);
  g_free (p->title);
  g_free (p);
}

static int
pack_name_cmp (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(PackName **) a)->title, (*(PackName **) b)->title);
}

/* the icon packs installed, as neo-launcher finds them (icons.c icons_list_packs): neo-launcher/iconpacks in the
 * user's data dir, then in each system data dir, the first of a name winning; a pack is a folder with a
 * gnome-map.json, named by its pack.json's "title" */
static GPtrArray *
icon_packs (void)
{
  GPtrArray *packs = g_ptr_array_new_with_free_func ((GDestroyNotify) pack_name_free);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GStrvBuilder) db = g_strv_builder_new ();
  g_strv_builder_take (db, g_build_filename (g_get_user_data_dir (), "neo-launcher", "iconpacks", NULL));
  for (const char *const *s = g_get_system_data_dirs (); *s; s++)
    g_strv_builder_take (db, g_build_filename (*s, "neo-launcher", "iconpacks", NULL));
  g_auto (GStrv) dirs = g_strv_builder_end (db);

  for (guint i = 0; dirs[i]; i++) {
    g_autoptr (GDir) d = g_dir_open (dirs[i], 0, NULL);
    for (const char *n; d && (n = g_dir_read_name (d));) {
      g_autofree char *map = g_build_filename (dirs[i], n, "gnome-map.json", NULL);
      if (g_hash_table_contains (seen, n) || !g_file_test (map, G_FILE_TEST_EXISTS))
        continue;
      g_hash_table_add (seen, g_strdup (n));
      g_autofree char *meta = g_build_filename (dirs[i], n, "pack.json", NULL);
      g_autoptr (JsonParser) parser = json_parser_new ();
      JsonNode *root = json_parser_load_from_file (parser, meta, NULL) ? json_parser_get_root (parser) : NULL;
      JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
      const char *title = o ? json_object_get_string_member_with_default (o, "title", NULL) : NULL;
      PackName *p = g_new0 (PackName, 1);
      p->id = g_strdup (n);
      p->title = g_strdup (title && *title ? title : n);
      g_ptr_array_add (packs, p);
    }
  }
  g_ptr_array_sort (packs, pack_name_cmp);
  return packs;
}

/* ---- Home screen: the layout ---- */

/* the grids One UI offers (columns × rows); Launcher3's 5 × 5 is the default. On a 360 dp phone these are what fit
 * the launcher's 56 dp icons with their labels: 6 columns would leave a 57 dp cell, 7 rows an 83 dp one under the
 * 85 dp an icon and its label take. */
static const int grid_sizes[][2] = { { 4, 5 }, { 4, 6 }, { 5, 5 }, { 5, 6 } };
static const char *const grid_labels[] = { "4 × 5", "4 × 6", "5 × 5", "5 × 6", NULL };
static const int drawer_sizes[] = { 4, 5 };
static const char *const drawer_labels[] = { "4 columns", "5 columns", NULL };
/* the icons' size, of Launcher3's 56 dp: 110 % is the most a 5-column cell holds (the press grows it by 10 %) */
static const double icon_scales[] = { 0.8, 0.9, 1.0, 1.1 };
static const char *const icon_labels[] = { "80%", "90%", "100%", "110%", NULL };

typedef struct {
  GSettings *neo;
  GtkWidget *grid, *drawer, *icons, *drawer_icons;
} Layout;

static void
layout_free (Layout *l)
{
  g_signal_handlers_disconnect_by_data (l->neo, l);
  g_object_unref (l->neo);
  g_free (l);
}

/* a pick row's choice, or `other` (a value set elsewhere, not one of the choices) as its subtitle */
static void
pick_show (GtkWidget *row, guint i, guint n, const char *other)
{
  ns_row_pick_select (row, i);
  if (i >= n)
    ns_row_set_subtitle (row, other);
}

static guint
scale_index (double v)
{
  for (guint i = 0; i < G_N_ELEMENTS (icon_scales); i++)
    if (fabs (icon_scales[i] - v) < 0.005)
      return i;
  return G_N_ELEMENTS (icon_scales);
}

static void
layout_show (Layout *l)
{
  int cols = g_settings_get_int (l->neo, "desktop-grid-columns"), rows = g_settings_get_int (l->neo, "desktop-grid-rows");
  guint i = 0;
  while (i < G_N_ELEMENTS (grid_sizes) && (grid_sizes[i][0] != cols || grid_sizes[i][1] != rows))
    i++;
  g_autofree char *grid = g_strdup_printf ("%d × %d", cols, rows);
  pick_show (l->grid, i, G_N_ELEMENTS (grid_sizes), grid);

  int dcols = g_settings_get_int (l->neo, "drawer-grid-columns");
  for (i = 0; i < G_N_ELEMENTS (drawer_sizes) && drawer_sizes[i] != dcols; i++)
    ;
  g_autofree char *drawer = g_strdup_printf ("%d columns", dcols);
  pick_show (l->drawer, i, G_N_ELEMENTS (drawer_sizes), drawer);

  GtkWidget *sizes[] = { l->icons, l->drawer_icons };
  const char *keys[] = { "desktop-icon-scale", "drawer-icon-scale" };
  for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
    if (!sizes[k])
      continue;
    double v = g_settings_get_double (l->neo, keys[k]);
    g_autofree char *pct = g_strdup_printf ("%.0f%%", v * 100);
    pick_show (sizes[k], scale_index (v), G_N_ELEMENTS (icon_scales), pct);
  }
}

static void
grid_picked (GtkWidget *row, guint i, gpointer data)
{
  /* columns and rows in one change, so that the launcher lays its pages out once (a separate object: delay-apply
   * mode stays on the object it is set on) */
  g_autoptr (GSettings) s = g_settings_new ("de.yesman.neo");
  g_settings_delay (s);
  g_settings_set_int (s, "desktop-grid-columns", grid_sizes[i][0]);
  g_settings_set_int (s, "desktop-grid-rows", grid_sizes[i][1]);
  g_settings_apply (s);
}

static void
drawer_picked (GtkWidget *row, guint i, gpointer data)
{
  Layout *l = data;
  g_settings_set_int (l->neo, "drawer-grid-columns", drawer_sizes[i]);
}

static void
icons_picked (GtkWidget *row, guint i, gpointer data)
{
  Layout *l = data;
  g_settings_set_double (l->neo, row == l->icons ? "desktop-icon-scale" : "drawer-icon-scale", icon_scales[i]);
}

/* the Layout card: the home screen's grid, icon size and app names, the drawer's grid, icon size and search pill */
static void
layout_group (AdwNavigationPage *page, GtkWidget *c, GSettings *neo)
{
  Layout *l = g_new0 (Layout, 1);
  l->neo = g_object_ref (neo);
  g_object_set_data_full (G_OBJECT (page), "ns-layout", l, (GDestroyNotify) layout_free);
  gboolean sizes = ns_has_key (neo, "desktop-icon-scale") && ns_has_key (neo, "drawer-icon-scale");

  GtkWidget *g = ns_group (c, "Layout");
  l->grid = ns_row_pick (g, "Home screen grid", grid_labels, 0, grid_picked, l);
  if (sizes)
    l->icons = ns_row_pick (g, "Home screen icon size", icon_labels, 0, icons_picked, l);
  GtkWidget *names = ns_row_switch (g, "App names", "Under the home screen's icons", NULL, NULL);
  g_settings_bind (neo, "desktop-hide-app-labels", ns_row_get_switch (names), "active",
                   G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_INVERT_BOOLEAN);
  l->drawer = ns_row_pick (g, "App drawer grid", drawer_labels, 0, drawer_picked, l);
  if (sizes)
    l->drawer_icons = ns_row_pick (g, "App drawer icon size", icon_labels, 0, icons_picked, l);
  ns_row_switch (g, "Search bar in the app drawer", NULL, neo, "search-drawer-enabled");

  static const char *const keys[] = { "changed::desktop-grid-columns", "changed::desktop-grid-rows",
                                      "changed::drawer-grid-columns", "changed::desktop-icon-scale",
                                      "changed::drawer-icon-scale" };
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    g_signal_connect_swapped (neo, keys[i], G_CALLBACK (layout_show), l);
  layout_show (l);
}

/* The home screen is neo-launcher. It follows de.yesman.neo live: the layout keys (main.c layout_read: the grid,
 * the icons' sizes, app names, the drawer's columns and search pill) lay it out again, a smaller grid moving the icons
 * it has no cell for to the next free one (model.c model_set_grid), and the icon keys (icons.c) draw its icons again
 * (tiles.c tiles_reload_icons).
 *
 * Not on this page, and why: the dock's icons (dock-num-icons: the dock keeps Launcher3's 5 cells whatever the grid),
 * the clock widget (home-widgets: always on the first page) and "Home gesture opens" (launcher: gnome-phone's home
 * gesture always shows Neo); the launcher reads none of them. */
void
ns_page_home (GtkWidget *row, gpointer data)
{
  GSettings *neo = ns_settings ("de.yesman.neo");
  if (!neo)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Home screen", &c);
  g_object_set_data_full (G_OBJECT (page), "neo", neo, g_object_unref);

  /* icon packs: what is installed */
  GStrvBuilder *vb = g_strv_builder_new (), *lb = g_strv_builder_new ();
  g_strv_builder_add (vb, "");
  g_strv_builder_add (lb, "None (theme icons)");
  g_autoptr (GPtrArray) packs = icon_packs ();
  for (guint i = 0; i < packs->len; i++) {
    PackName *p = packs->pdata[i];
    g_strv_builder_add (vb, p->id);
    g_strv_builder_add (lb, p->title);
  }
  char **values = g_strv_builder_end (vb), **labels = g_strv_builder_end (lb);
  g_strv_builder_unref (vb);
  g_strv_builder_unref (lb);
  g_object_set_data_full (G_OBJECT (page), "packs", values, (GDestroyNotify) g_strfreev);
  g_object_set_data_full (G_OBJECT (page), "pack-labels", labels, (GDestroyNotify) g_strfreev);
  layout_group (page, c, neo);
  GtkWidget *g = ns_group (c, "Icons");
  ns_row_choice (g, "Icon pack", neo, "icon-pack", (const char *const *) values, (const char *const *) labels);
  ns_row_choice (g, "Icon shape", neo, "icon-shape", shape_values, shape_labels);
  ns_row_switch (g, "Wrap unthemed icons", "Icons the pack does not cover get its background", neo,
                 "icon-wrap-unthemed");
  if (ns_has_key (neo, "icon-shape-legacy"))
    ns_row_switch (g, "Shape legacy icons", "Icons without pack art on a shaped background", neo,
                   "icon-shape-legacy");

  g = ns_group (c, NULL);
  page_row (g, "preferences-desktop-wallpaper-symbolic", "pink", "Wallpaper and style", "Parallax • Picture",
            ns_page_wallpaper);
  ns_push (page);
}

/* ---- General management ---- */

void
ns_page_general (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("General management", &c);
  GtkWidget *g = ns_group (c, NULL);
  page_row (g, "preferences-desktop-locale-symbolic", "violet", "Language and region", "Language • Formats",
            ns_page_language);
  page_row (g, "preferences-system-time-symbolic", "blue", "Date and time", "Automatic time • Time zone",
            ns_page_datetime);
  page_row (g, "input-keyboard-symbolic", "grey", "Keyboard", "On-screen keyboard • Layouts", ns_page_keyboard);
  page_row (g, "input-mouse-symbolic", "grey", "Mouse and touchpad", "Pointer speed • Hardware keyboard",
            ns_page_mouse);
  g = ns_group (c, NULL);
  page_row (g, "system-users-symbolic", "orange", "Users", "Your account • Password", ns_page_users);
  page_row (g, "view-grid-symbolic", "blue", "Default apps", "Browser • Email • Music", ns_page_default_apps);
  ns_push (page);
}

/* ---- About phone ---- */

static char *
first_line_after (const char *file, const char *key)
{
  g_autofree char *t = NULL;
  if (!g_file_get_contents (file, &t, NULL, NULL))
    return NULL;
  g_auto (GStrv) lines = g_strsplit (t, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    if (g_str_has_prefix (lines[i], key)) {
      char *v = g_strstrip (g_strdup (lines[i] + strlen (key)));
      if (*v == '"' && v[strlen (v) - 1] == '"') {
        v[strlen (v) - 1] = 0;
        memmove (v, v + 1, strlen (v));
      }
      return v;
    }
  return NULL;
}

static void
pretty_got (GObject *src, GAsyncResult *res, gpointer name)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) hn = NULL;
  if (v)
    g_variant_get (v, "(v)", &hn);
  const char *pretty = hn ? g_variant_get_string (hn, NULL) : "";
  if (*pretty)
    gtk_label_set_text (GTK_LABEL (name), pretty);
  g_object_unref (name);
}

void
ns_page_about (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("About phone", &c);

  g_autofree char *model = NULL;
  g_file_get_contents ("/sys/firmware/devicetree/base/model", &model, NULL, NULL);
  if (model)
    g_strstrip (model);

  /* the head: the phone's name and model, big */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (head, "ns-about-head");
  GtkWidget *name = gtk_label_new (g_get_host_name ());
  gtk_widget_add_css_class (name, "ns-big");
  GtkWidget *ml = gtk_label_new (model ? model : "");
  gtk_widget_add_css_class (ml, "ns-sub");
  gtk_box_append (GTK_BOX (head), name);
  gtk_box_append (GTK_BOX (head), ml);
  gtk_box_append (GTK_BOX (c), head);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.freedesktop.hostname1", "PrettyHostname"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, pretty_got,
                            g_object_ref (name));

  GtkWidget *g = ns_group (c, NULL);
  g_autofree char *os = first_line_after ("/etc/os-release", "PRETTY_NAME=");
  struct utsname u;
  uname (&u);
  ns_row_info (g, "Operating system", os);
  ns_row_info (g, "Kernel", u.release);
  g_autofree char *mem = first_line_after ("/proc/meminfo", "MemTotal:");
  g_autofree char *memh = mem ? g_format_size_full ((guint64) g_ascii_strtoull (mem, NULL, 10) * 1024,
                                                    G_FORMAT_SIZE_IEC_UNITS) : NULL;
  ns_row_info (g, "Memory", memh);
  g_autoptr (GFile) root = g_file_new_for_path ("/");
  g_autoptr (GFileInfo) fi = g_file_query_filesystem_info (root, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE ","
                                                           G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
  g_autofree char *st = NULL;
  if (fi) {
    g_autofree char *size = g_format_size (g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
    g_autofree char *free_ = g_format_size (g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
    st = g_strdup_printf ("%s free of %s", free_, size);
  }
  ns_row_info (g, "Storage", st);
  g_autofree char *cpus = g_strdup_printf ("%u cores", g_get_num_processors ());
  ns_row_info (g, "Processor", cpus);

  g = ns_group (c, NULL);
  page_row (g, NULL, NULL, "Software information", "Phone name • Versions", ns_page_software);
  ns_push (page);
}
