/* Battery > Max charge: hold the battery near a level while plugged in. The setting is battery.max_charge of
 * Minerva's brain.conf, written through `minerva-config` (ns-mc.c); the root charge-guard service (Minerva's
 * power/charge-guard) polls that file and cuts or restores charging. Nothing is rendered, nothing restarts. */
#include "ns-pages.h"
#include "ns-mc.h"
#include "ns-ui.h"

typedef struct {
  AdwNavigationPage *page;
  GtkWidget *val, *scale, *state;   /* the value row, the slider, the charge service's state */
  guint      timer;                 /* a drag is saved 600 ms after the last move */
  gboolean   syncing;               /* the slider is being set from the status */
  gboolean   saving;
} Ch;

static void
ch_free (Ch *c)
{
  if (c->timer)
    g_source_remove (c->timer);
  g_free (c);
}

static void
ch_show (Ch *c, int v)
{
  g_autofree char *t = v >= 100 ? g_strdup ("Off (100 %)") : g_strdup_printf ("%d %%", v);
  ns_row_set_subtitle (c->val, t);
}

static void
ch_status (JsonObject *root, const char *err, gpointer data)
{
  Ch *c = data;
  if (err || !root)
    return;
  if (!c->timer) {   /* a drag in progress is not overwritten */
    int v = (int) g_ascii_strtoll (ns_json_str (ns_json_obj (root, "brain"), "battery.max_charge"), NULL, 10);
    v = v < 50 || v > 100 ? 100 : v;
    c->syncing = TRUE;
    gtk_range_set_value (GTK_RANGE (c->scale), v);
    c->syncing = FALSE;
    ch_show (c, v);
  }
  JsonObject *b = ns_json_obj (root, "battery");   /* the charge service's state file; absent: nothing shown */
  const char *state = ns_json_str (b, "state"), *soc = ns_json_str (b, "soc_estimate"), *why = ns_json_str (b, "reason");
  gtk_widget_set_visible (c->state, *state != 0);
  if (*state) {
    g_autoptr (GString) t = g_string_new (state);
    t->str[0] = g_ascii_toupper (t->str[0]);
    if (*soc)
      g_string_append_printf (t, " • about %s %%", soc);
    if (*why)
      g_string_append_printf (t, " • %s", why);
    ns_row_set_subtitle (c->state, t->str);
  }
}

static void
ch_refresh (Ch *c)
{
  static const char *const args[] = { "status", "--json", "--fast", NULL };
  ns_mc_run (G_OBJECT (c->page), args, NULL, ch_status, c);
}

static void
ch_saved (JsonObject *root, const char *err, gpointer data)
{
  Ch *c = data;
  c->saving = FALSE;
  if (err) {
    g_autofree char *t = g_strdup_printf ("Not saved: %s", err);
    ns_row_set_subtitle (c->state, t);
    gtk_widget_set_visible (c->state, TRUE);
    return;
  }
  ch_refresh (c);
}

static gboolean
ch_save (gpointer data)
{
  Ch *c = data;
  c->timer = 0;
  g_autofree char *pair = g_strdup_printf ("battery.max_charge=%d", (int) gtk_range_get_value (GTK_RANGE (c->scale)));
  const char *const args[] = { "set", "--json", "--quiet", pair, NULL };
  c->saving = TRUE;
  ns_mc_run (G_OBJECT (c->page), args, NULL, ch_saved, c);
  return G_SOURCE_REMOVE;
}

static void
ch_moved (GtkRange *range, gpointer data)
{
  Ch *c = data;
  if (c->syncing)
    return;
  double raw = gtk_range_get_value (range), v = 5 * (int) (raw / 5 + 0.5);   /* steps of 5 */
  if (v != raw) {
    c->syncing = TRUE;
    gtk_range_set_value (range, v);
    c->syncing = FALSE;
  }
  ch_show (c, (int) v);
  if (c->timer)
    g_source_remove (c->timer);
  c->timer = g_timeout_add (600, ch_save, c);
}

void
ns_charge_limit_add (AdwNavigationPage *page, GtkWidget *content)
{
  if (!ns_minerva_available ())
    return;
  Ch *c = g_new0 (Ch, 1);
  c->page = page;
  g_object_set_data_full (G_OBJECT (page), "ns-charge-limit", c, (GDestroyNotify) ch_free);
  GtkWidget *g = ns_group (content, "Max charge");
  c->val = ns_row_info (g, "Charge limit", "");
  ns_row_scale (g, NULL, "battery-level-80-symbolic", 50, 100, 5, &c->scale);
  gtk_range_set_round_digits (GTK_RANGE (c->scale), 0);
  g_signal_connect (c->scale, "value-changed", G_CALLBACK (ch_moved), c);
  c->state = ns_row_info (g, "Charging", "");
  gtk_widget_set_visible (c->state, FALSE);
  ns_note (content, "Holds the battery near this level while plugged in; the phone reads the level from the battery "
                    "voltage. 100 % switches it off.");
  ch_refresh (c);
}
