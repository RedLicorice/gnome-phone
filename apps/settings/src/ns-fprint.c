/* Fingerprints: fprintd (net.reactivated.Fprint, on the system bus), used the way GNOME Settings' fingerprint
 * dialog uses it.
 *
 * The Lock screen page shows a Fingerprints row once fprintd names a default device, so a phone without a sensor
 * sees nothing. The Fingerprints page lists the user's enrolled fingers (ListEnrolledFingers, no claim needed),
 * deletes one (Claim, DeleteEnrolledFinger, Release) and adds one on an enroll page (Claim, EnrollStart, then
 * EnrollStatus signals until one says done, EnrollStop, Release). Claiming is the polkit action
 * net.reactivated.fprint.device.enroll (auth_self_keep): the shell's agent asks for the password once.
 *
 * Nothing here blocks: every call is async and cancelled with its page; a page that goes away while it holds the
 * device stops the enroll and releases it (fprintd also releases a claim when its client leaves the bus). */
#include "ns-pages.h"
#include "ns-ui.h"

#define FP_NAME     "net.reactivated.Fprint"
#define FP_MANAGER  "/net/reactivated/Fprint/Manager"
#define FP_IMANAGER "net.reactivated.Fprint.Manager"
#define FP_IDEVICE  "net.reactivated.Fprint.Device"
#define FP_ERROR    "net.reactivated.Fprint.Error."
/* a call that may wait on the password dialog */
#define FP_AUTH_TIMEOUT (5 * 60 * 1000)

static const struct {
  const char *id, *label;
} fingers[] = {
  { "right-thumb", "Right thumb" },           { "right-index-finger", "Right index finger" },
  { "right-middle-finger", "Right middle finger" }, { "right-ring-finger", "Right ring finger" },
  { "right-little-finger", "Right little finger" }, { "left-thumb", "Left thumb" },
  { "left-index-finger", "Left index finger" }, { "left-middle-finger", "Left middle finger" },
  { "left-ring-finger", "Left ring finger" },   { "left-little-finger", "Left little finger" },
};

static const char *
finger_label (const char *id)
{
  for (guint i = 0; i < G_N_ELEMENTS (fingers); i++)
    if (!g_strcmp0 (id, fingers[i].id))
      return fingers[i].label;
  return id;
}

static gboolean
fp_error_is (GError *err, const char *name)
{
  g_autofree char *remote = err ? g_dbus_error_get_remote_error (err) : NULL;
  return remote && g_str_has_prefix (remote, FP_ERROR) && g_str_equal (remote + strlen (FP_ERROR), name);
}

static gboolean
cancelled (GError *err)
{
  return g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* fprintd's errors in words */
static char *
fp_error_text (GError *err)
{
  if (fp_error_is (err, "PermissionDenied"))
    return g_strdup ("Not allowed: the password was not given.");
  if (fp_error_is (err, "AlreadyInUse"))
    return g_strdup ("The sensor is busy with something else. Try again in a moment.");
  if (fp_error_is (err, "NoSuchDevice"))
    return g_strdup ("The fingerprint sensor isn't available.");
  if (fp_error_is (err, "PrintsNotDeleted"))
    return g_strdup ("The fingerprint couldn't be deleted.");
  g_autoptr (GError) copy = g_error_copy (err);
  g_dbus_error_strip_remote_error (copy);
  return g_strdup (copy->message);
}

static void
device_call (GDBusConnection *bus, const char *device, const char *method, GVariant *args, const GVariantType *reply,
             GCancellable *cancel, GAsyncReadyCallback done, gpointer data)
{
  g_dbus_connection_call (bus, FP_NAME, device, FP_IDEVICE, method, args, reply,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, FP_AUTH_TIMEOUT, cancel, done, data);
}

/* EnrollStop and Release of a page that has gone: nobody waits for the answers */
static void
device_forget (GDBusConnection *bus, const char *device, gboolean enrolling)
{
  if (enrolling)
    device_call (bus, device, "EnrollStop", NULL, NULL, NULL, NULL, NULL);
  device_call (bus, device, "Release", NULL, NULL, NULL, NULL, NULL);
}

/* ---- the enroll page ---- */

typedef struct {
  GDBusConnection *bus;
  GCancellable    *cancel;
  char            *device, *finger;
  guint            sub;            /* the EnrollStatus subscription */
  gboolean         claimed, enrolling;
  int              stages, passed;
  GtkWidget       *ring, *icon, *title, *hint, *button;
  AdwNavigationPage *page;        /* the enroll page, which owns this */
} Enroll;

/* where the phone's sensor is, in the enroll page's words: device data (de.yesman.NeoSettings, a device override) */
static char *
sensor_text (const char *key)
{
  g_autoptr (GSettings) s = ns_settings ("de.yesman.NeoSettings");
  return s ? g_settings_get_string (s, key) : g_strdup ("");
}

static void
enroll_free (Enroll *e)
{
  g_cancellable_cancel (e->cancel);
  if (e->sub)
    g_dbus_connection_signal_unsubscribe (e->bus, e->sub);
  if (e->claimed)
    device_forget (e->bus, e->device, e->enrolling);
  g_object_unref (e->cancel);
  g_object_unref (e->bus);
  g_free (e->device);
  g_free (e->finger);
  g_free (e);
}

static void
ring_draw (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Enroll *e = data;
  double cx = w / 2.0, cy = h / 2.0, r = MIN (w, h) / 2.0 - 8;
  GdkRGBA c;
  gtk_widget_get_color (GTK_WIDGET (area), &c);
  cairo_set_line_width (cr, 8);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_source_rgba (cr, c.red, c.green, c.blue, 0.18);
  cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
  cairo_stroke (cr);
  double f = e->stages > 0 ? CLAMP ((double) e->passed / e->stages, 0, 1) : 0;
  if (f > 0) {
    gdk_cairo_set_source_rgba (cr, &c);
    cairo_arc (cr, cx, cy, r, -G_PI / 2, -G_PI / 2 + 2 * G_PI * f);
    cairo_stroke (cr);
  }
}

typedef enum { BUTTON_CANCEL, BUTTON_DONE, BUTTON_RETRY } ButtonRole;

static void
enroll_show (Enroll *e, const char *title, const char *hint, ButtonRole role)
{
  if (title)
    gtk_label_set_text (GTK_LABEL (e->title), title);
  if (hint)
    gtk_label_set_text (GTK_LABEL (e->hint), hint);
  static const char *const labels[] = { "Cancel", "Done", "Try again" };
  gtk_button_set_label (GTK_BUTTON (e->button), labels[role]);
  g_object_set_data (G_OBJECT (e->button), "ns-role", GINT_TO_POINTER (role));
  if (role == BUTTON_CANCEL)
    gtk_widget_remove_css_class (e->button, "suggested-action");
  else
    gtk_widget_add_css_class (e->button, "suggested-action");
  gtk_widget_queue_draw (e->ring);
}

static void enroll_start (Enroll *e);

static void
enroll_fail (Enroll *e, const char *title, const char *hint)
{
  enroll_show (e, title, hint, BUTTON_RETRY);
}

static void
status (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
        GVariant *params, gpointer data)
{
  Enroll *e = data;
  const char *result = NULL;
  gboolean done = FALSE;
  if (!e->enrolling || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(sb)")))
    return;
  g_variant_get (params, "(&sb)", &result, &done);
  if (done) {
    /* fprintd wants EnrollStop after the last status, before anything else */
    device_call (e->bus, e->device, "EnrollStop", NULL, NULL, NULL, NULL, NULL);
    e->enrolling = FALSE;
  }

  if (g_str_equal (result, "enroll-completed")) {
    e->passed = MAX (e->passed, e->stages);
    device_call (e->bus, e->device, "Release", NULL, NULL, NULL, NULL, NULL);
    e->claimed = FALSE;
    gtk_widget_add_css_class (e->icon, "ns-accent");
    g_autofree char *hint = g_strdup_printf ("%s is ready.", finger_label (e->finger));
    enroll_show (e, "Fingerprint added", hint, BUTTON_DONE);
    return;
  }
  if (g_str_equal (result, "enroll-stage-passed")) {
    e->passed++;
    g_autofree char *hint = e->stages > 0 ? g_strdup_printf ("%d of %d • Shift your finger a little each time",
                                                             MIN (e->passed, e->stages), e->stages)
                                          : g_strdup ("Shift your finger a little each time");
    enroll_show (e, "Lift and press again", hint, BUTTON_CANCEL);
  } else if (g_str_equal (result, "enroll-retry-scan")) {
    enroll_show (e, "Press again", "That touch didn't give a good scan.", BUTTON_CANCEL);
  } else if (g_str_equal (result, "enroll-swipe-too-short")) {
    enroll_show (e, "Press again", "Hold your finger on the sensor a little longer.", BUTTON_CANCEL);
  } else if (g_str_equal (result, "enroll-finger-not-centered")) {
    enroll_show (e, "Press again", "Put your finger in the middle of the sensor.", BUTTON_CANCEL);
  } else if (g_str_equal (result, "enroll-remove-and-retry")) {
    enroll_show (e, "Lift your finger", "Then press again.", BUTTON_CANCEL);
  } else if (g_str_equal (result, "enroll-duplicate")) {
    enroll_fail (e, "Already added", "This finger is already enrolled, maybe as another finger.");
  } else if (g_str_equal (result, "enroll-data-full")) {
    enroll_fail (e, "No room left", "The sensor can't hold more fingerprints. Delete one first.");
  } else if (g_str_equal (result, "enroll-disconnected")) {
    enroll_fail (e, "Sensor disconnected", NULL);
  } else if (done) {
    /* enroll-failed, enroll-unknown-error: the driver gives up after two minutes without a touch */
    if (e->passed == 0) {
      g_autofree char *hint = sensor_text ("fingerprint-no-touch-hint");
      enroll_fail (e, "No touch detected", hint);
    }
    else
      enroll_fail (e, "That didn't work", "Start again.");
  }
  if (done && e->enrolling == FALSE && !g_str_equal (result, "enroll-completed"))
    gtk_widget_remove_css_class (e->icon, "ns-accent");
}

static void
started (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (cancelled (err))
    return;
  Enroll *e = data;
  if (!r) {
    e->enrolling = FALSE;
    g_autofree char *text = fp_error_text (err);
    enroll_fail (e, "Couldn't start", text);
    return;
  }
  g_autofree char *title = sensor_text ("fingerprint-press-title");
  g_autofree char *hint = sensor_text ("fingerprint-press-hint");
  enroll_show (e, title, hint, BUTTON_CANCEL);
}

static void
enroll_start (Enroll *e)
{
  e->passed = 0;
  e->enrolling = TRUE;
  enroll_show (e, "Getting ready", "", BUTTON_CANCEL);
  device_call (e->bus, e->device, "EnrollStart", g_variant_new ("(s)", e->finger), NULL, e->cancel, started, e);
}

static void
got_stages (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (cancelled (err))
    return;
  Enroll *e = data;
  if (r) {
    g_autoptr (GVariant) v = NULL;
    g_variant_get (r, "(v)", &v);
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT32))
      e->stages = g_variant_get_int32 (v);
  }
  e->sub = g_dbus_connection_signal_subscribe (e->bus, FP_NAME, FP_IDEVICE, "EnrollStatus", e->device, NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE, status, e, NULL);
  enroll_start (e);
}

static void
claimed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (cancelled (err))
    return;
  Enroll *e = data;
  if (!r) {
    g_autofree char *text = fp_error_text (err);
    enroll_show (e, "Couldn't use the sensor", text, BUTTON_DONE);
    return;
  }
  e->claimed = TRUE;
  g_dbus_connection_call (e->bus, FP_NAME, e->device, "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", FP_IDEVICE, "num-enroll-stages"), G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, e->cancel, got_stages, e);
}

static void
enroll_button (GtkButton *button, Enroll *e)
{
  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "ns-role")) == BUTTON_RETRY && e->claimed)
    enroll_start (e);
  else {
    /* only this button leaves the page: back gestures would cancel a scan by accident */
    adw_navigation_page_set_can_pop (e->page, TRUE);
    gtk_widget_activate_action (GTK_WIDGET (button), "navigation.pop", NULL);
  }
}

static void
enroll_page (GDBusConnection *bus, const char *device, const char *finger)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (finger_label (finger), &c);
  Enroll *e = g_new0 (Enroll, 1);
  e->bus = g_object_ref (bus);
  e->cancel = g_cancellable_new ();
  e->device = g_strdup (device);
  e->finger = g_strdup (finger);
  g_object_set_data_full (G_OBJECT (page), "ns-enroll", e, (GDestroyNotify) enroll_free);
  e->page = page;
  /* no back button, back swipe or Escape while scanning: the page's own button leaves it */
  adw_navigation_page_set_can_pop (page, FALSE);

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_halign (overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (overlay, 24);
  gtk_widget_set_margin_bottom (overlay, 24);
  e->ring = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (e->ring), 200);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (e->ring), 200);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (e->ring), ring_draw, e, NULL);
  gtk_widget_add_css_class (e->ring, "ns-accent");
  gtk_overlay_set_child (GTK_OVERLAY (overlay), e->ring);
  e->icon = gtk_image_new_from_icon_name ("auth-fingerprint-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (e->icon), 96);
  gtk_widget_set_halign (e->icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (e->icon, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), e->icon);
  gtk_box_append (GTK_BOX (c), overlay);

  e->title = gtk_label_new ("Getting ready");
  gtk_widget_add_css_class (e->title, "title-2");
  gtk_label_set_wrap (GTK_LABEL (e->title), TRUE);
  gtk_label_set_justify (GTK_LABEL (e->title), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (c), e->title);
  e->hint = gtk_label_new ("Allow the use of the sensor if the phone asks for your password.");
  gtk_widget_add_css_class (e->hint, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (e->hint), TRUE);
  gtk_label_set_justify (GTK_LABEL (e->hint), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (e->hint, 30);
  gtk_widget_set_margin_end (e->hint, 30);
  gtk_widget_set_margin_top (e->hint, 8);
  gtk_box_append (GTK_BOX (c), e->hint);
  e->button = gtk_button_new_with_label ("Cancel");
  gtk_widget_add_css_class (e->button, "pill");
  gtk_widget_set_halign (e->button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (e->button, 30);
  g_signal_connect (e->button, "clicked", G_CALLBACK (enroll_button), e);
  gtk_box_append (GTK_BOX (c), e->button);

  ns_push (page);
  device_call (bus, device, "Claim", g_variant_new ("(s)", ""), NULL, e->cancel, claimed, e);
}

/* ---- the Fingerprints page ---- */

typedef struct {
  GDBusConnection *bus;
  GCancellable    *cancel;
  char            *device;
  gboolean         claimed;        /* while deleting */
  GtkWidget       *list, *add, *note;
} Fp;

static void
fp_free (Fp *fp)
{
  g_cancellable_cancel (fp->cancel);
  if (fp->claimed)
    device_forget (fp->bus, fp->device, FALSE);
  g_object_unref (fp->cancel);
  g_clear_object (&fp->bus);
  g_free (fp->device);
  g_free (fp);
}

static void fp_fill (Fp *fp);

typedef struct {
  Fp   *fp;
  char *finger;
} Del;

static void
del_free (Del *d)
{
  g_free (d->finger);
  g_free (d);
}

static void
del_released (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, NULL);
  (void) r;
}

static void
deleted (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  Del *d = data;
  if (cancelled (err)) {
    del_free (d);
    return;
  }
  Fp *fp = d->fp;
  device_call (fp->bus, fp->device, "Release", NULL, NULL, NULL, del_released, NULL);
  fp->claimed = FALSE;
  if (!r) {
    g_autofree char *text = fp_error_text (err);
    gtk_label_set_text (GTK_LABEL (fp->note), text);
  }
  del_free (d);
  fp_fill (fp);
}

static void
del_claimed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  Del *d = data;
  if (cancelled (err)) {
    del_free (d);
    return;
  }
  Fp *fp = d->fp;
  if (!r) {
    g_autofree char *text = fp_error_text (err);
    gtk_label_set_text (GTK_LABEL (fp->note), text);
    del_free (d);
    return;
  }
  fp->claimed = TRUE;
  device_call (fp->bus, fp->device, "DeleteEnrolledFinger", g_variant_new ("(s)", d->finger), NULL, fp->cancel,
               deleted, d);
}

static void
del_answered (gboolean yes, gpointer data)
{
  Del *d = data;
  if (!yes || d->fp->claimed) {
    del_free (d);
    return;
  }
  Fp *fp = d->fp;
  device_call (fp->bus, fp->device, "Claim", g_variant_new ("(s)", ""), NULL, fp->cancel, del_claimed, d);
}

static void
finger_tapped (GtkWidget *row, gpointer data)
{
  Fp *fp = data;
  Del *d = g_new0 (Del, 1);
  d->fp = fp;
  d->finger = g_strdup (g_object_get_data (G_OBJECT (row), "ns-finger"));
  g_autofree char *heading = g_strdup_printf ("Delete %s?", finger_label (d->finger));
  /* the dialog keeps Fp alive no longer than the page: a page that went away cancels the calls that follow */
  ns_confirm (row, heading, "It will no longer be recognised.", "Delete", TRUE, del_answered, d);
}

/* a finger is picked: the enroll page takes the picker's place (the picker sits above the Fingerprints page, so
 * Fp is alive) */
static void
finger_picked (GtkWidget *row, gpointer data)
{
  Fp *fp = data;
  guint i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "ns-index"));
  g_autoptr (GDBusConnection) bus = g_object_ref (fp->bus);
  g_autofree char *device = g_strdup (fp->device);
  gtk_widget_activate_action (row, "navigation.pop", NULL);
  enroll_page (bus, device, fingers[i].id);
}

static void
add_tapped (GtkWidget *row, gpointer data)
{
  Fp *fp = data;
  if (!fp->bus || !fp->device)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Which finger?", &c);
  GtkWidget *g = ns_group (c, NULL);
  GStrv have = g_object_get_data (G_OBJECT (fp->list), "ns-enrolled");
  for (guint i = 0; i < G_N_ELEMENTS (fingers); i++) {
    gboolean enrolled = have && g_strv_contains ((const char *const *) have, fingers[i].id);
    GtkWidget *r = ns_row (g, "auth-fingerprint-symbolic", enrolled ? "grey" : "blue", fingers[i].label,
                           enrolled ? "Added • Adding it again replaces it" : NULL);
    g_object_set_data (G_OBJECT (r), "ns-index", GUINT_TO_POINTER (i));
    ns_row_on_activate (r, finger_picked, fp);
  }
  ns_push (page);
}

static void
listed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (cancelled (err))
    return;
  Fp *fp = data;
  g_auto (GStrv) names = NULL;
  if (r)
    g_variant_get (r, "(^as)", &names);
  else if (!fp_error_is (err, "NoEnrolledPrints")) {
    g_autofree char *text = fp_error_text (err);
    gtk_label_set_text (GTK_LABEL (fp->note), text);
  }
  ns_group_clear (fp->list);
  guint n = names ? g_strv_length (names) : 0;
  for (guint i = 0; i < n; i++) {
    GtkWidget *row = ns_row (fp->list, "auth-fingerprint-symbolic", "blue", finger_label (names[i]), "Tap to delete");
    g_object_set_data_full (G_OBJECT (row), "ns-finger", g_strdup (names[i]), g_free);
    ns_row_on_activate (row, finger_tapped, fp);
  }
  ns_group_set_visible (fp->list, n > 0);
  gtk_widget_set_visible (fp->add, TRUE);
  g_object_set_data_full (G_OBJECT (fp->list), "ns-enrolled", g_steal_pointer (&names), (GDestroyNotify) g_strfreev);
}

static void
fp_fill (Fp *fp)
{
  if (!fp->device)
    return;
  g_dbus_connection_call (fp->bus, FP_NAME, fp->device, FP_IDEVICE, "ListEnrolledFingers", g_variant_new ("(s)", ""),
                          G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, fp->cancel, listed, fp);
}

static void
got_device (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (cancelled (err))
    return;
  Fp *fp = data;
  if (!r) {
    gtk_label_set_text (GTK_LABEL (fp->note), "No fingerprint sensor was found.");
    return;
  }
  g_variant_get (r, "(o)", &fp->device);
  fp_fill (fp);
}

static void
got_bus (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  GDBusConnection *bus = g_bus_get_finish (res, &err);
  if (cancelled (err))
    return;
  Fp *fp = data;
  if (!bus) {
    gtk_label_set_text (GTK_LABEL (fp->note), err->message);
    return;
  }
  fp->bus = bus;
  g_dbus_connection_call (bus, FP_NAME, FP_MANAGER, FP_IMANAGER, "GetDefaultDevice", NULL, G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, fp->cancel, got_device, fp);
}

/* back from the enroll page: the list again */
static void
fp_showing (AdwNavigationPage *page, Fp *fp)
{
  fp_fill (fp);
}

void
ns_page_fingerprint (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Fingerprints", &c);
  Fp *fp = g_new0 (Fp, 1);
  fp->cancel = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (page), "ns-fp", fp, (GDestroyNotify) fp_free);

  fp->list = ns_group (c, "Your fingerprints");
  ns_group_set_visible (fp->list, FALSE);
  GtkWidget *g = ns_group (c, NULL);
  fp->add = ns_row (g, "list-add-symbolic", "blue", "Add a fingerprint", NULL);
  ns_row_on_activate (fp->add, add_tapped, fp);
  gtk_widget_set_visible (fp->add, FALSE);
  fp->note = ns_note (c, "Fingerprints are scanned and matched by the sensor's secure firmware (TrustZone) and "
                         "stored encrypted. Settings only sees which fingers are enrolled.");

  g_signal_connect (page, "showing", G_CALLBACK (fp_showing), fp);
  ns_push (page);
  g_bus_get (G_BUS_TYPE_SYSTEM, fp->cancel, got_bus, fp);
}

/* ---- the row on the Lock screen page ---- */

typedef struct {
  GWeakRef         group;
  GDBusConnection *bus;
  char            *device;
} Check;

static void
check_free (Check *k)
{
  g_weak_ref_clear (&k->group);
  g_clear_object (&k->bus);
  g_free (k->device);
  g_free (k);
}

static void
check_listed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  Check *k = data;
  g_autoptr (GtkWidget) group = g_weak_ref_get (&k->group);
  if (group) {
    g_auto (GStrv) names = NULL;
    if (r)
      g_variant_get (r, "(^as)", &names);
    guint n = names ? g_strv_length (names) : 0;
    g_autofree char *sub = n == 0 ? g_strdup ("None added")
                           : n == 1 ? g_strdup_printf ("%s", finger_label (names[0]))
                                    : g_strdup_printf ("%u fingerprints", n);
    ns_row_set_subtitle (g_object_get_data (G_OBJECT (group), "ns-fp-row"), sub);
    ns_group_set_visible (group, TRUE);
  }
  check_free (k);
}

static void
check_device (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  Check *k = data;
  g_autoptr (GtkWidget) group = g_weak_ref_get (&k->group);
  if (!r || !group) {
    check_free (k);
    return;
  }
  g_variant_get (r, "(o)", &k->device);
  g_dbus_connection_call (k->bus, FP_NAME, k->device, FP_IDEVICE, "ListEnrolledFingers", g_variant_new ("(s)", ""),
                          G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, check_listed, k);
}

static void
check_bus (GObject *source, GAsyncResult *res, gpointer data)
{
  Check *k = data;
  k->bus = g_bus_get_finish (res, NULL);
  if (!k->bus) {
    check_free (k);
    return;
  }
  /* fprintd is bus activated: asking for the device starts it */
  g_dbus_connection_call (k->bus, FP_NAME, FP_MANAGER, FP_IMANAGER, "GetDefaultDevice", NULL, G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, check_device, k);
}

static void
lock_showing (AdwNavigationPage *page, GtkWidget *group)
{
  Check *k = g_new0 (Check, 1);
  g_weak_ref_init (&k->group, group);
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, check_bus, k);
}

void
ns_fprint_lock_group (AdwNavigationPage *page, GtkWidget *content)
{
  GtkWidget *g = ns_group (content, "Biometrics");
  GtkWidget *row = ns_row (g, "auth-fingerprint-symbolic", "blue", "Fingerprints", NULL);
  ns_row_on_activate (row, ns_page_fingerprint, NULL);
  g_object_set_data (G_OBJECT (g), "ns-fp-row", row);
  ns_group_set_visible (g, FALSE);
  g_signal_connect (page, "showing", G_CALLBACK (lock_showing), g);
}
