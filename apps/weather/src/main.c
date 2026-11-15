/* Neo Weather: the page behind the home screen's weather widget, laid out like One UI's Weather: the place and the
 * temperature over the sky, the next 24 hours as a curve, today against yesterday, the week, and the details.
 * GTK4, so scrolling is the GPU moving what is already drawn; the page scrolls in libspongy's view (Android's fling
 * and the stretch at the ends), like the other GNOME Phone apps, where it is installed.
 *
 * Where: a city the user chose here (the location button, top right: a search by name in Open-Meteo's geocoder, which
 * works with Location off, flagged "manual" and never moved); otherwise the first city in GNOME Weather, if it has
 * one; otherwise where the phone is, from GeoClue at city accuracy, named by OpenStreetMap's Nominatim. The place is
 * kept in ~/.cache/neo-launcher/weather.json (lat, lon, place, manual), so the page opens on the last place at once
 * and moves when the phone has (unless the place is manual).
 * What: one Open-Meteo forecast (no key) with yesterday in it (past_days=1), fetched in curl and kept in
 * ~/.cache/neo-launcher/forecast.json; the page shows that at once and asks again when it is 15 minutes old. */
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <math.h>
#ifdef HAVE_SPONGY
#include <spongy-gtk.h>
#endif

#define STALE_S (15 * 60)
#define UA "neo-weather/1.0 (GNOME Phone)"
#define HOURS 24
#define COL_W 66              /* px, one hour in the hourly strip */

/* the city search: Open-Meteo's geocoder (no key, the forecast's provider) answers a name with places */
#define GEOCODE_URL      "https://geocoding-api.open-meteo.com/v1/search"
#define GEOCODE_COUNT    10   /* places listed for a name */
#define GEOCODE_MIN_LEN  2    /* characters before a search is sent */
#define GEOCODE_WAIT_S   15
#define SEARCH_DELAY_MS  400  /* the entry waits this long after the last key */
#define DIALOG_W         360  /* px, the dialog's width where it floats */
#define DIALOG_H         560  /* px, and its height */

typedef struct {
  GtkWidget       *win, *sky, *body, *spinner;
  double           lat, lon;
  char            *place;
  gboolean         have_place;
  gboolean         manual;       /* the place was chosen by the user: GeoClue and GNOME Weather no longer move it */
  GDBusConnection *system;
  char            *geo_client;   /* the GeoClue client object, while it looks */
  guint            geo_signal, geo_timeout;
  gboolean         geo_asked;
} Page;

/* ---- what the weather codes (WMO) look like and are called ---- */

static const char *
wx_icon (int code, gboolean day)
{
  if (code == 0)
    return day ? "weather-clear-symbolic" : "weather-clear-night-symbolic";
  if (code <= 2)
    return day ? "weather-few-clouds-symbolic" : "weather-few-clouds-night-symbolic";
  if (code == 3)
    return "weather-overcast-symbolic";
  if (code == 45 || code == 48)
    return "weather-fog-symbolic";
  if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82))
    return "weather-showers-scattered-symbolic";
  if (code >= 61 && code <= 67)
    return "weather-showers-symbolic";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "weather-snow-symbolic";
  if (code >= 95)
    return "weather-storm-symbolic";
  return "weather-overcast-symbolic";
}

static const char *
wx_name (int code)
{
  static const struct { int upto; const char *name; } names[] = {
    { 0, "Clear" }, { 1, "Mostly clear" }, { 2, "Partly cloudy" }, { 3, "Cloudy" }, { 48, "Fog" },
    { 55, "Drizzle" }, { 57, "Freezing drizzle" }, { 63, "Rain" }, { 65, "Heavy rain" }, { 67, "Freezing rain" },
    { 75, "Snow" }, { 77, "Snow grains" }, { 81, "Showers" }, { 82, "Heavy showers" }, { 86, "Snow showers" },
    { 99, "Thunderstorm" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (code <= names[i].upto)
      return names[i].name;
  return "Cloudy";
}

/* The sky behind the page, top to bottom: four colours and where each sits (0..1).
 *
 * It is a texture, not a CSS linear-gradient on the window. Scrolling redraws the whole window every frame, and on the
 * POCO X3 (Adreno 618, 1080x2400 at 120 Hz) GTK's gradient over the full window took a frame every ~24 ms, a third
 * of the refresh rate, where a flat background or a texture keeps every 8.3 ms vblank (measured 2026-11-12). The
 * texture is one pixel wide and 256 tall, made once per sky, and stretched over the window with linear filtering. */
typedef struct {
  const char *name;
  guint32     rgb[4];
  float       at[4];
} Sky;

static const Sky skies[] = {
  { "cloudy", { 0x56637f, 0x7f8ca8, 0x3a4670, 0x26305a }, { 0, .30f, .55f, 1 } },
  { "fog",    { 0x4a5578, 0x93a0bc, 0x3c4878, 0x29336a }, { 0, .30f, .55f, 1 } },
  { "clear",  { 0x2f6fd0, 0x6fb0ee, 0x2c5aa8, 0x1d3c7a }, { 0, .32f, .58f, 1 } },
  { "night",  { 0x0c1230, 0x1f2a5c, 0x141c44, 0x0c1230 }, { 0, .32f, .60f, 1 } },
  { "rain",   { 0x35415a, 0x5d6b86, 0x2c3654, 0x1c2440 }, { 0, .30f, .58f, 1 } },
  { "snow",   { 0x7d8ea8, 0xb8c6da, 0x5a6b8c, 0x34436a }, { 0, .30f, .58f, 1 } },
  { "storm",  { 0x22263a, 0x474b66, 0x24283e, 0x14172a }, { 0, .30f, .58f, 1 } },
};

#define SKY_H 256

static GdkTexture *
sky_texture (const Sky *s)
{
  guint8 *px = g_malloc (SKY_H * 3);
  for (int y = 0; y < SKY_H; y++) {
    float t = y / (float) (SKY_H - 1);
    int i = 0;
    while (i < 2 && t > s->at[i + 1])
      i++;
    float f = CLAMP ((t - s->at[i]) / MAX (s->at[i + 1] - s->at[i], 1e-6f), 0, 1);
    for (int c = 0; c < 3; c++) {
      int shift = 16 - 8 * c;
      float a = (s->rgb[i] >> shift) & 0xff, b = (s->rgb[i + 1] >> shift) & 0xff;
      px[y * 3 + c] = (guint8) lroundf (a + (b - a) * f);
    }
  }
  g_autoptr (GBytes) bytes = g_bytes_new_take (px, SKY_H * 3);
  return gdk_memory_texture_new (1, SKY_H, GDK_MEMORY_R8G8B8, bytes, 3);
}

static const char *
wx_sky (int code, gboolean day)
{
  if (!day)
    return "night";
  if (code <= 1)
    return "clear";
  if (code == 45 || code == 48)
    return "fog";
  if (code >= 95)
    return "storm";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "snow";
  if (code >= 51)
    return "rain";
  return "cloudy";
}

/* the sun yellow, the moon pale, everything else white (style.css) */
static GtkWidget *
wx_image (const char *icon, int size)
{
  GtkWidget *img = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (img), size);
  if (g_str_has_prefix (icon, "weather-clear-symbolic") || g_str_has_prefix (icon, "daytime-"))
    gtk_widget_add_css_class (img, "wx-sun");
  else if (strstr (icon, "night"))
    gtk_widget_add_css_class (img, "wx-moon");
  return img;
}

/* ---- the forecast, read ---- */

static double
num (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NAN;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_double (n) : NAN;
}

static const char *
str (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NULL;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_string (n) : NULL;
}

static JsonArray *
arr (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
}

static double
cur (JsonObject *c, const char *name)
{
  return c && json_object_has_member (c, name) ? json_object_get_double_member (c, name) : NAN;
}

static GDateTime *
parse_local (const char *t)
{
  if (!t)
    return NULL;
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autofree char *full = strlen (t) == 16 ? g_strconcat (t, ":00", NULL) : g_strdup (t);
  return g_date_time_new_from_iso8601 (full, tz);
}

static char *
fmt_temp (double t)
{
  if (isnan (t))
    return g_strdup ("–");
  double r = round (t);
  return g_strdup_printf ("%.0f°", r == 0 ? 0 : r);     /* never "-0°" */
}

static GtkWidget *
label (const char *text, const char *cls, float xalign)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  if (cls)
    gtk_widget_add_css_class (l, cls);
  return l;
}

/* ---- the hourly strip: a column per slot (time, icon, value, chance of rain) over the temperature curve ---- */

#define CURVE_H 34

typedef struct {
  GArray *temps;               /* double per slot */
  GArray *dots;                /* gboolean per slot */
  double  lo, hi;
} Curve;

static void
curve_free (Curve *c)
{
  g_array_unref (c->temps);
  g_array_unref (c->dots);
  g_free (c);
}

static void
draw_curve (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Curve *c = data;
  guint n = c->temps->len;
  double span = MAX (c->hi - c->lo, 1);
  #define CY(t) (h - 6 - ((t) - c->lo) / span * (h - 12))
  #define CX(i) ((i) * COL_W + COL_W / 2.0)
  cairo_set_line_width (cr, 1.6);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.75);
  for (guint i = 0; i < n; i++) {
    double t = g_array_index (c->temps, double, i);
    if (i == 0) {
      cairo_move_to (cr, CX (i), CY (t));
      continue;
    }
    /* smooth: each segment leaves and arrives level */
    double p = g_array_index (c->temps, double, i - 1), mx = (CX (i - 1) + CX (i)) / 2;
    cairo_curve_to (cr, mx, CY (p), mx, CY (t), CX (i), CY (t));
  }
  cairo_stroke (cr);
  cairo_set_source_rgb (cr, 1, 1, 1);
  for (guint i = 0; i < n; i++)
    if (g_array_index (c->dots, gboolean, i)) {
      cairo_arc (cr, CX (i), CY (g_array_index (c->temps, double, i)), 4, 0, 2 * G_PI);
      cairo_fill (cr);
    }
}

static GtkWidget *
drop_label (double pop, gboolean small)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  if (isnan (pop))
    return b;
  GtkWidget *d = gtk_image_new_from_icon_name ("weather-showers-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (d), small ? 10 : 12);
  gtk_widget_add_css_class (d, "wx-dim");
  g_autofree char *t = g_strdup_printf ("%.0f%%", pop);
  gtk_box_append (GTK_BOX (b), d);
  gtk_box_append (GTK_BOX (b), label (t, "wx-small", 0));
  return b;
}

typedef struct {
  GtkWidget *row;              /* the columns */
  Curve     *curve;
  GPtrArray *pops;
} Strip;

static void
add_slot (Strip *st, const char *time, const char *icon, const char *value, gboolean big, double temp, double pop,
          gboolean dot)
{
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request (col, COL_W, -1);
  gtk_box_append (GTK_BOX (col), label (time, "wx-hour", 0.5));
  GtkWidget *img = wx_image (icon, 28);
  gtk_box_append (GTK_BOX (col), img);
  gtk_box_append (GTK_BOX (col), label (value, big ? "wx-hour-temp" : "wx-hour-event", 0.5));
  gtk_box_append (GTK_BOX (st->row), col);
  g_ptr_array_add (st->pops, drop_label (pop, TRUE));
  g_array_append_val (st->curve->temps, temp);
  g_array_append_val (st->curve->dots, dot);
  if (!isnan (temp)) {
    st->curve->lo = MIN (st->curve->lo, temp);
    st->curve->hi = MAX (st->curve->hi, temp);
  }
}

/* the next 24 hours from `first`, with today's and tomorrow's sunrise and sunset in between */
static GtkWidget *
hourly_strip (JsonObject *hourly, guint first, JsonArray *sunrise, JsonArray *sunset)
{
  Strip st = { gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), g_new0 (Curve, 1), g_ptr_array_new () };
  st.curve->temps = g_array_new (FALSE, FALSE, sizeof (double));
  st.curve->dots = g_array_new (FALSE, FALSE, sizeof (gboolean));
  st.curve->lo = G_MAXDOUBLE;
  st.curve->hi = -G_MAXDOUBLE;
  JsonArray *time = arr (hourly, "time"), *temp = arr (hourly, "temperature_2m");
  JsonArray *code = arr (hourly, "weather_code"), *pop = arr (hourly, "precipitation_probability");
  JsonArray *isday = arr (hourly, "is_day");

  g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) g_date_time_unref);
  g_autoptr (GPtrArray) kinds = g_ptr_array_new ();
  for (guint d = 0; sunrise && d < json_array_get_length (sunrise); d++) {
    GDateTime *r = parse_local (str (sunrise, d)), *s = parse_local (str (sunset, d));
    if (r) { g_ptr_array_add (events, r); g_ptr_array_add (kinds, "Sunrise"); }
    if (s) { g_ptr_array_add (events, s); g_ptr_array_add (kinds, "Sunset"); }
  }

  for (guint i = first; time && i < first + HOURS && i < json_array_get_length (time); i++) {
    g_autoptr (GDateTime) t = parse_local (str (time, i));
    if (!t)
      continue;
    double tc = num (temp, i);
    g_autofree char *hl = g_date_time_format (t, "%H:%M");
    g_autofree char *value = fmt_temp (tc);
    add_slot (&st, hl, wx_icon ((int) num (code, i), num (isday, i) != 0), value, TRUE, tc, num (pop, i), TRUE);
    /* a sunrise or sunset within this hour follows it */
    g_autoptr (GDateTime) next = g_date_time_add_hours (t, 1);
    for (guint e = 0; e < events->len; e++) {
      GDateTime *at = events->pdata[e];
      if (g_date_time_compare (at, t) > 0 && g_date_time_compare (at, next) < 0) {
        g_autofree char *el = g_date_time_format (at, "%H:%M");
        double frac = g_date_time_difference (at, t) / (double) G_TIME_SPAN_HOUR, tn = num (temp, i + 1);
        gboolean rise = !g_strcmp0 (kinds->pdata[e], "Sunrise");
        add_slot (&st, el, rise ? "daytime-sunrise-symbolic" : "daytime-sunset-symbolic", kinds->pdata[e], FALSE,
                  isnan (tn) ? tc : tc + (tn - tc) * frac, NAN, FALSE);
      }
    }
  }

  GtkWidget *curve = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (curve), CURVE_H);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (curve), st.curve->temps->len * COL_W);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (curve), draw_curve, st.curve, (GDestroyNotify) curve_free);
  GtkWidget *pops = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  for (guint i = 0; i < st.pops->len; i++) {
    gtk_widget_set_size_request (st.pops->pdata[i], COL_W, -1);
    gtk_box_append (GTK_BOX (pops), st.pops->pdata[i]);
  }
  g_ptr_array_unref (st.pops);

  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (v), st.row);
  gtk_box_append (GTK_BOX (v), curve);
  gtk_box_append (GTK_BOX (v), pops);
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), v);
  return sc;
}

/* ---- the page ---- */

static GtkWidget *
card (void)
{
  GtkWidget *c = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (c, "wx-card");
  return c;
}

static const char *
uv_word (double uv)
{
  return uv < 3 ? "Low" : uv < 6 ? "Moderate" : uv < 8 ? "High" : uv < 11 ? "Very high" : "Extreme";
}

static GtkWidget *
detail (const char *title, const char *value, const char *sub)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (b, "wx-detail");
  gtk_widget_set_hexpand (b, TRUE);
  gtk_box_append (GTK_BOX (b), label (title, "wx-dim", 0));
  gtk_box_append (GTK_BOX (b), label (value, "wx-big", 0));
  if (sub)
    gtk_box_append (GTK_BOX (b), label (sub, "wx-small", 0));
  return b;
}

static void
set_sky (Page *p, const char *sky)
{
  static GdkTexture *made[G_N_ELEMENTS (skies)];
  for (guint i = 0; i < G_N_ELEMENTS (skies); i++)
    if (!strcmp (skies[i].name, sky)) {
      if (!made[i])
        made[i] = sky_texture (&skies[i]);
      gtk_picture_set_paintable (GTK_PICTURE (p->sky), GDK_PAINTABLE (made[i]));
      return;
    }
}

static void
fill (Page *p, JsonObject *o)
{
  JsonObject *c = o && json_object_has_member (o, "current") ? json_object_get_object_member (o, "current") : NULL;
  JsonObject *h = o && json_object_has_member (o, "hourly") ? json_object_get_object_member (o, "hourly") : NULL;
  JsonObject *d = o && json_object_has_member (o, "daily") ? json_object_get_object_member (o, "daily") : NULL;
  if (!c || !h || !d)
    return;
  for (GtkWidget *ch; (ch = gtk_widget_get_first_child (p->body));)
    gtk_box_remove (GTK_BOX (p->body), ch);

  int code = (int) cur (c, "weather_code");
  gboolean day = cur (c, "is_day") != 0;
  set_sky (p, wx_sky (code, day));
  JsonArray *dmax = arr (d, "temperature_2m_max"), *dmin = arr (d, "temperature_2m_min");
  JsonArray *dcode = arr (d, "weather_code"), *dpop = arr (d, "precipitation_probability_max");
  JsonArray *dtime = arr (d, "time"), *rise = arr (d, "sunrise"), *set = arr (d, "sunset"), *uv = arr (d, "uv_index_max");
  /* daily[0] is yesterday, daily[1] today */

  /* the head: place, temperature, what it is, today's high and low, feels like */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (head, "wx-head");
  GtkWidget *place = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *pin = gtk_image_new_from_icon_name ("mark-location-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (pin), 22);
  gtk_box_append (GTK_BOX (place), pin);
  gtk_box_append (GTK_BOX (place), label (p->place ? p->place : "", "wx-place", 0));
  gtk_box_append (GTK_BOX (head), place);
  g_autofree char *now = fmt_temp (cur (c, "temperature_2m"));
  gtk_box_append (GTK_BOX (head), label (now, "wx-now", 0));
  gtk_box_append (GTK_BOX (head), label (wx_name (code), "wx-cond", 0));
  g_autofree char *hi = fmt_temp (num (dmax, 1)), *lo = fmt_temp (num (dmin, 1));
  g_autofree char *feels = fmt_temp (cur (c, "apparent_temperature"));
  g_autofree char *range = g_strdup_printf ("↑%s / ↓%s\nFeels like %s", hi, lo, feels);
  GtkWidget *rl = label (range, "wx-range", 0);
  gtk_widget_set_margin_top (rl, 70);
  gtk_box_append (GTK_BOX (head), rl);
  gtk_box_append (GTK_BOX (p->body), head);

  /* the next 24 hours */
  GtkWidget *hours = card ();
  g_autofree char *summary = g_strdup_printf ("%s. High %s, low %s.", wx_name ((int) num (dcode, 1)), hi, lo);
  gtk_box_append (GTK_BOX (hours), label (summary, "wx-summary", 0));
  gtk_box_append (GTK_BOX (hours), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  JsonArray *htime = arr (h, "time");
  g_autoptr (GDateTime) dnow = g_date_time_new_now_local ();
  g_autofree char *hour_now = g_date_time_format (dnow, "%Y-%m-%dT%H:00");
  guint first = 0;
  for (guint i = 0; htime && i < json_array_get_length (htime); i++)
    if (!g_strcmp0 (str (htime, i), hour_now)) {
      first = i;
      break;
    }
  gtk_box_append (GTK_BOX (hours), hourly_strip (h, first, rise, set));
  gtk_box_append (GTK_BOX (p->body), hours);

  /* now against this hour yesterday */
  double then = num (arr (h, "temperature_2m"), first >= 24 ? first - 24 : G_MAXUINT);
  double diff = cur (c, "temperature_2m") - then;
  if (!isnan (diff)) {
    GtkWidget *cmp = card ();
    GtkWidget *t = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (t), gtk_image_new_from_icon_name ("temperature-symbolic"));
    gtk_box_append (GTK_BOX (t), label ("Today's temperature", "wx-dim", 0));
    gtk_box_append (GTK_BOX (cmp), t);
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    const char *say = fabs (diff) < 1 ? "About the same as yesterday"
                    : diff < 0 ? (diff > -4 ? "Temperatures a little lower than yesterday"
                                            : "Much colder than yesterday")
                               : (diff < 4 ? "Temperatures a little higher than yesterday"
                                           : "Much warmer than yesterday");
    GtkWidget *sl = label (say, "wx-big", 0);
    gtk_widget_set_hexpand (sl, TRUE);
    gtk_box_append (GTK_BOX (row), sl);
    if (fabs (diff) >= 1) {
      g_autofree char *dd = g_strdup_printf ("%.0f°%s", fabs (diff), diff < 0 ? "↓" : "↑");
      gtk_box_append (GTK_BOX (row), label (dd, "wx-delta", 1));
    }
    gtk_box_append (GTK_BOX (cmp), row);
    gtk_box_append (GTK_BOX (p->body), cmp);
  }

  /* the week: yesterday dim, then today and on */
  GtkWidget *week = card ();
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 18);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  for (guint i = 0; dtime && i < json_array_get_length (dtime); i++) {
    g_autofree char *noon = g_strconcat (str (dtime, i) ? str (dtime, i) : "", "T12:00", NULL);
    g_autoptr (GDateTime) dt = parse_local (noon);
    g_autofree char *name = i == 0 ? g_strdup ("Yesterday") : i == 1 ? g_strdup ("Today")
                          : dt ? g_date_time_format (dt, "%a") : g_strdup ("");
    g_autofree char *mx = fmt_temp (num (dmax, i)), *mn = fmt_temp (num (dmin, i));
    g_autofree char *temps = g_strdup_printf ("%s  %s", mx, mn);
    const char *cls = i == 0 ? "wx-yesterday" : "wx-day";
    GtkWidget *n = label (name, cls, 0);
    gtk_widget_set_hexpand (n, TRUE);
    gtk_grid_attach (GTK_GRID (grid), n, 0, i, 1, 1);
    if (i > 0) {
      int dc = (int) num (dcode, i);
      gtk_grid_attach (GTK_GRID (grid), drop_label (num (dpop, i), FALSE), 1, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, TRUE), 26), 2, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, FALSE), 26), 3, i, 1, 1);
    }
    gtk_grid_attach (GTK_GRID (grid), label (temps, cls, 1), 4, i, 1, 1);
  }
  gtk_box_append (GTK_BOX (week), grid);
  gtk_box_append (GTK_BOX (p->body), week);

  /* the details */
  GtkWidget *more = card ();
  GtkWidget *dg = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dg), 16);
  gtk_grid_set_column_homogeneous (GTK_GRID (dg), TRUE);
  g_autoptr (GDateTime) r = parse_local (str (rise, 1));
  g_autoptr (GDateTime) s = parse_local (str (set, 1));
  g_autofree char *rs = r ? g_date_time_format (r, "%H:%M") : g_strdup ("–");
  g_autofree char *ss = s ? g_date_time_format (s, "%H:%M") : g_strdup ("–");
  g_autofree char *ssub = g_strdup_printf ("Sunset %s", ss);
  double u = num (uv, 1);
  g_autofree char *uvs = isnan (u) ? g_strdup ("–") : g_strdup_printf ("%.0f", u);
  g_autofree char *hum = g_strdup_printf ("%.0f%%", cur (c, "relative_humidity_2m"));
  g_autofree char *wind = g_strdup_printf ("%.0f km/h", cur (c, "wind_speed_10m"));
  g_autofree char *pres = g_strdup_printf ("%.0f hPa", cur (c, "pressure_msl"));
  gtk_grid_attach (GTK_GRID (dg), detail ("Sunrise", rs, ssub), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("UV index", uvs, isnan (u) ? NULL : uv_word (u)), 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Humidity", hum, NULL), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Wind", wind, NULL), 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Pressure", pres, NULL), 0, 2, 1, 1);
  gtk_box_append (GTK_BOX (more), dg);
  gtk_box_append (GTK_BOX (p->body), more);

  gtk_box_append (GTK_BOX (p->body), label ("Open-Meteo.com", "wx-credit", 0.5));
}

/* ---- the forecast, fetched (curl, away from the main loop) and kept ---- */

static char *
cache_path (const char *name)
{
  return g_build_filename (g_get_user_cache_dir (), "neo-launcher", name, NULL);
}

static void
fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  Page *p = data;
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out;
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!ok || !json_parser_load_from_data (jp, out, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
    g_message ("Open-Meteo gave no answer");
    return;
  }
  g_autofree char *path = cache_path ("forecast.json");
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, out, -1, NULL);
  fill (p, json_node_get_object (json_parser_get_root (jp)));
}

static void
fetch (Page *p)
{
  char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (lat, sizeof lat, "%.4f", p->lat);
  g_ascii_formatd (lon, sizeof lon, "%.4f", p->lon);
  g_autofree char *url = g_strdup_printf (
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&past_days=1&forecast_days=8"
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,relative_humidity_2m,wind_speed_10m,pressure_msl"
    "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
    "uv_index_max", lat, lon);
  const char *argv[] = { "curl", "-sf", "--max-time", "20", "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp)
    return;
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  g_subprocess_communicate_utf8_async (sp, NULL, NULL, fetched, p);
  g_object_unref (sp);
}

/* the cached forecast, if it is for here; TRUE when it is fresh too */
static gboolean
load_cache (Page *p)
{
  g_autofree char *path = cache_path ("forecast.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  double lat = json_object_get_double_member_with_default (o, "latitude", 999);
  double lon = json_object_get_double_member_with_default (o, "longitude", 999);
  if (fabs (lat - p->lat) > 0.2 || fabs (lon - p->lon) > 0.2)
    return FALSE;
  fill (p, o);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = g_file_query_info (f, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
  return info && g_get_real_time () / G_USEC_PER_SEC -
                   (gint64) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) < STALE_S;
}

/* where: what the home screen's widget last showed */
static gboolean
load_place (Page *p)
{
  g_autofree char *path = cache_path ("weather.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  p->lat = json_object_get_double_member_with_default (o, "lat", 0);
  p->lon = json_object_get_double_member_with_default (o, "lon", 0);
  p->place = g_strdup (json_object_get_string_member_with_default (o, "place", ""));
  p->manual = json_object_get_boolean_member_with_default (o, "manual", FALSE);
  return p->have_place = p->lat || p->lon;
}

/* ---- where: GNOME Weather's first city, or GeoClue (city accuracy) named by Nominatim ---- */

#define GEO_BUS     "org.freedesktop.GeoClue2"
#define GEO_MANAGER "/org/freedesktop/GeoClue2/Manager"
#define GEO_CITY    4            /* GClueAccuracyLevel: city */
#define GEO_WAIT_S  30

static void
save_place (Page *p)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "lat");
  json_builder_add_double_value (b, p->lat);
  json_builder_set_member_name (b, "lon");
  json_builder_add_double_value (b, p->lon);
  json_builder_set_member_name (b, "place");
  json_builder_add_string_value (b, p->place ? p->place : "");
  json_builder_set_member_name (b, "manual");
  json_builder_add_boolean_value (b, p->manual);
  json_builder_end_object (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  g_autofree char *data = json_generator_to_data (gen, NULL);
  g_autofree char *path = cache_path ("weather.json");
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, data, -1, NULL);
}

/* a place found: kept, and its forecast fetched unless the page shows this place's already (about 2 km) and that is
 * fresh; a new name alone is shown from the forecast on file. name NULL keeps the name, "" clears it. */
static void
use_place (Page *p, double lat, double lon, const char *name)
{
  gboolean moved = !p->have_place || fabs (lat - p->lat) > 0.02 || fabs (lon - p->lon) > 0.02;
  p->lat = lat;
  p->lon = lon;
  if (name) {                     /* NULL: the name stays */
    g_free (p->place);
    p->place = g_strdup (name);
  }
  p->have_place = TRUE;
  save_place (p);
  if (moved || !load_cache (p))
    fetch (p);
}

static void show_search (Page *p);

/* no place at all: say how to get one (a known place keeps its weather) */
static void
nowhere (Page *p)
{
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  if (p->have_place)
    return;
  for (GtkWidget *ch; (ch = gtk_widget_get_first_child (p->body));)
    gtk_box_remove (GTK_BOX (p->body), ch);
  gtk_box_append (GTK_BOX (p->body),
                  label ("No location. Choose a city, or turn on Location in Settings and tap reload.", "wx-summary", 0));
  GtkWidget *choose = gtk_button_new_with_label ("Choose a city");
  gtk_widget_add_css_class (choose, "pill");
  gtk_widget_set_halign (choose, GTK_ALIGN_START);
  g_signal_connect_swapped (choose, "clicked", G_CALLBACK (show_search), p);
  gtk_box_append (GTK_BOX (p->body), choose);
}

/* the user's language as two letters, for the services that name places ("en" when it is "C") */
static char *
lang_code (void)
{
  g_autofree char *lang = g_strndup (g_get_language_names ()[0], 2);
  return g_strdup (g_strcmp0 (lang, "C") ? lang : "en");
}

typedef struct {
  Page  *p;
  double lat, lon;
} Naming;

static void
named (GObject *src, GAsyncResult *res, gpointer data)
{
  Naming *n = data;
  g_autofree char *out = NULL;
  const char *name = NULL;
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
      json_parser_load_from_data (jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
    JsonObject *o = json_node_get_object (json_parser_get_root (jp));
    JsonObject *a = json_object_has_member (o, "address") ? json_object_get_object_member (o, "address") : NULL;
    const char *keys[] = { "suburb", "city_district", "village", "town", "city", "municipality", "county" };
    for (guint i = 0; a && !name && i < G_N_ELEMENTS (keys); i++)
      if (json_object_has_member (a, keys[i]))
        name = json_object_get_string_member (a, keys[i]);
  }
  /* unnamed: the old name only if the phone is still there */
  gboolean near = n->p->have_place && fabs (n->lat - n->p->lat) <= 0.02 && fabs (n->lon - n->p->lon) <= 0.02;
  if (!n->p->manual)              /* a city chosen while the fix was on its way stays */
    use_place (n->p, n->lat, n->lon, name ? name : near ? NULL : "");
  g_free (n);
}

/* the place's name from Nominatim, in the user's language */
static void
name_place (Page *p, double lat, double lon)
{
  char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (la, sizeof la, "%.4f", lat);
  g_ascii_formatd (lo, sizeof lo, "%.4f", lon);
  g_autofree char *lang = lang_code ();
  g_autofree char *url = g_strdup_printf ("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=14"
                                          "&lat=%s&lon=%s&accept-language=%s", la, lo, lang);
  const char *argv[] = { "curl", "-sf", "--max-time", "15", "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp) {
    use_place (p, lat, lon, NULL);
    return;
  }
  Naming *n = g_new0 (Naming, 1);
  n->p = p;
  n->lat = lat;
  n->lon = lon;
  g_subprocess_communicate_utf8_async (sp, NULL, NULL, named, n);
  g_object_unref (sp);
}

/* GeoClue drops a client whose owner leaves the bus; this ends one the app no longer needs */
static void
geo_stop (Page *p)
{
  if (p->geo_timeout)
    g_source_remove (p->geo_timeout);
  p->geo_timeout = 0;
  if (!p->geo_client)
    return;
  if (p->geo_signal)
    g_dbus_connection_signal_unsubscribe (p->system, p->geo_signal);
  p->geo_signal = 0;
  g_dbus_connection_call (p->system, GEO_BUS, p->geo_client, GEO_BUS ".Client", "Stop", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_dbus_connection_call (p->system, GEO_BUS, GEO_MANAGER, GEO_BUS ".Manager", "DeleteClient",
                          g_variant_new ("(o)", p->geo_client), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_clear_pointer (&p->geo_client, g_free);
}

static void
geo_failed (Page *p, const char *what, GError *error)
{
  g_message ("location: %s: %s", what, error ? error->message : "none");
  geo_stop (p);
  nowhere (p);
}

static void
on_location (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer data)
{
  Page *p = data;
  const char *old_path, *new_path;
  g_autoptr (GError) error = NULL;
  double lat, lon;

  if (p->manual) {                /* a city was chosen: the fix is not needed */
    geo_stop (p);
    return;
  }
  g_variant_get (params, "(&o&o)", &old_path, &new_path);
  g_autoptr (GVariant) all = g_dbus_connection_call_sync (bus, GEO_BUS, new_path, "org.freedesktop.DBus.Properties",
                                                          "GetAll", g_variant_new ("(s)", GEO_BUS ".Location"),
                                                          G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                                          NULL, &error);
  if (!all) {
    geo_failed (p, "location", error);
    return;
  }
  g_autoptr (GVariant) props = g_variant_get_child_value (all, 0);
  if (!g_variant_lookup (props, "Latitude", "d", &lat) || !g_variant_lookup (props, "Longitude", "d", &lon)) {
    geo_failed (p, "location", NULL);
    return;
  }
  geo_stop (p);                   /* one fix is enough for a forecast */
  name_place (p, lat, lon);
}

static gboolean
geo_waited (gpointer data)
{
  Page *p = data;
  p->geo_timeout = 0;
  geo_failed (p, "no fix", NULL);
  return G_SOURCE_REMOVE;
}

static gboolean
geo_set (Page *p, const char *prop, GVariant *value, GError **error)
{
  g_autoptr (GVariant) ret = g_dbus_connection_call_sync (p->system, GEO_BUS, p->geo_client,
                                                          "org.freedesktop.DBus.Properties", "Set",
                                                          g_variant_new ("(ssv)", GEO_BUS ".Client", prop, value),
                                                          NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, error);
  return ret != NULL;
}

static void
on_geo_started (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!ret)
    geo_failed (data, "start", error);   /* location off, refused, or no agent to ask */
}

static void
on_geo_client (GObject *src, GAsyncResult *res, gpointer data)
{
  Page *p = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);

  p->geo_asked = FALSE;
  if (!ret) {
    geo_failed (p, "client", error);
    return;
  }
  g_variant_get (ret, "(o)", &p->geo_client);
  if (!geo_set (p, "DesktopId", g_variant_new_string ("de.yesman.NeoWeather"), &error) ||
      !geo_set (p, "RequestedAccuracyLevel", g_variant_new_uint32 (GEO_CITY), &error)) {
    geo_failed (p, "client setup", error);
    return;
  }
  p->geo_signal = g_dbus_connection_signal_subscribe (p->system, GEO_BUS, GEO_BUS ".Client", "LocationUpdated",
                                                      p->geo_client, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_location, p,
                                                      NULL);
  p->geo_timeout = g_timeout_add_seconds (GEO_WAIT_S, geo_waited, p);
  g_dbus_connection_call (p->system, GEO_BUS, p->geo_client, GEO_BUS ".Client", "Start", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_geo_started, p);
}

static void
locate (Page *p)
{
  if (p->geo_client || p->geo_asked)
    return;
  if (!p->system) {
    g_autoptr (GError) error = NULL;
    p->system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!p->system) {
      geo_failed (p, "system bus", error);
      return;
    }
  }
  p->geo_asked = TRUE;
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  g_dbus_connection_call (p->system, GEO_BUS, GEO_MANAGER, GEO_BUS ".Manager", "GetClient", NULL,
                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_geo_client, p);
}

/* The first city in GNOME Weather (org.gnome.Weather locations: serialized GWeatherLocations, (uv) with
 * (name, code, current, [(lat, lon)], [(parent lat, lon)]), radians): a place the user chose there comes first */
static gboolean
weather_app_city (Page *p)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = src ? g_settings_schema_source_lookup (src, "org.gnome.Weather", TRUE) : NULL;
  if (!schema || !g_settings_schema_has_key (schema, "locations"))
    return FALSE;
  g_autoptr (GSettings) settings = g_settings_new_full (schema, NULL, NULL);
  g_autoptr (GVariant) all = g_settings_get_value (settings, "locations");
  if (!g_variant_is_of_type (all, G_VARIANT_TYPE ("av")) || g_variant_n_children (all) == 0)
    return FALSE;
  g_autoptr (GVariant) first = g_variant_get_child_value (all, 0);     /* v */
  g_autoptr (GVariant) loc = g_variant_get_variant (first);              /* (uv) */
  if (!g_variant_is_of_type (loc, G_VARIANT_TYPE ("(uv)")))
    return FALSE;
  g_autoptr (GVariant) inner = NULL;
  guint32 version;
  g_variant_get (loc, "(uv)", &version, &inner);
  if (!g_variant_is_of_type (inner, G_VARIANT_TYPE ("(ssba(dd)a(dd))")))
    return FALSE;
  const char *name, *code;
  gboolean current;
  g_autoptr (GVariantIter) own = NULL, parent = NULL;
  g_variant_get (inner, "(&s&sba(dd)a(dd))", &name, &code, &current, &own, &parent);
  double lat, lon;
  if (!g_variant_iter_next (own, "(dd)", &lat, &lon) && !g_variant_iter_next (parent, "(dd)", &lat, &lon))
    return FALSE;
  use_place (p, lat * 180 / G_PI, lon * 180 / G_PI, name);
  return TRUE;
}

/* a place the user chose stays; otherwise GNOME Weather's first city, then where the phone is */
static void
where (Page *p)
{
  if (p->manual && p->have_place)
    return;
  if (!weather_app_city (p))
    locate (p);
}

/* ---- choosing a city by name (works with Location off) ---- */

typedef struct {
  Page         *p;
  AdwDialog    *dlg;
  GtkWidget    *entry, *status, *scroller, *results;
  GCancellable *cancel;            /* the search in flight */
} Search;

typedef struct {
  double lat, lon;
  char  *name;
} Hit;

typedef struct {
  Search       *s;
  AdwDialog    *dlg;               /* held, so Search outlives the answer */
  GCancellable *cancel;
} Query;

static void
search_free (Search *s)
{
  g_cancellable_cancel (s->cancel);
  g_clear_object (&s->cancel);
  g_free (s);
}

static void
hit_free (Hit *h)
{
  g_free (h->name);
  g_free (h);
}

static void
search_closed (AdwDialog *dlg, Search *s)
{
  g_cancellable_cancel (s->cancel);
}

/* a city picked: kept as the place (GeoClue and GNOME Weather leave it alone), its forecast fetched */
static void
hit_activated (AdwActionRow *row, Search *s)
{
  Hit *h = g_object_get_data (G_OBJECT (row), "hit");
  geo_stop (s->p);
  s->p->manual = TRUE;
  use_place (s->p, h->lat, h->lon, h->name);
  adw_dialog_close (s->dlg);
}

/* "Use my location": the chosen city goes, the place follows GNOME Weather or GeoClue again */
static void
mine_activated (AdwActionRow *row, Search *s)
{
  s->p->manual = FALSE;
  if (s->p->have_place)
    save_place (s->p);
  where (s->p);
  adw_dialog_close (s->dlg);
}

static void
search_status (Search *s, const char *text)
{
  gtk_label_set_label (GTK_LABEL (s->status), text ? text : "");
  gtk_widget_set_visible (s->status, text != NULL);
  gtk_widget_set_visible (s->scroller, text == NULL);        /* the list shows only with places in it */
}

static void
search_add (Search *s, JsonObject *r)
{
  const char *name = json_object_get_string_member_with_default (r, "name", "");
  if (!*name || !json_object_has_member (r, "latitude") || !json_object_has_member (r, "longitude"))
    return;
  g_autoptr (GString) where_ = g_string_new (NULL);
  const char *parts[] = { json_object_get_string_member_with_default (r, "admin1", ""),
                          json_object_get_string_member_with_default (r, "country", "") };
  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    if (*parts[i] && g_strcmp0 (parts[i], name))
      g_string_append_printf (where_, "%s%s", where_->len ? ", " : "", parts[i]);
  Hit *h = g_new0 (Hit, 1);
  h->lat = json_object_get_double_member (r, "latitude");
  h->lon = json_object_get_double_member (r, "longitude");
  h->name = g_strdup (name);
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), where_->str);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "hit", h, (GDestroyNotify) hit_free);
  g_signal_connect (row, "activated", G_CALLBACK (hit_activated), s);
  gtk_list_box_append (GTK_LIST_BOX (s->results), row);
}

static void
searched (GObject *src, GAsyncResult *res, gpointer data)
{
  Query *q = data;
  Search *s = q->s;
  g_autofree char *out = NULL;
  g_autoptr (JsonParser) jp = json_parser_new ();
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL);
  if (!g_cancellable_is_cancelled (q->cancel)) {           /* not superseded by a newer word, nor closed */
    gtk_list_box_remove_all (GTK_LIST_BOX (s->results));
    if (!ok || !out || !json_parser_load_from_data (jp, out, -1, NULL) ||
        !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
      search_status (s, "No answer. Check the network and try again.");
    } else {
      JsonObject *o = json_node_get_object (json_parser_get_root (jp));
      JsonArray *list = json_object_has_member (o, "results") ? json_object_get_array_member (o, "results") : NULL;
      for (guint i = 0; list && i < json_array_get_length (list); i++)
        search_add (s, json_array_get_object_element (list, i));
      search_status (s, gtk_widget_get_first_child (s->results) ? NULL : "No city found.");
    }
  }
  g_object_unref (q->dlg);
  g_object_unref (q->cancel);
  g_free (q);
}

static void
search_changed (GtkSearchEntry *entry, Search *s)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
  g_cancellable_cancel (s->cancel);
  g_object_unref (s->cancel);
  s->cancel = g_cancellable_new ();
  gtk_list_box_remove_all (GTK_LIST_BOX (s->results));
  if (g_utf8_strlen (text, -1) < GEOCODE_MIN_LEN) {
    search_status (s, "Type the name of a city.");
    return;
  }
  g_autofree char *q = g_uri_escape_string (text, NULL, FALSE);
  g_autofree char *lang = lang_code ();
  g_autofree char *url = g_strdup_printf (GEOCODE_URL "?name=%s&count=%d&language=%s&format=json", q, GEOCODE_COUNT, lang);
  g_autofree char *wait = g_strdup_printf ("%d", GEOCODE_WAIT_S);
  const char *argv[] = { "curl", "-sf", "--max-time", wait, "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp) {
    search_status (s, "No answer. Check the network and try again.");
    return;
  }
  search_status (s, "Searching…");
  Query *query = g_new0 (Query, 1);
  query->s = s;
  query->dlg = g_object_ref (s->dlg);
  query->cancel = g_object_ref (s->cancel);
  g_subprocess_communicate_utf8_async (sp, NULL, s->cancel, searched, query);
  g_object_unref (sp);
}

static void
show_search (Page *p)
{
  Search *s = g_new0 (Search, 1);
  s->p = p;
  s->cancel = g_cancellable_new ();
  AdwDialog *dlg = s->dlg = adw_dialog_new ();
  g_object_set_data_full (G_OBJECT (dlg), "search", s, (GDestroyNotify) search_free);
  adw_dialog_set_title (dlg, "Location");
  adw_dialog_set_content_width (dlg, DIALOG_W);
  adw_dialog_set_content_height (dlg, DIALOG_H);
  g_signal_connect (dlg, "closed", G_CALLBACK (search_closed), s);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  s->entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (s->entry), "Search for a city");
  gtk_search_entry_set_search_delay (GTK_SEARCH_ENTRY (s->entry), SEARCH_DELAY_MS);
  g_signal_connect (s->entry, "search-changed", G_CALLBACK (search_changed), s);
  gtk_box_append (GTK_BOX (box), s->entry);

  GtkWidget *mine = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mine), "Use my location");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (mine), "Needs Location turned on in Settings");
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (mine), TRUE);
  g_signal_connect (mine, "activated", G_CALLBACK (mine_activated), s);
  GtkWidget *mine_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (mine_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (mine_list, "boxed-list");
  gtk_list_box_append (GTK_LIST_BOX (mine_list), mine);
  gtk_box_append (GTK_BOX (box), mine_list);

  s->status = gtk_label_new (NULL);
  gtk_widget_add_css_class (s->status, "dim-label");
  gtk_box_append (GTK_BOX (box), s->status);

  s->results = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (s->results), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (s->results, "boxed-list");
  GtkWidget *sc = s->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), s->results);
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_box_append (GTK_BOX (box), sc);
  search_status (s, "Type the name of a city.");

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), box);
  adw_dialog_set_child (dlg, tv);
  adw_dialog_set_focus (dlg, s->entry);
  adw_dialog_present (dlg, p->win);
}

static void
page_free (Page *p)
{
  geo_stop (p);
  g_clear_object (&p->system);
  g_free (p->place);
  g_free (p);
}

/* ---- the window ---- */

/* reload: the forecast for the place shown, and where the phone is now */
static void
refresh_clicked (GtkButton *b, Page *p)
{
  if (p->have_place)
    fetch (p);
  where (p);
}

/* back (Alt+Left from Neo's edge swipe) and Escape close the page, back to the home screen */
static void
add_back (GtkWidget *win)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  GtkShortcutAction *close = gtk_named_action_new ("window.close");
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      close));
  gtk_widget_add_controller (win, keys);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/de/yesman/NeoWeather/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  Page *p = g_new0 (Page, 1);
  GtkWidget *win = p->win = adw_application_window_new (app);
  g_object_set_data_full (G_OBJECT (win), "page", p, (GDestroyNotify) page_free);
  gtk_window_set_title (GTK_WINDOW (win), "Weather");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  gtk_widget_add_css_class (win, "wx");
  add_back (win);

  /* the sky under everything, the page scrolling over it */
  GtkWidget *overlay = gtk_overlay_new ();
  p->sky = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (p->sky), GTK_CONTENT_FIT_FILL);
  gtk_picture_set_can_shrink (GTK_PICTURE (p->sky), TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), p->sky);
  set_sky (p, "cloudy");
  p->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (p->body, "wx-body");
#ifdef HAVE_SPONGY
  GtkWidget *sc = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (sc), p->body);
#else
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), p->body);
#endif
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), sc);

  /* reload floats over the sky, top right; back is the system's (the swipe, or closing the app) */
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (bar, GTK_ALIGN_START);
  gtk_widget_set_halign (bar, GTK_ALIGN_END);
  gtk_widget_add_css_class (bar, "wx-bar");
  p->spinner = gtk_spinner_new ();
  GtkWidget *reload = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (reload, "flat");
  gtk_widget_add_css_class (reload, "circular");
  g_signal_connect (reload, "clicked", G_CALLBACK (refresh_clicked), p);
  GtkWidget *pick = gtk_button_new_from_icon_name ("mark-location-symbolic");
  gtk_widget_add_css_class (pick, "flat");
  gtk_widget_add_css_class (pick, "circular");
  gtk_widget_set_tooltip_text (pick, "Choose a city");
  g_signal_connect_swapped (pick, "clicked", G_CALLBACK (show_search), p);
  gtk_box_append (GTK_BOX (bar), p->spinner);
  gtk_box_append (GTK_BOX (bar), pick);
  gtk_box_append (GTK_BOX (bar), reload);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), bar);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), overlay);

  gtk_window_present (GTK_WINDOW (win));
  /* the last place at once, then where the phone is now */
  if (load_place (p)) {
    if (!load_cache (p))
      fetch (p);
  } else
    gtk_box_append (GTK_BOX (p->body), label ("Finding where you are…", "wx-summary", 0));
  where (p);
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new ("de.yesman.NeoWeather", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
