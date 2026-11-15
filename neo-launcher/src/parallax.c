/* The parallax wallpaper (see parallax.h). The filter is the GTK 3 launcher's, tuned there on this phone. The
 * accelerometer is IIO's, or on the POCO X3 NFC the ADSP sensor hub's, through libssc. */
#include "parallax.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "chisel.h"
#include "gfx.h"
#include "image.h"

#define ACCEL_DIR "/sys/bus/iio/devices"
/* Per frame at 60 Hz: time constants of ≈ 0.25 s and ≈ 0.15 s, the picture glides after the phone (owner: the
 * faster ones were "too responsive, it was smoother") */
#define SMOOTH 0.065        /* the low-pass on the sensor, against its noise */
#define EASE 0.11           /* how quickly the picture follows that */
#define RATE_RUN 60         /* Hz asked of the accelerometer while it moves, and back to RATE_IDLE */
#define RATE_IDLE 10
#define REST 0.004          /* per frame: how quickly the rest position follows the way the phone is held */
#define IDLE_MS 100         /* sensor polling while nothing moves */
/* on a table the filtered reading strays at most 0.0035 g: closer than REST_G to where it settled for REST_TICKS
 * readings, the phone lies still and the picture follows that settled reading instead */
#define REST_G 0.006
#define REST_TICKS 60
#define STILL 30            /* frames without a move: at rest, the idle poll takes over */
/* the chisel light follows the tilt, and drifts by itself once the phone has been still this long (the mockup's 2.5 s);
 * the drift is redrawn from a timer at this rate, only while the parallax runs (see chisel.h) */
#define LIGHT_FOLLOW_US 2500000
#define LIGHT_HZ 6

typedef struct {
  char   *file;
  double  depth;
  GfxTex *tex;
  float   margin;           /* px of the layer beyond each screen edge */
} Layer;

struct Parallax {
  char       *dir;
  GPtrArray  *layers;
  double      range;        /* dp a depth-1 layer moves either way */
  double      tilt;         /* the tilt (of g) that moves it that far */
  int         w, h;
  double      scale;
  bool        running;
  char       *accel[3];
  char       *rate;
  bool        use_ssc;      /* no IIO accelerometer: the sensor hub's, through libssc */
  GObject    *ssc;          /* its sensor, while the parallax runs */
  guint       ssc_gen;      /* a creation or opening for an older one is dropped */
  guint       ssc_retry;
  int         ssc_tries;
  bool        ssc_have;
  bool        ssc_opened;
  gint64      ssc_last;     /* monotonic µs of the last measurement (or of the opening) */
  double      ssc_g[3];     /* its last measurement, m/s² */
  double      g[3], rest[3], settled[3];
  int         calm;
  bool        have_g;
  double      ox, oy, tx, ty;
  int         still;
  guint       idle;
  Chisel     *chisel;       /* the marks drawn over the layers, if the set has them */
  guint       chisel_timer;
  gint64      last_move;    /* monotonic µs of the last tilt */
  void      (*wake) (void *data);
  void       *wake_data;
};

/* ---- the POCO X3 NFC's accelerometer: the ADSP sensor hub's (SSC), reached through libssc, not IIO ----
 * libssc is loaded at run time (only that phone has it). A sensor lives only while the parallax runs: the hub's
 * protection domain restarts at every screen-on there (surya-sleep-resume), and a sensor opened before that talks
 * to nothing. It measures ~23 Hz in m/s² on Android's axes; step() uses only the direction. */
typedef void (*SscNew) (GCancellable *, GAsyncReadyCallback, gpointer);
typedef GObject *(*SscNewFinish) (GAsyncResult *, GError **);
typedef void (*SscOpen) (GObject *, GCancellable *, GAsyncReadyCallback, gpointer);
typedef gboolean (*SscOpenFinish) (GObject *, GAsyncResult *, GError **);
static struct {
  bool          tried, ok;
  SscNew        create;
  SscNewFinish  create_finish;
  SscOpen       open, close;
  SscOpenFinish open_finish;
} ssc;
static Parallax *ssc_owner;  /* the running parallax that has the sensor */
static guint ssc_serial;     /* every start and stop takes a new one: a callback for an older one is stale */

#define SSC_TRIES 20         /* one a second: the hub's domain takes a few seconds to come back after a screen-on */
#define SSC_QUIET_US 3000000 /* no measurement for this long while open: the hub restarted under it, open anew */

static bool
ssc_load (void)
{
  if (ssc.tried)
    return ssc.ok;
  ssc.tried = true;
  void *h = dlopen ("libssc.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return false;
  ssc.create = (SscNew) dlsym (h, "ssc_sensor_accelerometer_new");
  ssc.create_finish = (SscNewFinish) dlsym (h, "ssc_sensor_accelerometer_new_finish");
  ssc.open = (SscOpen) dlsym (h, "ssc_sensor_accelerometer_open");
  ssc.open_finish = (SscOpenFinish) dlsym (h, "ssc_sensor_accelerometer_open_finish");
  ssc.close = (SscOpen) dlsym (h, "ssc_sensor_accelerometer_close");
  ssc.ok = ssc.create && ssc.create_finish && ssc.open && ssc.open_finish && ssc.close;
  return ssc.ok;
}

static void ssc_start (Parallax *p);

static gboolean
ssc_retry (gpointer data)
{
  Parallax *p = data;
  p->ssc_retry = 0;
  ssc_start (p);
  return G_SOURCE_REMOVE;
}

static void
ssc_drop (Parallax *p)
{
  if (p->ssc) {
    g_signal_handlers_disconnect_by_data (p->ssc, p);
    ssc.close (p->ssc, NULL, NULL, NULL);     /* its task keeps it until the hub answers */
    g_clear_object (&p->ssc);
  }
  p->ssc_have = false;
  p->ssc_opened = false;
}

static void
ssc_failed (Parallax *p, const char *what, GError *err)
{
  ssc_drop (p);
  if (++p->ssc_tries == 1 || p->ssc_tries == SSC_TRIES)
    g_message ("parallax: the sensor hub's accelerometer: %s%s%s%s", what, err ? ": " : "", err ? err->message : "",
               p->ssc_tries == SSC_TRIES ? " (giving up until the next screen-on)" : ", trying again");
  if (p->ssc_tries < SSC_TRIES && p->running)
    p->ssc_retry = g_timeout_add_seconds (1, ssc_retry, p);
}

static void
ssc_measured (GObject *sensor, gfloat x, gfloat y, gfloat z, gpointer data)
{
  Parallax *p = data;
  p->ssc_g[0] = x;
  p->ssc_g[1] = y;
  p->ssc_g[2] = z;
  p->ssc_have = true;
  p->ssc_last = g_get_monotonic_time ();
  if (p->ssc_tries) {       /* it works: a later failure gets its retries again */
    g_message ("parallax: the sensor hub's accelerometer measures");
    p->ssc_tries = 0;
  }
}

/* the parallax a callback was for, if it still wants that sensor */
static Parallax *
ssc_current (gpointer gen)
{
  return ssc_owner && ssc_owner->ssc_gen == GPOINTER_TO_UINT (gen) ? ssc_owner : NULL;
}

static void
ssc_opened (GObject *src, GAsyncResult *res, gpointer gen)
{
  g_autoptr (GError) err = NULL;
  gboolean ok = ssc.open_finish (src, res, &err);
  Parallax *p = ssc_current (gen);
  if (!p)
    return;                 /* stopped meanwhile: ssc_drop closed it */
  if (!ok) {
    ssc_failed (p, "open", err);
    return;
  }
  p->ssc_opened = true;
  p->ssc_last = g_get_monotonic_time ();
}

static void
ssc_created (GObject *src, GAsyncResult *res, gpointer gen)
{
  g_autoptr (GError) err = NULL;
  GObject *sensor = ssc.create_finish (res, &err);
  Parallax *p = ssc_current (gen);
  if (!p) {
    g_clear_object (&sensor);
    return;
  }
  if (!sensor) {
    ssc_failed (p, "connect", err);
    return;
  }
  p->ssc = sensor;
  g_signal_connect (sensor, "measurement", G_CALLBACK (ssc_measured), p);
  ssc.open (sensor, NULL, ssc_opened, gen);
}

static void
ssc_start (Parallax *p)
{
  if (!p->use_ssc || p->ssc || !p->running)
    return;
  if (ssc_owner && ssc_owner != p)
    return;                 /* one sensor: the other parallax has it */
  ssc_owner = p;
  p->ssc_gen = ++ssc_serial;
  ssc.create (NULL, ssc_created, GUINT_TO_POINTER (p->ssc_gen));
}

static void
ssc_stop (Parallax *p)
{
  p->ssc_gen = ++ssc_serial;
  if (p->ssc_retry) {
    g_source_remove (p->ssc_retry);
    p->ssc_retry = 0;
  }
  p->ssc_tries = 0;
  ssc_drop (p);
  if (ssc_owner == p)
    ssc_owner = NULL;
}

static bool
has_sensor (Parallax *p)
{
  return p->accel[0] || p->use_ssc;
}

static void
find_accel (Parallax *p)
{
  g_autoptr (GDir) dir = g_dir_open (ACCEL_DIR, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    g_autofree char *x = g_build_filename (ACCEL_DIR, name, "in_accel_x_raw", NULL);
    if (!g_file_test (x, G_FILE_TEST_EXISTS))
      continue;
    const char *axes[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
      g_autofree char *f = g_strdup_printf ("in_accel_%s_raw", axes[i]);
      p->accel[i] = g_build_filename (ACCEL_DIR, name, f, NULL);
    }
    p->rate = g_build_filename (ACCEL_DIR, name, "in_accel_sampling_frequency", NULL);
    return;
  }
  if (ssc_load ()) {
    p->use_ssc = true;
    g_message ("parallax: the sensor hub's accelerometer (libssc)");
    return;
  }
  g_message ("parallax: no accelerometer, the wallpaper stays still");
}

static void
set_rate (Parallax *p, int hz)
{
  char v[16];
  int fd = p->rate ? open (p->rate, O_WRONLY) : -1;
  if (fd < 0)
    return;
  int n = g_snprintf (v, sizeof v, "%d", hz);
  if (write (fd, v, n) != n)
    g_debug ("parallax: the accelerometer rate stays");
  close (fd);
}

static bool
read_accel (Parallax *p, double g[3])
{
  if (p->use_ssc) {
    if (p->ssc_opened && !p->ssc_retry && g_get_monotonic_time () - p->ssc_last > SSC_QUIET_US) {
      ssc_failed (p, "no measurements", NULL);
      return false;
    }
    if (!p->ssc_have)
      return false;
    memcpy (g, p->ssc_g, sizeof p->ssc_g);
    return true;
  }
  for (int i = 0; i < 3; i++) {
    char buf[32];
    if (!p->accel[i])
      return false;
    int fd = open (p->accel[i], O_RDONLY);
    if (fd < 0)
      return false;
    ssize_t n = read (fd, buf, sizeof buf - 1);
    close (fd);
    if (n <= 0)
      return false;
    buf[n] = '\0';
    g[i] = g_ascii_strtod (buf, NULL);
  }
  return true;
}

/* one reading: true when the picture moved by more than a hair */
static bool
step (Parallax *p)
{
  double g[3];
  if (!read_accel (p, g))
    return false;
  double n = sqrt (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (n <= 0)
    return false;
  for (int i = 0; i < 3; i++)
    g[i] /= n;
  if (!p->have_g) {
    memcpy (p->g, g, sizeof g);
    memcpy (p->rest, g, sizeof g);
    memcpy (p->settled, g, sizeof g);
    p->calm = 0;
    p->have_g = true;
  }
  double off = 0;
  for (int i = 0; i < 3; i++) {
    p->g[i] += (g[i] - p->g[i]) * SMOOTH;
    off = MAX (off, fabs (p->g[i] - p->settled[i]));
  }
  if (off > REST_G) {
    memcpy (p->settled, p->g, sizeof p->g);
    p->calm = 0;
  } else if (p->calm < REST_TICKS) {
    p->calm++;
  }
  const double *in = p->calm >= REST_TICKS ? p->settled : p->g;
  for (int i = 0; i < 3; i++)
    p->rest[i] += (in[i] - p->rest[i]) * REST;
  /* x: the right edge dipping; y: the top edge coming towards you */
  p->tx = CLAMP ((in[0] - p->rest[0]) / p->tilt, -1, 1);
  p->ty = CLAMP ((in[1] - p->rest[1]) / p->tilt, -1, 1);
  double ox = p->ox + (p->tx - p->ox) * EASE;
  double oy = p->oy + (p->ty - p->oy) * EASE;
  double moved = (fabs (ox - p->ox) + fabs (oy - p->oy)) * p->range * p->scale;
  if (moved < 0.02)
    return false;
  p->ox = ox;
  p->oy = oy;
  p->last_move = g_get_monotonic_time ();
  return true;
}

static gboolean
idle_poll (gpointer data)
{
  Parallax *p = data;
  if (!p->running) {
    p->idle = 0;
    return G_SOURCE_REMOVE;
  }
  if (step (p)) {
    set_rate (p, RATE_RUN);
    p->still = 0;
    p->idle = 0;
    if (p->wake)
      p->wake (p->wake_data);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

bool
parallax_tick (Parallax *p)
{
  if (!p || !p->running)
    return false;
  /* the light: where the tilt puts it, or drifting; a frame more while it still eases */
  bool light = false;
  if (p->chisel) {
    gint64 now = g_get_monotonic_time ();
    light = chisel_update (p->chisel, now, p->ox, p->oy, now - p->last_move < LIGHT_FOLLOW_US);
  }
  if (p->still >= STILL)
    return light;           /* the idle poll has it */
  if (step (p)) {
    p->still = 0;
    return true;
  }
  if (++p->still == STILL) {
    set_rate (p, RATE_IDLE);
    if (!p->idle && has_sensor (p))
      p->idle = g_timeout_add (IDLE_MS, idle_poll, p);
  }
  return true;              /* a few more frames before it counts as still */
}

static void
layer_free (gpointer data)
{
  Layer *l = data;
  gfx_tex_free (l->tex);
  g_free (l->file);
  g_free (l);
}

Parallax *
parallax_new (const char *dir, void (*wake) (void *data), void *data)
{
  g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, json, NULL))
    return NULL;
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  JsonArray *ls = o && json_object_has_member (o, "layers") ? json_object_get_array_member (o, "layers") : NULL;
  if (!ls)
    return NULL;
  Parallax *p = g_new0 (Parallax, 1);
  p->dir = g_strdup (dir);
  p->wake = wake;
  p->wake_data = data;
  p->layers = g_ptr_array_new_with_free_func (layer_free);
  p->range = json_object_has_member (o, "range") ? json_object_get_double_member (o, "range") : 30;
  p->tilt = json_object_has_member (o, "tilt") ? json_object_get_double_member (o, "tilt") : 0.2;
  for (guint i = 0; i < json_array_get_length (ls); i++) {
    JsonObject *lo = json_array_get_object_element (ls, i);
    Layer *l = g_new0 (Layer, 1);
    l->file = g_build_filename (dir, json_object_get_string_member (lo, "file"), NULL);
    l->depth = json_object_has_member (lo, "depth") ? json_object_get_double_member (lo, "depth") : 0;
    g_ptr_array_add (p->layers, l);
  }
  if (json_object_has_member (o, "chisel"))
    p->chisel = chisel_new (json_object_get_object_member (o, "chisel"), dir);
  p->last_move = G_MININT64 / 2;       /* not tilted yet: the light drifts from the start */
  find_accel (p);
  return p;
}

Parallax *
parallax_new_image (const char *file)
{
  Parallax *p = g_new0 (Parallax, 1);
  p->dir = g_path_get_dirname (file);
  p->layers = g_ptr_array_new_with_free_func (layer_free);
  p->tilt = 0.2;
  Layer *l = g_new0 (Layer, 1);
  l->file = g_strdup (file);
  g_ptr_array_add (p->layers, l);
  return p;
}

void
parallax_free (Parallax *p)
{
  if (!p)
    return;
  if (p->idle)
    g_source_remove (p->idle);
  if (p->chisel_timer)
    g_source_remove (p->chisel_timer);
  chisel_free (p->chisel);
  ssc_stop (p);
  set_rate (p, RATE_IDLE);
  g_ptr_array_unref (p->layers);
  for (int i = 0; i < 3; i++)
    g_free (p->accel[i]);
  g_free (p->rate);
  g_free (p->dir);
  g_free (p);
}

void
parallax_resize (Parallax *p, int w, int h, double scale)
{
  if (p->w == w && p->h == h && p->scale == scale)
    return;
  p->w = w;
  p->h = h;
  p->scale = scale;
  gint64 t0 = g_get_monotonic_time ();
  for (guint i = 0; i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    l->margin = ceilf ((float) (p->range * fabs (l->depth) * scale));
    int bw = w + 2 * (int) l->margin, bh = h + 2 * (int) l->margin;
    GdkPixbuf *pb = image_load (l->file, bw, bh, TRUE);
    gfx_tex_free (l->tex);
    l->tex = pb ? image_texture (pb) : NULL;
    g_clear_object (&pb);
  }
  if (p->chisel)
    chisel_load (p->chisel, w, h);
  g_message ("parallax: %u layers%s for %dx%d in %.0f ms", p->layers->len, p->chisel ? " and chisel" : "", w, h,
             (g_get_monotonic_time () - t0) / 1000.0);
}

static gboolean
chisel_timer (gpointer data)
{
  Parallax *p = data;
  if (!p->running) {
    p->chisel_timer = 0;
    return G_SOURCE_REMOVE;
  }
  gint64 now = g_get_monotonic_time ();
  if (chisel_update (p->chisel, now, p->ox, p->oy, now - p->last_move < LIGHT_FOLLOW_US) && p->wake)
    p->wake (p->wake_data);
  return G_SOURCE_CONTINUE;
}

void
parallax_run (Parallax *p, bool run)
{
  if (!p || p->running == run || (!has_sensor (p) && !p->chisel))
    return;                 /* a still picture has no sensor to follow, and no light to move */
  p->running = run;
  p->still = 0;
  set_rate (p, run ? RATE_RUN : RATE_IDLE);
  if (run)
    ssc_start (p);
  else
    ssc_stop (p);
  if (run && p->chisel && !p->chisel_timer)
    p->chisel_timer = g_timeout_add (1000 / LIGHT_HZ, chisel_timer, p);
  if (!run && p->chisel_timer) {
    g_source_remove (p->chisel_timer);
    p->chisel_timer = 0;
  }
  if (!run && p->idle) {
    g_source_remove (p->idle);
    p->idle = 0;
  }
}

void
parallax_draw (Parallax *p, float zoom)
{
  gfx_save ();
  if (zoom != 1)
    gfx_scale (zoom, zoom, p->w / 2.f, p->h / 2.f);
  for (guint i = 0; i < p->layers->len; i++) {
    Layer *l = p->layers->pdata[i];
    if (!l->tex)
      continue;
    /* depth -1…1: the layers behind the screen plane (negative) move against the ones in front */
    float dx = (float) (-p->ox * p->range * l->depth * p->scale);
    float dy = (float) (-p->oy * p->range * l->depth * p->scale);
    gfx_image (l->tex, -(l->margin + dx), -(l->margin + dy), l->tex->w, l->tex->h, 0, 1);
  }
  if (p->chisel)
    chisel_draw (p->chisel);
  gfx_restore ();
}
