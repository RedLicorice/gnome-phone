/* The Avatar page's live preview: Minerva's real face, drawn by the same engine and Cairo executor as the Phosh plugin
 * (libminerva-face, Minerva's kiosk/widget: one implementation, opened here with dlopen so that Neo Settings runs
 * without it). Its face data is the kiosk's Skin (face.json with the owner's settings laid in), read again on
 * SettingsChanged; its zoom is the owner's widget value the schema's preview names. The pose buttons drive only the
 * preview's own engine, never Minerva's state. It animates only while it is mapped (the page is shown), at the
 * profile's frame rate; its height is a share of the screen's, both from the schema's "preview" (data). */
#include "ns-avatar-preview.h"

#ifdef HAVE_MINERVA_FACE

#include <gmodule.h>
#include <minerva-face/face-engine.h>
#include <minerva-face/face-paint.h>

/* the library's soname (its ABI) and the kiosk's D-Bus API: the protocol's names */
#define FACE_LIBRARY "libminerva-face.so.0"
#define KIOSK_NAME   "org.minerva.Kiosk1"
#define KIOSK_PATH   "/org/minerva/Kiosk1"
#define KIOSK_IFACE  "org.minerva.Kiosk1"
#define KIOSK_SKIN   "Skin"    /* the face data the widgets build their engine from */
#define KIOSK_CALLS  "Calls"   /* the calls every widget replays */

/* libminerva-face's API, resolved once */
static struct {
  gboolean          tried, ok;
  FaceEngine      *(*engine_new) (const FaceEngineOptions *, FaceEngineFlushed, FaceEngineBroken, gpointer, GError **);
  void             (*engine_free) (FaceEngine *);
  gboolean         (*host_call) (FaceEngine *, const char *, guint, const char *const *);
  gboolean         (*set_active) (FaceEngine *, gboolean);
  const FaceFrame *(*frame) (FaceEngine *);
  FacePainter     *(*painter_new) (char **, guint, GError **);
  void             (*painter_free) (FacePainter *);
  void             (*paint) (FacePainter *, cairo_t *, const FaceFrame *, const double[5]);
} face;

static gboolean
face_load (void)
{
  if (face.tried)
    return face.ok;
  face.tried = TRUE;
  GModule *m = g_module_open (FACE_LIBRARY, G_MODULE_BIND_LOCAL);
  if (m == NULL) {
    g_message ("no avatar preview: %s", g_module_error ());
    return FALSE;
  }
  face.ok = g_module_symbol (m, "face_engine_new", (gpointer *) &face.engine_new) &&
            g_module_symbol (m, "face_engine_free", (gpointer *) &face.engine_free) &&
            g_module_symbol (m, "face_engine_host_call", (gpointer *) &face.host_call) &&
            g_module_symbol (m, "face_engine_set_active", (gpointer *) &face.set_active) &&
            g_module_symbol (m, "face_engine_frame", (gpointer *) &face.frame) &&
            g_module_symbol (m, "face_painter_new", (gpointer *) &face.painter_new) &&
            g_module_symbol (m, "face_painter_free", (gpointer *) &face.painter_free) &&
            g_module_symbol (m, "face_painter_paint", (gpointer *) &face.paint);
  if (!face.ok)
    g_message ("no avatar preview: %s lacks a symbol", FACE_LIBRARY);
  g_module_make_resident (m);   /* the library is not unloaded under a live engine */
  return face.ok;
}

typedef struct {
  GtkWidget       *box;        /* the preview: the face over its pose buttons */
  GtkWidget       *area;
  GDBusConnection *bus;
  guint            sub;
  JsonObject      *conf;       /* the schema's "preview" with the kiosk's "engine" options */
  FaceEngine      *engine;
  FacePainter     *painter;
  char            *pose;       /* the pose the buttons last chose */
  double           zoom;
  char            *zoom_key;   /* the owner's widget value of the zoom ("section.key") */
  GCancellable    *cancel;
} Preview;

static const char *
str_of (JsonObject *o, const char *k, const char *fallback)
{
  return o && json_object_has_member (o, k) ? json_object_get_string_member (o, k) : fallback;
}

static double
num_of (JsonObject *o, const char *k, double fallback)
{
  return o && json_object_has_member (o, k) ? json_object_get_double_member (o, k) : fallback;
}

static void
preview_drop (Preview *p)
{
  if (p->painter)
    face.painter_free (p->painter);
  if (p->engine)
    face.engine_free (p->engine);
  p->painter = NULL;
  p->engine = NULL;
}

static void
preview_free (Preview *p)
{
  g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  if (p->sub)
    g_dbus_connection_signal_unsubscribe (p->bus, p->sub);
  preview_drop (p);
  g_clear_object (&p->bus);
  g_clear_pointer (&p->conf, json_object_unref);
  g_free (p->pose);
  g_free (p->zoom_key);
  g_free (p);
}

static void on_flushed (gpointer data) { gtk_widget_queue_draw (((Preview *) data)->area); }
static void on_broken (gpointer data) { g_message ("the avatar preview's engine stopped"); }

static void
call_pose (Preview *p)
{
  if (p->engine == NULL)
    return;
  g_autofree char *call = g_strdup_printf ("[\"setPose\", \"%s\", \"\"]", p->pose);
  const char *args[] = { call };
  face.host_call (p->engine, "call", 1, args);
}

/* A new Skin: the engine made again from it, in the schema's profile, in the pose last chosen */
static void
preview_start (Preview *p, const char *skin, const char *calls)
{
  JsonObject *eng = json_object_get_object_member (p->conf, "engine");
  const char *profile = str_of (p->conf, "profile", "");
  FaceEngineOptions o = {
    .resource_prefix = str_of (eng, "resource_prefix", NULL), .entry = str_of (eng, "entry", NULL),
    .memory_limit = (gsize) (num_of (eng, "memory_limit_mb", 0) * 1024 * 1024),
    .stack_size = (gsize) (num_of (eng, "stack_kb", 0) * 1024),
  };
  g_autoptr (GError) err = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();

  preview_drop (p);
  if (o.resource_prefix == NULL || o.entry == NULL || !json_parser_load_from_data (parser, skin, -1, NULL))
    return;
  p->engine = face.engine_new (&o, on_flushed, on_broken, p, &err);
  if (p->engine == NULL) {
    g_message ("avatar preview: %s", err->message);
    return;
  }
  const FaceFrame *frame = face.frame (p->engine);
  p->painter = face.painter_new (frame->opcodes, frame->n_opcodes, &err);
  const char *start[] = { skin, str_of (eng, "methods", "") };
  if (p->painter == NULL || !face.host_call (p->engine, "start", 2, start)) {
    preview_drop (p);
    return;
  }
  /* what the kiosk told every widget (her look over the owner's settings, ...), as the widget replays it; then the
   * preview's own profile and pose */
  if (calls != NULL && *calls) {
    const char *replay[] = { calls };
    face.host_call (p->engine, "replay", 1, replay);
  }
  /* the profile's frame rate and idle life, as the kiosk gives the face it shows */
  JsonObject *face_obj = json_object_get_object_member (json_node_get_object (json_parser_get_root (parser)), "face");
  JsonObject *profiles = face_obj ? json_object_get_object_member (face_obj, "profiles") : NULL;
  if (profiles && json_object_has_member (profiles, profile)) {
    g_autoptr (JsonGenerator) gen = json_generator_new ();
    g_autoptr (JsonNode) call = json_node_new (JSON_NODE_ARRAY);
    JsonArray *a = json_array_new ();
    json_array_add_string_element (a, "profile");
    json_array_add_object_element (a, json_object_ref (json_object_get_object_member (profiles, profile)));
    json_node_take_array (call, a);
    json_generator_set_root (gen, call);
    g_autofree char *text = json_generator_to_data (gen, NULL);
    const char *args[] = { text };
    face.host_call (p->engine, "call", 1, args);
  }
  const char *idle[] = { profile };
  face.host_call (p->engine, "idle", 1, idle);
  call_pose (p);
  face.set_active (p->engine, gtk_widget_get_mapped (p->area));
}

static void
skin_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  const char *skin = NULL, *calls = NULL;
  Preview *p = data;

  if (out == NULL)
    return;   /* also when the page went away: cancelled */
  g_variant_get (out, "(@a{sv})", &props);
  g_variant_lookup (props, KIOSK_CALLS, "&s", &calls);
  if (g_variant_lookup (props, KIOSK_SKIN, "&s", &skin))
    preview_start (p, skin, calls);
}

static void
fetch_skin (Preview *p)
{
  g_dbus_connection_call (p->bus, KIOSK_NAME, KIOSK_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                          g_variant_new ("(s)", KIOSK_IFACE), G_VARIANT_TYPE ("(a{sv})"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, p->cancel, skin_got, p);
}

/* the owner's widget values: the zoom the preview draws with */
static void
apply_widget (Preview *p, GVariant *values)
{
  const char *v = NULL;

  p->zoom = num_of (p->conf, "zoom", 1);
  if (values && p->zoom_key && g_variant_lookup (values, p->zoom_key, "&s", &v))
    p->zoom = g_ascii_strtod (v, NULL);
  gtk_widget_queue_draw (p->area);
}

static void
on_settings (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer data)
{
  Preview *p = data;
  g_autoptr (GVariant) values = NULL;
  gboolean face_changed = FALSE;

  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(a{ss}b)")))
    return;
  g_variant_get (params, "(@a{ss}b)", &values, &face_changed);
  apply_widget (p, values);
  if (face_changed)
    fetch_skin (p);
}

static void
settings_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) v = NULL;

  if (out == NULL)
    return;
  g_variant_get (out, "(v)", &v);
  apply_widget (data, v);
}

/* the face in the square of the area's height, centred, drawn zoom times larger (its margin cut), as the plugin does */
static void
draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Preview *p = data;
  const FaceFrame *f;

  if (p->engine == NULL || p->painter == NULL || (f = face.frame (p->engine)) == NULL || f->width <= 0)
    return;
  double side = MIN (width, height), scale = side / f->width * p->zoom;
  cairo_rectangle (cr, (width - side) / 2, (height - side) / 2, side, side);
  cairo_clip (cr);
  cairo_translate (cr, width / 2.0, height / 2.0);
  cairo_scale (cr, scale, scale);
  cairo_translate (cr, -f->width / 2, -f->height / 2);
  face.paint (p->painter, cr, f, f->head);
}

/* only while the page is shown: zero cost once it is closed or covered */
static void
mapped (GtkWidget *w, Preview *p)
{
  GdkSurface *surface = gtk_native_get_surface (gtk_widget_get_native (w));
  GdkMonitor *mon = surface ? gdk_display_get_monitor_at_surface (gtk_widget_get_display (w), surface) : NULL;
  GdkRectangle geo = { 0 };

  if (mon && num_of (p->conf, "height_share", 0) > 0) {   /* the schema's sizes: none, the drawing's own */
    gdk_monitor_get_geometry (mon, &geo);
    int h = (int) (geo.height * num_of (p->conf, "height_share", 0));
    h = CLAMP (h, (int) num_of (p->conf, "min_height_px", 0), (int) num_of (p->conf, "max_height_px", G_MAXINT));
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (p->area), h);
  }
  if (p->engine)
    face.set_active (p->engine, TRUE);
}

static void
unmapped (GtkWidget *w, Preview *p)
{
  if (p->engine)
    face.set_active (p->engine, FALSE);
}

static void
pose_clicked (GtkButton *b, Preview *p)
{
  g_free (p->pose);
  p->pose = g_strdup (g_object_get_data (G_OBJECT (b), "pose"));
  call_pose (p);
}

GtkWidget *
ns_avatar_preview_new (JsonObject *conf, const char *lang)
{
  if (conf == NULL || !face_load ())
    return NULL;
  Preview *p = g_new0 (Preview, 1);
  JsonArray *poses = json_object_has_member (conf, "poses") ? json_object_get_array_member (conf, "poses") : NULL;

  p->conf = json_object_ref (conf);
  p->cancel = g_cancellable_new ();
  p->zoom_key = g_strdup (str_of (conf, "zoom_key", NULL));
  p->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  p->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (p->box, "ns-avatar-preview");
  p->area = gtk_drawing_area_new ();
  gtk_widget_set_hexpand (p->area, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->area), draw, p, NULL);
  gtk_box_append (GTK_BOX (p->box), p->area);

  /* the poses: a strip that scrolls sideways on a narrow phone */
  GtkWidget *strip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  for (guint i = 0; poses && i < json_array_get_length (poses); i++) {
    JsonObject *o = json_array_get_object_element (poses, i);
    JsonObject *label = json_object_get_object_member (o, "label");
    const char *text = label && json_object_has_member (label, lang) ? json_object_get_string_member (label, lang)
                       : str_of (label, "en", str_of (o, "pose", ""));
    GtkWidget *b = gtk_button_new_with_label (text);
    gtk_widget_add_css_class (b, "pill");
    g_object_set_data_full (G_OBJECT (b), "pose", g_strdup (str_of (o, "pose", "")), g_free);
    g_signal_connect (b, "clicked", G_CALLBACK (pose_clicked), p);
    gtk_box_append (GTK_BOX (strip), b);
    if (i == 0)
      p->pose = g_strdup (str_of (o, "pose", ""));
  }
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), strip);
  gtk_widget_set_margin_start (scroll, 12);
  gtk_widget_set_margin_end (scroll, 12);
  gtk_widget_set_margin_bottom (scroll, 6);
  gtk_box_append (GTK_BOX (p->box), scroll);
  if (p->pose == NULL)
    p->pose = g_strdup ("");

  g_signal_connect (p->area, "map", G_CALLBACK (mapped), p);
  g_signal_connect (p->area, "unmap", G_CALLBACK (unmapped), p);
  g_object_set_data_full (G_OBJECT (p->box), "ns-preview", p, (GDestroyNotify) preview_free);
  apply_widget (p, NULL);
  if (p->bus != NULL) {
    p->sub = g_dbus_connection_signal_subscribe (p->bus, KIOSK_NAME, KIOSK_IFACE, "SettingsChanged", KIOSK_PATH, NULL,
                                                 G_DBUS_SIGNAL_FLAGS_NONE, on_settings, p, NULL);
    g_dbus_connection_call (p->bus, KIOSK_NAME, KIOSK_PATH, "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", KIOSK_IFACE, "Settings"), G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, p->cancel, settings_got, p);
    fetch_skin (p);
  }
  return p->box;
}

#else

GtkWidget *
ns_avatar_preview_new (JsonObject *conf, const char *lang)
{
  return NULL;   /* built without libminerva-face's headers: no preview */
}

#endif
