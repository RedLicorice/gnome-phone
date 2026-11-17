/* cam-focus: see cam-focus.h. */
#include "cam-focus.h"
#include "cam-lens.h"

#include <math.h>
#include <string.h>

typedef enum { PHASE_NONE, PHASE_SEARCH, PHASE_VERIFY, PHASE_WATCH } Phase;   /* what the next measured score is for */

struct _CamFocus {
  GObject        parent;
  GSettings     *settings;
  CamAfConfig    cfg;
  GThreadPool   *lens_pool;      /* the lens' own thread: open, move, close, in order */
  CamLens       *lens;           /* the pool's thread only */

  /* the main thread's */
  char          *camera_id;
  gint           gen;            /* +1 per camera: notes of another one are dropped (atomic: the probe reads it) */
  gboolean       lens_open, streaming, started, hold;
  int            lens_min, lens_max;     /* what the lens reports */
  int            lo, hi;         /* the sweep: the config's, inside that */
  CamFocusMode   mode;
  CamFocusState  state;
  CamAfReason    reason;
  CamAfSearch   *search;
  CamAfResult    result;
  double         cx, cy;         /* the region's centre: fractions of the frame */
  double         manual;         /* MANUAL's position, 0 … 1 */
  int            pos;            /* where the lens was last sent */
  int            inflight;       /* lens jobs not answered yet */
  guint          move_serial;
  Phase          moved_phase;
  gboolean       want_valid;     /* MANUAL: a later position waits for the one in flight */
  int            want_pos;
  double         ref_score;      /* what the sharpness was when it last focused */
  int            bad_looks;
  gint64         last_search_end;
  guint          search_timer, start_timer, watch_timer;
  GHashTable    *last_good;      /* camera id -> position + 1 of the last search that had a clear peak */

  /* what the streaming thread reads (lock) */
  GMutex         lock;
  gboolean       armed;
  guint          arm_serial;
  Phase          arm_phase;
  gint64         not_before;
  int            skip;
  double         arm_roi, arm_luma, arm_cx, arm_cy;
};

G_DEFINE_FINAL_TYPE (CamFocus, cam_focus, G_TYPE_OBJECT)

enum { SIG_STATE, N_SIGNALS };
enum { PROP_0, PROP_AVAILABLE, N_PROPS };
static guint signals[N_SIGNALS];
static GParamSpec *props[N_PROPS];

/* ---- the lens' thread ------------------------------------------------------------------------------------------ */

typedef enum { JOB_OPEN, JOB_SET, JOB_CLOSE } JobKind;
enum { NOTE_OPENED, NOTE_MOVED, NOTE_SCORE };

typedef struct {
  JobKind  kind;
  guint    serial;
  gint     gen;
  char    *camera_id, *dt_root, *subdev_class, *dev_dir;
  int      position;
} Job;

typedef struct {
  CamFocus *self;
  int       kind;
  guint     serial;
  gint      gen;
  gboolean  ok;
  int       a, b;
  double    score;
  char     *text;
} Note;

static gboolean on_note (gpointer data);

static void
post (CamFocus *self, Note note)
{
  Note *copy = g_memdup2 (&note, sizeof note);

  copy->self = g_object_ref (self);
  g_main_context_invoke (NULL, on_note, copy);
}

static void
job_free (Job *job)
{
  g_free (job->camera_id);
  g_free (job->dt_root);
  g_free (job->subdev_class);
  g_free (job->dev_dir);
  g_free (job);
}

static void
lens_run (gpointer data, gpointer user_data)
{
  Job *job = data;
  CamFocus *self = user_data;
  g_autoptr(GError) error = NULL;

  switch (job->kind)
    {
    case JOB_OPEN:
      {
        CamAfConfig paths = { .dt_root = job->dt_root, .subdev_class = job->subdev_class, .dev_dir = job->dev_dir };
        Note note = { .kind = NOTE_OPENED, .gen = job->gen };

        g_clear_pointer (&self->lens, cam_lens_free);
        self->lens = cam_lens_open (&paths, job->camera_id, &error);
        note.ok = self->lens != NULL;
        if (self->lens)
          cam_lens_range (self->lens, &note.a, &note.b);
        else
          note.text = g_strdup (error->message);
        post (self, note);
        break;
      }
    case JOB_SET:
      {
        Note note = { .kind = NOTE_MOVED, .gen = job->gen, .serial = job->serial };

        note.ok = self->lens && cam_lens_set (self->lens, job->position, &error);
        if (!note.ok && error)
          note.text = g_strdup (error->message);
        post (self, note);
        break;
      }
    case JOB_CLOSE:
      g_clear_pointer (&self->lens, cam_lens_free);
      break;
    }
  job_free (job);
}

static void
push (CamFocus *self, Job *job)
{
  job->gen = g_atomic_int_get (&self->gen);
  g_thread_pool_push (self->lens_pool, job, NULL);
}

/* ---- state ----------------------------------------------------------------------------------------------------- */

static void
set_state (CamFocus *self, CamFocusState state, CamAfReason reason)
{
  self->state = state;
  self->reason = reason;
  g_signal_emit (self, signals[SIG_STATE], 0, (int) state, (int) reason);
}

static void
reload_config (CamFocus *self)
{
  CamAfConfig next;
  g_autoptr(GError) error = NULL;

  if (!cam_af_config_load (&next, self->settings, &error))
    {
      g_warning ("autofocus: %s (the last good numbers are kept)", error->message);
      return;
    }
  cam_af_config_clear (&self->cfg);
  self->cfg = next;
  if (self->lens_open)
    {
      self->lo = MAX (self->cfg.pos_min, self->lens_min);
      self->hi = MIN (self->cfg.pos_max, self->lens_max);
    }
}

static void
arm (CamFocus *self, Phase phase, int settle_ms, int skip)
{
  g_mutex_lock (&self->lock);
  self->armed = TRUE;
  self->arm_serial++;
  self->arm_phase = phase;
  self->not_before = g_get_monotonic_time () + (gint64) settle_ms * 1000;
  self->skip = skip;
  self->arm_roi = self->cfg.roi;
  self->arm_luma = self->cfg.min_luma;
  self->arm_cx = self->cx;
  self->arm_cy = self->cy;
  g_mutex_unlock (&self->lock);
}

static void
disarm (CamFocus *self)
{
  g_mutex_lock (&self->lock);
  self->armed = FALSE;
  g_mutex_unlock (&self->lock);
}

static void
move (CamFocus *self, int position, Phase after)
{
  Job *job = g_new0 (Job, 1);

  g_debug ("autofocus: lens to %d (clamped to %d … %d)", position, self->lo, self->hi);
  self->pos = CLAMP (position, self->lo, self->hi);
  self->moved_phase = after;
  self->inflight++;
  job->kind = JOB_SET;
  job->position = self->pos;
  job->serial = ++self->move_serial;
  push (self, job);
}

static void
stop_timers (CamFocus *self)
{
  g_clear_handle_id (&self->search_timer, g_source_remove);
}

static void search_finish (CamFocus *self, gboolean timed_out);

static void
search_step (CamFocus *self)
{
  int position;

  if (cam_af_search_next (self->search, &position))
    move (self, position, PHASE_SEARCH);
  else
    search_finish (self, FALSE);
}

static gboolean
on_search_timeout (gpointer data)
{
  CamFocus *self = data;

  self->search_timer = 0;
  if (self->search)
    search_finish (self, TRUE);
  else if (self->state == CAM_FOCUS_SEARCHING)
    /* the lens stands where it should; the frame that tells how sharp it is never came */
    set_state (self, self->result.ok ? CAM_FOCUS_FOCUSED : CAM_FOCUS_FAILED, self->result.reason);
  return G_SOURCE_REMOVE;
}

static void
search_cancel (CamFocus *self)
{
  g_clear_pointer (&self->search, cam_af_search_free);
  stop_timers (self);
  disarm (self);
}

static void
search_start (CamFocus *self, double cx, double cy)
{
  if (!self->lens_open || !self->streaming)
    return;
  search_cancel (self);
  reload_config (self);
  self->cx = CLAMP (cx, 0, 1);
  self->cy = CLAMP (cy, 0, 1);
  self->search = cam_af_search_new (&self->cfg, self->lo, self->hi);
  self->bad_looks = 0;
  self->moved_phase = PHASE_NONE;
  set_state (self, CAM_FOCUS_SEARCHING, CAM_AF_REASON_NONE);
  self->search_timer = g_timeout_add (self->cfg.timeout_ms, on_search_timeout, self);
  search_step (self);
}

static void
search_finish (CamFocus *self, gboolean timed_out)
{
  CamAfResult r = { 0 };
  gpointer remembered = self->camera_id ? g_hash_table_lookup (self->last_good, self->camera_id) : NULL;
  int final;

  if (timed_out)
    {
      r.ok = FALSE;
      r.reason = CAM_AF_REASON_TIMEOUT;
    }
  else
    cam_af_search_result (self->search, &r);
  g_clear_pointer (&self->search, cam_af_search_free);
  stop_timers (self);
  disarm (self);
  self->last_search_end = g_get_monotonic_time ();
  if (r.ok)
    {
      final = r.position;
      if (self->camera_id)
        g_hash_table_insert (self->last_good, g_strdup (self->camera_id), GINT_TO_POINTER (final + 1));
    }
  else
    /* no clear peak: where it focused last, else the middle of the sweep */
    final = remembered ? GPOINTER_TO_INT (remembered) - 1 : (self->lo + self->hi) / 2;
  self->result = r;
  if (timed_out || !self->streaming)
    {
      move (self, final, PHASE_NONE);
      set_state (self, r.ok ? CAM_FOCUS_FOCUSED : CAM_FOCUS_FAILED, r.reason);
      return;
    }
  /* the state goes out on the first frame measured at the final position: that is when the picture is sharp */
  move (self, final, PHASE_VERIFY);
  self->search_timer = g_timeout_add (self->cfg.timeout_ms, on_search_timeout, self);
}

static void
manual_apply (CamFocus *self)
{
  int position = self->lo + (int) lround (self->manual * (self->hi - self->lo));

  if (!self->lens_open || !self->streaming)
    return;
  if (self->inflight > 0)
    {
      self->want_valid = TRUE;
      self->want_pos = position;
      return;
    }
  move (self, position, PHASE_NONE);
}

static void
startup (CamFocus *self)
{
  self->started = TRUE;
  if (self->mode == CAM_FOCUS_MANUAL)
    {
      manual_apply (self);
      set_state (self, CAM_FOCUS_IDLE, CAM_AF_REASON_NONE);
    }
  else
    search_start (self, 0.5, 0.5);
}

static gboolean
on_start_timer (gpointer data)
{
  CamFocus *self = data;

  self->start_timer = 0;
  if (self->lens_open && self->streaming)
    startup (self);
  return G_SOURCE_REMOVE;
}

static void
maybe_start (CamFocus *self)
{
  if (!self->lens_open || !self->streaming || self->started || self->start_timer)
    return;
  reload_config (self);
  self->start_timer = g_timeout_add (self->cfg.start_delay_ms, on_start_timer, self);
}

static gboolean
on_watch (gpointer data)
{
  CamFocus *self = data;

  /* the sharpness of the middle of the frame, now: one measurement, nothing moves */
  if (self->lens_open && self->streaming && self->mode == CAM_FOCUS_AUTO && !self->hold && !self->search
      && self->state == CAM_FOCUS_FOCUSED && self->inflight == 0)
    {
      gboolean idle;

      g_mutex_lock (&self->lock);
      idle = !self->armed;
      g_mutex_unlock (&self->lock);
      if (idle)
        arm (self, PHASE_WATCH, 0, 0);
    }
  return G_SOURCE_CONTINUE;
}

static void
on_score (CamFocus *self, double score)
{
  Phase phase;

  g_debug ("autofocus: sharpness %.4f at %d", score, self->pos);
  g_mutex_lock (&self->lock);
  phase = self->arm_phase;
  g_mutex_unlock (&self->lock);
  switch (phase)
    {
    case PHASE_SEARCH:
      if (self->search)
        {
          cam_af_search_feed (self->search, score);
          search_step (self);
        }
      break;
    case PHASE_VERIFY:
      g_clear_handle_id (&self->search_timer, g_source_remove);
      self->ref_score = score;
      self->bad_looks = 0;
      set_state (self, self->result.ok ? CAM_FOCUS_FOCUSED : CAM_FOCUS_FAILED, self->result.reason);
      break;
    case PHASE_WATCH:
      if (score >= self->ref_score)
        {
          self->ref_score = score;
          self->bad_looks = 0;
        }
      else if (score < self->cfg.watch_drop * self->ref_score)
        self->bad_looks++;
      else
        self->bad_looks = 0;
      if (self->bad_looks >= self->cfg.watch_count
          && g_get_monotonic_time () - self->last_search_end >= (gint64) self->cfg.watch_gap_ms * 1000)
        search_start (self, self->cx, self->cy);
      break;
    case PHASE_NONE:
      break;
    }
}

static gboolean
on_note (gpointer data)
{
  Note *note = data;
  CamFocus *self = note->self;

  if (note->kind == NOTE_SCORE)
    {
      guint serial;

      g_mutex_lock (&self->lock);
      serial = self->arm_serial;
      g_mutex_unlock (&self->lock);
      if (note->gen == g_atomic_int_get (&self->gen) && note->serial == serial && self->lens_open)
        on_score (self, note->score);
    }
  else if (note->gen == g_atomic_int_get (&self->gen) && note->kind == NOTE_OPENED)
    {
      if (note->ok)
        {
          self->lens_min = note->a;
          self->lens_max = note->b;
          self->lo = MAX (self->cfg.pos_min, note->a);
          self->hi = MIN (self->cfg.pos_max, note->b);
          self->lens_open = self->hi > self->lo;
          g_message ("camera: autofocus on %s, lens %d to %d, sweep %d to %d", self->camera_id, note->a, note->b,
                     self->lo, self->hi);
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
          maybe_start (self);
        }
      else
        g_message ("camera: no autofocus on %s: %s", self->camera_id, note->text);
    }
  else if (note->gen == g_atomic_int_get (&self->gen) && note->kind == NOTE_MOVED)
    {
      self->inflight = MAX (0, self->inflight - 1);
      if (!note->ok)
        {
          g_warning ("autofocus: the lens did not move: %s", note->text);
          if (self->search)
            search_finish (self, TRUE);
        }
      else if (note->serial == self->move_serial)
        {
          if (self->moved_phase != PHASE_NONE)
            arm (self, self->moved_phase, self->cfg.settle_ms, self->cfg.skip_frames);
          else if (self->want_valid && self->inflight == 0)
            {
              self->want_valid = FALSE;
              move (self, self->want_pos, PHASE_NONE);
            }
        }
    }
  g_free (note->text);
  g_object_unref (note->self);
  g_free (note);
  return G_SOURCE_REMOVE;
}

/* ---- the streaming thread -------------------------------------------------------------------------------------- */

void
cam_focus_frame (CamFocus *self, GstBuffer *buffer, GstPad *pad)
{
  g_autoptr(GstCaps) caps = NULL;
  GstVideoInfo info;
  double roi, luma, cx, cy, score = 0;
  guint serial;
  gint gen;

  g_mutex_lock (&self->lock);
  if (!self->armed || g_get_monotonic_time () < self->not_before)
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  if (self->skip > 0)
    {
      self->skip--;
      g_mutex_unlock (&self->lock);
      return;
    }
  self->armed = FALSE;
  serial = self->arm_serial;
  roi = self->arm_roi;
  luma = self->arm_luma;
  cx = self->arm_cx;
  cy = self->arm_cy;
  g_mutex_unlock (&self->lock);
  gen = g_atomic_int_get (&self->gen);
  caps = gst_pad_get_current_caps (pad);
  if (!caps || !gst_video_info_from_caps (&info, caps)
      || !cam_af_buffer_sharpness (buffer, &info, roi, luma, cx, cy, &score))
    score = 0;
  post (self, (Note) { .kind = NOTE_SCORE, .gen = gen, .serial = serial, .score = score });
}

/* ---- the API --------------------------------------------------------------------------------------------------- */

gboolean
cam_focus_has_lens (CamFocus *self, const char *camera_id)
{
  g_autofree char *path = camera_id ? cam_lens_find (&self->cfg, camera_id) : NULL;

  return path != NULL;
}

static void
release (CamFocus *self)
{
  search_cancel (self);
  g_clear_handle_id (&self->start_timer, g_source_remove);
  self->started = FALSE;
  self->want_valid = FALSE;
  self->inflight = 0;
  self->bad_looks = 0;
  if (self->state != CAM_FOCUS_IDLE)
    set_state (self, CAM_FOCUS_IDLE, CAM_AF_REASON_NONE);
  if (self->lens_open)
    {
      self->lens_open = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
    }
}

void
cam_focus_set_camera (CamFocus *self, const char *camera_id)
{
  if (!g_strcmp0 (self->camera_id, camera_id))
    return;
  release (self);
  g_atomic_int_inc (&self->gen);
  g_free (self->camera_id);
  self->camera_id = g_strdup (camera_id);
  if (!camera_id)
    {
      Job *job = g_new0 (Job, 1);

      job->kind = JOB_CLOSE;
      push (self, job);
      return;
    }
  reload_config (self);
  if (cam_focus_has_lens (self, camera_id))
    {
      Job *job = g_new0 (Job, 1);

      job->kind = JOB_OPEN;
      job->camera_id = g_strdup (camera_id);
      job->dt_root = g_strdup (self->cfg.dt_root);
      job->subdev_class = g_strdup (self->cfg.subdev_class);
      job->dev_dir = g_strdup (self->cfg.dev_dir);
      push (self, job);
    }
}

void
cam_focus_streaming (CamFocus *self, gboolean streaming)
{
  self->streaming = streaming;
  if (streaming)
    maybe_start (self);
  else
    {
      search_cancel (self);
      g_clear_handle_id (&self->start_timer, g_source_remove);
      self->started = FALSE;
      if (self->state != CAM_FOCUS_IDLE)
        set_state (self, CAM_FOCUS_IDLE, CAM_AF_REASON_NONE);
    }
}

gboolean
cam_focus_available (CamFocus *self)
{
  return self->lens_open;
}

void
cam_focus_set_mode (CamFocus *self, CamFocusMode mode)
{
  CamFocusMode before = self->mode;

  if (mode == before)
    return;
  self->mode = mode;
  if (!self->lens_open || !self->streaming)
    return;
  search_cancel (self);
  if (mode == CAM_FOCUS_MANUAL)
    {
      manual_apply (self);
      set_state (self, CAM_FOCUS_IDLE, CAM_AF_REASON_NONE);
    }
  else if (before == CAM_FOCUS_MANUAL)
    search_start (self, 0.5, 0.5);
}

CamFocusMode
cam_focus_get_mode (CamFocus *self)
{
  return self->mode;
}

void
cam_focus_at (CamFocus *self, double x, double y)
{
  if (self->mode == CAM_FOCUS_MANUAL)
    return;
  g_debug ("autofocus: region at %.3f, %.3f of the frame", x, y);
  search_start (self, x, y);
}

void
cam_focus_set_position (CamFocus *self, double fraction)
{
  self->manual = CLAMP (fraction, 0, 1);
  if (self->mode == CAM_FOCUS_MANUAL)
    manual_apply (self);
}

double
cam_focus_get_position (CamFocus *self)
{
  return self->hi > self->lo ? CLAMP ((double) (self->pos - self->lo) / (self->hi - self->lo), 0, 1) : self->manual;
}

CamFocusState
cam_focus_get_state (CamFocus *self)
{
  return self->state;
}

CamAfReason
cam_focus_get_reason (CamFocus *self)
{
  return self->reason;
}

gboolean
cam_focus_busy (CamFocus *self)
{
  return self->state == CAM_FOCUS_SEARCHING;
}

void
cam_focus_set_hold (CamFocus *self, gboolean hold)
{
  self->hold = hold;
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
cam_focus_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_AVAILABLE)
    g_value_set_boolean (value, CAM_FOCUS (object)->lens_open);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
cam_focus_dispose (GObject *object)
{
  CamFocus *self = CAM_FOCUS (object);

  search_cancel (self);
  g_clear_handle_id (&self->start_timer, g_source_remove);
  g_clear_handle_id (&self->watch_timer, g_source_remove);
  if (self->lens_pool)
    {
      Job *job = g_new0 (Job, 1);

      job->kind = JOB_CLOSE;
      push (self, job);
      /* the lens is closed when this returns */
      g_thread_pool_free (self->lens_pool, FALSE, TRUE);
      self->lens_pool = NULL;
    }
  G_OBJECT_CLASS (cam_focus_parent_class)->dispose (object);
}

static void
cam_focus_finalize (GObject *object)
{
  CamFocus *self = CAM_FOCUS (object);

  g_free (self->camera_id);
  g_clear_object (&self->settings);
  g_hash_table_unref (self->last_good);
  cam_af_config_clear (&self->cfg);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (cam_focus_parent_class)->finalize (object);
}

static void
cam_focus_class_init (CamFocusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = cam_focus_get_property;
  object_class->dispose = cam_focus_dispose;
  object_class->finalize = cam_focus_finalize;
  props[PROP_AVAILABLE] = g_param_spec_boolean ("available", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);
  signals[SIG_STATE] = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void
cam_focus_init (CamFocus *self)
{
  g_mutex_init (&self->lock);
  self->last_good = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->manual = 0.5;
}

CamFocus *
cam_focus_new (GSettings *settings)
{
  CamFocus *self = g_object_new (CAM_TYPE_FOCUS, NULL);
  g_autoptr(GError) error = NULL;

  self->settings = g_object_ref (settings);
  if (!cam_af_config_load (&self->cfg, settings, &error))
    {
      g_warning ("autofocus: %s", error->message);
      /* the schema's own defaults are valid; this only happens with a broken dconf */
      g_settings_reset (settings, "af-pos-min");
      g_settings_reset (settings, "af-pos-max");
      g_settings_reset (settings, "af-coarse-steps");
      g_settings_reset (settings, "af-roi");
      g_clear_error (&error);
      cam_af_config_load (&self->cfg, settings, &error);
    }
  self->manual = g_settings_get_double (settings, "af-manual-position");
  self->lens_pool = g_thread_pool_new (lens_run, self, 1, FALSE, NULL);
  self->watch_timer = g_timeout_add (self->cfg.watch_every_ms, on_watch, self);
  return self;
}
