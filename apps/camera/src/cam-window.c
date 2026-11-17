/* cam-window: see cam-window.h. "A/" is CameraActivity.kt, "VM/" CameraViewModel.kt (spec/camera.md's notation). */
#include "cam-window.h"

#include <errno.h>
#include <unistd.h>
#include "cam-constants.h"
#include "cam-devices.h"
#include "cam-dialogs.h"
#include "cam-encoder.h"
#include "cam-focus.h"
#include "cam-mode-selector.h"
#include "cam-photo.h"
#include "cam-pipeline.h"
#include "cam-qr-sheet.h"
#include "cam-recorder.h"
#include "cam-restart.h"
#include "cam-sensors.h"
#include "cam-settings.h"
#include "cam-sounds.h"
#include "cam-storage.h"
#include <glib/gstdio.h>
#include "cam-tone.h"
#include "cam-tune.h"
#include "cam-viewfinder.h"
#include "cam-widgets.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

#define APP_ID            "de.yesman.Camera"
#define ZOOM_MAX          8.0      /* digital: a crop of the frame (§23); CameraX's maxZoomRatio of a camera */
#define BAR_ICON          40       /* the 24 dp drawable, fitCenter in the 48 dp button less its 4 dp padding */
#define TUNE_SAVE_MS      300      /* a tuning slider that stopped this long ago is written to dconf */
#define FLASH_SETTLE_MS   1000     /* the torch on before the frame is taken: the exposure follows it */
#define SCREEN_FLASH_MS   500      /* the white screen up before the frame is taken */
#define BITRATE_1080      16000000 /* the live app's 1080p files, ≈ 16 Mbit/s (§11.1) */
#define BITRATE_720       8000000
#define INDEFINITE        G_MAXINT /* Snackbar.LENGTH_INDEFINITE */

typedef enum {
  ST_IDLE,
  ST_COUNTDOWN,
  ST_PRE_RECORDING,
  ST_RECORDING,
  ST_PAUSED,
} CamState;

typedef enum {
  FLASH_OFF,
  FLASH_ON,
  FLASH_TORCH,
  FLASH_SCREEN,
} CamFlash;

typedef struct {
  GtkWidget *button, *rotator, *icon, *label;
} ProButton;

#define CAM_TYPE_VIEW (cam_view_get_type ())
G_DECLARE_FINAL_TYPE (CamView, cam_view, CAM, VIEW, GtkWidget)

struct _CamView {
  GtkWidget       parent;
  GtkWindow      *window;
  GtkApplication *app;
  GSettings      *settings;
  GPtrArray      *cameras;
  CamCamera      *camera;
  CamPipeline    *pipeline;
  CamSensors     *sensors;
  CamStorage     *storage;
  GCancellable   *cancel, *thumb_cancel;

  /* the camera screen */
  GtkWidget *viewfinder, *island, *countdown, *zoom_slider, *focus_slider, *pro_row, *snackbar;
  GtkWidget *pro_button, *lenses, *flash_button, *flash_icon;
  GtkWidget *gallery, *shutter, *flip, *pause, *modes, *tune_panel;
  GPtrArray *rotators;                  /* what turns with the phone (§17) */
  ProButton  p_aspect, p_quality, p_fps, p_hdr, p_mic, p_timer, p_grid, p_tune, p_settings;
  /* over the whole window */
  GtkWidget *settings_page, *toast, *white;

  /* state (§21.6) */
  CamMode    mode;
  CamState   state;
  CamFacing  facing;
  CamGrid    grid;
  int        timer;
  gboolean   wide;                      /* photos 16:9 */
  int        quality;                   /* video lines: 720, 1080 */
  gboolean   mic;
  CamFlash   photo_flash, video_flash, scan_flash;
  gboolean   force_torch;
  CamLed     led;                       /* what the LED was last set to */
  int        orientation;
  int        display_rotation;          /* the turn the shell laid the screen out for (cam-sensors.h) */

  /* the stream */
  gboolean   running;
  char      *bound_id;
  char      *ok_id;                     /* the camera whose stream ran last (the one to fall back on) */
  int        bound_w, bound_h;
  gboolean   in_settings, closing;
  guint      close_timer;

  /* zoom (§8) */
  double     zoom, pinch_base;
  gint64     pinch_end;
  NeoAnim   *zoom_anim;
  guint      slider_hide;

  /* focus (cam-focus.h): the lens of the camera in use, if it has one */
  CamFocus  *focus;
  gboolean   ring_up;                   /* the focus ring is showing this search */
  guint      focus_save;                /* the pending write of the manual position to dconf */

  /* the tuning sheet: the Soft ISP's gamma, contrast and saturation of the camera in use (cam-tune.h) */
  gboolean   tune_open;
  guint      tune_save;                 /* the pending write of tune_tone for tune_id */
  char      *tune_id;
  CamTone    tune_tone;

  /* the pro row (§7) */
  double     pro_shown;
  gboolean   pro_open;
  NeoAnim   *pro_anim;

  /* captures */
  CamRecorder *recorder;
  guint        duration_timer, start_timer, flash_timer, night_timer;
  gboolean     taking, rec_audio;
  gboolean     scanning_paused;

  /* the shell */
  gboolean   volume_keys, bright_hook, bright_on;
  guint      inhibit;
  CamThermal thermal;
};

G_DEFINE_FINAL_TYPE (CamView, cam_view, GTK_TYPE_WIDGET)

static void bind_camera (CamView *self);
static void update_controls (CamView *self);
static void update_flash (CamView *self);
static void sync_running (CamView *self);
static void use_camera (CamView *self, CamCamera *camera);
static void tune_sync (CamView *self);
static void tune_flush (CamView *self);
static void slide_pro (CamView *self, gboolean open);
static void take_photo (CamView *self);
static void take_night (CamView *self);
#define is_photo_mode(self) ((self)->mode == CAM_MODE_PHOTO || (self)->mode == CAM_MODE_NIGHT)
static void start_video (CamView *self);

/* ---- small helpers --------------------------------------------------------------------------------------------- */

static void
toast (CamView *self, const char *text)
{
  cam_toast_show (CAM_TOAST (self->toast), text);
}

static gboolean
on_close_timeout (gpointer data)
{
  CamView *self = data;

  self->close_timer = 0;
  if (self->window)
    gtk_window_destroy (self->window);
  return G_SOURCE_REMOVE;
}

/* an error toast, then the app closes as Aperture's activity finishes */
static void
fail (CamView *self, const char *text)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  toast (self, text);
  sync_running (self);
  self->close_timer = g_timeout_add (CAM_MS_TOAST + 300, on_close_timeout, self);
}

static GtkWidget *
rotator (CamView *self, GtkWidget *child)
{
  GtkWidget *rot = cam_rotator_new (child);

  g_ptr_array_add (self->rotators, rot);
  return rot;
}

/* a 48 dp bar button with a 40 dp icon that turns with the phone, and the borderless ripple (§6.1) */
static GtkWidget *
bar_button (CamView *self, const char *icon_name, GtkWidget **icon)
{
  GtkWidget *button;

  *icon = cam_icon_new (icon_name, BAR_ICON);
  button = neo_pressable_new (rotator (self, *icon));
  gtk_widget_set_size_request (button, CAM_BAR_BUTTON, CAM_BAR_BUTTON);
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, CAM_RIPPLE_RADIUS);
  return button;
}

static const char *
facing_name (CamFacing facing)
{
  return facing == CAM_FACING_FRONT ? "front" : facing == CAM_FACING_EXTERNAL ? "external" : "back";
}

static double
zoom_progress (double ratio)
{
  /* CameraX's linear zoom: the crop width moves linearly from 1/min to 1/max */
  return CLAMP ((1 - 1 / ratio) / (1 - 1 / ZOOM_MAX), 0, 1);
}

static double
zoom_ratio (double progress)
{
  return 1 / (1 - CLAMP (progress, 0, 1) * (1 - 1 / ZOOM_MAX));
}

static gboolean
is_front (CamView *self)
{
  return self->camera && self->camera->facing == CAM_FACING_FRONT;
}

/* ---- the shell ------------------------------------------------------------------------------------------------- */

static GDBusConnection *
session_bus (CamView *self)
{
  return g_application_get_dbus_connection (G_APPLICATION (self->app));
}

/* volume keys as the shutter (§18): the shell hands them to the app in front that asks (extension/service.js) */
static void
take_volume_keys (CamView *self, gboolean take)
{
  GDBusConnection *bus = session_bus (self);

  if (take == self->volume_keys || !bus)
    return;
  self->volume_keys = take;
  g_dbus_connection_call (bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone",
                          "TakeVolumeKeys", g_variant_new ("(sb)", APP_ID, take), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
}

static gboolean
shell_has (CamView *self, const char *method)
{
  GDBusConnection *bus = session_bus (self);
  g_autoptr(GVariant) reply = NULL;
  const char *xml;
  g_autoptr(GDBusNodeInfo) node = NULL;

  if (!bus)
    return FALSE;
  reply = g_dbus_connection_call_sync (bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone",
                                       "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
                                       G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!reply)
    return FALSE;
  g_variant_get (reply, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml (xml, NULL);
  for (int i = 0; node && node->interfaces && node->interfaces[i]; i++)
    if (g_dbus_interface_info_lookup_method (node->interfaces[i], method))
      return TRUE;
  return FALSE;
}

/* Bright screen and the screen flash (§1, §12.2): the window's brightness at 100 %. The shell owns the backlight,
 * so this needs its KeepScreenBright (not there yet: the row is left out without it) */
static void
keep_bright (CamView *self, gboolean on)
{
  GDBusConnection *bus = session_bus (self);

  if (!self->bright_hook || !bus || on == self->bright_on)
    return;
  self->bright_on = on;
  g_dbus_connection_call (bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone",
                          "KeepScreenBright", g_variant_new ("(sb)", APP_ID, on), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
}

static void
sync_shell (CamView *self)
{
  gboolean front = self->running && self->window && gtk_window_is_active (self->window);
  g_autofree char *action = g_settings_get_string (self->settings, "volume-buttons-action");

  /* "System default" leaves the keys to the shell's volume */
  take_volume_keys (self, front && !g_str_equal (action, "default"));
  keep_bright (self, front && g_settings_get_boolean (self->settings, "bright-screen"));
}

/* ---- the flash (§6.1) ------------------------------------------------------------------------------------------ */

static gboolean
has_led (CamView *self)
{
  return self->camera && self->camera->flash;
}

/* the mode's list ∩ the camera's: photo OFF, ON (no AUTO without metering), SCREEN on a front camera; video and
 * scan OFF, TORCH; the LED ones only with an LED */
static gboolean
flash_supported (CamView *self, CamFlash flash)
{
  switch (flash)
    {
    case FLASH_OFF:
      return TRUE;
    case FLASH_ON:
      return is_photo_mode (self) && has_led (self);
    case FLASH_TORCH:
      return !is_photo_mode (self) && has_led (self);
    case FLASH_SCREEN:
      return is_photo_mode (self) && is_front (self);
    }
  return FALSE;
}

static CamFlash *
mode_flash (CamView *self)
{
  return is_photo_mode (self) ? &self->photo_flash : self->mode == CAM_MODE_VIDEO ? &self->video_flash
                                                                                          : &self->scan_flash;
}

static CamFlash
current_flash (CamView *self)
{
  CamFlash flash = *mode_flash (self);

  if (is_photo_mode (self) && self->force_torch && has_led (self))
    return FLASH_TORCH;
  return flash_supported (self, flash) ? flash : FLASH_OFF;
}

static void
set_led (CamView *self, CamLed led)
{
  if (led == self->led)
    return;
  self->led = led;
  cam_flash_led_set (led);
}

static void
sync_torch (CamView *self)
{
  /* a torch mode lights the LED while the camera runs (CameraControl.enableTorch) */
  set_led (self, self->running && current_flash (self) == FLASH_TORCH ? CAM_LED_TORCH : CAM_LED_OFF);
}

static void
save_flash (CamView *self)
{
  static const char *const names[] = { "off", "on", "torch", "screen" };

  if (is_photo_mode (self))
    g_settings_set_string (self->settings, "photo-flash-mode", names[self->photo_flash]);
  else if (self->mode == CAM_MODE_VIDEO)
    g_settings_set_string (self->settings, "video-flash-mode", names[self->video_flash]);
}

static CamFlash
flash_from_name (const char *name)
{
  return g_str_equal (name, "on") ? FLASH_ON : g_str_equal (name, "torch") ? FLASH_TORCH
         : g_str_equal (name, "screen") ? FLASH_SCREEN : FLASH_OFF;
}

static void
update_flash (CamView *self)
{
  static const char *const icons[] = { "cam-flash-off-symbolic", "cam-flash-on-symbolic",
                                       "cam-flashlight-on-symbolic", "cam-flash-screen-symbolic" };
  gboolean any = FALSE;

  for (CamFlash f = FLASH_ON; f <= FLASH_SCREEN; f++)
    any |= flash_supported (self, f);
  /* hidden when OFF is all there is (still:camera-ultrawide) */
  gtk_widget_set_visible (self->flash_button, any);
  cam_icon_set_icon (CAM_ICON (self->flash_icon), icons[current_flash (self)]);
  sync_torch (self);
}

static void
on_torch_help_ok (gpointer data)
{
  CamView *self = data;

  g_settings_set_boolean (self->settings, "force-torch-help-shown", TRUE);
}

static void
on_flash_clicked (NeoPressable *button, CamView *self)
{
  CamFlash *flash = mode_flash (self);

  if (is_photo_mode (self) && self->force_torch)
    {
      /* torch mode on: a tap only turns it off */
      self->force_torch = FALSE;
      update_flash (self);
      return;
    }
  /* the next supported one, in FlashMode's order */
  for (int i = 1; i <= 4; i++)
    {
      CamFlash next = (current_flash (self) + i) % 4;

      if (flash_supported (self, next))
        {
          *flash = next;
          break;
        }
    }
  save_flash (self);
  update_flash (self);
  if (is_photo_mode (self) && has_led (self)
      && !g_settings_get_boolean (self->settings, "force-torch-help-shown"))
    neo_snackbar_show (NEO_SNACKBAR (self->snackbar),
                       _("On photo mode, you can long press the flash button to switch into torch mode."), _("OK"),
                       on_torch_help_ok, self, INDEFINITE);
}

static void
on_flash_long_pressed (NeoPressable *button, CamView *self)
{
  if (!is_photo_mode (self) || !has_led (self) || self->state != ST_IDLE)
    return;
  self->force_torch = !self->force_torch;
  g_settings_set_boolean (self->settings, "force-torch-help-shown", TRUE);
  neo_snackbar_dismiss (NEO_SNACKBAR (self->snackbar));
  update_flash (self);
}

/* ---- zoom (§8) ------------------------------------------------------------------------------------------------- */

static gboolean
on_slider_hide (gpointer data)
{
  CamView *self = data;

  self->slider_hide = 0;
  gtk_widget_set_visible (self->zoom_slider, FALSE);
  return G_SOURCE_REMOVE;
}

static void
apply_zoom (CamView *self, double ratio, gboolean show_slider)
{
  g_autofree char *text = NULL;

  self->zoom = CLAMP (ratio, 1, ZOOM_MAX);
  cam_viewfinder_set_zoom (CAM_VIEWFINDER (self->viewfinder), self->zoom);
  cam_pipeline_set_zoom (self->pipeline, self->zoom);
  text = g_strdup_printf ("%.1fx", self->zoom);
  cam_zoom_slider_set (CAM_ZOOM_SLIDER (self->zoom_slider), zoom_progress (self->zoom), text);
  if (show_slider)
    {
      /* shown on every change, hidden 2 s after the last (no animation) */
      gtk_widget_set_visible (self->zoom_slider, TRUE);
      g_clear_handle_id (&self->slider_hide, g_source_remove);
      self->slider_hide = g_timeout_add (CAM_MS_SLIDER_HIDE, on_slider_hide, self);
    }
}

static void
on_zoom_anim (NeoAnim *anim, double value, gpointer data)
{
  apply_zoom (CAM_VIEW (data), value, TRUE);
}

static void
smooth_zoom (CamView *self, double ratio)
{
  /* a ValueAnimator from the current ratio, 300 ms AD */
  neo_anim_start (self->zoom_anim, self->zoom, CLAMP (ratio, 1, ZOOM_MAX), CAM_MS_DEFAULT, 0,
                  SPONGY_ACCELERATE_DECELERATE);
}

static void
zoom_step (CamView *self, int direction)
{
  double target = neo_anim_is_running (self->zoom_anim) ? neo_anim_get_to (self->zoom_anim) : self->zoom;
  double power = direction > 0 ? floor (log2 (target) + 1e-6) + 1 : ceil (log2 (target) - 1e-6) - 1;

  /* the next or previous power of two */
  smooth_zoom (self, pow (2, MAX (power, 0)));
}

static void
on_slider_changed (CamZoomSlider *slider, double progress, CamView *self)
{
  neo_anim_stop (self->zoom_anim);
  apply_zoom (self, zoom_ratio (progress), TRUE);
}

static void
on_pinch_begin (CamViewfinder *viewfinder, CamView *self)
{
  neo_anim_stop (self->zoom_anim);
  self->pinch_base = self->zoom;
}

static void
on_pinch (CamViewfinder *viewfinder, double scale, CamView *self)
{
  self->pinch_end = g_get_monotonic_time ();
  apply_zoom (self, self->pinch_base * scale, TRUE);
}

static void
on_pinch_end (CamViewfinder *viewfinder, CamView *self)
{
  self->pinch_end = g_get_monotonic_time ();
}

/* ---- the lens buttons (§6.2) ----------------------------------------------------------------------------------- */

/* the phone's turn the UI still makes up for: its own, less the turn the shell gave the screen. 0 while the screen
 * follows the phone (a landscape layout), the phone's turn while the screen stays upright (Aperture's way) */
static int
ui_turn (CamView *self)
{
  return ((self->orientation - self->display_rotation) % 360 + 360) % 360;
}

/* the preview's clockwise turn in the window: the sensor's own on an upright screen. With the screen turned with
 * the phone, a back camera's picture turns with the screen's turn and a front camera's against it, as the photo's
 * turn does (capture_rotation): the front camera looks the other way. (Both the same way left a turned selfie
 * preview upside down.) */
static int
preview_turn (CamView *self)
{
  int sensor = self->camera->orientation, screen = self->display_rotation;

  return ((is_front (self) ? sensor - screen : sensor + screen) % 360 + 360) % 360;
}

/* the leveler's roll, against the window rather than the panel */
static double
ui_tilt (CamView *self)
{
  double tilt = cam_sensors_get_tilt (self->sensors);

  return isnan (tilt) ? tilt : fmod (tilt - self->display_rotation + 720, 360);
}

/* ---- the lens bar and the tuning sheet --------------------------------------------------------------------------
 * Aperture's lens bar lists zoom ratios from the lenses' focal lengths, which libcamera does not report: the order
 * and the labels of this phone's cameras are the "lens-labels" setting, a camera not listed follows under its
 * sensor's name, "hidden-cameras" are left out. */

static gboolean
camera_hidden (CamView *self, CamCamera *camera)
{
  g_auto(GStrv) hidden = g_settings_get_strv (self->settings, "hidden-cameras");

  return g_strv_contains ((const char *const *) hidden, camera->id);
}

/* the camera's label and its place in "lens-labels" (a large one for a camera that is not listed) */
static char *
camera_label (CamView *self, CamCamera *camera, int *order)
{
  g_autoptr(GVariant) list = g_settings_get_value (self->settings, "lens-labels");
  GVariantIter iter;
  const char *id, *label;
  int i = 0;

  g_variant_iter_init (&iter, list);
  while (g_variant_iter_next (&iter, "(&s&s)", &id, &label))
    {
      if (!g_strcmp0 (id, camera->id))
        {
          if (order)
            *order = i;
          return g_strdup (label);
        }
      i++;
    }
  if (order)
    *order = G_MAXINT;
  return camera->model ? g_ascii_strup (camera->model, -1) : g_strdup (camera->id);
}

static gint
compare_lenses (gconstpointer a, gconstpointer b, gpointer data)
{
  CamCamera *ca = *(CamCamera *const *) a, *cb = *(CamCamera *const *) b;
  int oa, ob;

  g_free (camera_label (data, ca, &oa));
  g_free (camera_label (data, cb, &ob));
  if (oa != ob)
    return oa < ob ? -1 : 1;
  /* the unlisted: the larger sensor first */
  return (gint64) cb->pixels_w * cb->pixels_h > (gint64) ca->pixels_w * ca->pixels_h ? 1 : -1;
}

/* the cameras of the facing the lens bar shows, in its order; the one in use is always among them */
static GPtrArray *
lens_cameras (CamView *self, CamFacing facing)
{
  GPtrArray *list = g_ptr_array_new ();

  for (guint i = 0; i < self->cameras->len; i++)
    {
      CamCamera *camera = self->cameras->pdata[i];

      if (camera->facing == facing && (camera == self->camera || !camera_hidden (self, camera)))
        g_ptr_array_add (list, camera);
    }
  g_ptr_array_sort_with_data (list, compare_lenses, self);
  return list;
}

static void
remember_camera (CamView *self)
{
  g_settings_set_string (self->settings, "last-camera-facing", facing_name (self->facing));
  g_settings_set_string (self->settings, "last-camera-id", self->camera ? self->camera->id : "");
}

static void
on_lens_clicked (CamLens *lens, CamView *self)
{
  CamCamera *camera = g_object_get_data (G_OBJECT (lens), "camera");

  if (camera && camera != self->camera && self->state == ST_IDLE)
    {
      use_camera (self, camera);
      remember_camera (self);
      update_controls (self);
      bind_camera (self);
      return;
    }
  /* the selected lens: a tap resets the zoom */
  smooth_zoom (self, 1);
}

static void
update_lenses (CamView *self)
{
  GtkWidget *child;
  g_autoptr(GPtrArray) list = NULL;

  while ((child = gtk_widget_get_first_child (self->lenses)))
    {
      g_ptr_array_remove (self->rotators, child);
      gtk_box_remove (GTK_BOX (self->lenses), child);
    }
  if (!self->camera)
    return;
  list = lens_cameras (self, self->camera->facing);
  for (guint i = 0; i < list->len; i++)
    {
      CamCamera *camera = list->pdata[i];
      /* a facing with one camera has "1×", the main camera */
      g_autofree char *label = list->len == 1 ? g_strdup ("1×") : camera_label (self, camera, NULL);
      GtkWidget *lens = cam_lens_new (label, camera == self->camera);
      GtkWidget *rot = rotator (self, lens);

      g_object_set_data (G_OBJECT (lens), "camera", camera);
      cam_rotator_set_angle (CAM_ROTATOR (rot), CAM_COMPENSATION (ui_turn (self)), FALSE);
      g_signal_connect (lens, "clicked", G_CALLBACK (on_lens_clicked), self);
      gtk_box_append (GTK_BOX (self->lenses), rot);
    }
}

/* ---- the tuning sheet ---- */

static char *
camera_title (CamView *self, CamCamera *camera)
{
  g_autofree char *label = camera_label (self, camera, NULL);

  return camera->facing == CAM_FACING_FRONT ? g_strdup (_("Front")) : g_strdup_printf (_("Back %s"), label);
}

static void
apply_tuning (CamView *self)
{
  CamTone tone = CAM_TONE_NEUTRAL;
  gboolean stored = self->camera && cam_tone_load (self->settings, self->camera->id, &tone);

  cam_pipeline_set_tone (self->pipeline, stored, tone.gamma, tone.contrast, tone.saturation);
}

static void
tune_sync (CamView *self)
{
  CamTone tone = CAM_TONE_NEUTRAL;
  g_autofree char *title = NULL;

  if (!self->tune_open || !self->camera)
    return;
  tune_flush (self);
  cam_tone_load (self->settings, self->camera->id, &tone);
  title = camera_title (self, self->camera);
  cam_tune_panel_set (CAM_TUNE_PANEL (self->tune_panel), title, &tone);
}

static void
tune_flush (CamView *self)
{
  if (!self->tune_save)
    return;
  g_clear_handle_id (&self->tune_save, g_source_remove);
  if (self->tune_id)
    cam_tone_store (self->settings, self->tune_id, &self->tune_tone);
}

static gboolean
on_tune_save (gpointer data)
{
  CamView *self = data;

  self->tune_save = 0;
  if (self->tune_id)
    cam_tone_store (self->settings, self->tune_id, &self->tune_tone);
  return G_SOURCE_REMOVE;
}

static void
tune_set (CamView *self, const CamTone *tone, gboolean at_once)
{
  if (!self->camera)
    return;
  cam_pipeline_set_tone (self->pipeline, TRUE, tone->gamma, tone->contrast, tone->saturation);
  g_free (self->tune_id);
  self->tune_id = g_strdup (self->camera->id);
  self->tune_tone = *tone;
  /* a drag moves a slider many times a second: dconf is written once it settles */
  g_clear_handle_id (&self->tune_save, g_source_remove);
  if (at_once)
    cam_tone_store (self->settings, self->tune_id, &self->tune_tone);
  else
    self->tune_save = g_timeout_add (TUNE_SAVE_MS, on_tune_save, self);
}

static void
on_tune_changed (CamTunePanel *panel, CamView *self)
{
  CamTone tone;

  cam_tune_panel_get (panel, &tone);
  tune_set (self, &tone, FALSE);
}

static void
on_tune_reset (CamTunePanel *panel, CamView *self)
{
  CamTone tone = CAM_TONE_NEUTRAL;

  if (!self->camera)
    return;
  /* the schema's seed for the camera, else neutral */
  cam_tone_load_default (self->settings, self->camera->id, &tone);
  tune_set (self, &tone, TRUE);
  tune_sync (self);
}

static void
tune_show (CamView *self, gboolean show)
{
  if (show == self->tune_open)
    return;
  self->tune_open = show;
  gtk_widget_set_visible (self->tune_panel, show);
  if (show)
    {
      slide_pro (self, FALSE);
      tune_sync (self);
    }
  else
    tune_flush (self);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_tune_done (CamTunePanel *panel, CamView *self)
{
  tune_show (self, FALSE);
}

/* ---- the pro row (§7) ------------------------------------------------------------------------------------------ */

static void
on_pro_anim (NeoAnim *anim, double value, gpointer data)
{
  CamView *self = data;

  self->pro_shown = value;
  gtk_widget_set_opacity (self->pro_row, value);
  gtk_widget_set_visible (self->pro_row, value > 0);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
slide_pro (CamView *self, gboolean open)
{
  if (open == self->pro_open)
    return;
  self->pro_open = open;
  /* slide up 250 ms, down 200 ms, both AD, with the alpha */
  neo_anim_start (self->pro_anim, self->pro_shown, open ? 1 : 0, open ? CAM_MS_SLIDE_UP : CAM_MS_SLIDE_DOWN, 0,
                  SPONGY_ACCELERATE_DECELERATE);
}

static void
pro_set (ProButton *pro, const char *icon_name, const char *label)
{
  cam_icon_set_icon (CAM_ICON (pro->icon), icon_name);
  gtk_label_set_text (GTK_LABEL (pro->label), label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pro->button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
pro_button (CamView *self, ProButton *pro, GtkWidget *row, GCallback clicked)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  pro->icon = cam_icon_new (NULL, CAM_PRO_ICON);
  gtk_widget_set_halign (pro->icon, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (pro->icon, CAM_PRO_ICON_TOP);
  gtk_box_append (GTK_BOX (box), pro->icon);
  pro->label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (pro->label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (pro->label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (pro->label), 3);
  gtk_label_set_ellipsize (GTK_LABEL (pro->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_justify (GTK_LABEL (pro->label), GTK_JUSTIFY_CENTER);
  gtk_label_set_max_width_chars (GTK_LABEL (pro->label), 1);
  gtk_widget_add_css_class (pro->label, "cam-pro-label");
  gtk_box_append (GTK_BOX (box), pro->label);
  gtk_widget_set_size_request (box, CAM_PRO_BUTTON_W, -1);
  pro->rotator = rotator (self, box);
  pro->button = neo_pressable_new (pro->rotator);
  /* borderless: half the 96 × 68⅓ dp button's diagonal */
  neo_pressable_set_ripple (NEO_PRESSABLE (pro->button), NEO_RIPPLE_CIRCLE,
                            hypot (CAM_PRO_BUTTON_W, CAM_PRO_ROW_H - 2 * CAM_PRO_ROW_PAD_V) / 2);
  gtk_widget_set_valign (pro->button, GTK_ALIGN_START);
  g_signal_connect (pro->button, "clicked", clicked, self);
  gtk_box_append (GTK_BOX (row), pro->button);
}

static gboolean
quality_available (CamView *self, int lines)
{
  const CamEncoder *encoder = cam_encoder_get ();

  if (!self->camera || !encoder || !encoder->element)
    return FALSE;
  return lines == 1080 ? encoder->fhd && cam_camera_has_size (self->camera, 1920, 1080)
                       : encoder->hd && cam_camera_has_size (self->camera, 1280, 720);
}

static gboolean
can_record (CamView *self)
{
  return quality_available (self, 1080) || quality_available (self, 720);
}

static void
update_pro_row (CamView *self)
{
  gboolean photo = is_photo_mode (self), video = self->mode == CAM_MODE_VIDEO;
  gboolean idle = self->state == ST_IDLE;
  gboolean recording = self->state == ST_RECORDING || self->state == ST_PAUSED;
  static const char *const grid_icons[] = { "cam-grid-3x3-off-symbolic", "cam-grid-3x3-symbolic",
                                            "cam-grid-4x4-symbolic", "cam-grid-goldenratio-symbolic" };
  const char *grid_labels[] = { _("GRID OFF"), _("GRID 3x3"), _("GRID 4x4"), _("GRID φ") };
  const char *timer_icon = self->timer == 3 ? "cam-timer-3-alt-1-symbolic"
                           : self->timer == 10 ? "cam-timer-10-alt-1-symbolic" : "cam-timer-off-symbolic";
  const char *timer_label = self->timer == 3 ? _("TIMER 3s") : self->timer == 10 ? _("TIMER 10s") : _("TIMER OFF");

  gtk_widget_set_visible (self->p_aspect.button, photo);
  gtk_widget_set_visible (self->p_quality.button, video);
  gtk_widget_set_visible (self->p_fps.button, video);
  gtk_widget_set_visible (self->p_hdr.button, video);
  gtk_widget_set_visible (self->p_mic.button, video);
  gtk_widget_set_visible (self->p_timer.button, photo || video);
  gtk_widget_set_visible (self->p_grid.button, photo || video);
  gtk_widget_set_visible (self->p_tune.button, photo || video);
  gtk_widget_set_visible (self->p_settings.button, photo || video);

  pro_set (&self->p_aspect, "cam-aspect-ratio-symbolic", self->wide ? "16:9" : "4:3");
  pro_set (&self->p_quality, self->quality == 720 ? "cam-hd-symbolic" : "cam-full-hd-symbolic",
           self->quality == 720 ? "720p" : "1080p");
  /* the camera's own rate: there is no other to choose, so AUTO and disabled, as Aperture shows a single option */
  pro_set (&self->p_fps, "cam-video-frame-rate-symbolic", _("AUTO FPS"));
  pro_set (&self->p_hdr, "cam-hdr-off-symbolic", "SDR");
  pro_set (&self->p_mic, self->mic ? "cam-mic-on-symbolic" : "cam-mic-off-symbolic",
           self->mic ? _("MIC ON") : _("MIC OFF"));
  pro_set (&self->p_timer, timer_icon, timer_label);
  pro_set (&self->p_grid, grid_icons[self->grid], grid_labels[self->grid]);
  pro_set (&self->p_tune, "cam-tune-symbolic", _("TUNE"));
  pro_set (&self->p_settings, "cam-settings-symbolic", _("SETTINGS"));

  gtk_widget_set_sensitive (self->p_aspect.button, idle);
  gtk_widget_set_sensitive (self->p_quality.button, idle && quality_available (self, 720)
                                                  && quality_available (self, 1080));
  gtk_widget_set_sensitive (self->p_fps.button, FALSE);
  gtk_widget_set_sensitive (self->p_hdr.button, FALSE);
  /* the mic: idle, or live while recording with a sound track */
  /* the mic: idle, or live while recording a sound track (MIC OFF at the start records none) */
  gtk_widget_set_sensitive (self->p_mic.button, idle ? cam_recorder_can_record_audio () : recording && self->rec_audio);
  gtk_widget_set_sensitive (self->p_timer.button, idle);
  gtk_widget_set_sensitive (self->p_tune.button, idle);
  gtk_widget_set_sensitive (self->p_settings.button, idle);
}

static void
on_aspect (NeoPressable *button, CamView *self)
{
  self->wide = !self->wide;
  g_settings_set_string (self->settings, "aspect-ratio", self->wide ? "16:9" : "4:3");
  update_pro_row (self);
  bind_camera (self);
}

static void
on_quality (NeoPressable *button, CamView *self)
{
  int next = self->quality == 1080 ? 720 : 1080;

  if (!quality_available (self, next))
    return;
  self->quality = next;
  g_settings_set_string (self->settings, "video-quality", next == 720 ? "720p" : "1080p");
  update_pro_row (self);
  bind_camera (self);
}

static void
on_nothing (NeoPressable *button, CamView *self)
{
}

static void
update_island (CamView *self)
{
  guint items = 0;
  CamThermal thermal = cam_sensors_get_thermal (self->sensors);

  if (thermal >= CAM_THERMAL_CRITICAL)
    items |= 1 << CAM_ISLAND_THERMAL_ERROR;
  else if (thermal >= CAM_THERMAL_MODERATE)
    items |= 1 << CAM_ISLAND_THERMAL;
  if (cam_sensors_get_battery_low (self->sensors))
    items |= 1 << (cam_sensors_get_charging (self->sensors) ? CAM_ISLAND_BATTERY_CHARGING : CAM_ISLAND_BATTERY);
  if (self->mode == CAM_MODE_VIDEO && !self->mic)
    items |= 1 << CAM_ISLAND_MIC_OFF;
  cam_island_set_items (CAM_ISLAND (self->island), items);
}

static void
on_mic (NeoPressable *button, CamView *self)
{
  self->mic = !self->mic;
  g_settings_set_boolean (self->settings, "last-mic-mode", self->mic);
  if (self->recorder)
    cam_recorder_set_muted (self->recorder, !self->mic);
  update_pro_row (self);
  update_island (self);
}

static void
on_timer (NeoPressable *button, CamView *self)
{
  self->timer = self->timer == 0 ? 3 : self->timer == 3 ? 10 : 0;
  g_settings_set_int (self->settings, "timer-mode", self->timer);
  update_pro_row (self);
}

static void
apply_grid (CamView *self)
{
  /* forced off in SCAN mode (not saved) */
  cam_viewfinder_set_grid (CAM_VIEWFINDER (self->viewfinder), self->mode == CAM_MODE_SCAN ? CAM_GRID_OFF
                                                                                             : self->grid);
}

static void
on_grid (NeoPressable *button, CamView *self)
{
  static const char *const names[] = { "off", "3x3", "4x4", "golden" };

  self->grid = (self->grid + 1) % 4;
  g_settings_set_string (self->settings, "last-grid-mode", names[self->grid]);
  apply_grid (self);
  update_pro_row (self);
}

static void
on_settings_closed (CamSettings *page, CamView *self)
{
  self->in_settings = FALSE;
  gtk_widget_set_visible (self->settings_page, FALSE);
  gtk_window_fullscreen (self->window);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  sync_running (self);
}

static void
on_tune (NeoPressable *button, CamView *self)
{
  tune_show (self, TRUE);
}

static void
on_settings (NeoPressable *button, CamView *self)
{
  tune_show (self, FALSE);
  slide_pro (self, FALSE);
  self->in_settings = TRUE;
  /* the settings activity has the status bar */
  gtk_window_unfullscreen (self->window);
  cam_settings_reset (CAM_SETTINGS (self->settings_page));
  gtk_widget_set_visible (self->settings_page, TRUE);
  gtk_widget_grab_focus (self->settings_page);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  sync_running (self);
}

static void
on_pro (NeoPressable *button, CamView *self)
{
  if (!self->pro_open)
    tune_show (self, FALSE);
  slide_pro (self, !self->pro_open);
}

/* ---- rotation (§17) -------------------------------------------------------------------------------------------- */

static void
apply_orientation (CamView *self, gboolean animate)
{
  double angle = CAM_COMPENSATION (ui_turn (self));

  for (guint i = 0; i < self->rotators->len; i++)
    cam_rotator_set_angle (self->rotators->pdata[i], angle, animate);
  cam_countdown_set_angle (CAM_COUNTDOWN (self->countdown), angle, animate);
  cam_zoom_slider_set_angle (CAM_ZOOM_SLIDER (self->zoom_slider), angle);
  cam_zoom_slider_set_angle (CAM_ZOOM_SLIDER (self->focus_slider), angle);
  cam_island_set_orientation (CAM_ISLAND (self->island), ui_turn (self));
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_orientation (CamSensors *sensors, GParamSpec *pspec, CamView *self)
{
  self->orientation = cam_sensors_get_orientation (sensors);
  apply_orientation (self, TRUE);
}

static gboolean preview_cover (CamView *self);

/* the shell turned the screen (the phone turned with auto-rotate on, or back upright): the layout follows the
 * window's new size by itself; the icons, the preview and the leveler are put right here, without the 300 ms turn
 * (the whole screen has just turned) */
static void
on_display_rotation (CamSensors *sensors, GParamSpec *pspec, CamView *self)
{
  int width, height;

  self->display_rotation = cam_sensors_get_display_rotation (sensors);
  apply_orientation (self, FALSE);
  cam_viewfinder_set_leveler (CAM_VIEWFINDER (self->viewfinder), g_settings_get_boolean (self->settings, "leveler")
                                                                   && cam_sensors_has_tilt (self->sensors),
                              ui_tilt (self));
  if (self->camera && cam_pipeline_is_streaming (self->pipeline))
    {
      cam_pipeline_get_size (self->pipeline, &width, &height);
      cam_viewfinder_set_geometry (CAM_VIEWFINDER (self->viewfinder), preview_turn (self), is_front (self),
                                   preview_cover (self), width, height);
    }
}

/* ---- state ----------------------------------------------------------------------------------------------------- */

static void
update_controls (CamView *self)
{
  gboolean idle = self->state == ST_IDLE;
  gboolean recording = self->state == ST_RECORDING || self->state == ST_PAUSED;
  gboolean facings = FALSE;

  for (CamFacing f = CAM_FACING_BACK; f <= CAM_FACING_EXTERNAL; f++)
    if (f != self->facing && cam_devices_main_for (self->cameras, f))
      facings = TRUE;
  /* flip: more than one facing and not recording; pause in its place while recording */
  gtk_widget_set_visible (self->flip, facings && !recording && self->state != ST_PRE_RECORDING);
  gtk_widget_set_sensitive (self->flip, idle);
  gtk_widget_set_visible (self->pause, recording || self->state == ST_PRE_RECORDING);
  gtk_widget_set_sensitive (self->gallery, idle);
  /* the shutter: none in SCAN; in photo mode disabled during the countdown; in video mode it cancels or stops */
  gtk_widget_set_visible (self->shutter, self->mode != CAM_MODE_SCAN);
  gtk_widget_set_sensitive (self->shutter, self->mode == CAM_MODE_VIDEO ? self->state != ST_PRE_RECORDING
                                                                        : idle && !self->taking);
  gtk_widget_set_visible (self->pro_button, self->mode != CAM_MODE_SCAN);
  /* flash: idle, or recording in video mode for the torch */
  gtk_widget_set_sensitive (self->flash_button, idle || (self->mode == CAM_MODE_VIDEO && recording));
  gtk_widget_set_sensitive (self->lenses, idle || recording);
  cam_mode_selector_set_recording (CAM_MODE_SELECTOR (self->modes), recording);
  update_pro_row (self);
}

static void
set_state (CamView *self, CamState state)
{
  self->state = state;
  /* a recording does not breathe: no automatic searching until it is over */
  if (self->focus)
    cam_focus_set_hold (self->focus, state == ST_RECORDING || state == ST_PAUSED);
  update_controls (self);
}

/* ---- the gallery thumbnail (§5.1) ------------------------------------------------------------------------------ */

static void
on_thumbnail (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = cam_storage_thumbnail_finish (result, &error);
  CamView *self;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  self = data;
  if (error)
    g_message ("thumbnail: %s", error->message);
  /* while recording the card keeps the placeholder (the new file is still pending) */
  if (self->state != ST_RECORDING && self->state != ST_PAUSED)
    cam_gallery_set_thumbnail (CAM_GALLERY (cam_rotator_get_child (CAM_ROTATOR (self->gallery))), texture);
}

static void
update_thumbnail (CamView *self)
{
  const char *newest = cam_storage_get_newest (self->storage);

  g_cancellable_cancel (self->thumb_cancel);
  g_clear_object (&self->thumb_cancel);
  if (!newest || self->state == ST_RECORDING || self->state == ST_PAUSED)
    {
      cam_gallery_set_thumbnail (CAM_GALLERY (cam_rotator_get_child (CAM_ROTATOR (self->gallery))), NULL);
      return;
    }
  self->thumb_cancel = g_cancellable_new ();
  cam_storage_thumbnail_async (newest, CAM_THUMB_PX, self->thumb_cancel, on_thumbnail, self);
}

static void
on_storage_changed (CamStorage *storage, CamView *self)
{
  update_thumbnail (self);
}

static void
on_gallery (CamGallery *gallery, CamView *self)
{
  g_autoptr(GdkAppLaunchContext) context = NULL;

  if (self->state != ST_IDLE)
    return;
  context = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (self)));
  if (!cam_storage_open_gallery (self->storage, context))
    toast (self, _("No app available to handle this action"));
}

/* ---- the camera ------------------------------------------------------------------------------------------------ */

/* the stream for the mode: about 2 MP of the photo aspect (owner, 11-10), the video quality, 4:3 to scan */
static gboolean
stream_size (CamView *self, int *width, int *height)
{
  if (self->mode == CAM_MODE_VIDEO && can_record (self))
    {
      if (!quality_available (self, self->quality))
        self->quality = quality_available (self, 1080) ? 1080 : 720;
      *width = self->quality == 1080 ? 1920 : 1280;
      *height = self->quality == 1080 ? 1080 : 720;
      return TRUE;
    }
  if (is_photo_mode (self) && self->wide
      && cam_camera_pick_size (self->camera, 16, 9, CAM_STREAM_TARGET_PIXELS, width, height))
    return TRUE;
  if (cam_camera_pick_size (self->camera, 4, 3, CAM_STREAM_TARGET_PIXELS, width, height))
    return TRUE;
  return cam_camera_pick_size (self->camera, 16, 9, CAM_STREAM_TARGET_PIXELS, width, height);
}

static gboolean
preview_cover (CamView *self)
{
  /* PreviewView's fitCenter as the live app shows it: 4:3 whole, 16:9 and video filling the viewfinder */
  return self->mode == CAM_MODE_VIDEO || (is_photo_mode (self) && self->wide);
}

/* The process starts afresh, on the camera the settings now name (see cam_pipeline_needs_fresh_process) */
static void
restart_app (CamView *self)
{
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_auto(GStrv) argv = cam_restart_argv (exe ? exe : "/proc/self/exe");

  g_message ("camera: %s was opened before; a new process opens it (libcamerasrc cannot open it again after another camera)",
             self->camera->id);
  tune_flush (self);
  if (self->led != CAM_LED_OFF)
    cam_flash_led_set_sync (CAM_LED_OFF);
  self->led = CAM_LED_OFF;
  cam_flash_led_flush ();
  g_settings_sync ();
  execv (argv[0], argv);  /* by its own path: the process keeps its name */
  g_warning ("camera: cannot start a new process: %s", g_strerror (errno));
  fail (self, _("There was an error while preparing the stream. Please report this to the developers."));
}

static void
bind_camera (CamView *self)
{
  int width, height;

  if (!self->running || !self->camera)
    return;
  if (cam_pipeline_needs_fresh_process (self->pipeline, self->camera->id))
    {
      restart_app (self);
      return;
    }
  if (!stream_size (self, &width, &height))
    {
      fail (self, _("There was an error while preparing the stream. Please report this to the developers."));
      return;
    }
  if (!g_strcmp0 (self->bound_id, self->camera->id) && self->bound_w == width && self->bound_h == height
      && cam_pipeline_is_streaming (self->pipeline))
    {
      cam_viewfinder_set_geometry (CAM_VIEWFINDER (self->viewfinder), preview_turn (self), is_front (self),
                                   preview_cover (self), width, height);
      return;
    }
  /* the last frame stays, blurred, until the new stream runs (PreviewBlurView) */
  if (cam_pipeline_is_streaming (self->pipeline))
    cam_viewfinder_freeze (CAM_VIEWFINDER (self->viewfinder));
  g_free (self->bound_id);
  self->bound_id = g_strdup (self->camera->id);
  self->bound_w = width;
  self->bound_h = height;
  /* a new binding starts at 1× (CameraX's zoom state is the camera's) */
  neo_anim_stop (self->zoom_anim);
  apply_zoom (self, 1, FALSE);
  cam_pipeline_set_scanning (self->pipeline, FALSE);
  g_message ("camera: %s (%s) at %d×%d", self->camera->id, facing_name (self->camera->facing), width, height);
  apply_tuning (self);
  tune_sync (self);
  cam_pipeline_start (self->pipeline, self->camera->id, width, height);
}

static void
on_streaming (CamPipeline *pipeline, CamView *self)
{
  int width, height;

  g_free (self->ok_id);
  self->ok_id = g_strdup (self->bound_id);
  cam_pipeline_get_size (pipeline, &width, &height);
  cam_viewfinder_set_geometry (CAM_VIEWFINDER (self->viewfinder), preview_turn (self), is_front (self),
                               preview_cover (self), width, height);
  cam_viewfinder_thaw (CAM_VIEWFINDER (self->viewfinder));
  cam_pipeline_set_zoom (pipeline, self->zoom);
  cam_pipeline_set_scanning (pipeline, self->mode == CAM_MODE_SCAN);
  self->scanning_paused = FALSE;
  sync_torch (self);
  update_controls (self);
}

/* A camera that did not start after a switch is not the end of the app: the one that streamed before is still
 * there, so the switch goes back to it (a camera that fails at start-up still fails the app, as it did) */
static gboolean
go_back (CamView *self)
{
  CamCamera *camera = NULL;

  if (!self->ok_id || !g_strcmp0 (self->ok_id, self->bound_id))
    return FALSE;
  for (guint i = 0; i < self->cameras->len && !camera; i++)
    if (!g_strcmp0 (((CamCamera *) self->cameras->pdata[i])->id, self->ok_id))
      camera = self->cameras->pdata[i];
  if (!camera)
    return FALSE;
  use_camera (self, camera);
  remember_camera (self);
  update_controls (self);
  toast (self, _("This camera did not start, back to the one before"));
  bind_camera (self);
  return TRUE;
}

static void
on_pipeline_error (CamPipeline *pipeline, int error, const char *detail, CamView *self)
{
  g_message ("camera: error %d: %s", error, detail ? detail : "");
  switch (error)
    {
    case CAM_PIPELINE_ERROR_BUSY:
      if (!go_back (self))
        fail (self, _("This camera is already in use, try closing the app using it."));
      break;
    case CAM_PIPELINE_ERROR_STREAM:
      if (!go_back (self))
        fail (self, _("There was an error while preparing the stream. Please report this to the developers."));
      break;
    default:
      toast (self, _("Error while setting up the session. The app will try to recover."));
      break;
    }
}

static void
select_camera (CamView *self, CamFacing facing)
{
  CamCamera *camera = cam_devices_main_for (self->cameras, facing);

  if (!camera)
    camera = cam_devices_main_for (self->cameras, CAM_FACING_BACK);
  if (!camera)
    camera = cam_devices_main_for (self->cameras, CAM_FACING_FRONT);
  if (!camera && self->cameras->len)
    camera = self->cameras->pdata[0];
  self->camera = camera;
  self->facing = camera ? camera->facing : facing;
  update_lenses (self);
  update_flash (self);
}

static void
use_camera (CamView *self, CamCamera *camera)
{
  self->camera = camera;
  self->facing = camera->facing;
  update_lenses (self);
  update_flash (self);
}

static void
on_flip (CamFlip *flip, CamView *self)
{
  CamFacing next = self->facing;

  if (self->state != ST_IDLE)
    return;
  /* BACK → FRONT → EXTERNAL → the first again, over the facings there are */
  for (int i = 1; i <= 3; i++)
    {
      CamFacing f = (self->facing + i) % 3;

      if (cam_devices_main_for (self->cameras, f))
        {
          next = f;
          break;
        }
    }
  if (next == self->facing)
    return;
  cam_flip_spin (flip);
  select_camera (self, next);
  remember_camera (self);
  update_controls (self);
  bind_camera (self);
}

/* ---- modes (§3) ------------------------------------------------------------------------------------------------ */

static void
set_mode (CamView *self, CamMode mode, gboolean animate)
{
  static const char *const names[] = { "photo", "video", "qr", "night" };
  CamMode old = self->mode;

  if (mode == CAM_MODE_VIDEO && cam_encoder_get () && !can_record (self))
    {
      toast (self, _("No camera supports video recording."));
      cam_mode_selector_set_mode (CAM_MODE_SELECTOR (self->modes), self->mode, FALSE);
      return;
    }
  self->mode = mode;
  if (mode == CAM_MODE_SCAN)
    tune_show (self, FALSE);
  g_settings_set_string (self->settings, "last-camera-mode", names[mode]);
  cam_mode_selector_set_mode (CAM_MODE_SELECTOR (self->modes), mode, animate);
  slide_pro (self, FALSE);
  neo_snackbar_dismiss (NEO_SNACKBAR (self->snackbar));
  if (mode != CAM_MODE_PHOTO && mode != CAM_MODE_NIGHT)
    self->force_torch = FALSE;
  self->scan_flash = FLASH_OFF;
  /* the shutter's morph: photo ↔ video; invisible in SCAN */
  if (mode != CAM_MODE_SCAN)
    cam_shutter_set_video (CAM_SHUTTER (self->shutter), mode == CAM_MODE_VIDEO, animate && old != CAM_MODE_SCAN);
  apply_grid (self);
  update_flash (self);
  update_island (self);
  update_controls (self);
  bind_camera (self);
}

static void
on_mode_chosen (CamModeSelector *selector, int mode, CamView *self)
{
  if (self->state != ST_IDLE)
    return;
  set_mode (self, mode, TRUE);
}

static void
on_fling (CamViewfinder *viewfinder, int direction, CamView *self)
{
  /* not during a pinch, nor 500 ms after its last change */
  if (self->state != ST_IDLE || g_get_monotonic_time () - self->pinch_end < CAM_MS_PINCH_GUARD * 1000)
    return;
  CamMode next = self->mode;

  /* the next mode the phone has, round the ends */
  do
    next = (next + CAM_N_MODES + direction) % CAM_N_MODES;
  while (!cam_mode_selector_is_available (CAM_MODE_SELECTOR (self->modes), next));
  if (next != self->mode)
    set_mode (self, next, TRUE);
}

/* ---- focus (§9) ------------------------------------------------------------------------------------------------ */

/* the words for a search that found no clear peak: what the lens saw, not an error code */
static const char *
focus_failed_text (CamAfReason reason)
{
  switch (reason)
    {
    case CAM_AF_REASON_FLAT:
      return _("Too dark to focus");
    case CAM_AF_REASON_TIMEOUT:
      return _("Focusing took too long");
    default:
      return _("Nothing to focus on here");
    }
}

static void
on_tapped (CamViewfinder *viewfinder, double x, double y, CamView *self)
{
  double fx, fy;

  slide_pro (self, FALSE);
  /* a camera without a lens has fixed focus (no ring, nothing to wait for), and MANUAL has its slider */
  if (!self->focus || !cam_focus_available (self->focus) || !cam_pipeline_is_streaming (self->pipeline)
      || cam_focus_get_mode (self->focus) == CAM_FOCUS_MANUAL || !cam_viewfinder_to_frame (viewfinder, x, y, &fx, &fy))
    return;
  /* the ring where the finger is, the search on that part of the frame */
  self->ring_up = TRUE;
  cam_viewfinder_focus (viewfinder, x, y);
  cam_focus_at (self->focus, fx, fy);
}

static void
on_focus_state (CamFocus *focus, int state, int reason, CamView *self)
{
  CamViewfinder *viewfinder = CAM_VIEWFINDER (self->viewfinder);

  switch (state)
    {
    case CAM_FOCUS_SEARCHING:
      /* a search nobody tapped for (the first one, a scene that changed): the ring is in the middle */
      if (!self->ring_up)
        {
          double x, y;

          cam_viewfinder_get_centre (viewfinder, &x, &y);
          self->ring_up = TRUE;
          cam_viewfinder_focus (viewfinder, x, y);
        }
      break;
    case CAM_FOCUS_FOCUSED:
      self->ring_up = FALSE;
      cam_viewfinder_focus_done (viewfinder, TRUE);
      break;
    case CAM_FOCUS_FAILED:
      self->ring_up = FALSE;
      cam_viewfinder_focus_done (viewfinder, FALSE);
      toast (self, focus_failed_text (reason));
      break;
    default:
      if (self->ring_up)
        cam_viewfinder_focus_done (viewfinder, TRUE);
      self->ring_up = FALSE;
      break;
    }
}

static CamFocusMode
focus_mode_of (CamView *self)
{
  g_autofree char *mode = g_settings_get_string (self->settings, "af-mode");

  return g_str_equal (mode, "tap") ? CAM_FOCUS_TAP : g_str_equal (mode, "manual") ? CAM_FOCUS_MANUAL : CAM_FOCUS_AUTO;
}

/* the manual slider: where the lens stands, as 0 to 100 */
static void
focus_slider_sync (CamView *self)
{
  gboolean manual = self->focus && cam_focus_available (self->focus) && cam_focus_get_mode (self->focus) == CAM_FOCUS_MANUAL;
  double position = g_settings_get_double (self->settings, "af-manual-position");
  g_autofree char *text = g_strdup_printf ("%d", (int) lround (position * 100));

  cam_zoom_slider_set (CAM_ZOOM_SLIDER (self->focus_slider), position, text);
  gtk_widget_set_visible (self->focus_slider, manual);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
sync_focus (CamView *self)
{
  if (!self->focus)
    return;
  cam_focus_set_position (self->focus, g_settings_get_double (self->settings, "af-manual-position"));
  cam_focus_set_mode (self->focus, focus_mode_of (self));
  focus_slider_sync (self);
}

static gboolean
on_focus_save (gpointer data)
{
  CamView *self = data;

  self->focus_save = 0;
  g_settings_set_double (self->settings, "af-manual-position", cam_focus_get_position (self->focus));
  return G_SOURCE_REMOVE;
}

static void
on_focus_slider (CamZoomSlider *slider, double progress, CamView *self)
{
  g_autofree char *text = g_strdup_printf ("%d", (int) lround (progress * 100));

  cam_zoom_slider_set (slider, progress, text);
  cam_focus_set_position (self->focus, progress);
  g_clear_handle_id (&self->focus_save, g_source_remove);
  self->focus_save = g_timeout_add (TUNE_SAVE_MS, on_focus_save, self);
}

static void update_settings_rows (CamView *self);

static void
on_focus_available (CamFocus *focus, GParamSpec *pspec, CamView *self)
{
  update_settings_rows (self);
  sync_focus (self);
}

/* ---- photos (§4.4, §12, §21) ----------------------------------------------------------------------------------- */

static int
capture_rotation (CamView *self)
{
  /* the frame's turn to upright for the phone as it is held: Android's JPEG orientation */
  int sensor = self->camera->orientation, device = self->orientation;

  return is_front (self) ? (sensor - device + 360) % 360 : (sensor + device) % 360;
}

static void
on_photo_saved (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *path = cam_photo_save_finish (result, &error);

  if (!path)
    g_warning ("photo: %s", error->message);
  else
    g_message ("photo: %s", path);
  g_object_unref (data);
}

static void
end_flash (CamView *self)
{
  gtk_widget_set_visible (self->white, FALSE);
  if (current_flash (self) != FLASH_TORCH)
    set_led (self, CAM_LED_OFF);
  sync_shell (self);
}

static void
on_grabbed (GstSample *frame, gpointer data)
{
  CamView *self = data;
  CamPhoto *photo;
  gboolean lit;
  g_autofree char *dir = NULL;
  g_autofree char *save_location = g_settings_get_string (self->settings, "save-location");

  /* whether the LED was lit for the frame, as sysfs has it, before it goes off */
  lit = cam_flash_led_is_lit ();
  self->taking = FALSE;
  end_flash (self);
  if (!frame)
    {
      update_controls (self);
      g_object_unref (self);
      return;
    }
  /* capture started: the blink and the sound */
  cam_viewfinder_blink (CAM_VIEWFINDER (self->viewfinder));
  cam_sounds_play (CAM_SOUND_SHUTTER, g_settings_get_boolean (self->settings, "shutter-sound"));
  photo = g_new0 (CamPhoto, 1);
  photo->frame = frame;
  photo->time = g_date_time_new_now_local ();
  photo->rotation = capture_rotation (self);
  photo->mirror = is_front (self) && g_settings_get_boolean (self->settings, "photo-ffc-mirror");
  photo->zoom = self->zoom;
  photo->path = cam_storage_new_path (self->storage, FALSE, photo->time);
  if (g_str_equal (save_location, "on"))
    photo->location = cam_sensors_get_location (self->sensors);
  photo->has_flash = has_led (self);
  photo->flash_fired = lit;
  update_controls (self);
  cam_photo_save_async (photo, NULL, on_photo_saved, self);
}

static gboolean
on_flash_ready (gpointer data)
{
  CamView *self = data;

  self->flash_timer = 0;
  cam_pipeline_grab (self->pipeline, on_grabbed, self);
  return G_SOURCE_REMOVE;
}

static void
take_photo (CamView *self)
{
  CamFlash flash = current_flash (self);

  if (self->taking)
    return;
  self->taking = TRUE;
  g_object_ref (self);
  update_controls (self);
  if (flash == FLASH_ON)
    {
      /* no pre-flash metering on this stack: the LED fully on for the shot, the exposure given time to follow */
      set_led (self, CAM_LED_FLASH);
      self->flash_timer = g_timeout_add (FLASH_SETTLE_MS, on_flash_ready, self);
    }
  else if (flash == FLASH_SCREEN)
    {
      /* ScreenFlashView: the window white (and at full brightness where the shell lets it) for the shot */
      gtk_widget_set_visible (self->white, TRUE);
      keep_bright (self, TRUE);
      self->flash_timer = g_timeout_add (SCREEN_FLASH_MS, on_flash_ready, self);
    }
  else
    cam_pipeline_grab (self->pipeline, on_grabbed, self);
}

/* ---- night: a burst merged into one picture ------------------------------------------------------------------- */

/* NIGHT takes NIGHT_FRAMES frames one after the other (the LED, when the flash is on, lit for the series only), saves
 * each as a JPEG in the cache, and hands them to Minerva's burst CLI (`python -m minerva_eyes.burst`: ECC alignment
 * and a robust mean, one tested implementation shared with the agent's `look`; this app has no OpenCV and a C port
 * would be a second copy to keep in step). The merged file keeps the first frame's EXIF and joins the gallery like a
 * normal shot. Without the CLI the first frame is the photo. The LED is off right after the last grab, and a
 * watchdog switches it off after NIGHT_LED_MAX_MS whatever happens. */
#define NIGHT_FRAMES 6
#define NIGHT_LED_MAX_MS 5000

typedef struct {
  CamView *view;
  char    *dir;                 /* the cache directory of the frames */
  char    *final;               /* the gallery name */
  int      grabbed, saved, failed;
  gboolean lit;
} NightShot;

static void
night_free (NightShot *shot)
{
  g_autoptr(GDir) dir = g_dir_open (shot->dir, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *path = g_build_filename (shot->dir, name, NULL);
      g_unlink (path);
    }
  g_rmdir (shot->dir);
  g_free (shot->dir);
  g_free (shot->final);
  g_free (shot);
}

static char *
night_frame_path (NightShot *shot, int i)
{
  g_autofree char *name = g_strdup_printf ("frame-%d.jpg", i);

  return g_build_filename (shot->dir, name, NULL);
}

static gboolean
night_watchdog (gpointer data)
{
  CamView *self = data;

  self->night_timer = 0;
  g_warning ("night: LED still on after %d ms, switching it off", NIGHT_LED_MAX_MS);
  set_led (self, CAM_LED_OFF);
  return G_SOURCE_REMOVE;
}

static void
night_done (NightShot *shot, gboolean ok, const char *why)
{
  CamView *self = shot->view;

  self->taking = FALSE;
  if (ok)
    g_message ("photo: %s", shot->final);
  else
    {
      g_warning ("night: %s", why);
      toast (self, _("The burst could not be saved."));
    }
  update_controls (self);
  update_thumbnail (self);
  g_application_release (G_APPLICATION (self->app));
  night_free (shot);
  g_object_unref (self);
}

static void
on_night_merged (GObject *object, GAsyncResult *result, gpointer data)
{
  NightShot *shot = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *first = night_frame_path (shot, 0);
  g_autofree char *merged = g_build_filename (shot->dir, "merged.jpg", NULL);
  g_autoptr(GFile) from = NULL;
  g_autoptr(GFile) to = g_file_new_for_path (shot->final);
  gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (object), result, &error);

  if (!ok)
    g_warning ("night: the merge failed (%s), keeping the first frame", error->message);
  from = g_file_new_for_path (ok ? merged : first);
  /* into the gallery under a hidden name first: it never sees half a file */
  g_autofree char *part = g_strdup_printf ("%s.part", shot->final);
  g_autoptr(GFile) tmp = g_file_new_for_path (part);

  if (g_file_copy (from, tmp, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error)
      && g_file_move (tmp, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error))
    night_done (shot, TRUE, NULL);
  else
    night_done (shot, FALSE, error->message);
}

static void
night_merge (NightShot *shot)
{
  const char *python = g_getenv ("NEO_CAMERA_BURST_PYTHON");
  g_autofree char *default_python = g_build_filename (g_get_home_dir (), ".local/share/minerva/venv/bin/python", NULL);
  g_autofree char *eyes = g_build_filename (g_get_home_dir (), ".local/share/minerva/eyes", NULL);
  g_autofree char *merged = g_build_filename (shot->dir, "merged.jpg", NULL);
  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) proc = NULL;

  if (!python)
    python = default_python;
  g_ptr_array_add (argv, g_strdup (python));
  g_ptr_array_add (argv, g_strdup ("-m"));
  g_ptr_array_add (argv, g_strdup ("minerva_eyes.burst"));
  g_ptr_array_add (argv, g_strdup ("--out"));
  g_ptr_array_add (argv, g_strdup (merged));
  for (int i = 0; i < NIGHT_FRAMES; i++)
    g_ptr_array_add (argv, night_frame_path (shot, i));
  g_ptr_array_add (argv, NULL);
  g_subprocess_launcher_setenv (launcher, "PYTHONPATH", eyes, TRUE);
  proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) argv->pdata, &error);
  if (!proc)
    {
      /* no burst CLI on this phone: the first frame is the photo */
      g_warning ("night: cannot start the merge (%s)", error->message);
      g_autoptr(GFile) from = g_file_new_for_path (g_ptr_array_index (argv, 5));
      g_autoptr(GFile) to = g_file_new_for_path (shot->final);
      g_autoptr(GError) copy_error = NULL;

      night_done (shot, g_file_copy (from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error),
                  copy_error ? copy_error->message : NULL);
      return;
    }
  g_subprocess_wait_check_async (proc, NULL, on_night_merged, shot);
}

static void
on_night_saved (GObject *object, GAsyncResult *result, gpointer data)
{
  NightShot *shot = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = cam_photo_save_finish (result, &error);

  if (!path)
    {
      g_warning ("night frame: %s", error->message);
      shot->failed++;
    }
  if (++shot->saved < NIGHT_FRAMES)
    return;
  if (shot->failed)
    night_done (shot, FALSE, "a frame could not be written");
  else
    night_merge (shot);
}

static void night_grab (NightShot *shot);

static void
on_night_grabbed (GstSample *frame, gpointer data)
{
  NightShot *shot = data;
  CamView *self = shot->view;
  CamPhoto *photo;

  if (!frame)
    {
      g_clear_handle_id (&self->night_timer, g_source_remove);
      set_led (self, CAM_LED_OFF);
      end_flash (self);
      night_done (shot, FALSE, "no frame");
      return;
    }
  if (shot->grabbed == 0)
    {
      shot->lit = cam_flash_led_is_lit ();
      cam_viewfinder_blink (CAM_VIEWFINDER (self->viewfinder));
      cam_sounds_play (CAM_SOUND_SHUTTER, g_settings_get_boolean (self->settings, "shutter-sound"));
    }
  photo = g_new0 (CamPhoto, 1);
  photo->frame = frame;
  photo->time = g_date_time_new_now_local ();
  photo->rotation = capture_rotation (self);
  photo->mirror = is_front (self) && g_settings_get_boolean (self->settings, "photo-ffc-mirror");
  photo->zoom = self->zoom;
  photo->path = night_frame_path (shot, shot->grabbed);
  if (shot->grabbed == 0)
    {
      g_autofree char *save_location = g_settings_get_string (self->settings, "save-location");

      if (g_str_equal (save_location, "on"))
        photo->location = cam_sensors_get_location (self->sensors);
    }
  photo->has_flash = has_led (self);
  photo->flash_fired = shot->lit;
  cam_photo_save_async (photo, NULL, on_night_saved, shot);
  if (++shot->grabbed < NIGHT_FRAMES)
    {
      night_grab (shot);
      return;
    }
  /* the series is taken: the light goes off now, not when the merge is done */
  g_clear_handle_id (&self->night_timer, g_source_remove);
  end_flash (self);
}

static void
night_grab (NightShot *shot)
{
  cam_pipeline_grab (shot->view->pipeline, on_night_grabbed, shot);
}

static gboolean
on_night_ready (gpointer data)
{
  NightShot *shot = data;

  shot->view->flash_timer = 0;
  night_grab (shot);
  return G_SOURCE_REMOVE;
}

static void
take_night (CamView *self)
{
  CamFlash flash = current_flash (self);
  NightShot *shot;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "neo-camera-night-XXXXXX", NULL);

  if (self->taking)
    return;
  if (!g_mkdtemp (dir))
    {
      toast (self, _("The burst could not be saved."));
      return;
    }
  self->taking = TRUE;
  g_object_ref (self);
  g_application_hold (G_APPLICATION (self->app));
  update_controls (self);
  shot = g_new0 (NightShot, 1);
  shot->view = self;
  shot->dir = g_steal_pointer (&dir);
  shot->final = cam_storage_new_path (self->storage, FALSE, now);
  if (flash == FLASH_ON || flash == FLASH_SCREEN)
    self->night_timer = g_timeout_add (NIGHT_LED_MAX_MS, night_watchdog, self);
  if (flash == FLASH_ON)
    {
      set_led (self, CAM_LED_FLASH);
      self->flash_timer = g_timeout_add (FLASH_SETTLE_MS, on_night_ready, shot);
    }
  else if (flash == FLASH_SCREEN)
    {
      gtk_widget_set_visible (self->white, TRUE);
      keep_bright (self, TRUE);
      self->flash_timer = g_timeout_add (SCREEN_FLASH_MS, on_night_ready, shot);
    }
  else
    night_grab (shot);
}

/* ---- the countdown (§10) --------------------------------------------------------------------------------------- */

static void
on_countdown_finished (CamCountdown *countdown, CamView *self)
{
  set_state (self, ST_IDLE);
  if (self->mode == CAM_MODE_NIGHT)
    take_night (self);
  else if (is_photo_mode (self))
    take_photo (self);
  else
    start_video (self);
}

static void
start_timer_and_run (CamView *self)
{
  if (self->timer > 0)
    {
      set_state (self, ST_COUNTDOWN);
      cam_countdown_start (CAM_COUNTDOWN (self->countdown), self->timer);
      return;
    }
  if (self->mode == CAM_MODE_NIGHT)
    take_night (self);
  else if (is_photo_mode (self))
    take_photo (self);
  else
    start_video (self);
}

/* ---- video (§11) ----------------------------------------------------------------------------------------------- */

static gboolean
on_duration_tick (gpointer data)
{
  CamView *self = data;

  if (self->recorder)
    cam_mode_selector_set_duration (CAM_MODE_SELECTOR (self->modes), cam_recorder_get_duration (self->recorder));
  return G_SOURCE_CONTINUE;
}

static void
on_recording_done (const char *path, const GError *error, gpointer data)
{
  CamView *self = data;

  self->recorder = NULL;
  g_application_release (G_APPLICATION (self->app));
  g_clear_handle_id (&self->duration_timer, g_source_remove);
  if (path)
    g_message ("video: %s", path);
  else
    g_warning ("video: %s", error ? error->message : "failed");
  cam_sounds_play (CAM_SOUND_VIDEO_STOP, g_settings_get_boolean (self->settings, "shutter-sound"));
  cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, TRUE);
  cam_pause_set_paused (CAM_PAUSE (cam_rotator_get_child (CAM_ROTATOR (self->pause))), FALSE, FALSE);
  set_state (self, ST_IDLE);
  update_thumbnail (self);
  if (!path && !self->closing)
    toast (self, _("Error while setting up the session. The app will try to recover."));
  g_object_unref (self);
}

static gboolean
on_start_recording (gpointer data)
{
  CamView *self = data;
  const CamEncoder *encoder = cam_encoder_get ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *path = NULL, *mirror_mode = g_settings_get_string (self->settings, "video-mirror-mode");
  g_autofree char *save_location = g_settings_get_string (self->settings, "save-location");
  CamLocation location = { 0 };
  gboolean mirror = g_str_equal (mirror_mode, "on") || (g_str_equal (mirror_mode, "front") && is_front (self));

  self->start_timer = 0;
  if (self->state != ST_PRE_RECORDING)
    return G_SOURCE_REMOVE;
  if (g_str_equal (save_location, "on"))
    location = cam_sensors_get_location (self->sensors);
  path = cam_storage_new_path (self->storage, TRUE, now);
  g_object_ref (self);
  self->rec_audio = self->mic && cam_recorder_can_record_audio ();
  self->recorder = cam_recorder_new (self->pipeline, path, encoder->element,
                                     self->quality == 1080 ? BITRATE_1080 : BITRATE_720,
                                     self->rec_audio, mirror, capture_rotation (self),
                                     &location, on_recording_done, self, &error);
  if (!self->recorder)
    {
      g_warning ("video: %s", error->message);
      g_object_unref (self);
      cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, TRUE);
      set_state (self, ST_IDLE);
      toast (self, _("There was an error while preparing the stream. Please report this to the developers."));
      return G_SOURCE_REMOVE;
    }
  /* the app outlives its window until the file is whole: mp4mux writes the index at the end */
  g_application_hold (G_APPLICATION (self->app));
  cam_recorder_start (self->recorder);
  set_state (self, ST_RECORDING);
  cam_gallery_set_thumbnail (CAM_GALLERY (cam_rotator_get_child (CAM_ROTATOR (self->gallery))), NULL);
  cam_mode_selector_set_duration (CAM_MODE_SELECTOR (self->modes), 0);
  self->duration_timer = g_timeout_add (200, on_duration_tick, self);
  return G_SOURCE_REMOVE;
}

static void
start_video (CamView *self)
{
  gboolean sound;

  if (!cam_encoder_get () || !can_record (self))
    {
      cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, TRUE);
      set_state (self, ST_IDLE);
      return;
    }
  set_state (self, ST_PRE_RECORDING);
  /* the start sound, and 500 ms so it is not in the recording */
  sound = cam_sounds_play (CAM_SOUND_VIDEO_START, g_settings_get_boolean (self->settings, "shutter-sound"));
  self->start_timer = g_timeout_add (sound ? CAM_MS_START_SOUND_GAP : 0, on_start_recording, self);
}

static void
stop_video (CamView *self)
{
  if (self->recorder)
    cam_recorder_stop (self->recorder);
}

static void
on_pause (CamPause *pause, CamView *self)
{
  if (self->state == ST_RECORDING)
    {
      cam_recorder_set_paused (self->recorder, TRUE);
      cam_pause_set_paused (pause, TRUE, TRUE);
      set_state (self, ST_PAUSED);
    }
  else if (self->state == ST_PAUSED)
    {
      cam_recorder_set_paused (self->recorder, FALSE);
      cam_pause_set_paused (pause, FALSE, TRUE);
      set_state (self, ST_RECORDING);
    }
}

/* ---- the shutter (§4.4) ---------------------------------------------------------------------------------------- */

static void
shutter (CamView *self)
{
  if (!self->running || !cam_pipeline_is_streaming (self->pipeline))
    return;
  if (is_photo_mode (self))
    {
      if (self->state != ST_IDLE || self->taking)
        return;
      cam_shutter_pulse (CAM_SHUTTER (self->shutter));
      slide_pro (self, FALSE);
      start_timer_and_run (self);
    }
  else if (self->mode == CAM_MODE_VIDEO)
    {
      if (self->state == ST_COUNTDOWN)
        {
          cam_countdown_cancel (CAM_COUNTDOWN (self->countdown));
          cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, TRUE);
          set_state (self, ST_IDLE);
        }
      else if (self->state == ST_IDLE)
        {
          if (!cam_encoder_get ())
            return;       /* the encoder is still being measured (first start only) */
          cam_shutter_set_recording (CAM_SHUTTER (self->shutter), TRUE, TRUE);
          slide_pro (self, FALSE);
          start_timer_and_run (self);
        }
      else if (self->state == ST_RECORDING || self->state == ST_PAUSED)
        stop_video (self);
    }
}

static void
on_shutter (CamShutter *button, CamView *self)
{
  shutter (self);
}

/* ---- the volume keys (§18) ------------------------------------------------------------------------------------- */

static void
volume_key (CamView *self, int direction)
{
  g_autofree char *action = g_settings_get_string (self->settings, "volume-buttons-action");

  if (!self->running || self->in_settings)
    return;
  if (g_str_equal (action, "shutter"))
    shutter (self);
  else if (g_str_equal (action, "focus"))
    {
      double w = gtk_widget_get_width (self->viewfinder), h = gtk_widget_get_height (self->viewfinder);

      on_tapped (CAM_VIEWFINDER (self->viewfinder), w / 2, h / 2, self);
    }
  else if (g_str_equal (action, "mic-mute"))
    {
      if (self->mode == CAM_MODE_VIDEO && gtk_widget_get_sensitive (self->p_mic.button))
        on_mic (NULL, self);
    }
  else if (g_str_equal (action, "zoom"))
    zoom_step (self, g_settings_get_boolean (self->settings, "volume-buttons-invert") ? -direction : direction);
}

/* ---- sensors --------------------------------------------------------------------------------------------------- */

static void
on_thermal_ok (gpointer data)
{
}

static void
on_thermal (CamSensors *sensors, GParamSpec *pspec, CamView *self)
{
  CamThermal thermal = cam_sensors_get_thermal (sensors);
  const char *text = NULL;

  update_island (self);
  if (thermal == self->thermal)
    return;
  self->thermal = thermal;
  switch (thermal)
    {
    case CAM_THERMAL_MODERATE:
      text = _("Moderate thermal throttling, make sure the device isn't under direct sunlight.");
      break;
    case CAM_THERMAL_SEVERE:
      text = _("Severe thermal throttling, it is recommended to let the device cool down a bit.");
      break;
    case CAM_THERMAL_CRITICAL:
      text = _("Critical thermal throttling, the app might be closed soon.");
      break;
    case CAM_THERMAL_EMERGENCY:
      text = _("Emergency thermal throttling, the app will close now.");
      break;
    case CAM_THERMAL_SHUTDOWN:
      text = _("Shutdown thermal throttling, the device will power off.");
      break;
    default:
      return;
    }
  neo_snackbar_show (NEO_SNACKBAR (self->snackbar), text, _("OK"), on_thermal_ok, self, INDEFINITE);
  if (thermal >= CAM_THERMAL_EMERGENCY)
    {
      /* the app closes (a recording is finished first, by sync_running) */
      self->closing = TRUE;
      sync_running (self);
      self->close_timer = g_timeout_add (CAM_MS_TOAST, on_close_timeout, self);
    }
}

static void
on_battery (CamSensors *sensors, GParamSpec *pspec, CamView *self)
{
  update_island (self);
}

static void
on_tilt (CamSensors *sensors, GParamSpec *pspec, CamView *self)
{
  cam_viewfinder_set_leveler (CAM_VIEWFINDER (self->viewfinder), g_settings_get_boolean (self->settings, "leveler"),
                              ui_tilt (self));
}

static void
on_location_refused (CamSensors *sensors, CamView *self)
{
  g_settings_set_string (self->settings, "save-location", "off");
  toast (self, _("Location permissions not granted by the user."));
}

static void
sync_sensors (CamView *self)
{
  g_autofree char *location = g_settings_get_string (self->settings, "save-location");
  gboolean leveler = g_settings_get_boolean (self->settings, "leveler") && cam_sensors_has_tilt (self->sensors);

  cam_sensors_set_active (self->sensors, self->running);
  cam_sensors_set_locating (self->sensors, g_str_equal (location, "on"));
  cam_sensors_set_leveling (self->sensors, leveler && self->running);
  cam_viewfinder_set_leveler (CAM_VIEWFINDER (self->viewfinder), leveler, ui_tilt (self));
}

static void
on_setting_changed (GSettings *settings, const char *key, CamView *self)
{
  if (g_str_equal (key, "save-location") || g_str_equal (key, "leveler"))
    sync_sensors (self);
  else if (g_str_equal (key, "bright-screen") || g_str_equal (key, "volume-buttons-action"))
    sync_shell (self);
  else if (g_str_equal (key, "af-mode"))
    sync_focus (self);
}

static void
on_location_answer (gboolean turn_on, gpointer data)
{
  CamView *self = data;

  g_settings_set_string (self->settings, "save-location", turn_on ? "on" : "off");
  g_object_unref (self);
}

/* ---- running or not -------------------------------------------------------------------------------------------- */

static void
sync_running (CamView *self)
{
  /* The cameras run only while the window is shown AND in focus (owner, 2026-11-12: "if camera is not in focus -
   * stop the cameras"). Shown alone is not enough: recents (like GNOME's overview) shows every window live, which
   * wakes a suspended one, so a camera left in the background opened its sensor each time recents opened (the
   * focus motor clicked). Recents, the shade or another app over the camera take the focus and stop it too. */
  gboolean run = !self->closing && !self->in_settings && self->camera && self->window
                 && gtk_widget_get_mapped (GTK_WIDGET (self)) && !gtk_window_is_suspended (self->window)
                 && gtk_window_is_active (self->window);

  if (run == self->running)
    {
      sync_shell (self);
      return;
    }
  self->running = run;
  if (run)
    {
      /* keepScreenOn */
      self->inhibit = gtk_application_inhibit (self->app, self->window, GTK_APPLICATION_INHIBIT_IDLE,
                                               _("Camera"));
      bind_camera (self);
    }
  else
    {
      /* a recording ends with the screen (onPause), a countdown is dropped */
      if (self->state == ST_COUNTDOWN)
        {
          cam_countdown_cancel (CAM_COUNTDOWN (self->countdown));
          if (self->mode == CAM_MODE_VIDEO)
            cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, FALSE);
          set_state (self, ST_IDLE);
        }
      if (self->state == ST_PRE_RECORDING)
        {
          g_clear_handle_id (&self->start_timer, g_source_remove);
          cam_shutter_set_recording (CAM_SHUTTER (self->shutter), FALSE, FALSE);
          set_state (self, ST_IDLE);
        }
      stop_video (self);
      if (self->inhibit)
        gtk_application_uninhibit (self->app, self->inhibit);
      self->inhibit = 0;
      cam_pipeline_stop (self->pipeline);
      g_clear_pointer (&self->bound_id, g_free);
      cam_viewfinder_thaw (CAM_VIEWFINDER (self->viewfinder));
    }
  sync_torch (self);
  sync_sensors (self);
  sync_shell (self);
}

static void
on_window_state (GtkWindow *window, GParamSpec *pspec, CamView *self)
{
  sync_running (self);
}

/* the Settings rows this phone can back (and the focus row only while the camera in use has a lens) */
static void
update_settings_rows (CamView *self)
{
  cam_settings_set_rows (CAM_SETTINGS (self->settings_page),
                         (can_record (self) ? CAM_SETTINGS_VIDEO : 0)
                         | (self->bright_hook ? CAM_SETTINGS_BRIGHT : 0)
                         | (cam_sensors_can_locate (self->sensors) ? CAM_SETTINGS_LOCATION : 0)
                         | (cam_sensors_has_tilt (self->sensors) ? CAM_SETTINGS_LEVELER : 0)
                         | (shell_has (self, "TakeVolumeKeys") ? CAM_SETTINGS_GESTURES : 0)
                         | (self->focus && cam_focus_available (self->focus) ? CAM_SETTINGS_FOCUS : 0)
                         | (cam_devices_main_for (self->cameras, CAM_FACING_FRONT) ? CAM_SETTINGS_FRONT : 0));
}

static void
on_encoder (gpointer data)
{
  CamView *self = data;
  const CamEncoder *encoder = cam_encoder_get ();

  g_message ("encoder: %s (720p %s, 1080p %s)", encoder && encoder->element ? encoder->element : "none",
             encoder && encoder->hd ? "yes" : "no", encoder && encoder->fhd ? "yes" : "no");
  update_settings_rows (self);
  if (self->mode == CAM_MODE_VIDEO && !can_record (self))
    set_mode (self, CAM_MODE_PHOTO, FALSE);
  else
    {
      update_pro_row (self);
      if (self->mode == CAM_MODE_VIDEO)
        bind_camera (self);
    }
  g_object_unref (self);
}

/* ---- layout (§2) ----------------------------------------------------------------------------------------------- */

static void
cam_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                  int *nat_base)
{
  *min = 0;
  *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? 360 : 761;
}

static int
viewfinder_height (int height)
{
  return height - CAM_MODE_SELECTOR_H - CAM_PRIMARY_BAR_H - CAM_SECONDARY_BAR_H;
}

/* wider than tall (the shell turned the screen with the phone): the bars stand as columns right of the preview, in
 * the order they lie under it upright: the secondary bar, the primary bar, the modes at the edge */
static gboolean
is_landscape (int width, int height)
{
  return width > height;
}

static int
viewfinder_width (int width)
{
  return width - CAM_SECONDARY_BAR_H - CAM_PRIMARY_BAR_H - CAM_MODE_COLUMN_W;
}

static void
place (GtkWidget *child, double x, double y, int w, int h)
{
  gtk_widget_allocate (child, w, h, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
place_island (CamView *self, int vf_w, int vf_h)
{
  int w, h, o = cam_island_get_orientation (CAM_ISLAND (self->island));
  double fw, fh, x, y;
  GskTransform *t;

  gtk_widget_measure (self->island, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  h = CAM_ISLAND_H;
  /* the corner that is top-left for the user, the card turned to read upright (IslandView.kt:161-200) */
  fw = o % 180 ? h : w;
  fh = o % 180 ? w : h;
  x = o == 0 || o == 90 ? CAM_ISLAND_MARGIN : vf_w - CAM_ISLAND_MARGIN - fw;
  y = o == 0 || o == 270 ? CAM_ISLAND_MARGIN : vf_h - CAM_ISLAND_MARGIN - fh;
  t = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x + fw / 2, y + fh / 2));
  t = gsk_transform_rotate (t, CAM_COMPENSATION (o));
  t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (-w / 2.0, -h / 2.0));
  gtk_widget_allocate (self->island, w, h, -1, t);
}

/* over the lower edge of the picture (not of the viewfinder, whose black bands the 4:3 picture leaves) */
static void
place_tune (CamView *self, int vf_w, int vf_h)
{
  graphene_rect_t picture;
  int w, h;
  double x, bottom;

  if (!gtk_widget_get_visible (self->tune_panel))
    return;
  cam_viewfinder_get_picture (CAM_VIEWFINDER (self->viewfinder), &picture);
  if (picture.size.width <= 0)
    picture = GRAPHENE_RECT_INIT (0, 0, vf_w, vf_h);
  x = picture.origin.x;
  bottom = MIN (vf_h, picture.origin.y + picture.size.height);
  gtk_widget_measure (self->tune_panel, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  w = MIN (w, (int) picture.size.width - 2 * CAM_TUNE_MARGIN);
  gtk_widget_measure (self->tune_panel, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
  place (self->tune_panel, x + (picture.size.width - w) / 2.0, bottom - h - CAM_TUNE_MARGIN, w, h);
}

/* the landscape layout: what the portrait one puts left to right in a bar goes top to bottom in its column, the
 * end buttons the same distances in from the top and the bottom (flash and flip at the top, pro and the gallery at
 * the bottom), the shutter in the middle 16 dp from the preview as it is 16 dp under it upright */
static void
allocate_landscape (CamView *self, int width, int height)
{
  int vf_w = viewfinder_width (width), w, h;
  double col2 = vf_w, col1 = vf_w + CAM_SECONDARY_BAR_H, col0 = col1 + CAM_PRIMARY_BAR_H;
  double pro_h = CAM_PRO_ROW_H, shutter_x = col1 + CAM_PRIMARY_BAR_PAD;
  graphene_rect_t picture;

  if (gtk_widget_get_visible (self->settings_page))
    place (self->settings_page, 0, 0, width, height);
  place (self->viewfinder, 0, 0, vf_w, height);
  cam_viewfinder_get_picture (CAM_VIEWFINDER (self->viewfinder), &picture);
  if (picture.size.width <= 0)
    picture = GRAPHENE_RECT_INIT (0, 0, vf_w, height);
  place (self->countdown, picture.origin.x, picture.origin.y, (int) picture.size.width, (int) picture.size.height);
  if (gtk_widget_get_visible (self->island))
    place_island (self, vf_w, height);
  if (gtk_widget_get_visible (self->zoom_slider))
    place (self->zoom_slider, (vf_w - CAM_ZOOM_SLIDER_W) / 2.0, height - CAM_ZOOM_SLIDER_H, CAM_ZOOM_SLIDER_W,
           CAM_ZOOM_SLIDER_H);
  if (gtk_widget_get_visible (self->focus_slider))
    place (self->focus_slider, (vf_w - CAM_ZOOM_SLIDER_W) / 2.0, height - 2 * CAM_ZOOM_SLIDER_H - CAM_FOCUS_SLIDER_GAP,
           CAM_ZOOM_SLIDER_W, CAM_ZOOM_SLIDER_H);
  place_tune (self, vf_w, height);
  /* the pro row along the preview's bottom, sliding down off it as it hides */
  if (gtk_widget_get_visible (self->pro_row))
    gtk_widget_allocate (self->pro_row, vf_w, (int) ceil (pro_h), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, height - pro_h
                                                                                 + (1 - self->pro_shown) * pro_h)));
  place (self->snackbar, 0, 0, vf_w, height);

  /* the secondary column: the flash 40 dp from the top, the lenses in the middle, pro 40 dp from the bottom */
  if (gtk_widget_get_visible (self->flash_button))
    place (self->flash_button, col2 + (CAM_SECONDARY_BAR_H - CAM_BAR_BUTTON) / 2, CAM_BAR_BUTTON_MARGIN,
           CAM_BAR_BUTTON, CAM_BAR_BUTTON);
  gtk_widget_measure (self->lenses, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  gtk_widget_measure (self->lenses, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
  place (self->lenses, col2 + (CAM_SECONDARY_BAR_H - CAM_DIVIDER_H - w) / 2.0 + 0.5, (height - h) / 2.0, w, h);
  if (gtk_widget_get_visible (self->pro_button))
    place (self->pro_button, col2 + (CAM_SECONDARY_BAR_H - CAM_BAR_BUTTON) / 2,
           height - CAM_BAR_BUTTON_MARGIN - CAM_BAR_BUTTON, CAM_BAR_BUTTON, CAM_BAR_BUTTON);

  /* the primary column: the flip 32 dp from the top, the shutter in the middle, the gallery 32 dp from the bottom */
  if (gtk_widget_get_visible (self->flip))
    place (self->flip, shutter_x + (CAM_SHUTTER_VIEW - CAM_FLIP_SIZE) / 2.0, CAM_GALLERY_MARGIN, CAM_FLIP_SIZE,
           CAM_FLIP_SIZE);
  if (gtk_widget_get_visible (self->pause))
    place (self->pause, shutter_x + (CAM_SHUTTER_VIEW - CAM_PAUSE_SIZE) / 2.0,
           CAM_GALLERY_MARGIN + (CAM_FLIP_SIZE - CAM_PAUSE_SIZE) / 2.0, CAM_PAUSE_SIZE, CAM_PAUSE_SIZE);
  if (gtk_widget_get_visible (self->shutter))
    place (self->shutter, shutter_x, (height - CAM_SHUTTER_VIEW) / 2.0, CAM_SHUTTER_VIEW, CAM_SHUTTER_VIEW);
  place (self->gallery, shutter_x + (CAM_SHUTTER_VIEW - CAM_GALLERY_SIZE) / 2.0,
         height - CAM_GALLERY_MARGIN - CAM_GALLERY_SIZE, CAM_GALLERY_SIZE, CAM_GALLERY_SIZE);
  place (self->modes, col0, 0, CAM_MODE_COLUMN_W, height);
}

static void
cam_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CamView *self = CAM_VIEW (widget);
  int vf_h = viewfinder_height (height);
  double bar2 = vf_h, bar1 = vf_h + CAM_SECONDARY_BAR_H, bar0 = height - CAM_MODE_SELECTOR_H;
  double pro_h = CAM_PRO_ROW_H;
  graphene_rect_t picture;
  int w, h;

  if (!self->viewfinder)
    return;
  /* the selector is laid out by the place() below, at its new size */
  cam_mode_selector_set_vertical (CAM_MODE_SELECTOR (self->modes), is_landscape (width, height));
  if (is_landscape (width, height))
    {
      allocate_landscape (self, width, height);
      return;
    }
  if (gtk_widget_get_visible (self->settings_page))
    place (self->settings_page, 0, 0, width, height);
  place (self->viewfinder, 0, 0, width, vf_h);
  /* the countdown over the preview's picture, not the screen */
  cam_viewfinder_get_picture (CAM_VIEWFINDER (self->viewfinder), &picture);
  if (picture.size.width <= 0)
    picture = GRAPHENE_RECT_INIT (0, 0, width, vf_h);
  place (self->countdown, picture.origin.x, picture.origin.y, (int) picture.size.width, (int) picture.size.height);
  if (gtk_widget_get_visible (self->island))
    place_island (self, width, vf_h);
  if (gtk_widget_get_visible (self->zoom_slider))
    place (self->zoom_slider, (width - CAM_ZOOM_SLIDER_W) / 2.0, vf_h - CAM_ZOOM_SLIDER_H, CAM_ZOOM_SLIDER_W,
           CAM_ZOOM_SLIDER_H);
  if (gtk_widget_get_visible (self->focus_slider))
    place (self->focus_slider, (width - CAM_ZOOM_SLIDER_W) / 2.0, vf_h - 2 * CAM_ZOOM_SLIDER_H - CAM_FOCUS_SLIDER_GAP,
           CAM_ZOOM_SLIDER_W, CAM_ZOOM_SLIDER_H);
  place_tune (self, width, vf_h);
  /* the pro row: its bottom on the secondary bar's top, sliding down by its height as it hides */
  if (gtk_widget_get_visible (self->pro_row))
    gtk_widget_allocate (self->pro_row, width, (int) ceil (pro_h), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, vf_h - pro_h
                                                                                 + (1 - self->pro_shown) * pro_h)));
  place (self->snackbar, 0, 0, width, vf_h);

  /* the secondary bar: pro 40 dp in, the lenses centred, the flash 40 dp from the end, all 12 dp down */
  if (gtk_widget_get_visible (self->pro_button))
    place (self->pro_button, CAM_BAR_BUTTON_MARGIN, bar2 + (CAM_SECONDARY_BAR_H - CAM_BAR_BUTTON) / 2,
           CAM_BAR_BUTTON, CAM_BAR_BUTTON);
  gtk_widget_measure (self->lenses, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  gtk_widget_measure (self->lenses, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
  place (self->lenses, (width - w) / 2.0, bar2 + (CAM_SECONDARY_BAR_H - CAM_DIVIDER_H - h) / 2.0 + 0.5, w, h);
  if (gtk_widget_get_visible (self->flash_button))
    place (self->flash_button, width - CAM_BAR_BUTTON_MARGIN - CAM_BAR_BUTTON,
           bar2 + (CAM_SECONDARY_BAR_H - CAM_BAR_BUTTON) / 2, CAM_BAR_BUTTON, CAM_BAR_BUTTON);

  /* the primary bar: 16 dp padding over the 75 dp row; the gallery 32 dp in, the flip 32 dp from the end */
  place (self->gallery, CAM_GALLERY_MARGIN, bar1 + CAM_PRIMARY_BAR_PAD + (CAM_SHUTTER_VIEW - CAM_GALLERY_SIZE) / 2.0,
         CAM_GALLERY_SIZE, CAM_GALLERY_SIZE);
  if (gtk_widget_get_visible (self->shutter))
    place (self->shutter, (width - CAM_SHUTTER_VIEW) / 2.0, bar1 + CAM_PRIMARY_BAR_PAD, CAM_SHUTTER_VIEW,
           CAM_SHUTTER_VIEW);
  if (gtk_widget_get_visible (self->flip))
    place (self->flip, width - CAM_GALLERY_MARGIN - CAM_FLIP_SIZE,
           bar1 + CAM_PRIMARY_BAR_PAD + (CAM_SHUTTER_VIEW - CAM_FLIP_SIZE) / 2.0, CAM_FLIP_SIZE, CAM_FLIP_SIZE);
  if (gtk_widget_get_visible (self->pause))
    place (self->pause, width - CAM_GALLERY_MARGIN - (CAM_FLIP_SIZE + CAM_PAUSE_SIZE) / 2.0,
           bar1 + CAM_PRIMARY_BAR_PAD + (CAM_SHUTTER_VIEW - CAM_PAUSE_SIZE) / 2.0, CAM_PAUSE_SIZE, CAM_PAUSE_SIZE);
  place (self->modes, 0, bar0, width, CAM_MODE_SELECTOR_H);
}

static void
cam_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CamView *self = CAM_VIEW (widget);
  int width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  int vf_h = viewfinder_height (height);
  GdkRGBA black = CAM_C (0), divider = neo_color (NEO_OUTLINE_VARIANT);

  if (!self->viewfinder)
    {
      gtk_snapshot_append_color (snapshot, &black, &GRAPHENE_RECT_INIT (0, 0, width, height));
      return;
    }
  if (self->in_settings)
    {
      gtk_widget_snapshot_child (widget, self->settings_page, snapshot);
      return;
    }
  gtk_snapshot_append_color (snapshot, &black, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_widget_snapshot_child (widget, self->viewfinder, snapshot);
  gtk_widget_snapshot_child (widget, self->countdown, snapshot);
  gtk_widget_snapshot_child (widget, self->island, snapshot);
  gtk_widget_snapshot_child (widget, self->zoom_slider, snapshot);
  gtk_widget_snapshot_child (widget, self->focus_slider, snapshot);
  gtk_widget_snapshot_child (widget, self->tune_panel, snapshot);
  gtk_widget_snapshot_child (widget, self->pro_row, snapshot);
  if (is_landscape (width, height))
    {
      int vf_w = viewfinder_width (width);

      /* the columns, the divider between the secondary and the primary one */
      gtk_snapshot_append_color (snapshot, &black, &GRAPHENE_RECT_INIT (vf_w, 0, width - vf_w, height));
      gtk_snapshot_append_color (snapshot, &divider,
                                 &GRAPHENE_RECT_INIT (vf_w + CAM_SECONDARY_BAR_H - CAM_DIVIDER_H, 0, CAM_DIVIDER_H,
                                                      height));
    }
  else
    {
      /* the bars are opaque over the pro row sliding down behind them */
      gtk_snapshot_append_color (snapshot, &black, &GRAPHENE_RECT_INIT (0, vf_h, width, height - vf_h));
      gtk_snapshot_append_color (snapshot, &divider,
                                 &GRAPHENE_RECT_INIT (0, vf_h + CAM_SECONDARY_BAR_H - CAM_DIVIDER_H, width,
                                                      CAM_DIVIDER_H));
    }
  gtk_widget_snapshot_child (widget, self->pro_button, snapshot);
  gtk_widget_snapshot_child (widget, self->lenses, snapshot);
  gtk_widget_snapshot_child (widget, self->flash_button, snapshot);
  gtk_widget_snapshot_child (widget, self->gallery, snapshot);
  gtk_widget_snapshot_child (widget, self->shutter, snapshot);
  gtk_widget_snapshot_child (widget, self->flip, snapshot);
  gtk_widget_snapshot_child (widget, self->pause, snapshot);
  gtk_widget_snapshot_child (widget, self->modes, snapshot);
  gtk_widget_snapshot_child (widget, self->snackbar, snapshot);
}

static void
cam_view_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (cam_view_parent_class)->map (widget);
  sync_running (CAM_VIEW (widget));
}

static void
cam_view_unmap (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (cam_view_parent_class)->unmap (widget);
  sync_running (CAM_VIEW (widget));
}

static void
cam_view_dispose (GObject *object)
{
  CamView *self = CAM_VIEW (object);
  GtkWidget *child;

  self->closing = TRUE;
  /* the window outlives this in its own dispose, and still notifies is-active */
  if (self->window)
    {
      g_signal_handlers_disconnect_by_data (self->window, self);
      g_object_remove_weak_pointer (G_OBJECT (self->window), (gpointer *) &self->window);
      self->window = NULL;
    }
  if (self->focus)
    g_signal_handlers_disconnect_by_data (self->focus, self);
  if (self->pipeline)
    {
      g_signal_handlers_disconnect_by_data (self->pipeline, self);
      cam_pipeline_stop (self->pipeline);
    }
  /* waits for logind: the app may be about to quit (and an OFF already asked for is sent before it does) */
  if (self->led != CAM_LED_OFF)
    cam_flash_led_set_sync (CAM_LED_OFF);
  self->led = CAM_LED_OFF;
  cam_flash_led_flush ();
  take_volume_keys (self, FALSE);
  keep_bright (self, FALSE);
  if (self->sensors)
    {
      g_signal_handlers_disconnect_by_data (self->sensors, self);
      cam_sensors_set_active (self->sensors, FALSE);
    }
  if (self->storage)
    g_signal_handlers_disconnect_by_data (self->storage, self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_cancellable_cancel (self->cancel);
  g_cancellable_cancel (self->thumb_cancel);
  g_clear_handle_id (&self->close_timer, g_source_remove);
  g_clear_handle_id (&self->slider_hide, g_source_remove);
  g_clear_handle_id (&self->focus_save, g_source_remove);
  g_clear_handle_id (&self->duration_timer, g_source_remove);
  g_clear_handle_id (&self->start_timer, g_source_remove);
  g_clear_handle_id (&self->flash_timer, g_source_remove);
  tune_flush (self);
  g_clear_pointer (&self->tune_id, g_free);
  g_clear_pointer (&self->zoom_anim, neo_anim_free);
  g_clear_pointer (&self->pro_anim, neo_anim_free);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_object (&self->cancel);
  g_clear_object (&self->thumb_cancel);
  g_clear_object (&self->pipeline);
  if (self->focus)
    g_signal_handlers_disconnect_by_data (self->focus, self);
  g_clear_object (&self->focus);
  g_clear_object (&self->sensors);
  g_clear_object (&self->storage);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->cameras, g_ptr_array_unref);
  g_clear_pointer (&self->rotators, g_ptr_array_unref);
  g_clear_pointer (&self->bound_id, g_free);
  g_clear_pointer (&self->ok_id, g_free);
  G_OBJECT_CLASS (cam_view_parent_class)->dispose (object);
}

static void
cam_view_class_init (CamViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cam_view_dispose;
  widget_class->measure = cam_view_measure;
  widget_class->size_allocate = cam_view_size_allocate;
  widget_class->snapshot = cam_view_snapshot;
  widget_class->map = cam_view_map;
  widget_class->unmap = cam_view_unmap;
}

static void
cam_view_init (CamView *self)
{
  self->cancel = g_cancellable_new ();
  self->rotators = g_ptr_array_new ();
  self->zoom = 1;
  self->quality = 1080;
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

/* the pieces, in the order they are drawn and take touches (last on top) */
static void
build (CamView *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *spacer;
  GtkWidget *scroller = gtk_scrolled_window_new ();

  self->viewfinder = cam_viewfinder_new (cam_pipeline_get_paintable (self->pipeline));
  gtk_widget_set_parent (self->viewfinder, widget);
  g_signal_connect (self->viewfinder, "tapped", G_CALLBACK (on_tapped), self);
  g_signal_connect (self->viewfinder, "fling", G_CALLBACK (on_fling), self);
  g_signal_connect (self->viewfinder, "pinch-begin", G_CALLBACK (on_pinch_begin), self);
  g_signal_connect (self->viewfinder, "pinch", G_CALLBACK (on_pinch), self);
  g_signal_connect (self->viewfinder, "pinch-end", G_CALLBACK (on_pinch_end), self);

  self->countdown = cam_countdown_new ();
  gtk_widget_set_parent (self->countdown, widget);
  g_signal_connect (self->countdown, "finished", G_CALLBACK (on_countdown_finished), self);
  self->island = cam_island_new ();
  gtk_widget_set_parent (self->island, widget);
  self->zoom_slider = cam_zoom_slider_new ();
  gtk_widget_set_visible (self->zoom_slider, FALSE);
  gtk_widget_set_parent (self->zoom_slider, widget);
  g_signal_connect (self->zoom_slider, "changed", G_CALLBACK (on_slider_changed), self);
  self->focus_slider = cam_zoom_slider_new ();
  gtk_widget_set_visible (self->focus_slider, FALSE);
  gtk_widget_set_parent (self->focus_slider, widget);
  g_signal_connect (self->focus_slider, "changed", G_CALLBACK (on_focus_slider), self);

  self->tune_panel = cam_tune_panel_new ();
  gtk_widget_set_visible (self->tune_panel, FALSE);
  gtk_widget_set_parent (self->tune_panel, widget);
  g_signal_connect (self->tune_panel, "changed", G_CALLBACK (on_tune_changed), self);
  g_signal_connect (self->tune_panel, "reset", G_CALLBACK (on_tune_reset), self);
  g_signal_connect (self->tune_panel, "done", G_CALLBACK (on_tune_done), self);

  /* the pro row: 16 dp spacers, 96 dp buttons, 8 dp over and under, scrolling sideways without a scrollbar */
  gtk_widget_set_margin_top (row, CAM_PRO_ROW_PAD_V);
  gtk_widget_set_margin_bottom (row, CAM_PRO_ROW_PAD_V);
  spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, CAM_PRO_ROW_SPACER, -1);
  gtk_box_append (GTK_BOX (row), spacer);
  pro_button (self, &self->p_aspect, row, G_CALLBACK (on_aspect));
  pro_button (self, &self->p_quality, row, G_CALLBACK (on_quality));
  pro_button (self, &self->p_fps, row, G_CALLBACK (on_nothing));
  pro_button (self, &self->p_hdr, row, G_CALLBACK (on_nothing));
  pro_button (self, &self->p_mic, row, G_CALLBACK (on_mic));
  pro_button (self, &self->p_timer, row, G_CALLBACK (on_timer));
  pro_button (self, &self->p_grid, row, G_CALLBACK (on_grid));
  pro_button (self, &self->p_tune, row, G_CALLBACK (on_tune));
  pro_button (self, &self->p_settings, row, G_CALLBACK (on_settings));
  spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, CAM_PRO_ROW_SPACER, -1);
  gtk_box_append (GTK_BOX (row), spacer);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), row);
  gtk_widget_add_css_class (scroller, "cam-pro-row");
  self->pro_row = scroller;
  gtk_widget_set_visible (self->pro_row, FALSE);
  gtk_widget_set_parent (self->pro_row, widget);

  /* the secondary bar */
  self->pro_button = bar_button (self, "cam-pro-button-symbolic", &(GtkWidget *) { NULL });
  gtk_widget_set_tooltip_text (self->pro_button, _("Pro settings"));
  gtk_widget_set_parent (self->pro_button, widget);
  g_signal_connect (self->pro_button, "clicked", G_CALLBACK (on_pro), self);
  self->lenses = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent (self->lenses, widget);
  self->flash_button = bar_button (self, "cam-flash-off-symbolic", &self->flash_icon);
  gtk_widget_set_tooltip_text (self->flash_button, _("Flash mode"));
  gtk_widget_set_parent (self->flash_button, widget);
  g_signal_connect (self->flash_button, "clicked", G_CALLBACK (on_flash_clicked), self);
  g_signal_connect (self->flash_button, "long-pressed", G_CALLBACK (on_flash_long_pressed), self);

  /* the primary bar */
  self->gallery = rotator (self, cam_gallery_new ());
  gtk_widget_set_tooltip_text (self->gallery, _("Gallery"));
  gtk_widget_set_parent (self->gallery, widget);
  g_signal_connect (cam_rotator_get_child (CAM_ROTATOR (self->gallery)), "clicked", G_CALLBACK (on_gallery), self);
  self->shutter = cam_shutter_new ();
  gtk_widget_set_tooltip_text (self->shutter, _("Shutter"));
  gtk_widget_set_parent (self->shutter, widget);
  g_signal_connect (self->shutter, "clicked", G_CALLBACK (on_shutter), self);
  self->flip = rotator (self, cam_flip_new ());
  gtk_widget_set_tooltip_text (self->flip, _("Flip camera"));
  gtk_widget_set_parent (self->flip, widget);
  g_signal_connect (cam_rotator_get_child (CAM_ROTATOR (self->flip)), "clicked", G_CALLBACK (on_flip), self);
  self->pause = rotator (self, cam_pause_new ());
  gtk_widget_set_tooltip_text (self->pause, _("Pause/resume video recording"));
  gtk_widget_set_visible (self->pause, FALSE);
  gtk_widget_set_parent (self->pause, widget);
  g_signal_connect (cam_rotator_get_child (CAM_ROTATOR (self->pause)), "clicked", G_CALLBACK (on_pause), self);

  self->modes = cam_mode_selector_new ();
  cam_mode_selector_set_available (CAM_MODE_SELECTOR (self->modes), CAM_MODE_SCAN,
                                   self->pipeline && cam_pipeline_can_scan (self->pipeline));
  gtk_widget_set_parent (self->modes, widget);
  g_signal_connect (self->modes, "mode-chosen", G_CALLBACK (on_mode_chosen), self);

  /* the snackbar over the viewfinder's bottom, above the secondary bar (§19.4) */
  self->snackbar = neo_snackbar_new ();
  gtk_widget_add_css_class (self->snackbar, "cam-snackbar");
  gtk_widget_set_parent (self->snackbar, widget);

  self->settings_page = cam_settings_new (self->settings, 0);
  gtk_widget_set_visible (self->settings_page, FALSE);
  gtk_widget_set_parent (self->settings_page, widget);
  g_signal_connect (self->settings_page, "closed", G_CALLBACK (on_settings_closed), self);

  self->zoom_anim = neo_anim_new (widget, on_zoom_anim, self);
  self->pro_anim = neo_anim_new (widget, on_pro_anim, self);
}

static CamGrid
grid_from_name (const char *name)
{
  return g_str_equal (name, "3x3") ? CAM_GRID_3X3 : g_str_equal (name, "4x4") ? CAM_GRID_4X4
         : g_str_equal (name, "golden") ? CAM_GRID_GOLDEN : CAM_GRID_OFF;
}

static gboolean start_scan = FALSE;

void
cam_window_set_start_scan (gboolean scan)
{
  start_scan = scan;
}

static void
restore (CamView *self)
{
  g_autofree char *facing = g_settings_get_string (self->settings, "last-camera-facing");
  g_autofree char *camera_id = g_settings_get_string (self->settings, "last-camera-id");
  g_autofree char *mode = g_settings_get_string (self->settings, "last-camera-mode");
  g_autofree char *grid = g_settings_get_string (self->settings, "last-grid-mode");
  g_autofree char *aspect = g_settings_get_string (self->settings, "aspect-ratio");
  g_autofree char *quality = g_settings_get_string (self->settings, "video-quality");
  g_autofree char *photo_flash = g_settings_get_string (self->settings, "photo-flash-mode");
  g_autofree char *video_flash = g_settings_get_string (self->settings, "video-flash-mode");
  CamMode last = g_str_equal (mode, "video") ? CAM_MODE_VIDEO : g_str_equal (mode, "qr") ? CAM_MODE_SCAN
                                                                                          : g_str_equal (mode, "night") ? CAM_MODE_NIGHT
                                                                                                                        : CAM_MODE_PHOTO;

  if (start_scan)
    last = CAM_MODE_SCAN;           /* `--scan`: this once; the saved mode stays what it was */
  self->grid = grid_from_name (grid);
  self->timer = g_settings_get_int (self->settings, "timer-mode");
  self->wide = g_str_equal (aspect, "16:9");
  self->quality = g_str_equal (quality, "720p") ? 720 : 1080;
  self->mic = g_settings_get_boolean (self->settings, "last-mic-mode");
  self->photo_flash = flash_from_name (photo_flash);
  self->video_flash = flash_from_name (video_flash);
  self->mode = last;
  select_camera (self, g_str_equal (facing, "front") ? CAM_FACING_FRONT
                       : g_str_equal (facing, "external") ? CAM_FACING_EXTERNAL : CAM_FACING_BACK);
  /* the lens that was in use, if it is still there and shown */
  for (guint i = 0; camera_id[0] && i < self->cameras->len; i++)
    {
      CamCamera *camera = self->cameras->pdata[i];

      if (!g_strcmp0 (camera->id, camera_id) && !camera_hidden (self, camera))
        use_camera (self, camera);
    }
  /* VIDEO without an encoder that can record falls back to PHOTO */
  if ((last == CAM_MODE_VIDEO && cam_encoder_get () && !can_record (self))
      || !cam_mode_selector_is_available (CAM_MODE_SELECTOR (self->modes), last))
    self->mode = CAM_MODE_PHOTO;
  cam_mode_selector_set_mode (CAM_MODE_SELECTOR (self->modes), self->mode, FALSE);
  if (self->mode != CAM_MODE_SCAN)
    cam_shutter_set_video (CAM_SHUTTER (self->shutter), self->mode == CAM_MODE_VIDEO, FALSE);
  apply_grid (self);
  update_flash (self);
  update_island (self);
  update_controls (self);
  update_thumbnail (self);
}

static void
on_qr_dismissed (gpointer data)
{
  CamView *self = data;

  self->scanning_paused = FALSE;
  if (!self->closing)
    cam_pipeline_scan_resume (self->pipeline);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  g_object_unref (self);
}

static void
on_barcode_found (CamPipeline *pipeline, const char *text, CamView *self)
{
  if (self->mode != CAM_MODE_SCAN || self->scanning_paused || !self->running)
    return;
  self->scanning_paused = TRUE;
  /* a key code is a secret: never in the log */
  g_message ("scan: %s", cam_qr_is_key (text) ? "(a Minerva key code)" : text);
  cam_qr_sheet_show (GTK_WIDGET (self), text, on_qr_dismissed, g_object_ref (self));
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CamView *self)
{
  /* a keyboard's volume keys, where the shell lets them through (and for tests) */
  if (keyval == GDK_KEY_AudioRaiseVolume || keyval == GDK_KEY_AudioLowerVolume)
    {
      volume_key (self, keyval == GDK_KEY_AudioRaiseVolume ? 1 : -1);
      return TRUE;
    }
  if (keyval == GDK_KEY_space || keyval == GDK_KEY_Return || keyval == GDK_KEY_WebCam)
    {
      shutter (self);
      return TRUE;
    }
  return FALSE;
}

static void
on_toast_action (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  toast (data, g_variant_get_string (parameter, NULL));
}

static void
on_window_destroy (GtkWidget *window, CamView *self)
{
  self->closing = TRUE;
  sync_running (self);
}

/* first start with GeoClue there: "Add photo and video locations" (§19.2) */
static gboolean
on_ask_location (gpointer data)
{
  CamView *self = data;

  if (!self->closing)
    cam_location_dialog (GTK_WIDGET (self), on_location_answer, g_object_ref (self));
  return G_SOURCE_REMOVE;
}

GtkWindow *
cam_window_new (GtkApplication *app)
{
  GtkWindow *window = GTK_WINDOW (neo_window_new (app));
  CamView *self = g_object_new (CAM_TYPE_VIEW, NULL);
  g_autoptr(GError) error = NULL;
  GtkOverlay *overlay;
  GtkEventController *keys = gtk_event_controller_key_new ();
  GSimpleAction *toast_action = g_simple_action_new ("toast", G_VARIANT_TYPE_STRING);
  g_autofree char *save_location = NULL;

  self->window = window;
  /* a recording keeps the view past its window (until the file is whole) */
  g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &self->window);
  self->app = app;
  self->settings = g_settings_new (APP_ID);
  self->cameras = cam_devices_enumerate ();
  self->pipeline = cam_pipeline_new (&error);
  self->focus = cam_focus_new (self->settings);
  if (self->pipeline)
    cam_pipeline_set_focus (self->pipeline, self->focus);
  self->sensors = cam_sensors_new ();
  self->storage = cam_storage_new ();
  self->bright_hook = shell_has (self, "KeepScreenBright");
  gtk_window_set_title (window, _("Camera"));
  neo_window_set_content (window, GTK_WIDGET (self));
  overlay = neo_window_get_overlay (GTK_WIDGET (window));
  gtk_widget_add_css_class (GTK_WIDGET (window), "cam-window");

  /* the screen flash and the toast over everything */
  self->white = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->white, "cam-screen-flash");
  gtk_widget_set_visible (self->white, FALSE);
  gtk_widget_set_can_target (self->white, FALSE);
  gtk_overlay_add_overlay (overlay, self->white);
  self->toast = cam_toast_new ();
  gtk_overlay_add_overlay (overlay, self->toast);
  g_signal_connect (toast_action, "activate", G_CALLBACK (on_toast_action), self);
  g_action_map_add_action (G_ACTION_MAP (window), G_ACTION (toast_action));
  g_object_unref (toast_action);

  if (!self->pipeline)
    {
      /* GStreamer lacks a piece (libcamerasrc, gtk4paintablesink, …): nothing can stream */
      g_warning ("camera: %s", error->message);
      self->pipeline = NULL;
      self->closing = TRUE;
      gtk_window_fullscreen (window);
      toast (self, _("There was an error while preparing the stream. Please report this to the developers."));
      self->close_timer = g_timeout_add (CAM_MS_TOAST + 300, on_close_timeout, self);
      return window;
    }
  build (self);
  restore (self);
  self->display_rotation = cam_sensors_get_display_rotation (self->sensors);
  apply_orientation (self, FALSE);
  g_signal_connect (self->pipeline, "streaming", G_CALLBACK (on_streaming), self);
  g_signal_connect (self->pipeline, "error", G_CALLBACK (on_pipeline_error), self);
  g_signal_connect (self->pipeline, "barcode", G_CALLBACK (on_barcode_found), self);
  g_signal_connect (self->focus, "state-changed", G_CALLBACK (on_focus_state), self);
  g_signal_connect (self->focus, "notify::available", G_CALLBACK (on_focus_available), self);
  sync_focus (self);
  g_signal_connect (self->sensors, "notify::orientation", G_CALLBACK (on_orientation), self);
  g_signal_connect (self->sensors, "notify::display-rotation", G_CALLBACK (on_display_rotation), self);
  g_signal_connect (self->sensors, "notify::thermal", G_CALLBACK (on_thermal), self);
  g_signal_connect (self->sensors, "notify::battery", G_CALLBACK (on_battery), self);
  g_signal_connect (self->sensors, "notify::tilt", G_CALLBACK (on_tilt), self);
  g_signal_connect (self->sensors, "location-refused", G_CALLBACK (on_location_refused), self);
  g_signal_connect (self->storage, "changed", G_CALLBACK (on_storage_changed), self);
  g_signal_connect (self->settings, "changed", G_CALLBACK (on_setting_changed), self);
  g_signal_connect (window, "notify::suspended", G_CALLBACK (on_window_state), self);
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_window_state), self);
  g_signal_connect (window, "destroy", G_CALLBACK (on_window_destroy), self);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);

  /* the encoder: measured once per kernel and GStreamer, in the background */
  if (cam_encoder_get ())
    on_encoder (g_object_ref (self));
  else
    cam_encoder_probe (G_CALLBACK (on_encoder), g_object_ref (self));

  gtk_window_fullscreen (window);
  if (!self->cameras->len)
    {
      fail (self, _("No cameras were found on the device, cannot start the app."));
      return window;
    }
  save_location = g_settings_get_string (self->settings, "save-location");
  if (g_str_equal (save_location, "unset") && cam_sensors_can_locate (self->sensors))
    g_timeout_add_full (G_PRIORITY_DEFAULT, 500, on_ask_location, g_object_ref (self), g_object_unref);
  return window;
}

void
cam_window_volume_key (GtkWindow *window, int direction)
{
  GtkWidget *content = neo_window_get_overlay (GTK_WIDGET (window))
                       ? gtk_overlay_get_child (neo_window_get_overlay (GTK_WIDGET (window))) : NULL;

  if (content && CAM_IS_VIEW (content))
    volume_key (CAM_VIEW (content), direction);
}

void
cam_window_show_scan (GtkWindow *window)
{
  GtkWidget *content = neo_window_get_overlay (GTK_WIDGET (window))
                       ? gtk_overlay_get_child (neo_window_get_overlay (GTK_WIDGET (window))) : NULL;
  CamView *self;

  if (!content || !CAM_IS_VIEW (content))
    return;
  self = CAM_VIEW (content);
  if (self->state == ST_IDLE && self->mode != CAM_MODE_SCAN
      && cam_mode_selector_is_available (CAM_MODE_SELECTOR (self->modes), CAM_MODE_SCAN))
    set_mode (self, CAM_MODE_SCAN, TRUE);
}
